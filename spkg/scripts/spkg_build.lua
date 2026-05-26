-- spkg_build.lua — Sharp Build System
--
-- Implements:
--   - Build Context (b object injected into Sharp.lua)
--   - Artifact objects (exe, staticlib, sharedlib)
--   - DAG-based build graph with topological sort
--   - Platform-aware artifact naming (.exe/.lib/.dll)
--   - Incremental compilation (mtime + depfile + fingerprint)
--   - Local execution with parallel compilation (--jobs)
--
-- Distributed compilation (Phase 3): build_graph is pure data, serializable.

local M = {}

-- ═══════════════════════════════════════════════════════════════
-- Color Helpers
-- ═══════════════════════════════════════════════════════════════

local COLOR = spkg.is_tty() and spkg.colorize or function(t, _) return t end

local function status_color(name, text)
    return string.format("  [%s] %s",
                         COLOR(name, "bold_blue"),
                         text)
end

local function error_msg(msg)
    return COLOR("error: " .. msg, "bold_red")
end

local function warn_msg(msg)
    return COLOR("warning: " .. msg, "bold_yellow")
end

-- ── Build Graph (populated after running Sharp.lua) ──
local build_graph = { artifacts = {} }

-- ═══════════════════════════════════════════════════════════════
-- Platform Helpers
-- ═══════════════════════════════════════════════════════════════

local function is_windows()
    local plat = spkg.current_platform()
    return plat:match("windows") or plat:match("mingw")
end

local function exe_suffix()
    return is_windows() and ".exe" or ""
end

local function staticlib_name(name)
    return is_windows() and (name .. ".lib") or ("lib" .. name .. ".a")
end

local function sharedlib_name(name)
    return is_windows() and (name .. ".dll") or ("lib" .. name .. ".so")
end

local function artifact_output(art)
    if art.type == "staticlib"  then return staticlib_name(art.name)
    elseif art.type == "sharedlib" then return sharedlib_name(art.name)
    else return art.name .. exe_suffix()
    end
end

-- ═══════════════════════════════════════════════════════════════
-- Build Context (the "b" object)
-- ═══════════════════════════════════════════════════════════════

local function create_build_context()
    local ctx = {}
    local artifacts = {}
    local install_list = {}
    local custom_steps = {}

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

    function ctx:get_jobs()
        return _SPKG_JOBS or 1
    end

    function ctx:get_host()
        return spkg.current_platform()
    end

    local optimize_flags = {
        Debug        = "-O0",
        ReleaseSafe  = "-O1",
        ReleaseFast  = "-O2",
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
            run_args  = {},
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

        function art:set_run_args(...)
            self.run_args = {...}
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
    ctx._custom_steps = custom_steps

    function ctx:add_custom_step(opts)
        local step = {
            name    = opts.name,
            command = opts.command,
            inputs  = opts.inputs or {},
            outputs = opts.outputs or {},
        }
        table.insert(custom_steps, step)
        return step
    end

    return ctx
end

-- ═══════════════════════════════════════════════════════════════
-- File Resolution
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

-- ═══════════════════════════════════════════════════════════════
-- DAG Topological Sort
-- ═══════════════════════════════════════════════════════════════

-- Kahn's algorithm: sort artifacts so dependencies are built first
local function topo_sort(artifacts)
    local by_name = {}
    local in_degree = {}
    local reverse_deps = {}

    for _, art in ipairs(artifacts) do
        by_name[art.name] = art
        in_degree[art.name] = 0
        reverse_deps[art.name] = {}
    end

    -- in_degree[X] = number of artifacts that X depends on (within our list)
    for _, art in ipairs(artifacts) do
        for _, dep_name in ipairs(art.link_deps) do
            if by_name[dep_name] then
                in_degree[art.name] = in_degree[art.name] + 1
                table.insert(reverse_deps[dep_name], art.name)
            end
        end
    end

    -- BFS: start with artifacts that have no dependencies
    local queue = {}
    for _, art in ipairs(artifacts) do
        if in_degree[art.name] == 0 then
            table.insert(queue, art.name)
        end
    end

    local result = {}
    local head = 1
    while head <= #queue do
        local name = queue[head]
        head = head + 1
        table.insert(result, by_name[name])
        for _, dependent in ipairs(reverse_deps[name]) do
            in_degree[dependent] = in_degree[dependent] - 1
            if in_degree[dependent] == 0 then
                table.insert(queue, dependent)
            end
        end
    end

    return result
end

-- ═══════════════════════════════════════════════════════════════
-- Build Graph Builder
-- ═══════════════════════════════════════════════════════════════

function M.build_graph_from_ctx(ctx)
    local target = ctx:get_target()

    build_graph = {
        target   = target,
        optimize = ctx:get_optimize(),
        artifacts = {},
    }

    local opt_flag = ctx._optimize_flags[ctx:get_optimize()] or "-O0"

    -- Topological sort to ensure correct build order
    local sorted = topo_sort(ctx._install_list)

    for _, art in ipairs(sorted) do
        local artifact_graph = {
            name          = art.name,
            type          = art.type,
            target        = target,
            compile_tasks = {},
            link_step     = {},
            run_args      = art.run_args or {},
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
                local dep_out = "build/" .. dep_name .. "/" .. artifact_output(dep_art)
                table.insert(all_inputs, dep_out)
            end
        end

        local lflags = {}
        for _, f in ipairs(art.ldflags) do table.insert(lflags, f) end
        for _, lib in ipairs(art.link_libs) do table.insert(lflags, "-l" .. lib) end

        local output_path = "build/" .. art.name .. "/" .. artifact_output(art)

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

local function parse_depfile(path)
    if not spkg.file_exists(path) then return {} end
    local content = spkg.read_file(path)
    if not content then return {} end

    local headers = {}
    local deps_part = content:match(":[%s]*\n(.*)")
    if not deps_part then deps_part = content:match(":%s*(.*)") end
    if not deps_part then return {} end

    deps_part = deps_part:gsub("\\%s*\n", " ")
    for h in deps_part:gmatch("%S+") do
        if spkg.file_exists(h) then table.insert(headers, h) end
    end
    return headers
end

-- ═══════════════════════════════════════════════════════════════
-- Fingerprint (detect cflag/include changes)
-- ═══════════════════════════════════════════════════════════════

local function compute_fingerprint(cflags)
    local sorted = {}
    for _, f in ipairs(cflags) do table.insert(sorted, f) end
    table.sort(sorted)
    local raw = table.concat(sorted, "|")
    return spkg.fingerprint(raw)
end

local function needs_compile(source, output, cflags)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then
        -- Source was deleted; clean up artifacts
        if spkg.file_exists(output) then spkg.remove(output) end
        local depfile = output:gsub("%.o$", "%.d")
        if spkg.file_exists(depfile) then spkg.remove(depfile) end
        return false
    end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end

    -- Check .d file for header dependencies
    local depfile = output:gsub("%.o$", "%.d")
    local headers = parse_depfile(depfile)
    for _, h in ipairs(headers) do
        local h_mtime = spkg.get_mtime(h)
        if h_mtime and h_mtime > out_mtime then return true end
    end

    -- Check command fingerprint
    local fp_file = output .. ".fp"
    local new_fp = compute_fingerprint(cflags or {})
    if spkg.file_exists(fp_file) then
        local old_fp = spkg.read_file(fp_file)
        if old_fp ~= new_fp then return true end
    else
        return true  -- no fingerprint file = first compile
    end

    return src_mtime > out_mtime
end

local function save_fingerprint(output, cflags)
    local fp_file = output .. ".fp"
    local fp = compute_fingerprint(cflags or {})
    spkg.write_file(fp_file, fp)
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
    if not compiler then return nil end

    local depfile = task.output:gsub("%.o$", "%.d")
    local cflags_str = table.concat(task.cflags, " ")
    return string.format('%s %s -MMD -MF "%s" "%s" -o "%s"',
        compiler, cflags_str, depfile, task.source, task.output)
end

local function compile_task(task, verbose)
    -- Check cache first (unless --no-cache)
    if not _SPKG_NO_CACHE then
        spkg.cache_init()
        local cache_key = compute_fingerprint(task.cflags) .. "_" ..
                          spkg.fingerprint(task.source)
        if spkg.cache_get(cache_key, task.output) then
            if verbose then print("  [cache hit] " .. task.source) end
            save_fingerprint(task.output, task.cflags)
            return true
        end
    end

    local cmd = compile_task_cmd(task, verbose)
    if not cmd then
        print("spkg: no compiler found. Install sharpc or set SHARPC env var.")
        return false
    end

    if not verbose then print(status_color("sp", task.source)) end
    if verbose then print("  " .. cmd) end

    local r = spkg.run_cmd(cmd)
    if not r.ok then
        print(error_msg("compilation failed:\n" .. r.out))
        return false
    end

    -- Save to cache (unless --no-cache)
    if not _SPKG_NO_CACHE then
        local cache_key = compute_fingerprint(task.cflags) .. "_" ..
                          spkg.fingerprint(task.source)
        spkg.cache_put(cache_key, task.output)
    end

    -- Save fingerprint on success
    save_fingerprint(task.output, task.cflags)

    if verbose and r.out ~= "" then print("    " .. r.out) end
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
            print("  [ar] " .. artifact_output(artifact))
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
        print("  [link] " .. artifact_output(artifact))
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
        for _, task in ipairs(tasks) do
            if not compile_task(task, verbose) then return false end
        end
        return true
    end

    -- Pre-filter: try cache for each task (sequential cache check is fast)
    local pending = {}
    for _, task in ipairs(tasks) do
        if not _SPKG_NO_CACHE then
            spkg.cache_init()
            local cache_key = compute_fingerprint(task.cflags) .. "_" ..
                              spkg.fingerprint(task.source)
            if spkg.cache_get(cache_key, task.output) then
                if verbose then print("  [cache hit] " .. task.source) end
                save_fingerprint(task.output, task.cflags)
                -- skip, already cached
            else
                table.insert(pending, { task = task })
            end
        else
            table.insert(pending, { task = task })
        end
    end

    if #pending == 0 then return true end

    local running = {}
    local any_failed = false

    local function start_next()
        while #pending > 0 and #running < max_jobs do
            local item = table.remove(pending, 1)
            local cmd = compile_task_cmd(item.task, verbose)
            if not cmd then any_failed = true; return false end
            if not verbose then print("  [sp] " .. item.task.source) end
            if verbose then print("  " .. cmd) end
            local task_id, err = spkg.start_cmd(cmd)
            if not task_id then
                any_failed = true
                print("    error: " .. err)
                return false
            end
            table.insert(running, { id = task_id, item = item })
        end
        return true
    end

    start_next()

    while #running > 0 do
        local new_running = {}
        for _, r in ipairs(running) do
            local result = spkg.wait_task(r.id)
            if result then
                if not result.ok then
                    any_failed = true
                    print("    error:\n" .. result.out)
                elseif verbose and result.out ~= "" then
                    print("    " .. result.out)
                end
                -- Save fingerprint and cache on success
                if result.ok then
                    save_fingerprint(r.item.task.output, r.item.task.cflags)
                    if not _SPKG_NO_CACHE then
                        local cache_key = compute_fingerprint(r.item.task.cflags) .. "_" ..
                                          spkg.fingerprint(r.item.task.source)
                        spkg.cache_put(cache_key, r.item.task.output)
                    end
                end
            else
                table.insert(new_running, r)
            end
        end
        running = new_running
        if #running < max_jobs and #pending > 0 and not any_failed then
            start_next()
            if any_failed then break end
        end
    end

    return not any_failed
end

-- ═══════════════════════════════════════════════════════════════
-- Distributed Compilation (Phase 3)
-- ═══════════════════════════════════════════════════════════════

local function parse_nodes()
    local nodes = {}

    -- Try SPKG_NODES environment variable first
    local env_nodes = os.getenv("SPKG_NODES")
    if env_nodes and env_nodes ~= "" then
        for n in env_nodes:gmatch("([^,]+)") do
            n = n:gsub("^%s+", ""):gsub("%s+$", "")
            if n ~= "" then table.insert(nodes, n) end
        end
    end

    -- Try spkg_nodes.json file
    if #nodes == 0 and spkg.file_exists("spkg_nodes.json") then
        local content = spkg.read_file("spkg_nodes.json")
        if content then
            for n in content:gmatch('"([^"]+)"') do
                table.insert(nodes, n)
            end
        end
    end

    return nodes
end

-- Base64 decode (simple Lua implementation)
local b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function b64_decode(str)
    local result = {}
    local bits = 0
    local val = 0
    for i = 1, #str do
        local c = str:sub(i, i)
        if c == '=' then break end
        local b = b64chars:find(c, 1, true)
        if b then
            b = b - 1
            val = (val << 6) | b
            bits = bits + 6
            if bits >= 8 then
                bits = bits - 8
                table.insert(result, string.char((val >> bits) & 0xff))
            end
        end
    end
    return table.concat(result)
end

function M.execute_distributed(verbose, max_jobs)
    local nodes = parse_nodes()
    if #nodes == 0 then
        print("spkg: no distributed nodes configured.")
        print("  Set SPKG_NODES=node1:port,node2:port or create spkg_nodes.json")
        print("  Falling back to local build.")
        return M._do_build(verbose, max_jobs)
    end

    -- 0. Fetch dependencies
    if not fetch_deps() then return false end

    -- 1. Create build context
    b = create_build_context()
    local ok, err = pcall(dofile, "Sharp.lua")
    if not ok then
        print("spkg: error: failed to execute Sharp.lua")
        print("  " .. tostring(err))
        return false
    end

    -- 2. Build the graph
    M.build_graph_from_ctx(b)

    -- 2.5. Custom steps (always local)
    if #b._custom_steps > 0 then
        if verbose then print("spkg: executing custom steps...") end
        if not execute_custom_steps(b._custom_steps, verbose) then return false end
    end

    print("spkg: distributed build with " .. #nodes .. " node(s)")
    for _, n in ipairs(nodes) do print("  node: " .. n) end

    -- 3. Execute each artifact
    local node_idx = 0
    for _, art in ipairs(build_graph.artifacts) do
        if verbose then
            print("spkg: building " .. art.name .. " [" .. art.type .. "]")
        else
            print("spkg: building " .. art.name .. " [" .. art.type .. "]")
        end

        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output, task.cflags) then
                spkg.mkdir_p("build/" .. art.name)

                -- Read source file
                local source = spkg.read_file(task.source)
                if not source then
                    print("  error: cannot read " .. task.source)
                    return false
                end

                -- Pick a node (round-robin)
                node_idx = (node_idx % #nodes) + 1
                local node = nodes[node_idx]

                -- Build JSON request
                local cflags_json = "["
                for i, f in ipairs(task.cflags) do
                    if i > 1 then cflags_json = cflags_json .. "," end
                    cflags_json = cflags_json .. '"' .. f .. '"'
                end
                cflags_json = cflags_json .. "]"

                local opt = _SPKG_OPTIMIZE or "Debug"
                local req = string.format(
                    '{"source":%s,"cflags":%s,"optimize":"%s"}',
                    '"' .. source:gsub('"', '\\"'):gsub('\n', '\\n') .. '"',
                    cflags_json, opt)

                -- Send to node
                if verbose then print("  [remote] " .. task.source .. " -> " .. node)
                else print("  [remote] " .. task.source) end

                local url = "http://" .. node .. "/compile"
                local r = spkg.http_post(url, req)

                if not r.ok or r.code ~= 200 then
                    print("  error: node " .. node .. " failed (code=" .. r.code .. ")")
                    print("  " .. r.body)
                    return false
                end

                -- Parse response
                local output = r.body:match('"output":"([^"]*)"')
                if output then
                    local decoded = b64_decode(output)
                    spkg.write_file(task.output, decoded)
                    save_fingerprint(task.output, task.cflags)

                    -- Write depfile if present
                    local depfile = r.body:match('"depfile":"([^"]*)"')
                    if depfile and depfile ~= "" then
                        -- Unescape JSON string
                        depfile = depfile:gsub("\\n", "\n"):gsub("\\\\", "\\")
                        local dep_path = task.output:gsub("%.o$", "%.d")
                        spkg.write_file(dep_path, depfile)
                    end
                else
                    print("  error: no output from node")
                    return false
                end
            else
                if verbose then print("  [skip] " .. task.source .. " (up to date)") end
            end
        end

        -- Link step (always local)
        if not link_artifact(art, verbose) then return false end
    end

    print("spkg: done.")
    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Custom Step Integration
-- ═══════════════════════════════════════════════════════════════

local function execute_custom_steps(steps, verbose)
    for _, step in ipairs(steps) do
        local needs_run = spkg.custom_needs_run(step.inputs, step.outputs)
        if not needs_run then
            if verbose then print("  [skip] custom step " .. step.name .. " (up to date)") end
        else
            if verbose then
                print("  [custom] " .. step.name .. ": " .. table.concat(step.command, " "))
            else
                print("  [custom] " .. step.name)
            end
            local r = spkg.custom_exec(step.command, nil)
            if not r.ok then
                print("    error:\n" .. r.out)
                return false
            end
            if verbose and r.out ~= "" then print("    " .. r.out) end
        end
    end
    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Fetching
-- ═══════════════════════════════════════════════════════════════

local function fetch_deps()
    local home = _SPKG_HOME or "/root"
    if not spkg_fetch.fetch_recursive(home) then return false end
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
    return { spkg.current_platform() }
end

function M._do_build(verbose, max_jobs)
    -- 0. Fetch dependencies
    if not fetch_deps() then return false end

    -- 1. Create build context and inject as global 'b'
    b = create_build_context()

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

    -- 2. Build the graph (DAG sorted)
    M.build_graph_from_ctx(b)

    -- 2.5. Execute custom steps before artifact compilation
    if #b._custom_steps > 0 then
        if verbose then print("spkg: executing custom steps...") end
        if not execute_custom_steps(b._custom_steps, verbose) then
            return false
        end
    end

    -- 3. Execute each artifact in topological order
    local all_ok = true
    for _, art in ipairs(build_graph.artifacts) do
        if verbose then
            print("spkg: building " .. art.name .. " [" .. art.type .. "] " ..
                  "target=" .. art.target)
        else
            print("spkg: building " .. art.name .. " [" .. art.type .. "]")
        end

        -- Separate pending vs up-to-date tasks
        local pending_tasks = {}
        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output, task.cflags) then
                spkg.mkdir_p("build/" .. art.name)
                table.insert(pending_tasks, task)
            else
                if verbose then
                    print("  [skip] " .. task.source .. " (up to date)")
                end
            end
        end

        if #pending_tasks > 0 then
            local compile_ok = compile_tasks_parallel(pending_tasks, verbose, max_jobs)
            if not compile_ok then
                all_ok = false
                break
            end
        end

        -- Link step
        if not link_artifact(art, verbose) then
            all_ok = false
            break
        end
    end

    if all_ok then print(COLOR("spkg: done.", "bold_green")) end
    return all_ok
end

-- ═══════════════════════════════════════════════════════════════
-- Run first executable artifact
-- ═══════════════════════════════════════════════════════════════

function M.run_first_artifact(extra_args)
    for _, art in ipairs(build_graph.artifacts) do
        if art.type == "exe" then
            local exe = art.link_step.output
            if not spkg.file_exists(exe) then
                print("spkg: executable not found: " .. exe)
                return false
            end

            -- Build command: exe + run_args + extra_args
            local args = {}
            for _, a in ipairs(art.run_args or {}) do table.insert(args, a) end
            if extra_args then
                for _, a in ipairs(extra_args) do table.insert(args, a) end
            end

            local cmd
            if is_windows() then
                cmd = '"' .. exe .. '"'
            else
                cmd = "./" .. exe
            end
            if #args > 0 then
                cmd = cmd .. " " .. table.concat(args, " ")
            end

            if _SPKG_VERBOSE then print("  [run] " .. cmd) end
            local r = spkg.run_cmd(cmd)
            if r.out ~= "" then print(r.out) end
            return r.ok
        end
    end

    print("spkg: no executable artifact to run.")
    return false
end

M.create_build_context = create_build_context

return M
