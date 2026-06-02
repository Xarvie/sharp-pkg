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
        arch, vendor, os = triple:match("^([^-]+)-([^-]+)-([^-]+)$")
    end
    if not arch then
        arch, os = triple:match("^([^-]+)-([^-]+)$")
    end
    if not arch then
        if triple and triple ~= "" then
            print(warn_msg("unexpected target triple format: '" .. triple .. "'"))
        end
        local fallback = triple or ""
        return { raw = fallback, os = fallback, arch = fallback, vendor = "", abi = "" }
    end
    return {
        raw    = triple,
        arch   = arch or "",
        vendor = vendor or "",
        os     = os or "",
        abi    = abi or "",
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

local function find_compiler()
    return spkg.find_sharpc()
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
    local deps = {}

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

    function ctx:get_sysroot()
        if _SPKG_SYSROOT and _SPKG_SYSROOT ~= "" then
            return _SPKG_SYSROOT
        end
        return nil
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

    function ctx:platform()
        local t = ctx:get_target()
        local h = spkg.current_platform()
        local parsed = parse_target(t)
        return {
            raw       = t,
            is_host   = (t == "" or t == h),
            is_android = function() return t:match("android") ~= nil end,
            is_ios     = function() return t:match("ios")     ~= nil end,
            is_macos   = function() return t:match("darwin")  ~= nil or t:match("macos") ~= nil end,
            is_linux   = function() return t:match("linux")   ~= nil end,
            is_windows = function() return t:match("windows") ~= nil or t:match("mingw") ~= nil end,
            arch       = parsed.arch,
            os         = parsed.os,
        }
    end
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
        -- Check for duplicate artifact
        for _, existing in ipairs(artifacts) do
            if existing.name == name then return existing end
        end
        local art = create_artifact(name, "exe")
        table.insert(artifacts, art)
        return art
    end

    function ctx:add_static_library(opts)
        local name = opts and opts.name or "lib"
        -- Check for duplicate artifact
        for _, existing in ipairs(artifacts) do
            if existing.name == name then return existing end
        end
        local art = create_artifact(name, "staticlib")
        table.insert(artifacts, art)
        return art
    end

    function ctx:add_shared_library(opts)
        local name = opts and opts.name or "lib"
        -- Check for duplicate artifact
        for _, existing in ipairs(artifacts) do
            if existing.name == name then return existing end
        end
        local art = create_artifact(name, "sharedlib")
        table.insert(artifacts, art)
        return art
    end

    function ctx:install(art)
        -- Deduplicate: don't install the same artifact twice
        for _, a in ipairs(install_list) do
            if a == art then return art end
        end
        table.insert(install_list, art)
        return art
    end

    function ctx:dependency(name)
        for _, a in ipairs(artifacts) do
            if a.name == name then return a end
        end
        return nil
    end

    function ctx:dep(name, opts)
        opts = opts or {}
        table.insert(deps, {
            name    = name,
            version = opts.version or "*",
            url     = opts.url,
        })
    end

    ctx._artifacts = artifacts
    ctx._install_list = install_list
    ctx._optimize_flags = optimize_flags
    ctx._custom_steps = custom_steps
    ctx._tests = tests
    ctx._deps = deps

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

    local _include_cache = {}

    function ctx:has_include(header)
        local target = ctx:get_target()
        local key = target .. "|" .. header
        if _include_cache[key] ~= nil then
            return _include_cache[key]
        end

        local compiler = find_compiler()
        if not compiler then
            _include_cache[key] = false
            return false
        end

        local target_flat = target:gsub("[/\\%.]", "_")
        local tmpfile = ".spkg_check_" .. target_flat .. "_" .. header:gsub("[/\\%.]", "_") .. ".c"
        spkg.write_file(tmpfile, "#include <" .. header .. ">\nint __spkg_check(void){return 0;}\n")

        local tflag = ""
        if _SPKG_TARGET and _SPKG_TARGET ~= "" then
            tflag = " --target " .. _SPKG_TARGET
        end
        local cmd = compiler .. tflag .. " -c " .. tmpfile .. " -o /dev/null"
        local r = spkg.run_cmd(cmd)

        if spkg.file_exists(tmpfile) then
            spkg.remove(tmpfile)
        end

        _include_cache[key] = r.ok
        return r.ok
    end

    function ctx:has_libc()
        return ctx:has_include("stdlib.h") and ctx:has_include("stdio.h")
    end

    function ctx:has_cflag(pattern)
        local env = os.getenv("CFLAGS") or ""
        if env:find(pattern, 1, true) then
            return true
        end
        local spkg_env = os.getenv("SPKG_CFLAGS") or ""
        if spkg_env:find(pattern, 1, true) then
            return true
        end
        return false
    end

    function ctx:is_freestanding()
        return ctx:has_cflag("-ffreestanding")
            or ctx:has_cflag("-nostdlib")
            or ctx:has_cflag("-nodefaultlibs")
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
        for _, dep_entry in ipairs(art.link_deps) do
            local dep_name = dep_entry.name
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
    local host = ctx:get_host()
    local cross_target = nil
    if _SPKG_TARGET and _SPKG_TARGET ~= "" then
        cross_target = _SPKG_TARGET
    end

    build_graph = {
        target   = target,
        optimize = ctx:get_optimize(),
        artifacts = {},
    }

    local opt_flag = ctx._optimize_flags[ctx:get_optimize()] or "-O0"
    local sysroot_flag = nil
    local sysroot = ctx:get_sysroot()
    if sysroot then sysroot_flag = "--sysroot=" .. sysroot end

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
            target           = cross_target or host,
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
            if sysroot_flag then
                table.insert(src_cflags, sysroot_flag)
            end

            for _, fp in ipairs(files) do
                local ext = fp:match("%.([^%.\\/]+)$") or ""
                local stem = fp:gsub("%.[^%.\\/]+$", ""):gsub("[/\\]", "_")
                local output = "build/" .. art.name .. "/" .. stem .. ".o"
                table.insert(artifact_graph.compile_tasks, {
                    source = fp,
                    output = output,
                    cflags = src_cflags,
                    target = cross_target or "",
                    src_ext = ext,
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

local BUILD_SYSTEM_FP = nil

local function compute_fingerprint(cflags)
    if not BUILD_SYSTEM_FP then
        BUILD_SYSTEM_FP = spkg.self_fingerprint()
    end
    local sorted = {}
    for _, f in ipairs(cflags) do table.insert(sorted, f) end
    table.sort(sorted)
    local raw = BUILD_SYSTEM_FP .. "|" .. table.concat(sorted, "|")
    return spkg.fingerprint(raw)
end

local _compiler_fp = nil
local function compiler_fingerprint()
    if _compiler_fp then return _compiler_fp end
    local compiler = find_compiler()
    if not compiler then
        _compiler_fp = "no-compiler"
        return _compiler_fp
    end
    local content = spkg.read_file(compiler)
    if content then
        _compiler_fp = spkg.fingerprint(content)
    else
        _compiler_fp = spkg.fingerprint(compiler)
    end
    return _compiler_fp
end

local _header_ctx_cache = {}

-- Extract all include-relevant flags from cflags
local function extract_all_includes(cflags)
    local dirs = {}
    for _, flag in ipairs(cflags) do
        local dir = flag:match("^-I(.+)$")
                 or flag:match("^-isystem%s+(.+)$")
                 or flag:match("^-idirafter%s+(.+)$")
                 or flag:match("^-iquote%s+(.+)$")
        if dir and dir ~= "" then table.insert(dirs, dir) end
    end
    return dirs
end

local function header_context_fingerprint(task)
    local cache_key = task.source .. "|" .. table.concat(task.cflags or {}, ",")
    if _header_ctx_cache[cache_key] then
        return _header_ctx_cache[cache_key]
    end

    local source = spkg.read_file(task.source)
    if not source then
        _header_ctx_cache[cache_key] = ""
        return ""
    end

    local include_dirs = extract_all_includes(task.cflags or {})
    local source_dir = task.source:match("(.*/)") or "."
    local scanned = {}
    local fps = {}

    local function collect(source_content, depth)
        if depth > 10 then return end
        for line in source_content:gmatch("[^\n]*") do
            local inc = line:match('#%s*include%s+"([^"]+)"')
                     or line:match('#%s*include%s+<([^>]+)>')
            if inc and not scanned[inc] then
                scanned[inc] = true
                local found = false
                for _, dir in ipairs(include_dirs) do
                    local full = dir .. "/" .. inc
                    if spkg.file_exists(full) then
                        local hc = spkg.read_file(full)
                        if hc then
                            table.insert(fps, spkg.fingerprint(hc))
                            collect(hc, depth + 1)
                        end
                        found = true
                        break
                    end
                end
                if not found then
                    local full = source_dir .. "/" .. inc
                    if spkg.file_exists(full) then
                        local hc = spkg.read_file(full)
                        if hc then
                            table.insert(fps, spkg.fingerprint(hc))
                            collect(hc, depth + 1)
                        end
                    end
                end
            end
        end
    end

    collect(source, 0)
    table.sort(fps)
    local result = #fps > 0 and spkg.fingerprint(table.concat(fps, "|")) or ""
    _header_ctx_cache[cache_key] = result
    return result
end

local function file_content_fingerprint(filepath)
    local content = spkg.read_file(filepath)
    if content then
        return spkg.fingerprint(content)
    end
    return spkg.fingerprint(filepath)
end

local function cache_key_for(task)
    return compute_fingerprint(task.cflags) .. "_" ..
           (task.target or "") .. "_" ..
           file_content_fingerprint(task.source) .. "_" ..
           compiler_fingerprint() .. "_" ..
           header_context_fingerprint(task)
end

local function needs_compile(source, output, cflags, target)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then
        if spkg.file_exists(output) then spkg.remove(output) end
        local depfile = output:gsub("%.o$", ".d")
        if spkg.file_exists(depfile) then spkg.remove(depfile) end
        return false
    end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end

    local depfile = output:gsub("%.o$", ".d")
    local headers = parse_depfile(depfile)
    for _, h in ipairs(headers) do
        local h_mtime = spkg.get_mtime(h)
        if h_mtime and h_mtime > out_mtime then return true end
    end

    local fp_file = output .. ".fp"
    local new_fp = compute_fingerprint(cflags or {})
    if target and target ~= "" then
        new_fp = new_fp .. "_" .. target
    end
    if spkg.file_exists(fp_file) then
        local old_fp = spkg.read_file(fp_file)
        if old_fp ~= new_fp then return true end
    else
        return true
    end

    return src_mtime > out_mtime
end

local function save_fingerprint(output, cflags, target)
    local fp_file = output .. ".fp"
    local fp = compute_fingerprint(cflags or {})
    if target and target ~= "" then
        fp = fp .. "_" .. target
    end
    spkg.write_file(fp_file, fp)
end

-- ═══════════════════════════════════════════════════════════════
-- Compiler / Linker Execution
-- ═══════════════════════════════════════════════════════════════

local function compile_task_cmd(task, verbose)
    local compiler = find_compiler()
    if not compiler then return nil end

    local depfile = task.output:gsub("%.o$", ".d")
    local cflags_str = table.concat(task.cflags, " ")
    local target_flag = ""
    if task.target and task.target ~= "" then
        target_flag = " --target " .. task.target
    end
    local sysroot_flag = ""
    if _SPKG_SYSROOT and _SPKG_SYSROOT ~= "" then
        sysroot_flag = " --sysroot " .. _SPKG_SYSROOT
    end
    local opt_flag = ""
    if _SPKG_OPTIMIZE and _SPKG_OPTIMIZE ~= "" then
        opt_flag = " --optimize " .. _SPKG_OPTIMIZE
    end
    return string.format('%s%s%s%s -c %s -MMD -MF "%s" "%s" -o "%s"',
        compiler, target_flag, sysroot_flag, opt_flag, cflags_str, depfile, task.source, task.output)
end

local function compile_task(task, verbose)
    -- Check cache first (unless --no-cache)
    if not _SPKG_NO_CACHE then
        spkg.cache_init()
        local cache_key = cache_key_for(task)
        if spkg.cache_get(cache_key, task.output) then
            if verbose then print("  [cache hit] " .. task.source) end
            save_fingerprint(task.output, task.cflags, task.target)
            return true
        end
    end

    local cmd = compile_task_cmd(task, verbose)
    if not cmd then
        print("spkg: no compiler found. Install sharpc or set SHARPC env var.")
        return false
    end

    if verbose then print("  " .. cmd) end

    local r = spkg.run_cmd(cmd)
    if not r then return false end
    if not r.ok then
        print(error_msg("compilation failed:\n" .. (r.out or "")))
        return false
    end

    -- Save to cache (unless --no-cache)
    if not _SPKG_NO_CACHE then
        spkg.cache_put(cache_key_for(task), task.output)
    end

    -- Save fingerprint on success
    save_fingerprint(task.output, task.cflags, task.target)

    if verbose and r.out and r.out ~= "" then print("    " .. r.out) end
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
        if not r then return false end
        if not r.ok then
            print("    ar error:\n" .. (r.out or ""))
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

    local tflag = ""
    if artifact.target and artifact.target ~= "" then
        tflag = " --target " .. artifact.target
    end
    local sysroot_flag = ""
    if _SPKG_SYSROOT and _SPKG_SYSROOT ~= "" then
        sysroot_flag = " --sysroot " .. _SPKG_SYSROOT
    end
    local opt_flag = ""
    if _SPKG_OPTIMIZE and _SPKG_OPTIMIZE ~= "" then
        opt_flag = " --optimize " .. _SPKG_OPTIMIZE
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
    local cmd = string.format('%s%s%s%s %s %s -o "%s"',
        compiler, tflag, sysroot_flag, opt_flag, inputs, ldflags, link.output)

    if verbose then print("    " .. cmd) end

    local r = spkg.run_cmd(cmd)
    if not r then return false end
    if not r.ok then
        print("    link error:\n" .. (r.out or ""))
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
            local cache_key = cache_key_for(task)
            if spkg.cache_get(cache_key, task.output) then
                if verbose then print("  [cache hit] " .. task.source) end
                save_fingerprint(task.output, task.cflags, task.target)
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

    local function drain_running()
        while #running > 0 do
            local still_running = {}
            for _, r in ipairs(running) do
                local result = spkg.wait_task(r.id)
                if not result then
                    table.insert(still_running, r)
                else
                    if result.ok then
                        save_fingerprint(r.item.task.output, r.item.task.cflags, r.item.task.target)
                        spkg.cache_put(cache_key_for(r.item.task), r.item.task.output)
                    end
                end
            end
            running = still_running
        end
    end

    if not start_next() then
        drain_running()
        return false
    end

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
                if result.ok then
                    save_fingerprint(r.item.task.output, r.item.task.cflags, r.item.task.target)
                    if not _SPKG_NO_CACHE then
                        spkg.cache_put(cache_key_for(r.item.task), r.item.task.output)
                    end
                end
            else
                table.insert(new_running, r)
            end
        end
        running = new_running
        if #running < max_jobs and #pending > 0 and not any_failed then
            if not start_next() then
                drain_running()
                return false
            end
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
-- Uses compiler-driven dependency scanning instead of Lua regex:
--   .ce/.c/.i  → sharpc -E -MMD -MF <tmp>.d  → parse depfile
--   .cpp/.m/.mm/.cxx/.cc → zig cc -MM         → parse stdout
-- ═══════════════════════════════════════════════════════════════

local CPP_EXTS = { cpp = true, cxx = true, cc = true, ["c++"] = true, m = true, mm = true }

-- Check if a path is a system header (should NOT be bundled to nodes)
local function is_system_header(path)
    if path:match("^/usr/include/") then return true end
    if path:match("^/usr/lib/") then return true end
    if path:match("^/System/Library/") then return true end
    return false
end

-- Parse depfile (.d) output from -MMD into a list of absolute paths
local function parse_depfile_output(raw, cflags)
    local headers = {}
    -- target.o: path1.h path2.h \n path3.h ...
    raw = raw:gsub("\\\n", " "):gsub("\n", " ")
    local colon = raw:find(":")
    if not colon then return headers end
    local rest = raw:sub(colon + 1)
    for p in rest:gmatch("%S+") do
        if not is_system_header(p) and spkg.file_exists(p) then
            headers[p] = true
        end
    end
    return headers
end

local function find_zig()
    local compiler = find_compiler()
    if not compiler then return nil end
    local exe_dir = compiler:match("(.*/)")
    if not exe_dir then return nil end
    local zig = exe_dir .. "zig"
    if spkg.file_exists(zig) then return zig end
    zig = exe_dir .. "../zig/zig"
    if spkg.file_exists(zig) then return zig end
    return nil
end

-- Scan header dependencies using compiler tooling.
-- For C++/ObjC sources: uses zig cc -MM (compiler-driven).
-- For Sharp/C sources: uses a fast regex-based fallback (sharpc -E
-- does not generate depfiles in current version).
-- Returns set { path → true } or nil on failure.
local function scan_deps_tool(source_path, cflags, target, sysroot)
    local ext = source_path:match("%.([^%.\\/]+)$")
    if not ext then return nil end

    if CPP_EXTS[ext] then
        local zig = find_zig()
        if not zig then return nil end
        local args = { zig, "cc", "-MM", "-MG" }
        if target and target ~= "" then
            table.insert(args, "-target")
            table.insert(args, target)
        end
        if sysroot and sysroot ~= "" then
            table.insert(args, "--sysroot")
            table.insert(args, sysroot)
        end
        for _, f in ipairs(cflags) do table.insert(args, f) end
        table.insert(args, source_path)
        local cmd = table.concat(args, " ")
        local r = spkg.run_cmd(cmd .. " 2>/dev/null")
        if not r or not r.ok or not r.out then return nil end
        return parse_depfile_output(r.out, cflags)
    end

    return nil
end

-- Collect all header files (read contents into map) for bundling to nodes.
-- Uses compiler-driven dependency scanning, falls back to regex.
local function collect_headers_for_task(task)
    local source = spkg.read_file(task.source)
    if not source then return nil end

    local sysroot = _SPKG_SYSROOT
    local target = task.target

    local header_paths = scan_deps_tool(task.source, task.cflags, target, sysroot)
    if not header_paths then
        local include_dirs = extract_all_includes(task.cflags)
        local source_dir = task.source:match("(.*/)") or "."
        local header_map = {}
        local function collect_regex(src, depth)
            if depth > 10 then return end
            for line in src:gmatch("[^\n]*") do
                local inc = line:match('#%s*include%s+"([^"]+)"')
                         or line:match('#%s*include%s+<([^>]+)>')
                if inc and not header_map[inc] then
                    for _, dir in ipairs(include_dirs) do
                        local full = dir .. "/" .. inc
                        if spkg.file_exists(full) then
                            local content = spkg.read_file(full)
                            if content then
                                header_map[inc] = content
                                collect_regex(content, depth + 1)
                            end
                            break
                        end
                    end
                    if not header_map[inc] and source_dir ~= "" then
                        local full = source_dir .. "/" .. inc
                        if spkg.file_exists(full) then
                            local content = spkg.read_file(full)
                            if content then
                                header_map[inc] = content
                                collect_regex(content, depth + 1)
                            end
                        end
                    end
                end
            end
        end
        collect_regex(source, 0)
        return header_map
    end

    -- Convert path set to path→content map
    local header_map = {}
    for p in pairs(header_paths) do
        if not is_system_header(p) then
            local content = spkg.read_file(p)
            if content then
                local rel = p
                for _, dir in ipairs(extract_all_includes(task.cflags)) do
                    if dir ~= "" and p:sub(1, #dir + 1) == dir .. "/" then
                        rel = p:sub(#dir + 2)
                        break
                    end
                end
                header_map[rel] = content
            end
        end
    end
    return header_map
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Fetching
-- ═══════════════════════════════════════════════════════════════

local function fetch_deps()
    if not spkg_fetch then
        local ok, mod = pcall(dofile, "spkg_fetch.lua")
        if not ok then
            print("spkg: warning: cannot load spkg_fetch.lua: " .. tostring(mod))
            return true
        end
        spkg_fetch = mod
    end
    if spkg_fetch.fetch_recursive then
        local home = _SPKG_HOME or "/root"
        local deps = b and b._deps or {}
        if not spkg_fetch.fetch_recursive(home, deps) then return false end
    end
    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Dependency Config Loading
-- ═══════════════════════════════════════════════════════════════
--
-- After dependencies are fetched into spkg_packages/<name>/,
-- this function recursively loads each dep's config.spkg using
-- the SAME build context (b). This ensures that:
--   1. All artifacts from all packages share the same registry
--   2. ctx:dependency(name) can find artifacts across packages
--   3. link_artifact("dep_artifact_name") resolves correctly
--   4. collect_transitive_deps can build the full dependency graph

local function load_dep_configs(b, visited)
    visited = visited or {}

    local deps = b and b._deps or {}
    if #deps == 0 then return true end

    for _, dep in ipairs(deps) do
        local name = dep.name
        if visited[name] then goto continue end
        visited[name] = true

        local dep_dir = "spkg_packages/" .. name
        local config_path = dep_dir .. "/config.spkg"
        if not spkg.file_exists(config_path) then
            if _SPKG_VERBOSE then
                print("spkg: warning: no config.spkg in dependency '" .. name .. "'")
            end
            goto continue
        end

        -- Save original methods so we can restore them after loading
        local orig_add_executable = b.add_executable
        local orig_add_static_library = b.add_static_library
        local orig_add_shared_library = b.add_shared_library
        local orig_dep = b.dep

        -- Helper: wrap an artifact's add_source/add_include to prefix relative paths
        local function wrap_artifact_methods(art, prefix)
            local orig_art_add_source = art.add_source
            local orig_art_add_include = art.add_include
            local orig_art_add_include_private = art.add_include_private

            art.add_source = function(self, spec)
                if type(spec) == "string" then
                    if not spec:match("^/") and not spec:match("^%.%./") and not spec:match("^spkg_packages/") then
                        spec = prefix .. "/" .. spec
                    end
                    return orig_art_add_source(self, spec)
                elseif type(spec) == "table" then
                    local f = spec.file
                    if f and not f:match("^/") and not f:match("^%.%./") and not f:match("^spkg_packages/") then
                        spec.file = prefix .. "/" .. f
                    end
                    return orig_art_add_source(self, spec)
                end
                return orig_art_add_source(self, spec)
            end

            art.add_include = function(self, inc)
                if inc and not inc:match("^/") and not inc:match("^%.%./") and not inc:match("^spkg_packages/") then
                    inc = prefix .. "/" .. inc
                end
                return orig_art_add_include(self, inc)
            end

            art.add_include_private = function(self, inc)
                if inc and not inc:match("^/") and not inc:match("^%.%./") and not inc:match("^spkg_packages/") then
                    inc = prefix .. "/" .. inc
                end
                return orig_art_add_include_private(self, inc)
            end

            return art
        end

        -- Wrap add_* methods to return wrapped artifacts
        b.add_executable = function(self, opts)
            local art = orig_add_executable(self, opts)
            return wrap_artifact_methods(art, dep_dir)
        end

        b.add_static_library = function(self, opts)
            local art = orig_add_static_library(self, opts)
            return wrap_artifact_methods(art, dep_dir)
        end

        b.add_shared_library = function(self, opts)
            local art = orig_add_shared_library(self, opts)
            return wrap_artifact_methods(art, dep_dir)
        end

        -- Load the dependency's config.spkg with the same b context.
        -- We do NOT chdir; instead, paths are prefixed with dep_dir.
        local abs_config = spkg.cwd() .. "/" .. config_path
        local ok, err = pcall(dofile, abs_config)

        -- Restore original methods
        b.add_executable = orig_add_executable
        b.add_static_library = orig_add_static_library
        b.add_shared_library = orig_add_shared_library
        b.dep = orig_dep

        if not ok then
            print("spkg: error: failed to load dependency '" .. name .. "' config.spkg")
            print("  " .. tostring(err))
            return false
        end

        if _SPKG_VERBOSE then
            print("spkg: loaded dependency config: " .. name)
        end

        -- Recursively load transitive dependencies of this dep
        if not load_dep_configs(b, visited) then
            return false
        end

        ::continue::
    end

    return true
end

-- ═══════════════════════════════════════════════════════════════
-- Parallel Distributed Compilation via curl
-- ═══════════════════════════════════════════════════════════════

local _curl_avail = nil
local function curl_available()
    if _curl_avail == nil then
        local r = spkg.run_cmd("curl --version 2>/dev/null")
        _curl_avail = (r and r.ok)
    end
    return _curl_avail
end

local _curl_tmp_id = 0
local function _curl_tmp_name(prefix)
    _curl_tmp_id = _curl_tmp_id + 1
    return "/tmp/spkg_" .. prefix .. "_" .. tostring(_curl_tmp_id)
end

local function _build_compile_json(source, cflags, headers, opt, src_ext)
    local cflags_json = "["
    for i, f in ipairs(cflags) do
        if i > 1 then cflags_json = cflags_json .. "," end
        local fe = f:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
        cflags_json = cflags_json .. '"' .. fe .. '"'
    end
    cflags_json = cflags_json .. "]"

    local escaped = source:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')

    local headers_json = ""
    local first = true
    for hpath, hcontent in pairs(headers) do
        if not first then headers_json = headers_json .. "," end
        first = false
        local he = hcontent:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
        headers_json = headers_json .. string.format('{"path":"%s","content":"%s"}', hpath, he)
    end

    return string.format(
        '{"source":"%s","cflags":%s,"optimize":"%s","headers":[%s],"src_ext":"%s"}',
        escaped, cflags_json, opt, headers_json, src_ext)
end

local function _distribute_parallel(task_list, healthy_nodes, verbose)
    local NH = #healthy_nodes
    local results = {}
    local pending = {}
    local tmp_req_files = {}
    local tmp_out_files = {}

    -- Phase 1: submit all tasks
    for ti, entry in ipairs(task_list) do
        local node = healthy_nodes[((ti - 1) % NH) + 1]
        local req_json = _build_compile_json(entry.source, entry.cflags, entry.headers or {}, entry.opt, entry.src_ext)

        local req_file = _curl_tmp_name("req")
        local out_file = _curl_tmp_name("resp")
        tmp_req_files[req_file] = true
        tmp_out_files[out_file] = true
        spkg.write_file(req_file, req_json)

        local url = "http://" .. node .. "/compile"
        local cmd = string.format(
            'curl -s -X POST --data-binary @%s -o %s --max-time 300 %s 2>/dev/null',
            req_file, out_file, url)

        if verbose then
            print("  [remote] " .. entry.source_path .. " -> " .. node)
        else
            print("  [remote] " .. entry.source_path)
        end

        local h = spkg.start_cmd(cmd)
        table.insert(pending, {
            handle = h,
            task_idx = ti,
            node = node,
            out_file = out_file,
            attempt = 1,
        })
    end

    -- Phase 2: wait for all, retry on failure
    while #pending > 0 do
        local still_pending = {}
        for _, p in ipairs(pending) do
            local r = spkg.wait_task(p.handle)
            if r == nil then
                table.insert(still_pending, p)
            else
                -- Task completed, parse result
                local ok = false
                local output_b64 = nil
                local depfile = nil
                local err_msg = nil

                if r.ok then
                    local result_raw = spkg.read_file(p.out_file)
                    if result_raw then
                        local ok_json, resp = pcall(spkg.json_parse, result_raw)
                        if ok_json and type(resp) == "table" then
                            if resp.status == "ok" and resp.output then
                                output_b64 = resp.output
                                depfile = resp.depfile
                                ok = true
                            else
                                err_msg = (resp.stderr or "unknown error"):gsub("\n", " "):sub(1, 120)
                            end
                        else
                            err_msg = "unparsable response from " .. p.node
                        end
                    else
                        err_msg = "cannot read response from " .. p.node
                    end
                else
                    err_msg = "request failed (code " .. tostring(r.code) .. ") to " .. p.node
                end

                if ok then
                     results[p.task_idx] = { ok = true, output = output_b64, depfile = depfile }
                 elseif p.attempt < NH then
                     if p.attempt > 1 then
                         local delay_ms = math.min(100 * (2 ^ (p.attempt - 2)), 5000)
                         spkg.run_cmd("sleep " .. (delay_ms / 1000))
                     end

                     local next_node = healthy_nodes[((p.task_idx - 1 + p.attempt) % NH) + 1]
                     local entry = task_list[p.task_idx]
                     local req_json = _build_compile_json(entry.source, entry.cflags, entry.headers or {}, entry.opt, entry.src_ext)

                     local retry_req = _curl_tmp_name("req_retry")
                     local retry_out = _curl_tmp_name("resp_retry")
                     tmp_req_files[retry_req] = true
                     tmp_out_files[retry_out] = true
                     spkg.write_file(retry_req, req_json)
                     p.out_file = retry_out

                     local url = "http://" .. next_node .. "/compile"
                     local cmd = string.format(
                         'curl -s -X POST --data-binary @%s -o %s --max-time 300 %s 2>/dev/null',
                         retry_req, retry_out, url)

                    if verbose then
                        print("    [retry] " .. entry.source_path .. " -> " .. next_node .. " (" .. (err_msg or "failed") .. ")")
                    end

                    p.handle = spkg.start_cmd(cmd)
                    p.node = next_node
                    p.attempt = p.attempt + 1
                    table.insert(still_pending, p)
                else
                    results[p.task_idx] = { ok = false, error = err_msg or "all nodes failed" }
                end
            end
        end
        pending = still_pending
        if #pending > 0 then
            spkg.run_cmd("sleep 0.1")
        end
    end

    -- Cleanup temp files
    for f in pairs(tmp_req_files) do
        spkg.remove(f)
    end
    for f in pairs(tmp_out_files) do
        spkg.remove(f)
    end

    return results
end

function M.execute_distributed(verbose, max_jobs)
    local nodes = parse_nodes()
    if #nodes == 0 then
        print("spkg: no distributed nodes configured.")
        print("  Set SPKG_NODES=node1:port,node2:port or create spkg_nodes.json")
        print("  Falling back to local build.")
        return M._do_build(verbose, max_jobs)
    end

    -- 1. Create build context and execute config.spkg
    b = create_build_context()
    local ok, err = pcall(dofile, "config.spkg")
    if not ok then
        print("spkg: error: failed to execute config.spkg")
        print("  " .. tostring(err))
        return false
    end

    -- 2. Fetch dependencies (reads from b._deps)
    if not fetch_deps() then return false end

    -- 2.5. Load dependency configs into the same build context
    if not load_dep_configs(b) then return false end

    -- 3. Build the graph
    M.build_graph_from_ctx(b)

    -- 2.5. Custom steps (always local)
    if #b._custom_steps > 0 then
        if verbose then print("spkg: executing custom steps...") end
        if not execute_custom_steps(b._custom_steps, verbose) then return false end
    end

    -- Health check all nodes, filter out unhealthy ones
    local healthy = {}
    for _, n in ipairs(nodes) do
        local url = "http://" .. n .. "/health"
        local r = spkg.http_get(url)
        if r.ok and r.code == 200 then
            table.insert(healthy, n)
            if verbose then print("  node " .. n .. ": healthy") end
        else
            print("  " .. warn_msg("node " .. n .. " is not responding, skipping"))
        end
    end

    if #healthy == 0 then
        print("spkg: no healthy nodes available, falling back to local build.")
        return M._do_build(verbose, max_jobs)
    end

    print("spkg: distributed build with " .. #healthy .. "/" .. #nodes .. " healthy node(s)")
    for _, n in ipairs(healthy) do print("  node: " .. n) end

    local use_parallel = curl_available()
    if use_parallel and verbose then
        print("spkg: parallel submission via curl")
    end

    -- Phase A: collect all pending tasks across artifacts
    local all_tasks = {}
    for _, art in ipairs(build_graph.artifacts) do
        for _, task in ipairs(art.compile_tasks) do
            if needs_compile(task.source, task.output, task.cflags, task.target) then
                spkg.mkdir_p("build/" .. art.name)
                local source = spkg.read_file(task.source)
                if not source then
                    print("  error: cannot read " .. task.source)
                    return false
                end
                local headers = collect_headers_for_task(task)
                table.insert(all_tasks, {
                    source = source,
                    cflags = task.cflags,
                    headers = headers,
                    opt = _SPKG_OPTIMIZE or "Debug",
                    src_ext = task.src_ext or "ce",
                    source_path = task.source,
                    output = task.output,
                    artifact_name = art.name,
                })
            else
                if verbose then print("  [skip] " .. task.source .. " (up to date)") end
            end
        end
    end

    if #all_tasks == 0 then
        -- All up-to-date, just do link steps
        for _, art in ipairs(build_graph.artifacts) do
            if not link_artifact(art, verbose) then return false end
        end
        print("spkg: done (all up-to-date).")
        return true
    end

    -- Phase B: execute tasks (parallel or sequential)
    if use_parallel then
        -- Parallel submission via curl
        local results = _distribute_parallel(all_tasks, healthy, verbose)
        for ti, res in pairs(results) do
            if res.ok then
                local task = all_tasks[ti]
                local decoded = b64_decode(res.output)
                spkg.write_file(task.output, decoded)
                save_fingerprint(task.output, task.cflags, task.target)
                if res.depfile and res.depfile ~= "" then
                    local dep_path = task.output:sub(1, -3) .. ".d"
                    spkg.write_file(dep_path, res.depfile)
                end
            else
                print("  error: " .. (res.error or "unknown"))
                return false
            end
        end
    else
        -- Sequential fallback: round-robin submission
        local node_idx = 0
        local node_failures = {}
        for _, n in ipairs(healthy) do node_failures[n] = 0 end

        for _, task in ipairs(all_tasks) do
            local task_ok = false
            local last_err = ""
            local tried = {}
            for attempt = 1, #healthy do
                if attempt > 1 then
                    local delay_ms = math.min(100 * (2 ^ (attempt - 2)), 5000)
                    spkg.run_cmd("sleep " .. (delay_ms / 1000))
                end

                local try_idx = nil
                for offset = 0, #healthy - 1 do
                    local candidate = ((node_idx + offset) % #healthy) + 1
                    local n = healthy[candidate]
                    if not tried[n] then
                        try_idx = candidate
                        tried[n] = true
                        break
                    end
                end
                if not try_idx then break end

                local node = healthy[try_idx]
                local req = _build_compile_json(task.source, task.cflags, task.headers or {}, task.opt, task.src_ext)

                if verbose then
                    local tag = (attempt == 1) and "remote" or "retry"
                    print("  [" .. tag .. "] " .. task.source_path .. " -> " .. node)
                else
                    print("  [remote] " .. task.source_path)
                end

                local url = "http://" .. node .. "/compile"
                local r = spkg.http_post(url, req)

                if not r.ok or r.code ~= 200 then
                    node_failures[node] = (node_failures[node] or 0) + 1
                    last_err = (r.body or ""):gsub("\n", " "):sub(1, 120)
                    if r.code == 503 then last_err = "node busy"
                    elseif r.code <= 0 then last_err = "connection failed" end
                    if verbose and attempt < #healthy then
                        print("    " .. node .. ": " .. last_err .. " (trying next node)")
                    end
                    if attempt >= #healthy then
                        print("  error: all nodes failed for " .. task.source_path)
                        print("  last error (" .. node .. "): " .. last_err)
                        return false
                    end
                else
                    local ok_json, resp = pcall(spkg.json_parse, r.body)
                    if not ok_json or type(resp) ~= "table" then
                        last_err = "unparsable response from " .. node
                        if attempt >= #healthy then
                            print("  error: " .. last_err)
                            return false
                        end
                    elseif resp.status == "error" then
                        last_err = (resp.stderr or "unknown error"):gsub("\n", " "):sub(1, 120)
                        if attempt >= #healthy then
                            print("  error (" .. node .. "): " .. last_err)
                            return false
                        end
                    else
                        if resp.output then
                            local decoded = b64_decode(resp.output)
                            spkg.write_file(task.output, decoded)
                            save_fingerprint(task.output, task.cflags, task.target)
                            if resp.depfile and resp.depfile ~= "" then
                                local dep_path = task.output:sub(1, -3) .. ".d"
                                spkg.write_file(dep_path, resp.depfile)
                            end
                            task_ok = true
                            node_idx = try_idx
                            break
                        else
                            last_err = "no output from " .. node
                            if attempt >= #healthy then
                                print("  error: " .. last_err)
                                return false
                            end
                        end
                    end
                end
            end
            if not task_ok then return false end
        end
    end

    -- Phase C: link steps (always local)
    for _, art in ipairs(build_graph.artifacts) do
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
            if not r then return false end
            if not r.ok then
                print("    error:\n" .. (r.out or ""))
                return false
            end
            if verbose and r.out and r.out ~= "" then print("    " .. r.out) end
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

    -- 2. Fetch dependencies (reads from b._deps)
    if not fetch_deps() then return false end

    -- 2.5. Load dependency configs into the same build context
    if not load_dep_configs(b) then return false end

    -- 3. Build the graph (DAG sorted)
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
            if needs_compile(task.source, task.output, task.cflags, task.target) then
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
            if not r then return false end
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
    -- 1. Create build context and execute config.spkg
    b = create_build_context()
    local ok, err = pcall(dofile, "config.spkg")
    if not ok then
        print(error_msg("failed to execute config.spkg"))
        print("  " .. tostring(err))
        return false
    end

    -- 2. Fetch dependencies (reads from b._deps)
    if not fetch_deps() then return false end

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
            if needs_compile(task.source, task.output, task.cflags, task.target) then
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
        if not r then return false end
        if r.out ~= "" then print(r.out) end
        if not r.ok then
            print(error_msg("test " .. art.name .. " failed (exit code " .. tostring(r.code) .. ")"))
            return false
        end
        print(COLOR("  [pass] " .. art.name, "bold_green"))

        ::continue::
    end

    print(COLOR("spkg: all tests passed.", "bold_green"))
    return true
end

return M
