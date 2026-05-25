-- spkg_fetch.lua — dependency fetching via git clone

local M = {}

function M.fetch_deps(manifest, home)
    manifest.deps = manifest.deps or {}
    local has_deps = false
    for _, _ in pairs(manifest.deps) do has_deps = true; break end
    if not has_deps then return true end

    for name, version in pairs(manifest.deps) do
        local pkg_dir = "spkg_packages/" .. name

        -- Already fetched; skip
        if spkg.dir_exists(pkg_dir) then
            print("  [cached] " .. name)
        else
            local resolved = spkg_resolve.resolve(name, version, home)
            if not resolved or not resolved.url then
                print("spkg: cannot resolve dependency: " .. name)
                return false
            end

            print("  [fetch] " .. resolved.url)
            spkg.mkdir_p("spkg_packages")

            -- Try clone with system git
            local cmd = string.format(
                'git clone --depth 1 "%s" "%s"', resolved.url, pkg_dir)
            local r = spkg.run_cmd(cmd)
            if not r.ok then
                print("    error:\n" .. r.out)
                return false
            end

            -- Write resolved info to Sharp.lock
            resolved.commit = "HEAD"
            spkg_lock.save({resolved})
        end
    end

    return true
end

-- Fetch recursively: resolve transitive deps of each dependency
function M.fetch_recursive(manifest, home, visited)
    visited = visited or {}
    local ok = M.fetch_deps(manifest, home)
    if not ok then return false end

    -- For each dependency, load its Sharp.lua and fetch its deps
    manifest.deps = manifest.deps or {}
    for name, _ in pairs(manifest.deps) do
        if not visited[name] then
            visited[name] = true
            local dep_manifest_path = "spkg_packages/" .. name .. "/Sharp.lua"
            if spkg.file_exists(dep_manifest_path) then
                local ok2, dep_man = pcall(dofile, dep_manifest_path)
                if ok2 and dep_man then
                    ok = M.fetch_recursive(dep_man, home, visited)
                    if not ok then return false end
                end
            end
        end
    end

    return true
end

return M