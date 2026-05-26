-- spkg_build.lua — Sharp Build System
--
-- Implements:
--   - Build Context (b object injected into Sharp.lua)
--   - Artifact objects (executable, staticlib, sharedlib)
--   - Build Graph generation
--   - Incremental compilation (mtime-based)
--   - Local execution (sharpc / zig cc)
--
-- Distributed compilation (Phase 2): build_graph is pure data, serializable.

local M = {}

-- ── Build Graph (populated after running Sharp.lua) ──
local build_graph = {
    artifacts = {},    -- list of install artifacts
    all_tasks = {},    -- flat list of all compile tasks
}

-- ═══════════════════════════════════════════════════════════════
-- Build Context (the "b" object)
-- ═══════════════════════════════════════════════════════════════

local function create_build_context()
    local ctx = {}
    local artifacts = {}   -- all declared artifacts
    local install_list = {} -- artifacts marked for install

    -- ── Query functions ──
    function ctx:get_target()
        if _SPKG_TARGET and _SPKG_TARGET ~= "" then
            return _SPKG_TARGET
        end
        return spkg.current_platform()
    end

    function ctx:get_optimize()
        return _SPKG_OPTIMIZE or "Debug"
    end

    function ctx:get_verbose()
        return _SPKG_VERBOSE == true
    end

    -- ── Optimize → sharpc flags ──
    local optimize_flags = {
        Debug       = "-O0",
        ReleaseSafe = "-O1",
        ReleaseFast = "-O2",
        ReleaseSmall = "-Os",
    }

    -- ── Artifact factory ──
    local function create_artifact(name, atype)
        local art = {
            name       = name,
            type       = atype,  -- exe | staticlib | sharedlib
            sources    = {},     -- { {file=..., cflags=..., include=...}, ... }
            includes   = {},     -- global includes for this artifact
            cflags     = {},     -- extra global cflags for this artifact
            ldflags    = {},     -- extra ldflags for this artifact
            link_libs  = {},     -- -l flags
            link_deps  = {},     -- linked artifact references (names)
        }

        -- ── Source methods ──
        function art:add_source(spec)
            if type(spec) == "string" then
                table.insert(self.sources, { file = spec })
            elseif type(spec) == "table" then
                table.insert(self.sources, spec)
            else
                error("add_source: expected string or table")
            end
            return self
        end

        function art:add_include(dir)
            table.insert(self.includes, dir)
            return self
        end

        function art:add_cflags(...)
            local args = {...}
            for _, f in ipairs(args) do
                table.insert(self.cflags, f)
            end
            return self
        end

        function art:add_ldflags(...)
            local args = {...}
            for _, f in ipairs(args) do
                table.insert(self.ldflags, f)
            end
            return self
        end

        function art:link_library(lib)
            table.insert(self.link_libs, lib)
            return self
        end

        function art:link_artifact(other_art)
            table.insert(self.link_deps, other_art.name)
            return self
        end

        return art
    end

    -- ── Public API ──
    function ctx:add_executable(opts)
        local name = opts and opts.name or "main"
        local art = create_artifact(name, "exe")
        table.insert(artifacts, art)
        return art
    end

    function ctx:add_static_library(opts)
        local name = opts and opts.name or "lib"
        local art = create_artifact(name, "staticlib")
        table.insert(artifacts, art)
        return art
    end

    function ctx:add_shared_library(opts)
        local name = opts and opts.name or "lib"
        local art = create_artifact(name, "sharedlib")
        table.insert(artifacts, art)
        return art
    end

    function ctx:install(art)
        table.insert(install_list, art)
        return art
    end

    function ctx:dependency(name)
        for _, a in ipairs(artifacts) do
            if a.name == name then return a end
        end
        return nil
    end

    -- ── Internal access (for spkg_build) ──
    ctx._artifacts = artifacts
    ctx._install_list = install_list
    ctx._optimize_flags = optimize_flags

    return ctx
end

-- ═══════════════════════════════════════════════════════════════
-- Build Graph Builder
-- ═══════════════════════════════════════════════════════════════

local function resolve_files(file_pattern)
    if file_pattern:find("%*%*") or file_pattern:find("%*") then
        return spkg.glob(file_pattern)
    else
        if spkg.file_exists(file_pattern) then
            return { file_pattern }
        end
        return {}
    end
end

function M.build_graph_from_ctx(ctx)
    local target = ctx:get_target()
    local verbose = ctx:get_verbose()

    build_graph = {
        target = target,
        optimize = ctx:get_optimize(),
        artifacts = {},
    }

    local opt_flag = ctx._optimize_flags[ctx:get_optimize()] or "-O0"

    for _, art in ipairs(ctx._install_list) do
        local artifact_graph = {
            name = art.name,
            type = art.type,
            target = target,
            compile_tasks = {},
            link_step = {},
        }

        -- Combine global includes + per-source includes
        for _, src_spec in ipairs(art.sources) do
            local files = resolve_files(src_spec.file)
            if #files == 0 then
                if verbose then
                    print("  [warn] no files matched: " .. src_spec.file)
                end
            end

            -- Build cflags for this source entry
            local src_cflags = { opt_flag }
            -- global includes
            for _, inc in ipairs(art.includes) do
                table.insert(src_cflags, "-I" .. inc)
            end
            -- per-source includes
            if src_spec.include then
                for _, inc in ipairs(src_spec.include) do
                    table.insert(src_cflags, "-I" .. inc)
                end
            end
            -- per-source cflags
            if src_spec.cflags then
                for _, f in ipairs(src_spec.cflags) do
                    table.insert(src_cflags, f)
                end
            end
            -- global artifact cflags
            for _, f in ipairs(art.cflags) do
                table.insert(src_cflags, f)
            end

            for _, fp in ipairs(files) do
                -- Derive output path: build/<artifact_name>/<stem>.o
                local stem = fp:gsub("%.sp$", ""):gsub("[/\\]", "_")
                local output = "build/" .. art.name .. "/" .. stem .. ".o"

                table.insert(artifact_graph.compile_tasks, {
                    source = fp,
                    output = output,
                    cflags = src_cflags,
                })
            end
        end

        -- Link step
        local all_inputs = {}
        for _, task in ipairs(artifact_graph.compile_tasks) do
            table.insert(all_inputs, task.output)
        end

        -- Add linked artifact outputs
        for _, dep_name in ipairs(art.link_deps) do
            local dep_art = ctx:dependency(dep_name)
            if dep_art then
                local dep_out
                if dep_art.type == "staticlib" then
                    dep_out = "build/" .. dep_name .. "/lib" .. dep_name .. ".a"
                elseif dep_art.type == "sharedlib" then
                    dep_out = "build/" .. dep_name .. "/lib" .. dep_name .. ".so"
                else
                    dep_out = "build/" .. dep_name .. "/" .. dep_name
                end
                table.insert(all_inputs, dep_out)
            end
        end

        -- ldflags
        local lflags = {}
        for _, f in ipairs(art.ldflags) do
            table.insert(lflags, f)
        end
        for _, lib in ipairs(art.link_libs) do
            table.insert(lflags, "-l" .. lib)
        end

        -- Output path
        local output_path
        if art.type == "staticlib" then
            output_path = "build/" .. art.name .. "/lib" .. art.name .. ".a"
        elseif art.type == "sharedlib" then
            output_path = "build/" .. art.name .. "/lib" .. art.name .. ".so"
        else
            output_path = "build/" .. art.name .. "/" .. art.name
        end

        artifact_graph.link_step = {
            inputs = all_inputs,
            output = output_path,
            ldflags = lflags,
        }

        table.insert(build_graph.artifacts, artifact_graph)
    end

    return build_graph
end

-- ═══════════════════════════════════════════════════════════════
-- Incremental Compilation Check
-- ═══════════════════════════════════════════════════════════════

local function needs_compile(source, output)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then return false end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end  -- output doesn't exist

    return src_mtime > out_mtime
end

-- ═══════════════════════════════════════════════════════════════
-- Compiler / Linker Execution
-- ═══════════════════════════════════════════════════════════════

local function find_compiler()
    local sharpc = spkg.find_sharpc()
    if sharpc then return sharpc end

    -- Fallback: try zig cc for C code if sharpc not found
    local zig = spkg.find_zigcc()
    if zig then return zig end

    return nil
end

local function compile_task(task, verbose)
    local compiler = find_compiler()
    if not compiler then
        print("spkg: no compiler found. Install sharpc or set SHARPC env var.")
        return false
    end

    if not verbose then
        print("  [sp] " .. task.source)
    end

    -- Build command: sharpc <cflags> <source> -o <output>
    local cflags_str = table.concat(task.cflags, " ")
    local cmd = string.format('%s %s "%s" -o "%s"',
        compiler, cflags_str, task.source, task.output)

    if verbose then
        print("  " .. cmd)
    end

    local r = spkg.run_cmd(cmd)
    if not r.ok then
        print("    error:\n" .. r.out)
        return false
    end

    if verbose and r.out ~= "" then
        print("    " .. r.out)
    end

    return true
end

local function link_artifact(artifact, verbose)
    local link = artifact.link_step
    if #link.inputs == 0 then
        print("  [warn] " .. artifact.name .. ": no object files to link")
        return true
    end

    local name = artifact.name
    local atype = artifact.type

    if atype == "staticlib" then
        -- Use ar to create static library
        if verbose then
            print("  [ar] " .. link.output)
        else
            print("  [ar] lib" .. name .. ".a")
        end

        spkg.mkdir_p("build/" .. name)
        local inputs = table.concat(link.inputs, " ")
        local cmd = string.format('zig ar rcs "%s" %s', link.output, inputs)
        if verbose then print("    " .. cmd) end

        local r = spkg.run_cmd(cmd)
        if not r.ok then
            print("    ar error:\n" .. r.out)
            return false
        end
        return true
    end

    -- exe or sharedlib: link via compiler
    local compiler = find_compiler()
    if not compiler then
        print("spkg: no linker found.")
        return false
    end

    if verbose then
        print("  [link] " .. link.output)
    else
        print("  [link] " .. name)
    end

    spkg.mkdir_p("build/" .. name)
    local inputs = table.concat(link.inputs, " ")
    local ldflags = table.concat(link.ldflags, " ")
    local cmd = string.format('%s %s %s -o "%s"',
        compiler, inputs, ldflags, link.output)

    if verbose then print("    " .. cmd) end

    local r = spkg.run_cmd(cmd)
    if not r.ok then
        print("    link error:\n" .. r.out)
        return false
    end

    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Fetching (before build)
-- ═══════════════════════════════════════════════════════════════

local function fetch_deps()
    local home = _SPKG_HOME or "/root"
    if not spkg_fetch.fetch_recursive(home) then
        return false
    end
    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Execute
-- ═══════════════════════════════════════════════════════════════

function M.execute()
    local verbose = (_SPKG_VERBOSE == true)

    -- 1. Create build context and inject as global 'b' for Sharp.lua
    b = create_build_context()

    -- Load and run Sharp.lua
    local ok, err = pcall(dofile, "Sharp.lua")
    if not ok then
        print("spkg: failed to load Sharp.lua: " .. tostring(err))
        return false
    end

    -- 2. Build the graph
    M.build_graph_from_ctx(b)

    -- 3. Execute each artifact
    local all_ok = true
    for _, art in ipairs(build_graph.artifacts) do
        if verbose then
            print("spkg: building " .. art.name .. " [" .. art.type .. "] " ..
                  "target=" .. art.target)
        else
            print("spkg: building " .. art.name .. " [" .. art.type .. "]")
        end

        -- Compile tasks (incremental)
        local compile_ok = true
        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output) then
                spkg.mkdir_p("build/" .. art.name)
                if not compile_task(task, verbose) then
                    compile_ok = false
                    break
                end
            else
                if verbose then
                    print("  [skip] " .. task.source .. " (up to date)")
                end
            end
        end
        if not compile_ok then
            all_ok = false
            break
        end

        -- Link step
        if not link_artifact(art, verbose) then
            all_ok = false
            break
        end
    end

    if all_ok then
        print("spkg: done.")
    end
    return all_ok
end

-- ═══════════════════════════════════════════════════════════════
-- Run first artifact
-- ═══════════════════════════════════════════════════════════════

function M.run_first_artifact()
    if #build_graph.artifacts == 0 then
        print("spkg: no artifacts to run.")
        return false
    end

    local art = build_graph.artifacts[1]
    local exe = art.link_step.output

    if not spkg.file_exists(exe) then
        print("spkg: executable not found: " .. exe)
        return false
    end

    local r = spkg.run_cmd("./" .. exe)
    if r.out ~= "" then print(r.out) end
    return r.ok
end

-- Export build context creator for use in spkg_init
M.create_build_context = create_build_context

return M
