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
#include <errno.h>
#include <limits.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <glob.h>
    #include <limits.h>
    #define PATH_SEP '/'
    #define PATH_SEP_STR "/"
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <TargetConditionals.h>
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* ── helpers ──────────────────────────────────────────────────────── */

/* Test whether a path is an executable file */
static int is_executable(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES &&
           (GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
#endif
}

/* Resolve own exe path into buf */
static const char *resolve_self_exe(char *buf, size_t bufsize) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, bufsize - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return buf;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)bufsize;
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    /* Resolve symlinks using a separate temp buffer */
    char tmp[PATH_MAX];
    char *real = realpath(buf, tmp);
    if (real) {
        memcpy(buf, real, strlen(real) + 1);
    }
    return buf;
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)bufsize);
    if (len == 0 || len >= bufsize) return NULL;
    return buf;
#else
    (void)buf; (void)bufsize;
    return NULL;
#endif
}

/* Find path separator (handles both / and \) */
static const char *find_path_sep(const char *path) {
    const char *last = NULL;
    const char *p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p;
        p++;
    }
    return last;
}

/* Try to find zig relative to self-exe, following sharp's layout:
 *   {bin}/zig          (zig next to sharpc/spkg)
 *   {bin}/../zig/zig   (zig in sibling directory)
 * Returns static buffer or NULL. */
static const char *find_zig_near_exe(void) {
    static char zig_path[PATH_MAX];
    char self_exe[PATH_MAX];
    if (!resolve_self_exe(self_exe, sizeof(self_exe))) return NULL;

    char *slash = (char *)find_path_sep(self_exe);
    if (!slash) return NULL;
    *slash = '\0';  /* self_dir */
    size_t dirlen = strlen(self_exe);

#ifdef _WIN32
    #define EXE_EXT ".exe"
    const char *zig_name = "zig.exe";
#else
    #define EXE_EXT ""
    const char *zig_name = "zig";
#endif

    /* Priority 1a: {self_dir}/zig */
    if (dirlen + 1 + strlen(zig_name) < sizeof(zig_path)) {
        memcpy(zig_path, self_exe, dirlen);
        zig_path[dirlen] = PATH_SEP;
        strcpy(zig_path + dirlen + 1, zig_name);
        if (is_executable(zig_path)) return zig_path;
    }

    /* Priority 1b: {self_dir}/../zig/zig */
    char *parent_sep = (char *)find_path_sep(self_exe);
    if (parent_sep) {
        size_t pdirlen = (size_t)(parent_sep - self_exe);
        size_t need = pdirlen + 1 + strlen(zig_name) + 4 + 1; /* +4 for /zig/ */
        if (need < sizeof(zig_path)) {
            memcpy(zig_path, self_exe, pdirlen);
            zig_path[pdirlen] = PATH_SEP;
            strcpy(zig_path + pdirlen + 1, "zig" PATH_SEP_STR);
            strcat(zig_path, zig_name);
            if (is_executable(zig_path)) return zig_path;
        }
    }

#undef EXE_EXT
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
#ifdef _WIN32
    int code = status;
    int ok = (code == 0);
#else
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int ok = (code == 0);
#endif

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
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    lua_pushboolean(L, attr != INVALID_FILE_ATTRIBUTES &&
                       (attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0 && S_ISREG(st.st_mode));
#endif
    return 1;
}

/* ── spkg.dir_exists ─────────────────────────────────────────────── */
static int n_dir_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    lua_pushboolean(L, attr != INVALID_FILE_ATTRIBUTES &&
                       (attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
#endif
    return 1;
}

/* ── spkg.mkdir_p ────────────────────────────────────────────────── */
#ifdef _WIN32
static int win_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    /* Normalize slashes */
    for (char *p = tmp; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    /* Walk path segments and create each directory */
    char *p = tmp;
    if (p[0] && p[1] == ':') p += 2;  /* skip drive letter */
    if (*p == '\\' || *p == '/') p++;
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = saved;
        }
    }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}
#endif

static int n_mkdir_p(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
    int r = win_mkdir_p(path);
    lua_pushboolean(L, r == 0);
#else
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        lua_pushboolean(L, 1); return 1;
    }
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    int r = system(cmd);
    lua_pushboolean(L, r == 0);
#endif
    return 1;
}

/* ── spkg.glob ───────────────────────────────────────────────────── */
static int n_glob(lua_State *L) {
    const char *pattern = luaL_checkstring(L, 1);

#ifdef _WIN32
    /* Windows: use dir command via popen for ** patterns */
    char cmd[4096];
    /* Convert Lua glob pattern to Windows dir pattern */
    char win_pattern[1024];
    strncpy(win_pattern, pattern, sizeof(win_pattern) - 1);
    win_pattern[sizeof(win_pattern) - 1] = '\0';
    /* Replace / with \ */
    for (char *p = win_pattern; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    /* For ** patterns, use dir /s /b */
    if (strstr(win_pattern, "**")) {
        /* Extract base directory and filename pattern */
        char base[1024] = ".";
        const char *name = "*";
        const char *dbl_star = strstr(win_pattern, "**");
        if (dbl_star > win_pattern) {
            size_t len = (size_t)(dbl_star - win_pattern);
            if (len > 0 && (win_pattern[len - 1] == '\\' || win_pattern[len - 1] == '/')) len--;
            if (len < sizeof(base)) {
                memcpy(base, win_pattern, len);
                base[len] = '\0';
            }
        }
        const char *last_sep = find_path_sep(win_pattern);
        if (last_sep && last_sep > dbl_star) name = last_sep + 1;

        snprintf(cmd, sizeof(cmd), "dir /s /b \"%s\\%s\" 2>nul", base, name);
    } else {
        /* Simple glob: use dir /b */
        snprintf(cmd, sizeof(cmd), "dir /b \"%s\" 2>nul", win_pattern);
    }

    FILE *fp = popen(cmd, "r");
    lua_newtable(L);
    if (fp) {
        char line[4096];
        int idx = 1;
        while (fgets(line, sizeof(line), fp)) {
            size_t ln = strlen(line);
            if (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) {
                line[ln - 1] = '\0';
                if (ln > 1 && line[ln - 2] == '\r') line[ln - 2] = '\0';
            }
            /* Skip empty lines */
            if (line[0]) {
                lua_pushstring(L, line);
                lua_rawseti(L, -2, idx++);
            }
        }
        pclose(fp);
    }
    return 1;
#else
    char cmd[4096];
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
#endif
}

/* ── spkg.read_file ──────────────────────────────────────────────── */
static int n_read_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *fp = fopen(path, "rb");
    if (!fp) { lua_pushnil(L); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); lua_pushnil(L); return 1; }
    fread(buf, 1, sz, fp); buf[sz] = '\0';
    fclose(fp);
    lua_pushlstring(L, buf, sz);
    free(buf);
    return 1;
}

/* ── spkg.write_file ─────────────────────────────────────────────── */
static int n_write_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *content = lua_tolstring(L, 2, &len);
    if (!content) { lua_pushboolean(L, 0); return 1; }
    FILE *fp = fopen(path, "wb");
    if (!fp) { lua_pushboolean(L, 0); return 1; }
    fwrite(content, 1, len, fp);
    fclose(fp);
    lua_pushboolean(L, 1);
    return 1;
}

/* ── spkg.find_sharpc ────────────────────────────────────────────── */
static int n_find_sharpc(lua_State *L) {
    /* 1. SHARPC env var */
    const char *env = getenv("SHARPC");
    if (env && is_executable(env)) {
        lua_pushstring(L, env); return 1;
    }

    /* 2. Search relative to spkg binary */
    char self_exe[PATH_MAX];
    if (resolve_self_exe(self_exe, sizeof(self_exe))) {
        char *slash = (char *)find_path_sep(self_exe);
        if (slash) *slash = '\0';

        char cand[PATH_MAX];
#ifdef _WIN32
        const char *rel[] = {
            "..\\..\\..\\build\\sharpc.exe",
            "..\\..\\build\\sharpc.exe",
            "..\\build\\sharpc.exe",
            "..\\sharpc.exe",
            NULL
        };
#else
        const char *rel[] = {
            "../../../build/sharpc",
            "../../build/sharpc",
            "../build/sharpc",
            "../sharpc",
            NULL
        };
#endif
        for (int i = 0; rel[i]; i++) {
#ifdef _WIN32
            snprintf(cand, sizeof(cand), "%s\\%s", self_exe, rel[i]);
#else
            snprintf(cand, sizeof(cand), "%s/%s", self_exe, rel[i]);
#endif
            if (is_executable(cand)) {
                lua_pushstring(L, cand); return 1;
            }
        }
    }

    lua_pushnil(L);
    return 1;
}

/* ── spkg.find_zigcc ─────────────────────────────────────────────── */
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
#ifdef _WIN32
    FILE *fp = popen("where zig.exe 2>nul", "r");
#else
    FILE *fp = popen("which zig 2>/dev/null", "r");
#endif
    if (fp) {
        char path[PATH_MAX] = "";
        if (fgets(path, sizeof(path), fp)) {
            size_t n = strlen(path);
            if (n > 0 && (path[n-1] == '\n' || path[n-1] == '\r')) path[n-1] = '\0';
            if (n > 1 && path[n-2] == '\r') path[n-2] = '\0';
        }
        pclose(fp);
        if (path[0] && is_executable(path)) {
            lua_pushstring(L, path); return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

/* ── spkg.home_dir ───────────────────────────────────────────────── */
static int n_home_dir(lua_State *L) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h) h = getenv("USERPROFILE");
    if (!h) h = "C:\\Users\\Default";
#else
    if (!h) h = "/root";
#endif
    lua_pushstring(L, h);
    return 1;
}

/* ── spkg.cwd ────────────────────────────────────────────────────── */
static int n_cwd(lua_State *L) {
    char buf[PATH_MAX];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, ".");
#else
    if (getcwd(buf, sizeof(buf)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, ".");
#endif
    return 1;
}

/* ── spkg.get_mtime ──────────────────────────────────────────────── */
static int n_get_mtime(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        lua_pushnil(L);
        return 1;
    }
    /* FILETIME is 100-nanosecond intervals since 1601-01-01 */
    ULARGE_INTEGER ft;
    ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    /* Convert to Unix timestamp (seconds since 1970-01-01) */
    double seconds = (double)(ft.QuadPart - 116444736000000000ULL) / 10000000.0;
    lua_pushnumber(L, seconds);
#else
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
#endif
    return 1;
}

/* ── spkg.current_platform ───────────────────────────────────────── */
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

/* ── async command tracking (POSIX only) ─────────────────────────── */
#ifndef _WIN32
#include <sys/wait.h>

#define MAX_ASYNC_CMDS 64
static struct {
    int pid;
    int in_use;
    char out_file[256];
} async_cmds[MAX_ASYNC_CMDS];

static int find_async_slot(void) {
    for (int i = 0; i < MAX_ASYNC_CMDS; i++) {
        if (!async_cmds[i].in_use) return i;
    }
    return -1;
}

/* ── spkg.start_cmd(cmd) → task_id (number) or nil ─────────────── */
static int n_start_cmd(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    int slot = find_async_slot();
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many async commands");
        return 2;
    }

    /* Create temp file for output */
    char tmpfile[] = "/tmp/spkg_cmd_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "cannot create temp file");
        return 2;
    }
    close(fd);

    /* Build command: cmd > tmpfile 2>&1 */
    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd), "%s > '%s' 2>&1", cmd, tmpfile);

    pid_t pid = fork();
    if (pid < 0) {
        remove(tmpfile);
        lua_pushnil(L);
        lua_pushliteral(L, "fork failed");
        return 2;
    }
    if (pid == 0) {
        /* Child */
        execl("/bin/sh", "sh", "-c", full_cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    async_cmds[slot].pid = (int)pid;
    async_cmds[slot].in_use = 1;
    strncpy(async_cmds[slot].out_file, tmpfile, sizeof(async_cmds[slot].out_file) - 1);

    lua_pushinteger(L, slot + 1);  /* 1-based ID */
    return 1;
}

/* ── spkg.wait_task(task_id) → {ok, out, code} or nil ──────────── */
static int n_wait_task(lua_State *L) {
    int slot = (int)luaL_checkinteger(L, 1) - 1;  /* Convert to 0-based */
    if (slot < 0 || slot >= MAX_ASYNC_CMDS || !async_cmds[slot].in_use) {
        lua_pushnil(L);
        return 1;
    }

    int status;
    waitpid(async_cmds[slot].pid, &status, 0);
    async_cmds[slot].in_use = 0;

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* Read output file */
    FILE *fp = fopen(async_cmds[slot].out_file, "r");
    char *out = NULL;
    size_t total = 0;
    if (fp) {
        char buf[4096];
        out = malloc(1);
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
        fclose(fp);
    }

    /* Clean up temp file */
    remove(async_cmds[slot].out_file);

    lua_newtable(L);
    lua_pushboolean(L, code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out ? out : ""); lua_setfield(L, -2, "out");
    lua_pushinteger(L, code); lua_setfield(L, -2, "code");

    if (out) free(out);
    return 1;
}
#else
/* Windows stubs */
static int n_start_cmd(lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "async commands not supported on Windows");
    return 2;
}
static int n_wait_task(lua_State *L) {
    lua_pushnil(L);
    return 1;
}
#endif

/* ── register ────────────────────────────────────────────────────── */
static const luaL_Reg spkg_lib[] = {
    {"run_cmd",          n_run_cmd},
    {"file_exists",      n_file_exists},
    {"dir_exists",       n_dir_exists},
    {"mkdir_p",          n_mkdir_p},
    {"glob",             n_glob},
    {"read_file",        n_read_file},
    {"write_file",       n_write_file},
    {"find_sharpc",      n_find_sharpc},
    {"find_zigcc",       n_find_zigcc},
    {"home_dir",         n_home_dir},
    {"cwd",              n_cwd},
    {"get_mtime",        n_get_mtime},
    {"current_platform", n_current_platform},
    {"start_cmd",        n_start_cmd},
    {"wait_task",        n_wait_task},
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
