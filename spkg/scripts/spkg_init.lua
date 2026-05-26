-- spkg_init.lua — CLI entry, dispatches commands
-- Called from C as: spkg_main(cmd, home, target, optimize, verbose, all_targets, args_table)

spkg_main = function(cmd, home, target, optimize, verbose, all_targets, args)

    -- ── Build context flags (passed to Sharp.lua via globals) ──
    _SPKG_TARGET   = target
    _SPKG_OPTIMIZE = optimize
    _SPKG_VERBOSE  = verbose
    _SPKG_ALL      = all_targets
    _SPKG_HOME     = home

    if cmd == "help" or cmd == "-h" or cmd == "--help" then
        print([[
spkg — Sharp Package Manager

  spkg init                       create Sharp.lua
  spkg build                      build current target
  spkg build --target <triple>    cross-compile
  spkg build --optimize <level>   Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
  spkg build --verbose            detailed output
  spkg build --all                build all targets
  spkg run                        build + run executable
  spkg add <name>                 add a dependency to SharpDeps.lua
  spkg remove <name>              remove a dependency from SharpDeps.lua
  spkg list                       list dependencies
  spkg clean                      remove build/, spkg_packages/, Sharp.lock
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
    else
        print("spkg: unknown command '" .. cmd .. "'. Try 'spkg help'.")
        return false
    end
end

-- ═══════════════════════════════════════════════════════════════
-- Lua table serializer (for Sharp.lua init template)
-- ═══════════════════════════════════════════════════════════════

local function spkg_dump_lua(val, indent)
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
        spkg.run_cmd("rm -rf '" .. pkg_dir .. "'")
        print("spkg: removed package '" .. name .. "'")
    end

    print("spkg: removed dependency '" .. name .. "'")
    return true
end

function spkg_cmd_list()
    return spkg_fetch.list_deps()
end

function spkg_cmd_clean()
    if spkg.dir_exists("build") then
        spkg.run_cmd("rm -rf build")
    end
    if spkg.dir_exists("spkg_packages") then
        spkg.run_cmd("rm -rf spkg_packages")
    end
    if spkg.file_exists("Sharp.lock") then
        spkg.run_cmd("rm -f Sharp.lock")
    end
    print("spkg: cleaned.")
    return true
end

return true
