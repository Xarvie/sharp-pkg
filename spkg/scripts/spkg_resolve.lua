-- spkg_resolve.lua — dependency resolution

local M = {}

local function path_join(a, b)
    if a:sub(-1) == "/" then return a .. b end
    return a .. "/" .. b
end

function M.resolve(name, version, home)
    -- Check Sharp.lock first
    local ok, lock = pcall(dofile, "Sharp.lock")
    if ok and lock and lock[name] then
        return lock[name]
    end

    -- Read source config (~/.sharp/config.spkm)
    local config_file = path_join(home, ".sharp/config.spkm")
    local config = nil
    local ok2, cfg = pcall(dofile, config_file)
    if ok2 and cfg then config = cfg end

    if config and config.source and config.source[name] then
        return {
            name = name,
            version = version,
            url = config.source[name],
            tag = version ~= "*" and ("v" .. version) or nil
        }
    end

    local default_url = "https://gitee.com/sharp-repo/{name}.git"
    if config and config.source and config.source["default"] then
        default_url = config.source["default"]
    end

    local url = default_url:gsub("{name}", name)
    return {
        name = name,
        version = version,
        url = url,
        tag = version ~= "*" and ("v" .. version) or nil
    }
end

return M