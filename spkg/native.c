/*
 * native.c — C native functions exposed to Lua via the "spkg" module.
 *
 * Filesystem:
 *   spkg.file_exists(path)    → bool
 *   spkg.dir_exists(path)     → bool
 *   spkg.mkdir_p(path)        → bool
 *   spkg.glob(pattern)        → {file1, file2, ...}
 *   spkg.read_file(path)      → string or nil
 *   spkg.write_file(path, s)  → bool
 *   spkg.get_mtime(path)      → number or nil
 *   spkg.remove(path)         → bool
 *
 * Process:
 *   spkg.run_cmd(cmd)         → {ok, out, code}
 *   spkg.start_cmd(cmd)       → task_id (number) or nil, err
 *   spkg.wait_task(task_id)   → {ok, out, code} | nil (still running)
 *   spkg.custom_exec(cmd_tbl, workdir) → {ok, out, code}
 *   spkg.custom_needs_run(inputs, outputs) → bool
 *
 * Tools:
 *   spkg.find_sharpc()        → string or nil
 *   spkg.find_zigcc()         → string or nil
 *   spkg.home_dir()           → string
 *   spkg.cwd()                → string
 *   spkg.current_platform()   → string
 *
 * Cache (Phase 2):
 *   spkg.cache_init()         → bool
 *   spkg.cache_get(key, out)  → bool
 *   spkg.cache_put(key, path) → bool
 *   spkg.cache_stats()        → {hit, miss, size, count}
 *   spkg.cache_clear()        → bool
 *   spkg.fingerprint(data)    → string (FNV-1a 64-bit hash)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

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
    #include <sys/wait.h>
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

/* Try to find zig relative to self-exe, following sharp's layout */
static const char *find_zig_near_exe(void) {
    static char zig_path[PATH_MAX];
    char self_exe[PATH_MAX];
    if (!resolve_self_exe(self_exe, sizeof(self_exe))) return NULL;

    char *slash = (char *)find_path_sep(self_exe);
    if (!slash) return NULL;
    *slash = '\0';
    size_t dirlen = strlen(self_exe);

#ifdef _WIN32
    const char *zig_name = "zig.exe";
#else
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
        size_t need = pdirlen + 1 + strlen(zig_name) + 4 + 1;
        if (need < sizeof(zig_path)) {
            memcpy(zig_path, self_exe, pdirlen);
            zig_path[pdirlen] = PATH_SEP;
            strcpy(zig_path + pdirlen + 1, "zig" PATH_SEP_STR);
            strcat(zig_path, zig_name);
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
#ifdef _WIN32
    int code = status;
#else
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    lua_newtable(L);
    lua_pushboolean(L, code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out);          lua_setfield(L, -2, "out");
    lua_pushinteger(L, code);        lua_setfield(L, -2, "code");

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
    for (char *p = tmp; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    char *p = tmp;
    if (p[0] && p[1] == ':') p += 2;
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
    char cmd[4096];
    char win_pattern[1024];
    strncpy(win_pattern, pattern, sizeof(win_pattern) - 1);
    win_pattern[sizeof(win_pattern) - 1] = '\0';
    for (char *p = win_pattern; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    if (strstr(win_pattern, "**")) {
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
        snprintf(cmd, sizeof(cmd), "dir /b \"%s\" 2>nul", win_pattern);
    }

    FILE *fp = popen(cmd, "r");
    lua_newtable(L);
    if (fp) {
        char line[4096];
        int idx = 1;
        while (fgets(line, sizeof(line), fp)) {
            size_t ln = strlen(line);
            while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) {
                line[--ln] = '\0';
            }
            if (ln > 0) {
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
    const char *env = getenv("SHARPC");
    if (env && is_executable(env)) {
        lua_pushstring(L, env); return 1;
    }

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
    const char *env = getenv("ZIGCC");
    if (env && is_executable(env)) {
        lua_pushstring(L, env); return 1;
    }

    const char *near = find_zig_near_exe();
    if (near) {
        lua_pushstring(L, near); return 1;
    }

#ifdef _WIN32
    FILE *fp = popen("where zig.exe 2>nul", "r");
#else
    FILE *fp = popen("which zig 2>/dev/null", "r");
#endif
    if (fp) {
        char path[PATH_MAX] = "";
        if (fgets(path, sizeof(path), fp)) {
            size_t n = strlen(path);
            while (n > 0 && (path[n-1] == '\n' || path[n-1] == '\r')) path[--n] = '\0';
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
    ULARGE_INTEGER ft;
    ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
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

/* ═══════════════════════════════════════════════════════════════════
 * Async command execution — cross-platform (POSIX + Windows)
 * ═══════════════════════════════════════════════════════════════════ */

#define MAX_ASYNC_CMDS 64

#ifdef _WIN32
/* Windows: store process handle and temp file path */
static struct {
    HANDLE hProcess;
    int    in_use;
    char   out_file[MAX_PATH];
} win_async_cmds[MAX_ASYNC_CMDS];

static int find_async_slot(void) {
    for (int i = 0; i < MAX_ASYNC_CMDS; i++) {
        if (!win_async_cmds[i].in_use) return i;
    }
    return -1;
}

/* ── spkg.start_cmd(cmd) → task_id (number) or nil, err ────────── */
static int n_start_cmd(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    int slot = find_async_slot();
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many async commands");
        return 2;
    }

    /* Create temp file for output */
    char tmpfile[MAX_PATH];
    GetTempPathA(sizeof(tmpfile), tmpfile);
    char tmpname[MAX_PATH];
    GetTempFileNameA(tmpfile, "spk", 0, tmpname);

    /* Build command: cmd.exe /c "command > tmpfile 2>&1" */
    char full_cmd[8192];
    snprintf(full_cmd, sizeof(full_cmd),
             "cmd.exe /c \"%s > \\\"%s\\\" 2>&1\"", cmd, tmpname);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(NULL, full_cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DeleteFileA(tmpname);
        lua_pushnil(L);
        lua_pushstring(L, "CreateProcess failed");
        return 2;
    }

    CloseHandle(pi.hThread);

    win_async_cmds[slot].hProcess = pi.hProcess;
    win_async_cmds[slot].in_use = 1;
    strncpy(win_async_cmds[slot].out_file, tmpname, sizeof(win_async_cmds[slot].out_file) - 1);

    lua_pushinteger(L, slot + 1);  /* 1-based ID */
    return 1;
}

/* ── spkg.wait_task(task_id) → {ok, out, code} | nil (running) ─── */
static int n_wait_task(lua_State *L) {
    int slot = (int)luaL_checkinteger(L, 1) - 1;
    if (slot < 0 || slot >= MAX_ASYNC_CMDS || !win_async_cmds[slot].in_use) {
        lua_pushnil(L);
        return 1;
    }

    /* Non-blocking check */
    DWORD wait = WaitForSingleObject(win_async_cmds[slot].hProcess, 0);
    if (wait == WAIT_TIMEOUT) {
        lua_pushnil(L);  /* still running */
        return 1;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(win_async_cmds[slot].hProcess, &exit_code);
    CloseHandle(win_async_cmds[slot].hProcess);

    /* Read output file */
    FILE *fp = fopen(win_async_cmds[slot].out_file, "r");
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
    DeleteFileA(win_async_cmds[slot].out_file);
    win_async_cmds[slot].in_use = 0;

    lua_newtable(L);
    lua_pushboolean(L, exit_code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out ? out : "");  lua_setfield(L, -2, "out");
    lua_pushinteger(L, (int)exit_code);  lua_setfield(L, -2, "code");

    if (out) free(out);
    return 1;
}

#else
/* POSIX: fork/exec with temp file output */
static struct {
    pid_t pid;
    int   in_use;
    char  out_file[PATH_MAX];
} posix_async_cmds[MAX_ASYNC_CMDS];

static int find_async_slot(void) {
    for (int i = 0; i < MAX_ASYNC_CMDS; i++) {
        if (!posix_async_cmds[i].in_use) return i;
    }
    return -1;
}

/* ── spkg.start_cmd(cmd) → task_id (number) or nil, err ────────── */
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
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    close(fd);

    /* Build command: /bin/sh -c "cmd > tmpfile 2>&1" */
    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd), "%s > '%s' 2>&1", cmd, tmpfile);

    pid_t pid = fork();
    if (pid < 0) {
        remove(tmpfile);
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    if (pid == 0) {
        /* Child */
        execl("/bin/sh", "sh", "-c", full_cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    posix_async_cmds[slot].pid = pid;
    posix_async_cmds[slot].in_use = 1;
    strncpy(posix_async_cmds[slot].out_file, tmpfile, sizeof(posix_async_cmds[slot].out_file) - 1);

    lua_pushinteger(L, slot + 1);
    return 1;
}

/* ── spkg.wait_task(task_id) → {ok, out, code} | nil (running) ─── */
static int n_wait_task(lua_State *L) {
    int slot = (int)luaL_checkinteger(L, 1) - 1;
    if (slot < 0 || slot >= MAX_ASYNC_CMDS || !posix_async_cmds[slot].in_use) {
        lua_pushnil(L);
        return 1;
    }

    /* Non-blocking check with WNOHANG */
    int status;
    pid_t result = waitpid(posix_async_cmds[slot].pid, &status, WNOHANG);
    if (result == 0) {
        lua_pushnil(L);  /* still running */
        return 1;
    }
    if (result < 0) {
        lua_pushnil(L);
        return 1;
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* Read output file */
    FILE *fp = fopen(posix_async_cmds[slot].out_file, "r");
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
    remove(posix_async_cmds[slot].out_file);
    posix_async_cmds[slot].in_use = 0;

    lua_newtable(L);
    lua_pushboolean(L, code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out ? out : ""); lua_setfield(L, -2, "out");
    lua_pushinteger(L, code); lua_setfield(L, -2, "code");

    if (out) free(out);
    return 1;
}
#endif

/* ── spkg.remove ─────────────────────────────────────────────────── */
static int n_remove(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int r;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        /* RemoveDirectoryA only works on empty dirs; use a simple recursive approach */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
        r = system(cmd);
    } else {
        r = DeleteFileA(path) ? 0 : -1;
    }
    lua_pushboolean(L, r == 0);
#else
    int r = remove(path);
    if (r != 0) {
        /* Try as directory */
        char cmd[1280];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
        r = system(cmd);
    }
    lua_pushboolean(L, r == 0);
#endif
    return 1;
}

/* ── spkg.fingerprint ────────────────────────────────────────────── */
/* Simple FNV-1a 64-bit hash (Phase 2 placeholder; real SHA-256 can be added later) */
static int n_fingerprint(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    uint64_t hash = 0xcbf29ce484222325ULL;  /* FNV-1a 64-bit init */
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)(unsigned char)data[i];
        hash *= 0x100000001b3ULL;            /* FNV-1a 64-bit prime */
    }

    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
    lua_pushstring(L, hex);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Phase 3: HTTP Client (using mongoose) — coordinator side
 * ═══════════════════════════════════════════════════════════════════ */

/* Include mongoose for HTTP client support */
#define MG_ENABLE_LINES  1
#include "mongoose.h"

/* Helper: synchronous HTTP request via mongoose event loop */
struct http_ctx {
    int         done;
    int         status;
    char       *body;
    size_t      body_len;
};

static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    struct http_ctx *ctx = (struct http_ctx *) c->fn_data;
    if (!ctx) return;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        ctx->status = mg_http_status(hm);
        ctx->body = (char *)malloc(hm->body.len + 1);
        if (ctx->body) {
            memcpy(ctx->body, hm->body.buf, hm->body.len);
            ctx->body[hm->body.len] = '\0';
            ctx->body_len = hm->body.len;
        }
        ctx->done = 1;
    }
    if (ev == MG_EV_CLOSE) {
        if (!ctx->done) {
            ctx->done = 1;  /* connection closed without response */
        }
    }
}

/* spkg.http_post(url, body) → {ok, body, code} */
static int n_http_post(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    const char *body = luaL_checkstring(L, 2);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct http_ctx ctx = {0};
    struct mg_connection *c = mg_http_connect(&mgr, url, http_ev_handler, &ctx);
    if (!c) {
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "connect failed"); lua_setfield(L, -2, "body");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        mg_mgr_free(&mgr);
        return 1;
    }

    mg_printf(c,
              "POST %s HTTP/1.0\r\n"
              "Host: %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "\r\n"
              "%s",
              mg_url_uri(url), mg_url_host(url), strlen(body), body);

    /* Run event loop with timeout */
    int timeout_ms = 60000;  /* 60 seconds */
    while (!ctx.done) {
        mg_mgr_poll(&mgr, 100);
        timeout_ms -= 100;
        if (timeout_ms <= 0) break;
    }

    lua_newtable(L);
    int ok = (ctx.done && ctx.status >= 200 && ctx.status < 300);
    lua_pushboolean(L, ok);  lua_setfield(L, -2, "ok");
    lua_pushstring(L, ctx.body ? ctx.body : ""); lua_setfield(L, -2, "body");
    lua_pushinteger(L, ctx.status ? ctx.status : 0); lua_setfield(L, -2, "code");

    if (ctx.body) free(ctx.body);
    mg_mgr_free(&mgr);
    return 1;
}

/* spkg.http_get(url) → {ok, body, code} */
static int n_http_get(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct http_ctx ctx = {0};
    struct mg_connection *c = mg_http_connect(&mgr, url, http_ev_handler, &ctx);
    if (!c) {
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "connect failed"); lua_setfield(L, -2, "body");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        mg_mgr_free(&mgr);
        return 1;
    }

    mg_printf(c,
              "GET %s HTTP/1.0\r\n"
              "Host: %s\r\n"
              "\r\n",
              mg_url_uri(url), mg_url_host(url));

    int timeout_ms = 30000;  /* 30 seconds */
    while (!ctx.done) {
        mg_mgr_poll(&mgr, 100);
        timeout_ms -= 100;
        if (timeout_ms <= 0) break;
    }

    lua_newtable(L);
    int ok = (ctx.done && ctx.status >= 200 && ctx.status < 300);
    lua_pushboolean(L, ok);  lua_setfield(L, -2, "ok");
    lua_pushstring(L, ctx.body ? ctx.body : ""); lua_setfield(L, -2, "body");
    lua_pushinteger(L, ctx.status ? ctx.status : 0); lua_setfield(L, -2, "code");

    if (ctx.body) free(ctx.body);
    mg_mgr_free(&mgr);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Phase 2: Content-addressable Build Cache
 * ═══════════════════════════════════════════════════════════════════
 *
 * Cache directory: $HOME/.spkg-cache/
 * Layout: $HOME/.spkg-cache/<key>/output.o
 *                           /meta (text metadata: hit counter)
 * Stats: stored in $HOME/.spkg-cache/.stats
 */

static char g_cache_dir[PATH_MAX] = {0};

static const char *get_cache_dir(void) {
    if (g_cache_dir[0]) return g_cache_dir;

    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = "C:\\Users\\Default";
#else
    if (!home) home = "/root";
#endif

    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/.spkg-cache", home);
    return g_cache_dir;
}

static void ensure_cache_dir(void) {
    const char *dir = get_cache_dir();
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", dir);
    system(cmd);
#endif
}

/* Helper: build cache path for a key -> buf */
static void cache_path_for(const char *key, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%s/%s", get_cache_dir(), key);
}

/* Helper: copy file from src to dst */
static int copy_file(const char *src, const char *dst) {
    FILE *fin = fopen(src, "rb");
    if (!fin) return -1;
    FILE *fout = fopen(dst, "wb");
    if (!fout) { fclose(fin); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        fwrite(buf, 1, n, fout);
    }
    fclose(fin);
    fclose(fout);
    return 0;
}

/* ── spkg.cache_init() → bool ──────────────────────────────────── */
static int n_cache_init(lua_State *L) {
    ensure_cache_dir();
    lua_pushboolean(L, 1);
    return 1;
}

/* Helper: check if file exists (any file, not just executable) */
static int file_exists_native(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

/* ── spkg.cache_get(key, output_path) → bool ───────────────────── */
static int n_cache_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *output_path = luaL_checkstring(L, 2);

    char cache_dir_path[PATH_MAX];
    cache_path_for(key, cache_dir_path, sizeof(cache_dir_path));

    char cached_file[PATH_MAX];
#ifdef _WIN32
    snprintf(cached_file, sizeof(cached_file), "%s\\output.o", cache_dir_path);
#else
    snprintf(cached_file, sizeof(cached_file), "%s/output.o", cache_dir_path);
#endif

    if (!file_exists_native(cached_file)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Copy cached .o to output_path */
    if (copy_file(cached_file, output_path) == 0) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

/* ── spkg.cache_put(key, file_path) → bool ─────────────────────── */
static int n_cache_put(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *file_path = luaL_checkstring(L, 2);

    ensure_cache_dir();

    char cache_dir_path[PATH_MAX];
    cache_path_for(key, cache_dir_path, sizeof(cache_dir_path));

    /* Create cache subdirectory */
#ifdef _WIN32
    CreateDirectoryA(cache_dir_path, NULL);
#else
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", cache_dir_path);
    system(cmd);
#endif

    /* Copy file_path → cache_dir/output.o */
    char cached_file[PATH_MAX];
#ifdef _WIN32
    snprintf(cached_file, sizeof(cached_file), "%s\\output.o", cache_dir_path);
#else
    snprintf(cached_file, sizeof(cached_file), "%s/output.o", cache_dir_path);
#endif

    lua_pushboolean(L, copy_file(file_path, cached_file) == 0);
    return 1;
}

/* ── spkg.cache_stats() → {hit, miss, size, count} ─────────────── */
static int n_cache_stats(lua_State *L) {
    const char *dir = get_cache_dir();

    int hit = 0, miss = 0, count = 0;
    long long size = 0;

    /* Read stats file if exists */
    char stats_file[PATH_MAX];
#ifdef _WIN32
    snprintf(stats_file, sizeof(stats_file), "%s\\.stats", dir);
#else
    snprintf(stats_file, sizeof(stats_file), "%s/.stats", dir);
#endif

    FILE *fp = fopen(stats_file, "r");
    if (fp) {
        fscanf(fp, "hit=%d miss=%d", &hit, &miss);
        fclose(fp);
    }

    /* Count cache entries and total size */
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "for /d %%D in (\"%s\\*\") do @set /a count+=1 & for %%F in (\"%%D\\output.o\") do @set /a size+=%%~zF",
        dir);
    /* Simpler approach: just count dirs */
    snprintf(cmd, sizeof(cmd), "dir /b /ad \"%s\" 2>nul | find /c /v \"\"", dir);
    FILE *cmd_fp = popen(cmd, "r");
    if (cmd_fp) {
        char line[64];
        if (fgets(line, sizeof(line), cmd_fp)) {
            count = atoi(line);
        }
        pclose(cmd_fp);
    }
#else
    char cmd[1280];
    snprintf(cmd, sizeof(cmd),
        "ls -1d %s/*/ 2>/dev/null | wc -l", dir);
    FILE *cmd_fp = popen(cmd, "r");
    if (cmd_fp) {
        char line[64];
        if (fgets(line, sizeof(line), cmd_fp)) {
            count = atoi(line);
        }
        pclose(cmd_fp);
    }
    /* Total size */
    snprintf(cmd, sizeof(cmd),
        "du -sb %s 2>/dev/null | cut -f1", dir);
    cmd_fp = popen(cmd, "r");
    if (cmd_fp) {
        char line[64];
        if (fgets(line, sizeof(line), cmd_fp)) {
            size = atoll(line);
        }
        pclose(cmd_fp);
    }
#endif

    lua_newtable(L);
    lua_pushinteger(L, hit);   lua_setfield(L, -2, "hit");
    lua_pushinteger(L, miss);  lua_setfield(L, -2, "miss");
    lua_pushinteger(L, count); lua_setfield(L, -2, "count");
    lua_pushinteger(L, size);  lua_setfield(L, -2, "size");
    return 1;
}

/* ── spkg.cache_clear() → bool ─────────────────────────────────── */
static int n_cache_clear(lua_State *L) {
    const char *dir = get_cache_dir();

#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
    lua_pushboolean(L, system(cmd) == 0);
#else
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    lua_pushboolean(L, system(cmd) == 0);
#endif
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Phase 2: Custom Step
 * ═══════════════════════════════════════════════════════════════════ */

/* ── spkg.custom_needs_run(inputs_table, outputs_table) → bool ─── */
static int n_custom_needs_run(lua_State *L) {
    /* Check: if any output doesn't exist, or any input is newer than all outputs, needs run */

    double newest_output = 0.0;

    /* First pass: find newest output mtime */
    if (!lua_istable(L, 2)) { lua_pushboolean(L, 1); return 1; }

    int out_count = (int)lua_rawlen(L, 2);
    if (out_count == 0) { lua_pushboolean(L, 1); return 1; }

    for (int i = 1; i <= out_count; i++) {
        lua_rawgeti(L, 2, i);
        const char *opath = lua_tostring(L, -1);
        double mt = 0.0;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(opath, GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER ft;
            ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            mt = (double)(ft.QuadPart - 116444736000000000ULL) / 10000000.0;
        }
#else
        struct stat st;
        if (stat(opath, &st) == 0) {
#ifdef __APPLE__
            mt = (double)st.st_mtimespec.tv_sec + (double)st.st_mtimespec.tv_nsec / 1e9;
#else
            mt = (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9;
#endif
        }
#endif
        lua_pop(L, 1);
        if (mt == 0.0) { lua_pushboolean(L, 1); return 1; } /* output missing */
        if (mt > newest_output) newest_output = mt;
    }

    /* Second pass: check if any input is newer than newest output */
    if (lua_istable(L, 1)) {
        int in_count = (int)lua_rawlen(L, 1);
        for (int i = 1; i <= in_count; i++) {
            lua_rawgeti(L, 1, i);
            const char *ipath = lua_tostring(L, -1);
            double mt = 0.0;
#ifdef _WIN32
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(ipath, GetFileExInfoStandard, &fad)) {
                ULARGE_INTEGER ft;
                ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mt = (double)(ft.QuadPart - 116444736000000000ULL) / 10000000.0;
            }
#else
            struct stat st;
            if (stat(ipath, &st) == 0) {
#ifdef __APPLE__
                mt = (double)st.st_mtimespec.tv_sec + (double)st.st_mtimespec.tv_nsec / 1e9;
#else
                mt = (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9;
#endif
            }
#endif
            lua_pop(L, 1);
            if (mt > newest_output) { lua_pushboolean(L, 1); return 1; }
        }
    }

    lua_pushboolean(L, 0);
    return 1;
}

/* ── spkg.custom_exec(cmd_table, workdir) → {ok, out, code} ────── */
static int n_custom_exec(lua_State *L) {
    if (!lua_istable(L, 1)) {
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "expected table for command"); lua_setfield(L, -2, "out");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        return 1;
    }

    /* Build command string from argv table */
    char cmd[4096] = {0};
    size_t total = 0;
    int argc = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= argc; i++) {
        lua_rawgeti(L, 1, i);
        const char *arg = lua_tostring(L, -1);
        if (arg) {
            size_t n = strlen(arg);
            if (total + n + 2 < sizeof(cmd)) {
                if (i > 1) { cmd[total++] = ' '; }
                memcpy(cmd + total, arg, n);
                total += n;
            }
        }
        lua_pop(L, 1);
    }
    cmd[total] = '\0';

    /* Optionally set workdir (not supported cross-platform simply; use cd trick) */
    const char *workdir = lua_tostring(L, 2);

    char full_cmd[4096];
    if (workdir && workdir[0]) {
        snprintf(full_cmd, sizeof(full_cmd), "cd '%s' && %s", workdir, cmd);
    } else {
        strncpy(full_cmd, cmd, sizeof(full_cmd) - 1);
    }

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "popen failed"); lua_setfield(L, -2, "out");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        return 1;
    }

    char buf[4096];
    size_t out_total = 0;
    char *out = malloc(1);
    out[0] = '\0';

    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        char *tmp = realloc(out, out_total + n + 1);
        if (!tmp) { free(out); break; }
        out = tmp;
        memcpy(out + out_total, buf, n);
        out_total += n;
        out[out_total] = '\0';
    }

    int status = pclose(fp);
#ifdef _WIN32
    int code = status;
#else
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    lua_newtable(L);
    lua_pushboolean(L, code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out ? out : ""); lua_setfield(L, -2, "out");
    lua_pushinteger(L, code); lua_setfield(L, -2, "code");

    if (out) free(out);
    return 1;
}

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
    {"remove",           n_remove},
    {"fingerprint",      n_fingerprint},
    {"cache_init",       n_cache_init},
    {"cache_get",        n_cache_get},
    {"cache_put",        n_cache_put},
    {"cache_stats",      n_cache_stats},
    {"cache_clear",      n_cache_clear},
    {"custom_needs_run", n_custom_needs_run},
    {"custom_exec",      n_custom_exec},
    {"http_post",        n_http_post},
    {"http_get",         n_http_get},
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
