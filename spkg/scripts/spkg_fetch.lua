-- spkg_fetch.lua — dependency fetching via git clone
--
-- Reads dependencies from config.spkg (via b:dep() API).
-- Fetches each dep into spkg_packages/<name>/.
-- Updates Sharp.lock with resolved versions.
-- Supports recursive fetching of transitive dependencies.

local M = {}

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

local function resolve_dep(dep, home)
    local name = dep.name
    local version = dep.version or "*"

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

    if dep.url then
        return {
            name    = name,
            version = version,
            url     = dep.url,
            tag     = version ~= "*" and ("v" .. version) or nil,
        }
    end

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

    local default_url = "https://gitee.com/sharp-repo/{name}.git"
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

local function shell_escape_path(path)
    return path:gsub("'", "'\\''")
end

local function clone_dep(resolved, pkg_dir)
    if spkg.dir_exists(pkg_dir) then
        if spkg.dir_exists(pkg_dir .. "/.git") then
            print("  [cached] " .. (resolved.name or "unknown"))
            return true
        end
        spkg.remove(pkg_dir)
    end

    if not resolved or not resolved.url then
        print("spkg: cannot resolve dependency: " .. (resolved and resolved.name or "unknown"))
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
    if not r then return false end
    if not r.ok then
        print("    error:\n" .. (r.out or ""))
        return false
    end

    cmd = "cd '" .. shell_escape_path(pkg_dir) .. "' && git rev-parse HEAD"
    r = spkg.run_cmd(cmd)
    local commit = r and r.ok and r.out and r.out:gsub("%s+", "") or nil
    if commit and commit ~= "" then
        resolved.commit = commit
    else
        resolved.commit = "HEAD"
    end
    spkg_lock.save_one(resolved)
    return true
end

local fetch_from_deps_file

-- 沙箱执行 config.spkg，只收集 b:dep() 调用
-- 用 Lua 沙箱而非 regex，语义正确，能处理任意书写形式
local function discover_deps_from_config_spkg(path)
    local deps = {}
    local any_func = function() return {} end
    local sandbox_b = setmetatable({}, {
        __index = function(_, k)
            if k == "dep" then
                return function(_, name, opts)
                    table.insert(deps, { name = name, version = (opts and opts.version) or "*" })
                end
            end
            return any_func
        end
    })
    local env = { b = sandbox_b, spkg = spkg }
    local chunk, _ = loadfile(path, "t", env)
    if chunk then pcall(chunk) end
    return deps
end

local function fetch_deps_list(deps, home, visited)
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

        local dep_deps_path = pkg_dir .. "/SharpDeps.lua"
        if spkg.file_exists(dep_deps_path) then
            if not fetch_from_deps_file(dep_deps_path, home, visited) then
                return false
            end
        end

        -- 从 config.spkg 发现传递依赖（沙箱执行，安全且语义正确）
        local config_path = pkg_dir .. "/config.spkg"
        if spkg.file_exists(config_path) then
            local config_deps = discover_deps_from_config_spkg(config_path)
            if #config_deps > 0 then
                if not fetch_deps_list(config_deps, home, visited) then
                    return false
                end
            end
        end

        ::continue::
    end

    return true
end

fetch_from_deps_file = function(deps_path, home, visited)
    local deps = load_deps_file(deps_path)
    if #deps == 0 then return true end
    return fetch_deps_list(deps, home, visited)
end

function M.fetch_recursive(home, deps)
    deps = deps or {}
    if #deps == 0 then return true end
    local visited = {}
    return fetch_deps_list(deps, home, visited)
end

function M.fetch_deps(home, deps)
    deps = deps or {}
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

function M.list_deps(deps)
    deps = deps or {}
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

function M.info(home, deps)
    home = home or _SPKG_HOME or "."
    deps = deps or {}

    local lock = spkg_lock.load() or {}

    print("Project:")

    if spkg.file_exists("config.spkg") then
        local ok, content = pcall(function()
            return spkg.read_file("config.spkg")
        end)
        if ok and content then
            local name = content:match('name%s*=%s*"([^"]+)"')
            if name then print("  name:     " .. name) end
        end
    end

    print("  target:   " .. (_SPKG_TARGET or "native"))
    print("  optimize: " .. (_SPKG_OPTIMIZE or "default"))

    print("")
    print("Dependencies (" .. #deps .. "):")
    if #deps == 0 then
        print("  (none)")
    else
        for i, dep in ipairs(deps) do
            local locked = lock[dep.name]
            local version = dep.version or "*"
            local commit_info = ""
            if locked then
                if locked.commit and locked.commit ~= "" then
                    commit_info = " @" .. locked.commit:sub(1, 8)
                end
            end

            local pkg_dir = "spkg_packages/" .. dep.name
            local status = spkg.dir_exists(pkg_dir) and "installed" or "missing"

            print(string.format("  [%d] %s (%s) [%s]%s", i, dep.name, version, status, commit_info))

            local dep_deps_path = pkg_dir .. "/SharpDeps.lua"
            if spkg.file_exists(dep_deps_path) then
                local ok, sub_deps = pcall(dofile, dep_deps_path)
                if ok and type(sub_deps) == "table" and #sub_deps > 0 then
                    for _, sd in ipairs(sub_deps) do
                        print(string.format("    └─ %s (%s)", sd.name, sd.version or "*"))
                    end
                end
            end
        end
    end

    return true
end

function M.check_updates(deps)
    deps = deps or {}
    if #deps == 0 then
        print("spkg: no dependencies to update.")
        return true
    end

    local lock = spkg_lock.load() or {}
    local has_update = false

    for _, dep in ipairs(deps) do
        local name = dep.name
        local locked = lock[name]
        if locked and locked.commit then
            local resolved = resolve_dep(dep, _SPKG_HOME or ".")
            if resolved and resolved.url then
                local tmpdir = "spkg_packages/.tmp_update_" .. name
                if spkg.dir_exists(tmpdir) then spkg.remove(tmpdir) end

                print("  [check] " .. name .. " (locked: " .. locked.commit:sub(1, 8) .. ")")
                local cmd = string.format('git clone --depth 1 "%s" "%s" 2>&1', resolved.url, tmpdir)
                local r = spkg.run_cmd(cmd)
                if r and r.ok then
                    local r2 = spkg.run_cmd("cd '" .. shell_escape_path(tmpdir) .. "' && git rev-parse HEAD")
                    local remote_commit = r2 and r2.ok and r2.out and r2.out:gsub("%s+", "") or nil

                    if remote_commit and remote_commit ~= locked.commit then
                        print("    available: " .. remote_commit:sub(1, 8) .. " (newer)")
                        has_update = true
                        dep.remote_commit = remote_commit
                    else
                        print("    up to date")
                    end
                else
                    print("    [warn] failed to fetch remote: " .. (r and r.out and r.out:gsub("\n", " ") or "unknown error"))
                end

                if spkg.dir_exists(tmpdir) then spkg.remove(tmpdir) end
            end
        else
            print("  [check] " .. name .. " (not locked, will be fetched)")
            has_update = true
        end
    end

    return has_update
end

function M.update_deps(home, deps)
    deps = deps or {}
    if #deps == 0 then
        print("spkg: no dependencies to update.")
        return true
    end

    if not M.check_updates(deps) then
        print("spkg: all dependencies up to date.")
        return true
    end

    spkg.mkdir_p("spkg_packages")
    local lock = spkg_lock.load() or {}
    local updated = {}

    for _, dep in ipairs(deps) do
        local name = dep.name
        local pkg_dir = "spkg_packages/" .. name
        local old_commit = lock[name] and lock[name].commit or nil

        if dep.remote_commit and old_commit ~= dep.remote_commit then
            local tmp_dir = "spkg_packages/.tmp_update_" .. name
            if spkg.dir_exists(tmp_dir) then spkg.remove(tmp_dir) end

            local resolved = resolve_dep(dep, home or _SPKG_HOME)
            if not clone_dep(resolved, tmp_dir) then
                spkg.remove(tmp_dir)
                print("spkg: failed to update '" .. name .. "'")
                return false
            end
            if spkg.dir_exists(pkg_dir) then spkg.remove(pkg_dir) end
            spkg.run_cmd("mv '" .. shell_escape_path(tmp_dir) .. "' '" .. shell_escape_path(pkg_dir) .. "'")
            print("  [update] " .. name .. " " .. (old_commit and old_commit:sub(1, 8) or "?") .. " -> " .. dep.remote_commit:sub(1, 8))
            table.insert(updated, {
                name = resolved.name,
                url = resolved.url,
                version = resolved.version,
                tag = resolved.tag,
                commit = resolved.commit,
            })
        else
            local resolved = resolve_dep(dep, home or _SPKG_HOME)
            if not clone_dep(resolved, pkg_dir) then
                print("spkg: failed to fetch '" .. name .. "'")
                return false
            end
            table.insert(updated, resolved)
        end
    end

    spkg_lock.save(updated)
    print("spkg: dependencies updated.")
    return true
end

return M
