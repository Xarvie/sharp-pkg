/*
 * main.c — spkg CLI entry point.
 *
 * Embeds Lua 5.4, loads build scripts (embedded as C arrays at build time),
 * dispatches commands: init, build, run, add, update, clean, help.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "spkg.h"

// ── native bindings (declared in spkg.h, defined in native.c) ──

// ── embedded Lua scripts (generated at build time by embed_lua.cmake) ──
extern const unsigned char scripts_spkg_init_lua[];
extern const unsigned int  scripts_spkg_init_lua_len;
extern const unsigned char scripts_spkg_build_lua[];
extern const unsigned int  scripts_spkg_build_lua_len;
extern const unsigned char scripts_spkg_resolve_lua[];
extern const unsigned int  scripts_spkg_resolve_lua_len;
extern const unsigned char scripts_spkg_lock_lua[];
extern const unsigned int  scripts_spkg_lock_lua_len;
extern const unsigned char scripts_spkg_fetch_lua[];
extern const unsigned int  scripts_spkg_fetch_lua_len;

/* ── load a script from embedded C array, set as global ────────── */
static int load_embedded(lua_State *L, const char *name,
                         const unsigned char *data, unsigned int len) {
    char *src = malloc(len + 1);
    if (!src) return -1;
    memcpy(src, data, len);
    src[len] = '\0';

    int rc = luaL_loadbuffer(L, src, len, name);
    free(src);
    if (rc != LUA_OK) {
        fprintf(stderr, "spkg: error loading %s: %s\n",
                name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    rc = lua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) {
        fprintf(stderr, "spkg: error running %s: %s\n",
                name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    /* Set the returned module table as a global, e.g. spkg_build */
    /* Strip trailing ".lua" from name */
    char global_name[256];
    strncpy(global_name, name, sizeof(global_name) - 1);
    global_name[sizeof(global_name) - 1] = '\0';
    size_t nlen = strlen(global_name);
    if (nlen > 4 && strcmp(global_name + nlen - 4, ".lua") == 0)
        global_name[nlen - 4] = '\0';
    lua_setglobal(L, global_name);
    return 0;
}

/* ── main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    const char *cmd = (argc > 1) ? argv[1] : "build";
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    /* Create Lua VM */
    lua_State *L = luaL_newstate();
    if (!L) { fprintf(stderr, "spkg: cannot create Lua VM\n"); return 1; }
    luaL_openlibs(L);
    spkg_register_native(L);

    /* Load embedded scripts */
    if (load_embedded(L, "spkg_init.lua",
            scripts_spkg_init_lua, scripts_spkg_init_lua_len) < 0) goto fail;
    if (load_embedded(L, "spkg_build.lua",
            scripts_spkg_build_lua, scripts_spkg_build_lua_len) < 0) goto fail;
    if (load_embedded(L, "spkg_resolve.lua",
            scripts_spkg_resolve_lua, scripts_spkg_resolve_lua_len) < 0) goto fail;
    if (load_embedded(L, "spkg_lock.lua",
            scripts_spkg_lock_lua, scripts_spkg_lock_lua_len) < 0) goto fail;
    if (load_embedded(L, "spkg_fetch.lua",
            scripts_spkg_fetch_lua, scripts_spkg_fetch_lua_len) < 0) goto fail;

    /* Dispatch */
    lua_getglobal(L, "spkg_main");
    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "spkg: spkg_main() not found\n");
        goto fail;
    }

    lua_pushstring(L, cmd);
    lua_pushstring(L, home);
    /* Push remaining args as a table */
    lua_newtable(L);
    for (int i = 2; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }

    int rc = lua_pcall(L, 3, 1, 0);
    if (rc != LUA_OK) {
        fprintf(stderr, "spkg: %s\n", lua_tostring(L, -1));
        goto fail;
    }

    int ret = lua_toboolean(L, -1) ? 0 : 1;
    lua_close(L);
    return ret;

fail:
    lua_close(L);
    return 1;
}