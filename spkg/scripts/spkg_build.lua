-- spkg_build.lua — build execution: .sp → .o → exe/lib
-- sharpc handles .sp compilation and linking (uses zig cc internally).

local M = {}

function M.execute(manifest)
    local name = manifest.name or "project"
    local ttype = manifest.type or "exe"
    local src_patterns = manifest.src or {}
    local link_libs = manifest.link or {}
    local include_dirs = manifest.include or {"."}

    local sharpc = spkg.find_sharpc()
    if not sharpc then
        print("spkg: sharpc not found. Install it or set SHARPC env var.")
        return false
    end

    -- Fetch dependencies first
    local home = spkg.home_dir()
    if not spkg_fetch.fetch_deps(manifest, home) then
        return false
    end

    print(string.format("spkg: building %s [%s]...", name, ttype))
    spkg.mkdir_p("build/c")

    -- Collect .sp source files
    local sp_files = {}
    for _, p in ipairs(src_patterns) do
        for _, f in ipairs(spkg.glob(p)) do
            table.insert(sp_files, f)
        end
    end
    if #sp_files == 0 then
        local tmp = spkg.glob("src/*.sp")
        for _, f in ipairs(tmp) do table.insert(sp_files, f) end
    end
    if #sp_files == 0 then
        print("spkg: error: no .sp source files found")
        return false
    end

    -- Include flags
    local inc = ""
    for _, d in ipairs(include_dirs) do
        inc = inc .. " -I" .. d
    end

    -- Compile each .sp → .o
    local o_files = {}
    for _, sp in ipairs(sp_files) do
        local stem = sp:gsub("%.sp$", ""):gsub("/", "_")
        local of = "build/c/" .. stem .. ".o"

        print("  [sp] " .. sp)
        local r = spkg.run_cmd(string.format('%s -c %s "%s" -o "%s"',
                                              sharpc, inc, sp, of))
        if not r.ok then
            print("    error:\n" .. r.out)
            return false
        end
        table.insert(o_files, of)
    end

    -- Link
    local objs = table.concat(o_files, " ")
    local lflags = ""
    for _, l in ipairs(link_libs) do lflags = lflags .. " -l" .. l end

    if ttype == "staticlib" then
        local out = "build/lib" .. name .. ".a"
        print("  [ar] " .. out)
        local r = spkg.run_cmd(string.format('zig ar rcs "%s" %s', out, objs))
        if not r.ok then print("    error:\n" .. r.out); return false end
    else
        local out = "build/" .. name
        print("  [link] " .. out)
        local r = spkg.run_cmd(string.format('zig cc %s %s -o "%s"',
                                              objs, lflags, out))
        if not r.ok then print("    error:\n" .. r.out); return false end
    end

    print("spkg: done.")
    return true
end

return M