/*
 * main.c — spkg CLI entry point.
 *
 * Embeds Lua 5.4, loads build scripts (embedded as C arrays at build time),
 * dispatches commands: init, build, run, add, clean, help.
 *
 * Supports:
 *   spkg build --target <triple>    Cross-compile target
 *   spkg build --optimize <level>   Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
 *   spkg build --verbose            Detailed output
 *   spkg build --all                Build all targets
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "spkg.h"

/* ── embedded Lua scripts (generated at build time by embed_lua.cmake) ── */
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

/* ── parse CLI flags ─────────────────────────────────────────────── */
typedef struct {
    const char *cmd;
    const char *target;       /* --target <triple>       */
    const char *optimize;     /* --optimize <level>      */
    int         verbose;      /* --verbose               */
    int         all_targets;  /* --all                   */
    const char *extra_args[64];
    int         extra_count;
} cli_args_t;

static void parse_cli(int argc, char **argv, cli_args_t *out) {
    memset(out, 0, sizeof(*out));
    out->optimize = "Debug";

    int first_pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            out->target = argv[++i];
        } else if (strcmp(argv[i], "--optimize") == 0 && i + 1 < argc) {
            out->optimize = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            out->verbose = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            out->all_targets = 1;
        } else if (argv[i][0] != '-') {
            if (!first_pos) {
                out->cmd = argv[i];
                first_pos = 1;
            } else {
                out->extra_args[out->extra_count++] = argv[i];
            }
        }
    }
    if (!out->cmd) out->cmd = "build";
}

/* ── main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    cli_args_t cli;
    parse_cli(argc, argv, &cli);

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

    lua_pushstring(L, cli.cmd);
    lua_pushstring(L, home);
    lua_pushstring(L, cli.target   ? cli.target   : "");
    lua_pushstring(L, cli.optimize ? cli.optimize : "Debug");
    lua_pushboolean(L, cli.verbose);
    lua_pushboolean(L, cli.all_targets);

    /* Push extra args as a table */
    lua_newtable(L);
    for (int i = 0; i < cli.extra_count; i++) {
        lua_pushstring(L, cli.extra_args[i]);
        lua_rawseti(L, -2, i + 1);
    }

    int rc = lua_pcall(L, 7, 1, 0);
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
