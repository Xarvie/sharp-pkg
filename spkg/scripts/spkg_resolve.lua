-- spkg_resolve.lua — dependency resolution

local M = {}

function M.resolve(name, version, home)
    -- Check Sharp.lock first
    if spkg.file_exists("Sharp.lock") then
        local ok, lock = pcall(dofile, "Sharp.lock")
        if ok and lock and lock[name] then
            return lock[name]
        end
    end

    -- Read source config (~/.sharp/config.spkm)
    local config_file = home .. "/.sharp/config.spkm"
    local config = nil
    if spkg.file_exists(config_file) then
        local ok, cfg = pcall(dofile, config_file)
        if ok then config = cfg end
    end

    if config and config.source and config.source[name] then
        return {
            name = name,
            version = version,
            url = config.source[name],
            tag = version ~= "*" and ("v" .. version) or nil
        }
    end

    local default_url = "https://gitee.com/sharp-libs/{name}.git"
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