-- spkg_build.lua — Sharp Build System
--
-- Implements:
--   - Build Context (b object injected into Sharp.lua)
--   - Artifact objects (executable, staticlib, sharedlib)
--   - Build Graph generation
--   - Incremental compilation (mtime-based)
--   - Local execution with parallel compilation (--jobs)
--
-- Distributed compilation (Phase 2): build_graph is pure data, serializable.

local M = {}

-- ── Build Graph (populated after running Sharp.lua) ──
local build_graph = {
    artifacts = {},
}

-- ═══════════════════════════════════════════════════════════════
-- Build Context (the "b" object)
-- ═══════════════════════════════════════════════════════════════

local function create_build_context()
    local ctx = {}
    local artifacts = {}
    local install_list = {}

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

    local optimize_flags = {
        Debug       = "-O0",
        ReleaseSafe = "-O1",
        ReleaseFast = "-O2",
        ReleaseSmall = "-Os",
    }

    local function create_artifact(name, atype)
        local art = {
            name      = name,
            type      = atype,
            sources   = {},
            includes  = {},
            cflags    = {},
            ldflags   = {},
            link_libs = {},
            link_deps = {},
        }

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

    build_graph = {
        target = target,
        optimize = ctx:get_optimize(),
        artifacts = {},
    }

    local opt_flag = ctx._optimize_flags[ctx:get_optimize()] or "-O0"

    for _, art in ipairs(ctx._install_list) do
        local artifact_graph = {
            name          = art.name,
            type          = art.type,
            target        = target,
            compile_tasks = {},
            link_step     = {},
        }

        for _, src_spec in ipairs(art.sources) do
            local files = resolve_files(src_spec.file)

            local src_cflags = { opt_flag }
            for _, inc in ipairs(art.includes) do
                table.insert(src_cflags, "-I" .. inc)
            end
            if src_spec.include then
                for _, inc in ipairs(src_spec.include) do
                    table.insert(src_cflags, "-I" .. inc)
                end
            end
            if src_spec.cflags then
                for _, f in ipairs(src_spec.cflags) do
                    table.insert(src_cflags, f)
                end
            end
            for _, f in ipairs(art.cflags) do
                table.insert(src_cflags, f)
            end

            for _, fp in ipairs(files) do
                local stem = fp:gsub("%.sp$", ""):gsub("[/\\]", "_")
                local output = "build/" .. art.name .. "/" .. stem .. ".o"
                table.insert(artifact_graph.compile_tasks, {
                    source = fp,
                    output = output,
                    cflags = src_cflags,
                })
            end
        end

        -- Link inputs
        local all_inputs = {}
        for _, task in ipairs(artifact_graph.compile_tasks) do
            table.insert(all_inputs, task.output)
        end
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

        local lflags = {}
        for _, f in ipairs(art.ldflags) do table.insert(lflags, f) end
        for _, lib in ipairs(art.link_libs) do table.insert(lflags, "-l" .. lib) end

        local output_path
        if art.type == "staticlib" then
            output_path = "build/" .. art.name .. "/lib" .. art.name .. ".a"
        elseif art.type == "sharedlib" then
            output_path = "build/" .. art.name .. "/lib" .. art.name .. ".so"
        else
            output_path = "build/" .. art.name .. "/" .. art.name
        end

        artifact_graph.link_step = {
            inputs  = all_inputs,
            output  = output_path,
            ldflags = lflags,
        }

        table.insert(build_graph.artifacts, artifact_graph)
    end

    return build_graph
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency File (.d) Parser
-- ═══════════════════════════════════════════════════════════════

-- Parse a Makefile-style .d file and return list of header file paths
local function parse_depfile(path)
    if not spkg.file_exists(path) then
        return {}
    end
    local content = spkg.read_file(path)
    if not content then return {} end

    local headers = {}
    local deps_part = content:match(":[%s]*\n(.*)")
    if not deps_part then
        deps_part = content:match(":%s*(.*)")
    end
    if not deps_part then return {} end

    deps_part = deps_part:gsub("\\%s*\n", " ")
    for h in deps_part:gmatch("%S+") do
        if spkg.file_exists(h) then
            table.insert(headers, h)
        end
    end
    return headers
end

-- ═══════════════════════════════════════════════════════════════
-- Incremental Compilation Check
-- ═══════════════════════════════════════════════════════════════

local function needs_compile(source, output)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then
        if spkg.file_exists(output) then
            spkg.run_cmd("rm -f '" .. output .. "'")
        end
        local depfile = output:gsub("%.o$", ".d")
        if spkg.file_exists(depfile) then
            spkg.run_cmd("rm -f '" .. depfile .. "'")
        end
        return false
    end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end

    local depfile = output:gsub("%.o$", ".d")
    local headers = parse_depfile(depfile)
    for _, h in ipairs(headers) do
        local h_mtime = spkg.get_mtime(h)
        if h_mtime and h_mtime > out_mtime then
            return true
        end
    end

    return src_mtime > out_mtime
end

-- ═══════════════════════════════════════════════════════════════
-- Compiler / Linker Execution
-- ═══════════════════════════════════════════════════════════════

local function find_compiler()
    local sharpc = spkg.find_sharpc()
    if sharpc then return sharpc end

    local zig = spkg.find_zigcc()
    if zig then return zig end

    return nil
end

local function compile_task_cmd(task, verbose)
    local compiler = find_compiler()
    if not compiler then
        return nil
    end

    local depfile = task.output:gsub("%.o$", ".d")
    local cflags_str = table.concat(task.cflags, " ")
    return string.format('%s %s -MMD -MF "%s" "%s" -o "%s"',
        compiler, cflags_str, depfile, task.source, task.output)
end

local function compile_task(task, verbose)
    local cmd = compile_task_cmd(task, verbose)
    if not cmd then
        print("spkg: no compiler found. Install sharpc or set SHARPC env var.")
        return false
    end

    if not verbose then
        print("  [sp] " .. task.source)
    end

    if verbose then print("  " .. cmd) end

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
        local zig = spkg.find_zigcc()
        if not zig then
            print("spkg: zig not found (required for static library creation).")
            return false
        end

        if verbose then
            print("  [ar] " .. link.output)
        else
            print("  [ar] lib" .. name .. ".a")
        end

        spkg.mkdir_p("build/" .. name)
        local inputs_list = {}
        for _, inp in ipairs(link.inputs) do
            table.insert(inputs_list, '"' .. inp .. '"')
        end
        local inputs = table.concat(inputs_list, " ")
        local cmd = string.format('"%s" ar rcs "%s" %s', zig, link.output, inputs)
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
    local inputs_list = {}
    for _, inp in ipairs(link.inputs) do
        table.insert(inputs_list, '"' .. inp .. '"')
    end
    local inputs = table.concat(inputs_list, " ")
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
-- Parallel Compilation Engine
-- ═══════════════════════════════════════════════════════════════

local function compile_tasks_parallel(tasks, verbose, max_jobs)
    if max_jobs <= 1 then
        -- Sequential mode
        for _, task in ipairs(tasks) do
            if not compile_task(task, verbose) then
                return false
            end
        end
        return true
    end

    local pending = {}
    local running = {}
    local completed = 0
    local total = #tasks
    local any_failed = false

    -- Initialize pending list
    for i, task in ipairs(tasks) do
        table.insert(pending, { idx = i, task = task })
    end

    -- Start initial batch
    local function start_next()
        while #pending > 0 and #running < max_jobs do
            local item = table.remove(pending, 1)
            local cmd = compile_task_cmd(item.task, verbose)
            if not cmd then
                any_failed = true
                completed = total
                return false
            end
            if not verbose then
                print("  [sp] " .. item.task.source)
            end
            if verbose then print("  " .. cmd) end
            local task_id, err = spkg.start_cmd(cmd)
            if not task_id then
                any_failed = true
                print("    error: " .. err)
                completed = total
                return false
            end
            table.insert(running, { id = task_id, item = item })
        end
        return true
    end

    start_next()

    -- Wait and manage
    while #running > 0 do
        -- Poll each running task
        local new_running = {}
        for _, r in ipairs(running) do
            local result = spkg.wait_task(r.id)
            if result then
                completed = completed + 1
                if not result.ok then
                    any_failed = true
                    print("    error:\n" .. result.out)
                elseif verbose and result.out ~= "" then
                    print("    " .. result.out)
                end
            else
                -- Still running, keep it
                table.insert(new_running, r)
            end
        end
        running = new_running
        -- Start more tasks if slots are available
        if #running < max_jobs and #pending > 0 and not any_failed then
            start_next()
            if any_failed then break end
        end
    end

    return not any_failed
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Fetching
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
    local all_targets = (_SPKG_ALL == true)
    local max_jobs = (_SPKG_JOBS and _SPKG_JOBS > 0) and _SPKG_JOBS or 1

    if all_targets then
        return M.execute_all_targets(verbose, max_jobs)
    end

    return M.execute_single(verbose, max_jobs)
end

function M.execute_single(verbose, max_jobs)
    return M._do_build(verbose, max_jobs)
end

function M.execute_all_targets(verbose, max_jobs)
    -- Phase 1: Sharp.lua doesn't statically declare target list.
    -- --all builds the current platform target.
    print("spkg: --all not yet supported (Sharp.lua doesn't declare static targets).")
    return M._do_build(verbose, max_jobs)
end

function M._discover_targets()
    -- Phase 1: returns current platform only
    return { spkg.current_platform() }
end

function M._do_build(verbose, max_jobs)
    -- 0. Fetch dependencies
    if not fetch_deps() then
        return false
    end

    -- 1. Create build context and inject as global 'b' for Sharp.lua
    b = create_build_context()

    -- Load and run Sharp.lua
    local ok, err = pcall(dofile, "Sharp.lua")
    if not ok then
        print("spkg: error: failed to execute Sharp.lua")
        local msg = tostring(err)
        local line_num = msg:match(":(%d+):")
        if line_num then
            print("  at line " .. line_num .. ": " .. msg:match(":(%d+: .*)"))
        else
            print("  " .. msg)
        end
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

        -- Compile tasks (incremental, parallel if max_jobs > 1)
        local compile_tasks = {}
        local pending_tasks = {}
        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output) then
                spkg.mkdir_p("build/" .. art.name)
                table.insert(pending_tasks, task)
            else
                if verbose then
                    print("  [skip] " .. task.source .. " (up to date)")
                end
                -- Still add to compile_tasks for link step
                table.insert(compile_tasks, task)
            end
        end

        if #pending_tasks > 0 then
            local compile_ok = compile_tasks_parallel(pending_tasks, verbose, max_jobs)
            if not compile_ok then
                all_ok = false
                break
            end
            -- Add all tasks (including skipped ones) to compile_tasks
            for _, task in ipairs(art.compile_tasks) do
                table.insert(compile_tasks, task)
            end
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
-- Run first executable artifact
-- ═══════════════════════════════════════════════════════════════

function M.run_first_artifact()
    for _, art in ipairs(build_graph.artifacts) do
        if art.type == "exe" then
            local exe = art.link_step.output
            if not spkg.file_exists(exe) then
                print("spkg: executable not found: " .. exe)
                return false
            end
            local r = spkg.run_cmd("./" .. exe)
            if r.out ~= "" then print(r.out) end
            return r.ok
        end
    end

    print("spkg: no executable artifact to run.")
    return false
end

M.create_build_context = create_build_context

return M
