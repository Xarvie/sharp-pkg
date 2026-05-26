-- spkg_fetch.lua — dependency fetching via git clone
--
-- Reads SharpDeps.lua for dependency declarations.
-- Fetches each dep into spkg_packages/<name>/.
-- Updates Sharp.lock with resolved versions.

local M = {}

-- Load SharpDeps.lua; return deps table or empty table
local function load_deps()
    if not spkg.file_exists("SharpDeps.lua") then
        return {}
    end
    local ok, deps = pcall(dofile, "SharpDeps.lua")
    if not ok or type(deps) ~= "table" then
        return {}
    end
    return deps
end

-- Resolve a single dependency
local function resolve_dep(dep, home)
    local name = dep.name
    local version = dep.version or "*"

    -- Check Sharp.lock first
    local lock = spkg_lock.load()
    if lock and lock[name] then
        return {
            name    = name,
            version = lock[name].version,
            url     = lock[name].source,
            tag     = lock[name].tag,
            commit  = lock[name].commit,
        }
    end

    -- Read source config (~/.sharp/config.spkm)
    local config_file = home .. "/.sharp/config.spkm"
    local config = nil
    if spkg.file_exists(config_file) then
        local ok, cfg = pcall(dofile, config_file)
        if ok then config = cfg end
    end

    if config and config.source and config.source[name] then
        return {
            name    = name,
            version = version,
            url     = config.source[name],
            tag     = version ~= "*" and ("v" .. version) or nil,
        }
    end

    local default_url = "https://gitee.com/sharp-libs/{name}.git"
    if config and config.source and config.source["default"] then
        default_url = config.source["default"]
    end

    local url = default_url:gsub("{name}", name)
    return {
        name    = name,
        version = version,
        url     = url,
        tag     = version ~= "*" and ("v" .. version) or nil,
    }
end

function M.fetch_deps(home)
    local deps = load_deps()
    if #deps == 0 then return true end

    spkg.mkdir_p("spkg_packages")

    for _, dep in ipairs(deps) do
        local pkg_dir = "spkg_packages/" .. dep.name

        if spkg.dir_exists(pkg_dir) then
            print("  [cached] " .. dep.name)
        else
            local resolved = resolve_dep(dep, home)
            if not resolved or not resolved.url then
                print("spkg: cannot resolve dependency: " .. dep.name)
                return false
            end

            print("  [fetch] " .. resolved.url)

            local cmd
            if resolved.tag then
                cmd = string.format(
                    'git clone --depth 1 --branch "%s" "%s" "%s"',
                    resolved.tag, resolved.url, pkg_dir)
            else
                cmd = string.format(
                    'git clone --depth 1 "%s" "%s"',
                    resolved.url, pkg_dir)
            end

            local r = spkg.run_cmd(cmd)
            if not r.ok then
                print("    error:\n" .. r.out)
                return false
            end

            -- Get commit hash
            local saved = spkg.cwd()
            spkg.run_cmd("cd '" .. pkg_dir .. "' && git rev-parse HEAD > /tmp/spkg_commit.txt")
            spkg.run_cmd("cd '" .. saved .. "'")
            local commit = spkg.read_file("/tmp/spkg_commit.txt")
            if commit then commit = commit:gsub("%s+", "") end

            resolved.commit = commit or "HEAD"
            spkg_lock.save_one(resolved)
        end
    end

    return true
end

-- Fetch recursively: resolve transitive deps of each dependency
function M.fetch_recursive(home, visited)
    visited = visited or {}
    local deps = load_deps()

    for _, dep in ipairs(deps) do
        if not visited[dep.name] then
            visited[dep.name] = true

            local pkg_dir = "spkg_packages/" .. dep.name
            if not spkg.dir_exists(pkg_dir) then
                if not M.fetch_deps(home) then return false end
            end

            -- Load dep's SharpDeps.lua and fetch its deps
            local dep_deps_path = pkg_dir .. "/SharpDeps.lua"
            if spkg.file_exists(dep_deps_path) then
                local ok, dep_deps = pcall(dofile, dep_deps_path)
                if ok and type(dep_deps) == "table" then
                    for _, sub_dep in ipairs(dep_deps) do
                        if not visited[sub_dep.name] then
                            local sub_pkg = "spkg_packages/" .. sub_dep.name
                            if spkg.dir_exists(sub_pkg) then
                                print("  [cached] " .. sub_dep.name)
                            else
                                local resolved = resolve_dep(sub_dep, home)
                                if not resolved or not resolved.url then
                                    print("spkg: cannot resolve dependency: " .. sub_dep.name)
                                    return false
                                end

                                print("  [fetch] " .. resolved.url)
                                local cmd
                                if resolved.tag then
                                    cmd = string.format(
                                        'git clone --depth 1 --branch "%s" "%s" "%s"',
                                        resolved.tag, resolved.url, sub_pkg)
                                else
                                    cmd = string.format(
                                        'git clone --depth 1 "%s" "%s"',
                                        resolved.url, sub_pkg)
                                end
                                local r = spkg.run_cmd(cmd)
                                if not r.ok then
                                    print("    error:\n" .. r.out)
                                    return false
                                end

                                local saved = spkg.cwd()
                                spkg.run_cmd("cd '" .. sub_pkg .. "' && git rev-parse HEAD > /tmp/spkg_commit.txt")
                                spkg.run_cmd("cd '" .. saved .. "'")
                                local commit = spkg.read_file("/tmp/spkg_commit.txt")
                                if commit then commit = commit:gsub("%s+", "") end

                                resolved.commit = commit or "HEAD"
                                spkg_lock.save_one(resolved)
                            end
                        end
                    end
                end
            end
        end
    end

    return true
end

-- List dependencies (for spkg list command)
function M.list_deps()
    local deps = load_deps()
    if #deps == 0 then
        print("spkg: no dependencies declared.")
        return true
    end

    for _, dep in ipairs(deps) do
        local pkg_dir = "spkg_packages/" .. dep.name
        local status = spkg.dir_exists(pkg_dir) and "installed" or "missing"
        print("  " .. dep.name .. " (" .. (dep.version or "*") .. ") [" .. status .. "]")
    end
    return true
end

return M
