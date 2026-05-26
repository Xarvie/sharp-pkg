/*
 * native.c — C native functions exposed to Lua via the "spkg" module.
 *
 * spkg.run_cmd(cmd)         → {ok, out, code}
 * spkg.file_exists(path)    → bool
 * spkg.dir_exists(path)     → bool
 * spkg.mkdir_p(path)        → bool
 * spkg.glob(pattern)        → {file1, file2, ...}
 * spkg.read_file(path)      → string or nil
 * spkg.write_file(path, s)  → bool
 * spkg.find_sharpc()        → string or nil
 * spkg.find_zigcc()         → string or nil
 * spkg.home_dir()           → string
 * spkg.cwd()                → string
 * spkg.get_mtime(path)      → number or nil
 * spkg.current_platform()   → string
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <glob.h>
#include <errno.h>
#include <limits.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* ── helpers ──────────────────────────────────────────────────────── */

/* Test whether a path is an executable file */
static int is_executable(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Resolve own exe path into buf (Linux: /proc/self/exe, macOS: _NSGetExecutablePath) */
static const char *resolve_self_exe(char *buf, size_t bufsize) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, bufsize - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return buf;
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    uint32_t size = (uint32_t)bufsize;
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    /* Resolve symlinks */
    char *real = realpath(buf, buf);
    return real ? real : buf;
#else
    (void)buf; (void)bufsize;
    return NULL;
#endif
}

/* Try to find zig relative to self-exe, following sharp's layout:
 *   {bin}/zig          (zig next to sharpc/spkg)
 *   {bin}/../zig/zig   (zig in sibling directory)
 * Returns static buffer or NULL. */
static const char *find_zig_near_exe(void) {
    static char zig_path[PATH_MAX];
    char self_exe[PATH_MAX];
    if (!resolve_self_exe(self_exe, sizeof(self_exe))) return NULL;

    char *slash = strrchr(self_exe, '/');
    if (!slash) return NULL;
    *slash = '\0';  /* self_dir */
    size_t dirlen = strlen(self_exe);

    /* Priority 1a: {self_dir}/zig */
    if (dirlen + 5 < sizeof(zig_path)) {
        memcpy(zig_path, self_exe, dirlen);
        memcpy(zig_path + dirlen, "/zig", 5);
        if (is_executable(zig_path)) return zig_path;
    }

    /* Priority 1b: {self_dir}/../zig/zig */
    char *parent_sep = strrchr(self_exe, '/');
    if (parent_sep) {
        size_t pdirlen = (size_t)(parent_sep - self_exe);
        if (pdirlen + 9 < sizeof(zig_path)) {
            memcpy(zig_path, self_exe, pdirlen);
            memcpy(zig_path + pdirlen, "/zig/zig", 9);
            if (is_executable(zig_path)) return zig_path;
        }
    }

    return NULL;
}

/* ── spkg.run_cmd ────────────────────────────────────────────────── */
static int n_run_cmd(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "popen failed"); lua_setfield(L, -2, "out");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        return 1;
    }

    char buf[4096];
    size_t total = 0;
    char *out = malloc(1);
    out[0] = '\0';

    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        char *tmp = realloc(out, total + n + 1);
        if (!tmp) { free(out); break; }
        out = tmp;
        memcpy(out + total, buf, n);
        total += n;
        out[total] = '\0';
    }

    int status = pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int ok = (code == 0);

    lua_newtable(L);
    lua_pushboolean(L, ok);   lua_setfield(L, -2, "ok");
    lua_pushstring(L, out);   lua_setfield(L, -2, "out");
    lua_pushinteger(L, code); lua_setfield(L, -2, "code");

    free(out);
    return 1;
}

/* ── spkg.file_exists ────────────────────────────────────────────── */
static int n_file_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0 && S_ISREG(st.st_mode));
    return 1;
}

/* ── spkg.dir_exists ─────────────────────────────────────────────── */
static int n_dir_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    return 1;
}

/* ── spkg.mkdir_p ────────────────────────────────────────────────── */
static int n_mkdir_p(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        lua_pushboolean(L, 1); return 1;
    }
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    int r = system(cmd);
    lua_pushboolean(L, r == 0);
    return 1;
}

/* ── spkg.glob ───────────────────────────────────────────────────── */
static int n_glob(lua_State *L) {
    const char *pattern = luaL_checkstring(L, 1);

    if (strstr(pattern, "**")) {
        char base[1024] = ".";
        const char *name = "*";
        const char *dbl_star = strstr(pattern, "**");
        if (dbl_star > pattern) {
            size_t len = (size_t)(dbl_star - pattern);
            if (len > 0 && pattern[len - 1] == '/') len--;
            if (len < sizeof(base)) {
                memcpy(base, pattern, len);
                base[len] = '\0';
            }
        }
        const char *last_slash = strrchr(pattern, '/');
        if (last_slash && last_slash > dbl_star) name = last_slash + 1;

        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
                 "find '%s' -name '%s' -type f 2>/dev/null", base, name);
        FILE *fp = popen(cmd, "r");
        lua_newtable(L);
        if (fp) {
            char line[4096];
            int idx = 1;
            while (fgets(line, sizeof(line), fp)) {
                size_t ln = strlen(line);
                if (ln > 0 && line[ln - 1] == '\n') line[ln - 1] = '\0';
                lua_pushstring(L, line);
                lua_rawseti(L, -2, idx++);
            }
            pclose(fp);
        }
        return 1;
    }

    glob_t g;
    int rc = glob(pattern, 0, NULL, &g);
    lua_newtable(L);
    if (rc == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
        { lua_pushstring(L, g.gl_pathv[i]); lua_rawseti(L, -2, (int)(i+1)); }
    }
    globfree(&g);
    return 1;
}

/* ── spkg.read_file ────────────────────────────────────────────────── */
static int n_read_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *fp = fopen(path, "r");
    if (!fp) { lua_pushnil(L); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); lua_pushnil(L); return 1; }
    fread(buf, 1, sz, fp); buf[sz] = '\0';
    fclose(fp);
    lua_pushstring(L, buf);
    free(buf);
    return 1;
}

/* ── spkg.write_file ───────────────────────────────────────────────── */
static int n_write_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *content = luaL_checkstring(L, 2);
    FILE *fp = fopen(path, "w");
    if (!fp) { lua_pushboolean(L, 0); return 1; }
    fputs(content, fp);
    fclose(fp);
    lua_pushboolean(L, 1);
    return 1;
}

/* ── spkg.find_sharpc ──────────────────────────────────────────────── */
static int n_find_sharpc(lua_State *L) {
    /* 1. SHARPC env var */
    const char *env = getenv("SHARPC");
    if (env && is_executable(env)) {
        lua_pushstring(L, env); return 1;
    }

    /* 2. Search relative to spkg binary (via self-exe path) */
    char self_exe[PATH_MAX];
    if (resolve_self_exe(self_exe, sizeof(self_exe))) {
        char *slash = strrchr(self_exe, '/');
        if (slash) *slash = '\0';
        char cand[PATH_MAX];
        const char *rel[] = {
            "../../../build/sharpc",
            "../../build/sharpc",
            "../build/sharpc",
            "../sharpc",
            NULL
        };
        for (int i = 0; rel[i]; i++) {
            snprintf(cand, sizeof(cand), "%s/%s", self_exe, rel[i]);
            if (is_executable(cand)) {
                lua_pushstring(L, cand); return 1;
            }
        }
    }

    lua_pushnil(L);
    return 1;
}

/* ── spkg.find_zigcc ───────────────────────────────────────────────── */
static int n_find_zigcc(lua_State *L) {
    /* 1. ZIGCC env var */
    const char *env = getenv("ZIGCC");
    if (env && is_executable(env)) {
        lua_pushstring(L, env); return 1;
    }

    /* 2. zig next to spkg binary (sharp's bundling layout) */
    const char *near = find_zig_near_exe();
    if (near) {
        lua_pushstring(L, near); return 1;
    }

    /* 3. zig in PATH */
    FILE *fp = popen("which zig 2>/dev/null", "r");
    if (fp) {
        char path[PATH_MAX] = "";
        if (fgets(path, sizeof(path), fp)) {
            size_t n = strlen(path);
            if (n > 0 && path[n-1] == '\n') path[n-1] = '\0';
        }
        pclose(fp);
        if (path[0] && is_executable(path)) {
            lua_pushstring(L, path); return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

/* ── spkg.home_dir ─────────────────────────────────────────────────── */
static int n_home_dir(lua_State *L) {
    const char *h = getenv("HOME");
    lua_pushstring(L, h ? h : "/root");
    return 1;
}

/* ── spkg.cwd ──────────────────────────────────────────────────────── */
static int n_cwd(lua_State *L) {
    char buf[PATH_MAX];
    lua_pushstring(L, getcwd(buf, sizeof(buf)) ? buf : ".");
    return 1;
}

/* ── spkg.get_mtime ────────────────────────────────────────────────── */
static int n_get_mtime(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(path, &st) != 0) {
        lua_pushnil(L);
        return 1;
    }
#ifdef __APPLE__
    lua_pushnumber(L, (double)st.st_mtimespec.tv_sec + (double)st.st_mtimespec.tv_nsec / 1e9);
#else
    lua_pushnumber(L, (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9);
#endif
    return 1;
}

/* ── spkg.current_platform ─────────────────────────────────────────── */
static int n_current_platform(lua_State *L) {
#if defined(__linux__) && defined(__ANDROID__)
    #if defined(__aarch64__)
        lua_pushstring(L, "aarch64-linux-android");
    #else
        lua_pushstring(L, "x86_64-linux-android");
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__)
        lua_pushstring(L, "aarch64-pc-linux-gnu");
    #elif defined(__arm__)
        lua_pushstring(L, "armv7l-pc-linux-gnueabihf");
    #else
        lua_pushstring(L, "x86_64-pc-linux-gnu");
    #endif
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE
        #if defined(__aarch64__)
            lua_pushstring(L, "arm64-apple-ios");
        #else
            lua_pushstring(L, "x86_64-apple-ios");
        #endif
    #else
        #if defined(__aarch64__)
            lua_pushstring(L, "arm64-apple-darwin");
        #else
            lua_pushstring(L, "x86_64-apple-darwin");
        #endif
    #endif
#elif defined(_WIN32)
    #if defined(_M_ARM64) || defined(__aarch64__)
        lua_pushstring(L, "arm64-pc-windows-msvc");
    #else
        lua_pushstring(L, "x86_64-pc-windows-msvc");
    #endif
#else
    lua_pushstring(L, "unknown");
#endif
    return 1;
}

/* ── register ──────────────────────────────────────────────────────── */
static const luaL_Reg spkg_lib[] = {
    {"run_cmd",         n_run_cmd},
    {"file_exists",     n_file_exists},
    {"dir_exists",      n_dir_exists},
    {"mkdir_p",         n_mkdir_p},
    {"glob",            n_glob},
    {"read_file",       n_read_file},
    {"write_file",      n_write_file},
    {"find_sharpc",     n_find_sharpc},
    {"find_zigcc",      n_find_zigcc},
    {"home_dir",        n_home_dir},
    {"cwd",             n_cwd},
    {"get_mtime",       n_get_mtime},
    {"current_platform", n_current_platform},
    {NULL, NULL}
};

void spkg_register_native(lua_State *L) {
    lua_newtable(L);
    for (int i = 0; spkg_lib[i].name; i++) {
        lua_pushcfunction(L, spkg_lib[i].func);
        lua_setfield(L, -2, spkg_lib[i].name);
    }
    lua_setglobal(L, "spkg");
}
