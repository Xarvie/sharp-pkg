-- spkg_build.lua — Sharp Build System
--
-- Implements:
--   - Build Context (b object injected into config.spkg)
--   - Artifact objects (exe, staticlib, sharedlib)
--   - DAG-based build graph with topological sort
--   - Platform-aware artifact naming (.exe/.lib/.dll)
--   - Incremental compilation (mtime + depfile + fingerprint)
--   - Local execution with parallel compilation (--jobs)
--
-- Distributed compilation (Phase 3): build_graph is pure data, serializable.

local M = {}

-- Dependency visibility (matches CMake PUBLIC/PRIVATE/INTERFACE)
local V = { PUBLIC = "public", PRIVATE = "private", INTERFACE = "interface" }

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

-- ── Build Graph (populated after running config.spkg) ──
local build_graph = { artifacts = {} }

-- ═══════════════════════════════════════════════════════════════
-- Platform Helpers
-- ═══════════════════════════════════════════════════════════════

local function is_windows()
    local plat = spkg.current_platform()
    return plat:match("windows") or plat:match("mingw")
end

local function parse_target(triple)
    local arch, vendor, os, abi = triple:match("^([^-]+)-([^-]+)-([^-]+)-([^-]+)$")
    if not arch then
        local fallback = triple or ""
        return { raw = fallback, os = fallback, arch = fallback, vendor = "", abi = "" }
    end
    return {
        raw    = triple,
        arch   = arch,
        vendor = vendor,
        os     = os,
        abi    = abi,
    }
end

local function parse_options(declared_opts)
    local result = {}
    for name, opt in pairs(declared_opts) do
        result[name] = opt.default
    end
    for _, arg in ipairs(_SPKG_ARGS or {}) do
        local key, val = arg:match("^%-%-([^=]+)=?(.*)$")
        if key and declared_opts[key] then
            if val == "true" or val == "" then
                result[key] = true
            elseif val == "false" then
                result[key] = false
            elseif val ~= "" then
                result[key] = val
            end
        end
    end
    return result
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
    local tests = {}

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

    local declared_options = {}
    local resolved_options = nil

    function ctx:option(name, opts)
        opts = opts or {}
        declared_options[name] = {
            description = opts.description or "",
            default     = opts.default,
        }
    end

    function ctx:_resolve_options()
        if not resolved_options then
            resolved_options = parse_options(declared_options)
        end
        return resolved_options
    end

    ctx.target   = parse_target(ctx:get_target())
    ctx.host     = parse_target(ctx:get_host())
    ctx.options  = setmetatable({}, {
        __index = function(_, k)
            return ctx:_resolve_options()[k]
        end
    })

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

        function art:add_define(...)
            local args = {...}
            for _, d in ipairs(args) do
                table.insert(self.cflags, "-D" .. d)
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

        function art:link_artifact(other_name_or_art, opts)
            opts = opts or {}
            local name = type(other_name_or_art) == "string"
                and other_name_or_art
                or other_name_or_art.name
            table.insert(self.link_deps, {
                name       = name,
                visibility = opts.visibility or V.PUBLIC,
            })
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
    ctx._tests = tests

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

    function ctx:add_test(opts)
        local art = opts.artifact
        if not art then
            local name = opts and opts.name or "test"
            art = create_artifact(name, "exe")
            table.insert(artifacts, art)
        end
        art._is_test = true
        table.insert(tests, art)
        return art
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
-- Transitive Dependency Resolution
-- ═══════════════════════════════════════════════════════════════
--
-- Resolves the full dependency closure for an artifact.
--
-- Visibility semantics (matches CMake):
--   "public"    — link + compile flags propagate to consumers
--   "private"   — used for building this artifact only, NOT propagated
--   "interface" — propagated to consumers but NOT used for self-build
--
-- Link order guarantee:
--   Dependencies are emitted in topological order so that each
--   library appears BEFORE the libraries it depends on.
--   This matches Unix linker semantics (-lA -lB where A depends on B).
--
-- Returns: ordered list of { name, artifact, visibility, depth }

local function collect_transitive_deps(art_name, ctx, visited, depth)
    depth = depth or 0
    visited = visited or {}
    if visited[art_name] then return {} end
    visited[art_name] = true

    local art = ctx:dependency(art_name)
    if not art then return {} end

    local result = {}
    for _, dep_entry in ipairs(art.link_deps) do
        local dep_name, visibility = dep_entry.name, dep_entry.visibility or "public"
        local dep_art = ctx:dependency(dep_name)
        if dep_art then
            if dep_art.type == "staticlib" then
                -- Recurse into static lib deps first (they must link AFTER us)
                local transitive = collect_transitive_deps(dep_name, ctx, visited, depth + 1)
                for _, t in ipairs(transitive) do
                    table.insert(result, t)
                end
            end
            -- Add this dependency AFTER its own transitive deps
            table.insert(result, {
                name       = dep_name,
                artifact   = dep_art,
                visibility = visibility,
                depth      = depth,
            })
        end
    end
    return result
end

-- ═══════════════════════════════════════════════════════════════
-- Build Graph Builder
-- ═══════════════════════════════════════════════════════════════

function M.build_graph_from_ctx(ctx, include_tests)
    local target = ctx:get_target()

    build_graph = {
        target   = target,
        optimize = ctx:get_optimize(),
        artifacts = {},
    }

    local opt_flag = ctx._optimize_flags[ctx:get_optimize()] or "-O0"

    -- Build combined list: install_list + tests (if requested)
    local all_artifacts = {}
    for _, a in ipairs(ctx._install_list) do table.insert(all_artifacts, a) end
    if include_tests then
        for _, t in ipairs(ctx._tests or {}) do table.insert(all_artifacts, t) end
    end

    -- Topological sort to ensure correct build order
    local sorted = topo_sort(all_artifacts)

    for _, art in ipairs(sorted) do
        local artifact_graph = {
            name             = art.name,
            type             = art.type,
            target           = target,
            compile_tasks    = {},
            link_step        = {},
            run_args         = art.run_args or {},
            _is_test_artifact = art._is_test or false,
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
                local stem = fp:gsub("%.ce$", ""):gsub("[/\\]", "_")
                local output = "build/" .. art.name .. "/" .. stem .. ".o"
                table.insert(artifact_graph.compile_tasks, {
                    source = fp,
                    output = output,
                    cflags = src_cflags,
                })
            end
        end

        -- Link inputs: own objects + all transitive dependency outputs
        local all_inputs = {}
        for _, task in ipairs(artifact_graph.compile_tasks) do
            table.insert(all_inputs, task.output)
        end

        local lflags = {}

        -- Resolve ALL transitive dependencies (ordered, deduplicated)
        local all_deps = collect_transitive_deps(art.name, ctx)
        local seen = {}
        for _, dep in ipairs(all_deps) do
            if not seen[dep.name] then
                seen[dep.name] = true

                if dep.artifact.type == "staticlib" then
                    table.insert(lflags, "-Lbuild/" .. dep.name)
                    table.insert(lflags, "-l" .. dep.name)

                    -- Propagate public/interface includes and cflags to consumer
                    if dep.visibility ~= V.PRIVATE then
                        for _, inc in ipairs(dep.artifact.includes or {}) do
                            if not artifact_graph.propagated_includes then
                                artifact_graph.propagated_includes = {}
                            end
                            table.insert(artifact_graph.propagated_includes, inc)
                        end
                        for _, cf in ipairs(dep.artifact.cflags or {}) do
                            if not artifact_graph.propagated_cflags then
                                artifact_graph.propagated_cflags = {}
                            end
                            table.insert(artifact_graph.propagated_cflags, cf)
                        end
                    end
                else
                    local dep_out = "build/" .. dep.name .. "/" .. artifact_output(dep.artifact)
                    table.insert(all_inputs, dep_out)
                end
            end
        end

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

local BUILD_SYSTEM_VERSION = "spkg-v4"

local function compute_fingerprint(cflags)
    local sorted = {}
    for _, f in ipairs(cflags) do table.insert(sorted, f) end
    table.sort(sorted)
    local raw = BUILD_SYSTEM_VERSION .. "|" .. table.concat(sorted, "|")
    return spkg.fingerprint(raw)
end

local function needs_compile(source, output, cflags)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then
        -- Source was deleted; clean up artifacts
        if spkg.file_exists(output) then spkg.remove(output) end
        local depfile = output:gsub("%.o$", ".d")
        if spkg.file_exists(depfile) then spkg.remove(depfile) end
        return false
    end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end

    -- Check .d file for header dependencies
    local depfile = output:gsub("%.o$", ".d")
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

local function file_content_fingerprint(filepath)
    local content = spkg.read_file(filepath)
    if content then
        return spkg.fingerprint(content)
    end
    return spkg.fingerprint(filepath)
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

    local depfile = task.output:gsub("%.o$", ".d")
    local cflags_str = table.concat(task.cflags, " ")
    return string.format('%s -c %s -MMD -MF "%s" "%s" -o "%s"',
        compiler, cflags_str, depfile, task.source, task.output)
end

local function compile_task(task, verbose)
    -- Check cache first (unless --no-cache)
    if not _SPKG_NO_CACHE then
        spkg.cache_init()
        local cache_key = compute_fingerprint(task.cflags) .. "_" ..
                          file_content_fingerprint(task.source)
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
                          file_content_fingerprint(task.source)
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
        -- Use zig ar if available, otherwise fall back to system ar
        local ar_cmd
        local zig = spkg.find_zigcc()
        if zig then
            ar_cmd = string.format('"%s" ar rcs', zig)
        else
            -- Fallback: use system ar from PATH
            ar_cmd = "ar rcs"
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
        local cmd = string.format('%s "%s" %s', ar_cmd, link.output, inputs)
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
                              file_content_fingerprint(task.source)
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
                                          file_content_fingerprint(r.item.task.source)
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

-- ═══════════════════════════════════════════════════════════════
-- Header Dependency Collection for Distributed Build
-- ═══════════════════════════════════════════════════════════════

-- Parse #include directives from source code
-- Returns list of include paths (e.g., "stdio.h", "lib/mylib.h")
local function parse_includes(source)
    local includes = {}
    for line in source:gmatch("[^\n]*") do
        -- Match #include "..." or #include <...>
        local inc = line:match('#%s*include%s+"([^"]+)"')
                    or line:match('#%s*include%s+<([^>]+)>')
        if inc then table.insert(includes, inc) end
    end
    return includes
end

-- Resolve an include path against a list of include directories
-- Returns the full path if found, nil otherwise
local function resolve_include_path(inc_path, include_dirs, source_dir)
    -- Try include directories from task cflags (-I flags)
    for _, dir in ipairs(include_dirs) do
        local full = dir .. "/" .. inc_path
        if spkg.file_exists(full) then return full end
    end
    -- Try relative to source file directory
    if source_dir and source_dir ~= "" then
        local full = source_dir .. "/" .. inc_path
        if spkg.file_exists(full) then return full end
    end
    return nil
end

-- Extract -I directories from cflags list
local function extract_include_dirs(cflags)
    local dirs = {}
    for _, flag in ipairs(cflags) do
        if flag:sub(1, 2) == "-I" then
            local dir = flag:sub(3)
            if dir ~= "" then table.insert(dirs, dir) end
        end
    end
    return dirs
end

-- Collect all header dependencies recursively
-- Returns a map: relative_path -> file_content
local function collect_headers(source, include_dirs, source_dir, visited, depth)
    visited = visited or {}
    depth = depth or 0
    if depth > 10 then return visited end  -- Prevent infinite recursion

    local includes = parse_includes(source)
    for _, inc in ipairs(includes) do
        if not visited[inc] then
            local full_path = resolve_include_path(inc, include_dirs, source_dir)
            if full_path then
                local content = spkg.read_file(full_path)
                if content then
                    visited[inc] = content
                    -- Recursively collect headers from this header
                    collect_headers(content, include_dirs, source_dir, visited, depth + 1)
                end
            end
        end
    end
    return visited
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Fetching
-- ═══════════════════════════════════════════════════════════════

local function fetch_deps()
    -- Load spkg_fetch module if not already loaded
    if not spkg_fetch then
        local ok, mod = pcall(dofile, "spkg_fetch.lua")
        if not ok then
            print("spkg: warning: cannot load spkg_fetch.lua: " .. tostring(mod))
            return true  -- Skip fetching, not critical
        end
        spkg_fetch = mod
    end
    if spkg_fetch.fetch_recursive then
        local home = _SPKG_HOME or "/root"
        if not spkg_fetch.fetch_recursive(home) then return false end
    end
    return true
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
    local ok, err = pcall(dofile, "config.spkg")
    if not ok then
        print("spkg: error: failed to execute config.spkg")
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

                -- Collect header dependencies for distributed build
                local include_dirs = extract_include_dirs(task.cflags)
                local source_dir = task.source:match("(.*/)") or "."
                local headers = collect_headers(source, include_dirs, source_dir)

                -- Try each node (round-robin starting from current idx)
                local task_ok = false
                local last_err = ""
                for attempt = 1, #nodes do
                    local try_idx = ((node_idx + attempt - 1) % #nodes) + 1
                    local node = nodes[try_idx]

                    -- Build JSON request with headers
                    local cflags_json = "["
                    for i, f in ipairs(task.cflags) do
                        if i > 1 then cflags_json = cflags_json .. "," end
                        local f_escaped = f:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
                        cflags_json = cflags_json .. '"' .. f_escaped .. '"'
                    end
                    cflags_json = cflags_json .. "]"

                    -- Escape source: backslash first, then quotes and control chars
                    local escaped = source:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')

                    -- Build headers JSON: {"path":"path1","content":"..."},{"path":"path2","content":"..."}
                    local headers_json = ""
                    local first = true
                    for hpath, hcontent in pairs(headers) do
                        if not first then headers_json = headers_json .. "," end
                        first = false
                        local h_escaped = hcontent:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
                        headers_json = headers_json .. string.format('{"path":"%s","content":"%s"}', hpath, h_escaped)
                    end

                    local opt = _SPKG_OPTIMIZE or "Debug"
                    local req = string.format(
                        '{"source":"%s","cflags":%s,"optimize":"%s","headers":[%s]}',
                        escaped, cflags_json, opt, headers_json)

                    -- Send to node
                    if verbose and attempt == 1 then
                        print("  [remote] " .. task.source .. " -> " .. node)
                    elseif verbose and attempt > 1 then
                        print("  [retry]  " .. task.source .. " -> " .. node)
                    else
                        print("  [remote] " .. task.source)
                    end

                    local url = "http://" .. node .. "/compile"
                    local r = spkg.http_post(url, req)

                    if not r.ok or r.code ~= 200 then
                        last_err = (r.body or ""):gsub("\n", " "):sub(1, 120)
                        if attempt < #nodes then
                            -- Try next node
                        else
                            print("  error: all nodes failed for " .. task.source)
                            print("  last error: " .. last_err)
                            return false
                        end
                    else
                        -- Parse response with proper JSON parser
                        local ok_json, resp = pcall(spkg.json_parse, r.body)
                        if not ok_json or type(resp) ~= "table" then
                            last_err = "unparsable response from node"
                            if attempt >= #nodes then
                                print("  error: " .. last_err)
                                return false
                            end
                        elseif resp.status == "error" then
                            last_err = resp.stderr or "unknown error"
                            if attempt >= #nodes then
                                print("  error: " .. last_err)
                                return false
                            end
                        else
                            -- Success: resp.status == "ok"
                            local output = resp.output
                            if output then
                                local decoded = b64_decode(output)
                                spkg.write_file(task.output, decoded)
                                save_fingerprint(task.output, task.cflags)

                                -- Write depfile if present
                                if resp.depfile and resp.depfile ~= "" then
                                    local dep_path = task.output:gsub("%.o$", "%.d")
                                    spkg.write_file(dep_path, resp.depfile)
                                end

                                task_ok = true
                                node_idx = try_idx
                                break
                            else
                                last_err = "no output from node"
                                if attempt >= #nodes then
                                    print("  error: " .. last_err)
                                    return false
                                end
                            end
                        end
                    end
                end

                if not task_ok then return false end
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
    -- Phase 1: config.spkg doesn't statically declare target list.
    -- --all builds the current platform target.
    print("spkg: --all not yet supported (config.spkg doesn't declare static targets).")
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

    local ok, err = pcall(dofile, "config.spkg")
    if not ok then
        print("spkg: error: failed to execute config.spkg")
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

-- ═══════════════════════════════════════════════════════════════
-- Test Runner (Phase 4)
-- ═══════════════════════════════════════════════════════════════

function M.execute_tests(verbose)
    -- 0. Fetch dependencies
    if not fetch_deps() then return false end

    -- 1. Create build context
    b = create_build_context()
    local ok, err = pcall(dofile, "config.spkg")
    if not ok then
        print(error_msg("failed to execute config.spkg"))
        print("  " .. tostring(err))
        return false
    end

    if #b._tests == 0 then
        print("spkg: no tests declared (use b:add_test() in config.spkg)")
        return true
    end

    -- 2. Build graph (include test artifacts)
    M.build_graph_from_ctx(b, true)

    -- 3. Build test artifacts (topological order from build_graph)
    for _, art in ipairs(build_graph.artifacts) do
        if not art._is_test_artifact then goto continue end

        print(COLOR("spkg: building test " .. art.name, "bold_cyan"))

        -- Compile tasks
        local pending_tasks = {}
        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output, task.cflags) then
                spkg.mkdir_p("build/" .. art.name)
                table.insert(pending_tasks, task)
            else
                if verbose then print("  [skip] " .. task.source .. " (up to date)") end
            end
        end

        if #pending_tasks > 0 then
            if not compile_tasks_parallel(pending_tasks, verbose, 1) then
                return false
            end
        end

        -- Link
        if not link_artifact(art, verbose) then return false end

        -- Run test
        local exe = art.link_step.output
        if not spkg.file_exists(exe) then
            print(error_msg("test executable not found: " .. exe))
            return false
        end

        local cmd = (is_windows() and '"' or "./") .. exe .. (is_windows() and '"' or "")
        print(COLOR("  [test] " .. art.name, "bold_cyan"))

        local r = spkg.run_cmd(cmd)
        if r.out ~= "" then print(r.out) end
        if not r.ok then
            print(error_msg("test " .. art.name .. " failed (exit code " .. r.code .. ")"))
            return false
        end
        print(COLOR("  [pass] " .. art.name, "bold_green"))

        ::continue::
    end

    print(COLOR("spkg: all tests passed.", "bold_green"))
    return true
end

return M
