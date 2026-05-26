-- spkg_fetch.lua — dependency fetching via git clone
--
-- Reads SharpDeps.lua for dependency declarations.
-- Fetches each dep into spkg_packages/<name>/.
-- Updates Sharp.lock with resolved versions.
-- Supports recursive fetching of transitive dependencies.

local M = {}

-- Load SharpDeps.lua; return deps table or empty table
local function load_deps_file(path)
    if not spkg.file_exists(path) then
        return {}
    end
    local ok, result = pcall(dofile, path)
    if not ok then
        print("spkg: error loading " .. path .. ": " .. tostring(result))
        return {}
    end
    if type(result) ~= "table" then
        print("spkg: " .. path .. " must return a table of dependencies")
        return {}
    end
    return result
end

-- Resolve a single dependency to a URL
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

-- Clone a single dependency
local function clone_dep(resolved, pkg_dir)
    if spkg.dir_exists(pkg_dir) then
        return true  -- already cloned
    end

    if not resolved or not resolved.url then
        print("spkg: cannot resolve dependency: " .. resolved.name)
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

    -- Get commit hash (avoid /tmp file race condition)
    local cmd = "cd '" .. pkg_dir .. "' && git rev-parse HEAD"
    local r = spkg.run_cmd(cmd)
    local commit = r.ok and r.out:gsub("%s+", "") or nil
    if commit and commit ~= "" then
        resolved.commit = commit
    else
        resolved.commit = "HEAD"
    end
    spkg_lock.save_one(resolved)
    return true
end

-- Fetch one level of deps from a SharpDeps.lua file, given a base directory
local function fetch_from_deps_file(deps_path, home, visited)
    local deps = load_deps_file(deps_path)
    if #deps == 0 then return true end

    spkg.mkdir_p("spkg_packages")

    for _, dep in ipairs(deps) do
        if visited[dep.name] then goto continue end
        visited[dep.name] = true

        local pkg_dir = "spkg_packages/" .. dep.name
        local resolved = resolve_dep(dep, home)

        if not clone_dep(resolved, pkg_dir) then
            return false
        end

        -- Recurse into dep's SharpDeps.lua
        local dep_deps_path = pkg_dir .. "/SharpDeps.lua"
        if spkg.file_exists(dep_deps_path) then
            if not fetch_from_deps_file(dep_deps_path, home, visited) then
                return false
            end
        end

        ::continue::
    end

    return true
end

-- Public: fetch all deps from project's SharpDeps.lua recursively
function M.fetch_recursive(home)
    local visited = {}
    return fetch_from_deps_file("SharpDeps.lua", home, visited)
end

-- Public: fetch deps (non-recursive, for backward compat)
function M.fetch_deps(home)
    local deps = load_deps_file("SharpDeps.lua")
    if #deps == 0 then return true end

    spkg.mkdir_p("spkg_packages")

    for _, dep in ipairs(deps) do
        local pkg_dir = "spkg_packages/" .. dep.name
        local resolved = resolve_dep(dep, home)
        if not clone_dep(resolved, pkg_dir) then
            return false
        end
    end

    return true
end

-- List dependencies
function M.list_deps()
    local deps = load_deps_file("SharpDeps.lua")
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
