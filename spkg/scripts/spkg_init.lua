-- spkg_init.lua — CLI entry, dispatches commands
-- Called from C as: spkg_main(cmd, home, target, optimize, verbose, all_targets, jobs, no_cache, args)

spkg_main = function(cmd, home, target, optimize, verbose, all_targets, jobs, no_cache, args)

    -- ── Build context flags (passed to Sharp.lua via globals) ──
    _SPKG_TARGET   = target
    _SPKG_OPTIMIZE = optimize
    _SPKG_VERBOSE  = verbose
    _SPKG_ALL      = all_targets
    _SPKG_JOBS     = jobs
    _SPKG_HOME     = home
    _SPKG_NO_CACHE = no_cache or false

    if cmd == "help" or cmd == "-h" or cmd == "--help" then
        print([[
spkg — Sharp Package Manager

  spkg init                       create Sharp.lua
  spkg build                      build current target
  spkg build --target <triple>    cross-compile
  spkg build --optimize <level>   Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
  spkg build --verbose            detailed output
  spkg build --all                build all targets
  spkg build --jobs <N>           parallel compile jobs
  spkg build --no-cache           disable build cache
  spkg run                        build + run executable
  spkg test                       build + run tests
  spkg add <name>                 add a dependency to SharpDeps.lua
  spkg remove <name>              remove a dependency from SharpDeps.lua
  spkg list                       list dependencies
  spkg clean                      remove build/, spkg_packages/, Sharp.lock
  spkg cache --stats              show cache statistics
  spkg cache --clear              clear entire cache
  spkg help                       show this message
]])
        return true
    end

    if cmd == "init" then
        return spkg_cmd_init(home)
    end

    if not spkg.file_exists("Sharp.lua") then
        print("spkg: Sharp.lua not found. Run 'spkg init' first.")
        return false
    end

    if cmd == "build" then
        return spkg_cmd_build()
    elseif cmd == "run" then
        return spkg_cmd_run()
    elseif cmd == "add" then
        return spkg_cmd_add(args)
    elseif cmd == "remove" then
        return spkg_cmd_remove(args)
    elseif cmd == "list" then
        return spkg_cmd_list()
    elseif cmd == "clean" then
        return spkg_cmd_clean()
    elseif cmd == "test" then
        return spkg_cmd_test()
    elseif cmd == "cache" then
        return spkg_cmd_cache(args)
    else
        print("spkg: unknown command '" .. cmd .. "'. Try 'spkg help'.")
        return false
    end
end

-- ═══════════════════════════════════════════════════════════════
-- Lua table serializer (for Sharp.lua init template)
-- ═══════════════════════════════════════════════════════════════

function spkg_dump_lua(val, indent)
    indent = indent or 0
    local t = type(val)
    if t == "string" then return string.format("%q", val) end
    if t == "number" or t == "boolean" then return tostring(val) end
    if t == "nil" then return "nil" end
    if t == "table" then
        local pad = string.rep("  ", indent)
        local max_k = 0
        local is_arr = true
        for k, _ in pairs(val) do
            if type(k) ~= "number" then is_arr = false; break end
            if k > max_k then max_k = k end
        end
        if is_arr and max_k > 0 then
            local parts = {}
            for i = 1, max_k do
                table.insert(parts, pad .. "  " .. spkg_dump_lua(val[i], indent + 1))
            end
            return "{\n" .. table.concat(parts, ",\n") .. "\n" .. pad .. "}"
        end
        local keys = {}
        for k, _ in pairs(val) do table.insert(keys, k) end
        table.sort(keys)
        local parts = {}
        for _, k in ipairs(keys) do
            table.insert(parts, pad .. "  " .. k .. " = " .. spkg_dump_lua(val[k], indent + 1))
        end
        return "{\n" .. table.concat(parts, ",\n") .. "\n" .. pad .. "}"
    end
    return "nil"
end

-- ═══════════════════════════════════════════════════════════════
-- Commands
-- ═══════════════════════════════════════════════════════════════

function spkg_cmd_init(home)
    if spkg.file_exists("Sharp.lua") then
        print("spkg: Sharp.lua already exists.")
        return false
    end
    local name = spkg.dir_exists("src") and "my-project" or "untitled"
    local src = [[
-- Sharp.lua — Sharp Build System
-- API mirrors zig build: declare artifacts, add sources, link, install.

local target   = b:get_target()
local optimize = b:get_optimize()

-- ── Executable ──
local exe = b:add_executable({ name = "%s" })
exe:add_source({ file = "src/**/*.sp" })
exe:add_include("src")
b:install(exe)
]]
    spkg.write_file("Sharp.lua", string.format(src, name))
    print("spkg: created Sharp.lua")

    if not spkg.file_exists("SharpDeps.lua") then
        local deps = [[-- SharpDeps.lua — Dependency declarations
-- Format: array of { name = "...", version = "..." }
-- Example:
--   { name = "sharp-lib", version = "1.0.0" },
return {}
]]
        spkg.write_file("SharpDeps.lua", deps)
        print("spkg: created SharpDeps.lua")
    end

    return true
end

function spkg_cmd_build()
    return spkg_build.execute()
end

function spkg_cmd_run()
    local ok = spkg_build.execute()
    if not ok then return false end
    return spkg_build.run_first_artifact()
end

function spkg_cmd_add(args)
    if #args < 1 then
        print("usage: spkg add <name> [version]")
        return false
    end

    local name = args[1]
    local version = args[2] or "*"

    -- Load existing SharpDeps.lua
    local deps = {}
    if spkg.file_exists("SharpDeps.lua") then
        local ok, d = pcall(dofile, "SharpDeps.lua")
        if ok and type(d) == "table" then deps = d end
    end

    -- Check if already exists
    for _, dep in ipairs(deps) do
        if dep.name == name then
            print("spkg: dependency '" .. name .. "' already exists.")
            return false
        end
    end

    table.insert(deps, { name = name, version = version })

    -- Write back
    local out = "-- SharpDeps.lua — managed by spkg\nreturn " .. spkg_dump_lua(deps)
    spkg.write_file("SharpDeps.lua", out)
    print("spkg: added dependency '" .. name .. "' (" .. version .. ")")
    return true
end

function spkg_cmd_remove(args)
    if #args < 1 then
        print("usage: spkg remove <name>")
        return false
    end

    local name = args[1]
    if not spkg.file_exists("SharpDeps.lua") then
        print("spkg: SharpDeps.lua not found.")
        return false
    end

    local ok, deps = pcall(dofile, "SharpDeps.lua")
    if not ok or type(deps) ~= "table" then
        print("spkg: failed to parse SharpDeps.lua")
        return false
    end

    local found = false
    local new_deps = {}
    for _, dep in ipairs(deps) do
        if dep.name == name then
            found = true
        else
            table.insert(new_deps, dep)
        end
    end

    if not found then
        print("spkg: dependency '" .. name .. "' not found.")
        return false
    end

    local out = "-- SharpDeps.lua — managed by spkg\nreturn " .. spkg_dump_lua(new_deps)
    spkg.write_file("SharpDeps.lua", out)

    -- Also remove from spkg_packages if present
    local pkg_dir = "spkg_packages/" .. name
    if spkg.dir_exists(pkg_dir) then
        spkg.remove(pkg_dir)
        print("spkg: removed package '" .. name .. "'")
    end

    print("spkg: removed dependency '" .. name .. "'")
    return true
end

function spkg_cmd_list()
    return spkg_fetch.list_deps()
end

function spkg_cmd_clean()
    if spkg.dir_exists("build") then spkg.remove("build") end
    if spkg.dir_exists("spkg_packages") then spkg.remove("spkg_packages") end
    if spkg.file_exists("Sharp.lock") then spkg.remove("Sharp.lock") end
    print("spkg: cleaned.")
    return true
end

function spkg_cmd_test()
    -- Phase 4: test framework
    print("spkg: test command not yet implemented (Phase 4).")
    return true
end

function spkg_cmd_cache(args)
    spkg.cache_init()
    if #args == 0 then
        print([[spkg cache — Build cache management

  spkg cache --stats    show cache statistics
  spkg cache --clear    clear entire cache
]])
        return true
    end

    if args[1] == "--stats" then
        local stats = spkg.cache_stats()
        print(string.format("Cache statistics:"))
        print(string.format("  entries:  %d", stats.count or 0))
        print(string.format("  hits:     %d", stats.hit or 0))
        print(string.format("  misses:   %d", stats.miss or 0))
        local size = stats.size or 0
        if size > 1048576 then
            print(string.format("  size:     %.1f MB", size / 1048576.0))
        elseif size > 1024 then
            print(string.format("  size:     %.1f KB", size / 1024.0))
        else
            print(string.format("  size:     %d bytes", size))
        end
        return true
    elseif args[1] == "--clear" then
        if spkg.cache_clear() then
            print("spkg: cache cleared.")
        else
            print("spkg: failed to clear cache.")
        end
        return true
    else
        print("spkg: unknown cache option '" .. args[1] .. "'.")
        return false
    end
end

return true
