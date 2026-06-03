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
 *   spkg.find_sharp_std()     → string or nil
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
    #include <winsock2.h>
    #include <windows.h>
    #include <io.h>
    #include <direct.h>
    #include <sys/stat.h>
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
    #ifndef PATH_MAX
        #define PATH_MAX MAX_PATH
    #endif
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <glob.h>
    #include <limits.h>
    #include <sys/wait.h>
    #include <dirent.h>
    #include <pwd.h>
    #include <fcntl.h>
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

/* Find path separator (handles both / and \) */
static char *find_path_sep(char *path) {
    char *last = NULL;
    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p;
    }
    return last;
}

#ifndef _WIN32
static int mkdir_p_native(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            struct stat st2;
            if (stat(tmp, &st2) != 0) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);
    if (!dir) return -1;

    struct dirent *entry;
    int ret = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char subpath[PATH_MAX];
        size_t plen = strlen(path);
        size_t nlen = strlen(entry->d_name);
        if (plen + 1 + nlen >= sizeof(subpath)) {
            ret = -1;
            break;
        }
        memcpy(subpath, path, plen);
        subpath[plen] = '/';
        memcpy(subpath + plen + 1, entry->d_name, nlen + 1);

        if (remove_recursive(subpath) != 0) {
            ret = -1;
            break;
        }
    }
    closedir(dir);

    if (ret == 0 && rmdir(path) != 0) ret = -1;
    return ret;
}
#endif

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
    if (!out) {
        pclose(fp);
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "out of memory"); lua_setfield(L, -2, "out");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        return 1;
    }
    out[0] = '\0';

    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        char *tmp = realloc(out, total + n + 1);
        if (!tmp) { free(out); out = NULL; break; }
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
    lua_pushstring(L, out ? out : ""); lua_setfield(L, -2, "out");
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
    lua_pushboolean(L, mkdir_p_native(path) == 0);
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
    char cmd[8192];
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
        char safe_base[2048], safe_name[2048];
        {
            size_t bi = 0;
            for (const char *p = base; *p && bi < sizeof(safe_base) - 5; p++) {
                if (*p == '\'') {
                    safe_base[bi++] = '\''; safe_base[bi++] = '\\';
                    safe_base[bi++] = '\''; safe_base[bi++] = '\'';
                } else {
                    safe_base[bi++] = *p;
                }
            }
            safe_base[bi] = '\0';
        }
        {
            size_t ni = 0;
            for (const char *p = name; *p && ni < sizeof(safe_name) - 5; p++) {
                if (*p == '\'') {
                    safe_name[ni++] = '\''; safe_name[ni++] = '\\';
                    safe_name[ni++] = '\''; safe_name[ni++] = '\'';
                } else {
                    safe_name[ni++] = *p;
                }
            }
            safe_name[ni] = '\0';
        }
        snprintf(cmd, sizeof(cmd),
                 "find '%s' -name '%s' -type f 2>/dev/null", safe_base, safe_name);
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
        { lua_pushstring(L, g.gl_pathv[i]); lua_rawseti(L, -2, (lua_Integer)(i+1)); }
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
    if (sz < 0) { fclose(fp); lua_pushnil(L); return 1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); lua_pushnil(L); return 1; }
    size_t nread = fread(buf, 1, sz, fp);
    fclose(fp);
    if (nread != (size_t)sz) { free(buf); lua_pushnil(L); return 1; }
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
    size_t nwrote = fwrite(content, 1, len, fp);
    int ok = (nwrote == len);
    fclose(fp);
    lua_pushboolean(L, ok);
    return 1;
}

/* ── spkg.find_sharpc ────────────────────────────────────────────── */
static int n_find_sharpc(lua_State *L) {
    const char *root = getenv("SHARP_ROOT");
    if (root && root[0]) {
        char cand[PATH_MAX];
#ifdef _WIN32
        snprintf(cand, sizeof(cand), "%s\\bin\\sharpc.exe", root);
#else
        snprintf(cand, sizeof(cand), "%s/bin/sharpc", root);
#endif
        if (is_executable(cand)) { lua_pushstring(L, cand); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

/* ── spkg.find_zigcc ─────────────────────────────────────────────── */
static int n_find_zigcc(lua_State *L) {
    const char *root = getenv("SHARP_ROOT");
    if (root && root[0]) {
        char cand[PATH_MAX];
#ifdef _WIN32
        snprintf(cand, sizeof(cand), "%s\\zig\\zig.exe", root);
#else
        snprintf(cand, sizeof(cand), "%s/zig/zig", root);
#endif
        if (is_executable(cand)) { lua_pushstring(L, cand); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

/* ── spkg.find_sharp_std ──────────────────────────────────────────── */
static int n_find_sharp_std(lua_State *L) {
    const char *root = getenv("SHARP_ROOT");
    if (root && root[0]) {
        char cand[PATH_MAX];
#ifdef _WIN32
        snprintf(cand, sizeof(cand), "%s\\std", root);
#else
        snprintf(cand, sizeof(cand), "%s/std", root);
#endif
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
            lua_pushstring(L, cand); return 1;
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
    if (!h) {
        struct passwd *pw = getpwuid(getuid());
        h = pw ? pw->pw_dir : "/";
    }
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

/* ── spkg.chdir ────────────────────────────────────────────────────── */
static int n_chdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
    if (_chdir(path) == 0) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
#else
    if (chdir(path) == 0) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
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
        lua_pushstring(L, "aarch64-linux-gnu");
    #elif defined(__arm__)
        lua_pushstring(L, "armv7l-linux-gnueabihf");
    #else
        lua_pushstring(L, "x86_64-linux-gnu");
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

/* ── spkg.self_fingerprint ───────────────────────────────────────── */
static int n_self_fingerprint(lua_State *L) {
    static char cached[17] = {0};
    if (cached[0]) {
        lua_pushstring(L, cached);
        return 1;
    }

    uint64_t hash = 0xcbf29ce484222325ULL;
    int hashed = 0;

#ifdef __linux__
    {
        FILE *fp = fopen("/proc/self/exe", "rb");
        if (fp) {
            unsigned char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                for (size_t i = 0; i < n; i++) {
                    hash ^= (uint64_t)buf[i];
                    hash *= 0x100000001b3ULL;
                }
            }
            if (ferror(fp)) hash = 0xcbf29ce484222325ULL;
            fclose(fp);
            hashed = 1;
        }
    }
#endif

    if (!hashed) {
        /* Fallback: hash a fixed token — still better than no fingerprint */
        const char *fallback = "spkg-self";
        for (; *fallback; fallback++) {
            hash ^= (uint64_t)(unsigned char)*fallback;
            hash *= 0x100000001b3ULL;
        }
    }

    snprintf(cached, sizeof(cached), "%016llx", (unsigned long long)hash);
    lua_pushstring(L, cached);
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
    DWORD tmplen = GetTempPathA(sizeof(tmpfile), tmpfile);
    if (tmplen == 0 || tmplen >= sizeof(tmpfile)) {
        lua_pushnil(L);
        lua_pushliteral(L, "GetTempPath failed");
        return 2;
    }
    char tmpname[MAX_PATH];
    if (GetTempFileNameA(tmpfile, "spk", 0, tmpname) == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "GetTempFileName failed");
        return 2;
    }

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
    lua_Integer val = luaL_checkinteger(L, 1);
    int slot = val > (lua_Integer)INT_MAX ? -1 : (int)val - 1;
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
    if (wait == WAIT_FAILED || wait == WAIT_ABANDONED) {
        CloseHandle(win_async_cmds[slot].hProcess);
        DeleteFileA(win_async_cmds[slot].out_file);
        win_async_cmds[slot].in_use = 0;
        lua_pushnil(L);
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
        if (out) {
            out[0] = '\0';
            while (fgets(buf, sizeof(buf), fp)) {
                size_t n = strlen(buf);
                char *tmp = realloc(out, total + n + 1);
                if (!tmp) { free(out); out = NULL; break; }
                out = tmp;
                memcpy(out + total, buf, n);
                total += n;
                out[total] = '\0';
            }
        }
        fclose(fp);
    }

    /* Clean up temp file */
    DeleteFileA(win_async_cmds[slot].out_file);
    win_async_cmds[slot].in_use = 0;

    lua_newtable(L);
    lua_pushboolean(L, exit_code == 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, out ? out : "");  lua_setfield(L, -2, "out");
    lua_pushinteger(L, (lua_Integer)exit_code);  lua_setfield(L, -2, "code");

    free(out);
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

    char tmpfile[] = "/tmp/spkg_cmd_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        remove(tmpfile);
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        /* Set TMPDIR to current directory so compilers create temp files
           on the same filesystem as their output, avoiding EXDEV rename
           failures when /tmp and the build directory are on different fs. */
        char cwd_buf[PATH_MAX];
        if (getcwd(cwd_buf, sizeof(cwd_buf))) {
            setenv("TMPDIR", cwd_buf, 1);
        }

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fd);

    posix_async_cmds[slot].pid = pid;
    posix_async_cmds[slot].in_use = 1;
    strncpy(posix_async_cmds[slot].out_file, tmpfile, sizeof(posix_async_cmds[slot].out_file) - 1);

    lua_pushinteger(L, slot + 1);
    return 1;
}

/* ── spkg.wait_task(task_id) → {ok, out, code} | nil (running) ─── */
static int n_wait_task(lua_State *L) {
    lua_Integer val = luaL_checkinteger(L, 1);
    int slot = val > (lua_Integer)INT_MAX ? -1 : (int)val - 1;
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
        remove(posix_async_cmds[slot].out_file);
        posix_async_cmds[slot].in_use = 0;
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
        if (out) {
            out[0] = '\0';
            while (fgets(buf, sizeof(buf), fp)) {
                size_t n = strlen(buf);
                char *tmp = realloc(out, total + n + 1);
                if (!tmp) { free(out); out = NULL; break; }
                out = tmp;
                memcpy(out + total, buf, n);
                total += n;
                out[total] = '\0';
            }
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

    free(out);
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
        char cmd[PATH_MAX + 64];
        char safe_path[PATH_MAX * 2];
        {
            size_t si = 0;
            for (const char *p = path; *p && si < sizeof(safe_path) - 3; p++) {
                if (*p == '"') {
                    safe_path[si++] = '"'; safe_path[si++] = '"';
                } else {
                    safe_path[si++] = *p;
                }
            }
            safe_path[si] = '\0';
        }
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", safe_path);
        r = system(cmd);
    } else {
        r = DeleteFileA(path) ? 0 : -1;
    }
    lua_pushboolean(L, r == 0);
#else
    lua_pushboolean(L, remove_recursive(path) == 0);
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

/* Helper: build HTTP request string using mg_url parsing */
static char *build_http_post(const char *url, const char *body, size_t *out_len) {
    struct mg_str host = mg_url_host(url);
    const char *uri = mg_url_uri(url);
    size_t body_len = strlen(body);

    /* Clamp host length to safe value for snprintf */
    size_t hlen_safe = host.len;
    if (hlen_safe > 255) hlen_safe = 255;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
              "POST %s HTTP/1.0\r\n"
              "Host: %.*s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "\r\n",
              uri, (int)hlen_safe, host.buf, body_len);

    if (hlen < 0) return NULL;
    *out_len = (size_t)hlen + body_len;
    char *req = (char *)malloc(*out_len + 1);
    if (!req) return NULL;
    memcpy(req, header, hlen);
    memcpy(req + hlen, body, body_len);
    req[*out_len] = '\0';
    return req;
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

    size_t req_len = 0;
    char *req = build_http_post(url, body, &req_len);
    if (req) {
        mg_send(c, req, req_len);
        free(req);
    }

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

/* Helper: build HTTP GET request string */
static char *build_http_get(const char *url, size_t *out_len) {
    struct mg_str host = mg_url_host(url);
    const char *uri = mg_url_uri(url);

    /* Clamp host length to safe value for snprintf */
    size_t hlen_safe = host.len;
    if (hlen_safe > 255) hlen_safe = 255;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
              "GET %s HTTP/1.0\r\n"
              "Host: %.*s\r\n"
              "\r\n",
              uri, (int)hlen_safe, host.buf);

    if (hlen < 0) return NULL;
    *out_len = (size_t)hlen;
    char *req = (char *)malloc(*out_len + 1);
    if (!req) return NULL;
    memcpy(req, header, hlen);
    req[*out_len] = '\0';
    return req;
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

    size_t req_len = 0;
    char *req = build_http_get(url, &req_len);
    if (req) {
        mg_send(c, req, req_len);
        free(req);
    }

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
 * Phase 4: Colorized output
 * ═══════════════════════════════════════════════════════════════════ */

/* ANSI color codes */
static const char *color_codes[] = {
    "",        /* 0: reset/default */
    "\033[31m", /* 1: red */
    "\033[32m", /* 2: green */
    "\033[33m", /* 3: yellow */
    "\033[34m", /* 4: blue */
    "\033[35m", /* 5: magenta */
    "\033[36m", /* 6: cyan */
    "\033[1m",  /* 7: bold */
    "\033[31;1m", /* 8: bold red */
    "\033[32;1m", /* 9: bold green */
    "\033[33;1m", /* 10: bold yellow */
    "\033[34;1m", /* 11: bold blue */
};

/* spkg.colorize(text, color_name) → string with ANSI codes */
static int n_colorize(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    const char *color_name = luaL_optstring(L, 2, "");
    int color_idx = 0;

    if (strcmp(color_name, "red") == 0) color_idx = 1;
    else if (strcmp(color_name, "green") == 0) color_idx = 2;
    else if (strcmp(color_name, "yellow") == 0) color_idx = 3;
    else if (strcmp(color_name, "blue") == 0) color_idx = 4;
    else if (strcmp(color_name, "magenta") == 0) color_idx = 5;
    else if (strcmp(color_name, "cyan") == 0) color_idx = 6;
    else if (strcmp(color_name, "bold") == 0) color_idx = 7;
    else if (strcmp(color_name, "bold_red") == 0) color_idx = 8;
    else if (strcmp(color_name, "bold_green") == 0) color_idx = 9;
    else if (strcmp(color_name, "bold_yellow") == 0) color_idx = 10;
    else if (strcmp(color_name, "bold_blue") == 0) color_idx = 11;

    /* If not a TTY, return plain text */
    if (color_idx == 0) {
        lua_pushstring(L, text);
        return 1;
    }

    /* Dynamically allocate to handle arbitrarily long text */
    size_t text_len = strlen(text);
    size_t code_len = strlen(color_codes[color_idx]);
    size_t reset_len = strlen("\033[0m");
    char *buf = malloc(text_len + code_len + reset_len + 1);
    if (!buf) { lua_pushstring(L, text); return 1; }
    memcpy(buf, color_codes[color_idx], code_len);
    memcpy(buf + code_len, text, text_len);
    memcpy(buf + code_len + text_len, "\033[0m", reset_len + 1);
    lua_pushlstring(L, buf, code_len + text_len + reset_len);
    free(buf);
    return 1;
}

/* spkg.is_tty() → bool */
static int n_is_tty(lua_State *L) {
#ifdef _WIN32
    DWORD mode;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    lua_pushboolean(L, h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode));
#else
    lua_pushboolean(L, isatty(STDOUT_FILENO));
#endif
    return 1;
}

/* spkg.tty_width() → int */
static int n_tty_width(lua_State *L) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(h, &csbi)) {
        lua_pushinteger(L, csbi.srWindow.Right - csbi.srWindow.Left + 1);
        return 1;
    }
#elif defined(TIOCGWINSZ)
    #include <sys/ioctl.h>
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        lua_pushinteger(L, ws.ws_col);
        return 1;
    }
#endif
    lua_pushinteger(L, 80);  /* fallback */
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
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/";
    }
#endif

    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/.spkg-cache", home);
    return g_cache_dir;
}

static void ensure_cache_dir(void) {
    const char *dir = get_cache_dir();
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir_p_native(dir);
#endif
}

/* Helper: build cache path for a key -> buf, returns buf or NULL on truncation */
static char *cache_path_for(const char *key, char *buf, size_t bufsize) {
    const char *dir = get_cache_dir();
    size_t dlen = strlen(dir);
    size_t klen = strlen(key);
    if (dlen + 1 + klen + 1 > bufsize) return NULL;
    memcpy(buf, dir, dlen);
    buf[dlen] = '/';
    memcpy(buf + dlen + 1, key, klen);
    buf[dlen + 1 + klen] = '\0';
    return buf;
}

/* Helper: append a filename to a cache dir path -> buf, returns buf or NULL */
static char *cache_file_path(const char *cache_dir, const char *filename, char *buf, size_t bufsize) {
    size_t dlen = strlen(cache_dir);
    size_t flen = strlen(filename);
    if (dlen + 1 + flen + 1 > bufsize) return NULL;
    memcpy(buf, cache_dir, dlen);
    buf[dlen] = '/';
    memcpy(buf + dlen + 1, filename, flen);
    buf[dlen + 1 + flen] = '\0';
    return buf;
}

/* Helper: copy file from src to dst */
static int copy_file(const char *src, const char *dst) {
    FILE *fin = fopen(src, "rb");
    if (!fin) return -1;
    FILE *fout = fopen(dst, "wb");
    if (!fout) { fclose(fin); return -1; }

    char buf[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) { ok = 0; break; }
    }
    if (ferror(fin)) ok = 0;
    fclose(fin);
    fclose(fout);
    if (!ok) remove(dst);
    return ok ? 0 : -1;
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
    char cached_file[PATH_MAX];
    if (!cache_path_for(key, cache_dir_path, sizeof(cache_dir_path))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!cache_file_path(cache_dir_path, "output.o", cached_file, sizeof(cached_file))) {
        lua_pushboolean(L, 0);
        return 1;
    }

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
    char cached_file[PATH_MAX];
    if (!cache_path_for(key, cache_dir_path, sizeof(cache_dir_path))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!cache_file_path(cache_dir_path, "output.o", cached_file, sizeof(cached_file))) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Create cache subdirectory */
#ifdef _WIN32
    CreateDirectoryA(cache_dir_path, NULL);
#else
    mkdir_p_native(cache_dir_path);
#endif

    /* Copy file_path → cache_dir/output.o */
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
    if (!cache_file_path(dir, ".stats", stats_file, sizeof(stats_file))) {
        /* Stats file path too long; return empty stats */
    } else {
        FILE *fp = fopen(stats_file, "r");
        if (fp) {
            if (fscanf(fp, "hit=%d miss=%d", &hit, &miss) != 2) {
                hit = 0; miss = 0;
            }
            fclose(fp);
        }
    }

    /* Count cache entries and total size */
#ifdef _WIN32
    char cmd[PATH_MAX + 128];
    char safe_dir[PATH_MAX * 2];
    {
        size_t si = 0;
        for (const char *p = dir; *p && si < sizeof(safe_dir) - 3; p++) {
            if (*p == '"') {
                safe_dir[si++] = '"'; safe_dir[si++] = '"';
            } else {
                safe_dir[si++] = *p;
            }
        }
        safe_dir[si] = '\0';
    }
    snprintf(cmd, sizeof(cmd), "dir /b /ad \"%s\" 2>nul | find /c /v \"\"", safe_dir);
    FILE *cmd_fp = popen(cmd, "r");
    if (cmd_fp) {
        char line[64];
        if (fgets(line, sizeof(line), cmd_fp)) {
            count = atoi(line);
        }
        pclose(cmd_fp);
    }
#else
    /* Escape single-quotes in dir for safe shell use */
    char safe_dir[PATH_MAX];
    {
        size_t di = 0;
        for (const char *p = dir; *p && di < sizeof(safe_dir) - 5; p++) {
            if (*p == '\'') {
                safe_dir[di++] = '\''; safe_dir[di++] = '\\';
                safe_dir[di++] = '\''; safe_dir[di++] = '\'';
            } else {
                safe_dir[di++] = *p;
            }
        }
        safe_dir[di] = '\0';
    }

    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd),
        "ls -1d '%s'/*/ 2>/dev/null | wc -l", safe_dir);
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
        "du -sb '%s' 2>/dev/null | cut -f1", safe_dir);
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
    char cmd[PATH_MAX + 64];
    char safe_dir[PATH_MAX * 2];
    {
        size_t si = 0;
        for (const char *p = dir; *p && si < sizeof(safe_dir) - 3; p++) {
            if (*p == '"') {
                safe_dir[si++] = '"'; safe_dir[si++] = '"';
            } else {
                safe_dir[si++] = *p;
            }
        }
        safe_dir[si] = '\0';
    }
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", safe_dir);
    lua_pushboolean(L, system(cmd) == 0);
#else
    lua_pushboolean(L, remove_recursive(dir) == 0);
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

    lua_Unsigned out_count = lua_rawlen(L, 2);
    if (out_count == 0) { lua_pushboolean(L, 1); return 1; }

    for (lua_Unsigned i = 1; i <= out_count; i++) {
        lua_rawgeti(L, 2, (lua_Integer)i);
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
        lua_Unsigned in_count = lua_rawlen(L, 1);
        for (lua_Unsigned i = 1; i <= in_count; i++) {
            lua_rawgeti(L, 1, (lua_Integer)i);
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
    lua_Unsigned argc = lua_rawlen(L, 1);
    for (lua_Unsigned i = 1; i <= argc; i++) {
        lua_rawgeti(L, 1, (lua_Integer)i);
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

    char full_cmd[8192];
    if (workdir && workdir[0]) {
        char safe_workdir[PATH_MAX];
        {
            size_t wi = 0;
            for (const char *p = workdir; *p && wi < sizeof(safe_workdir) - 5; p++) {
                if (*p == '\'') {
                    safe_workdir[wi++] = '\''; safe_workdir[wi++] = '\\';
                    safe_workdir[wi++] = '\''; safe_workdir[wi++] = '\'';
                } else {
                    safe_workdir[wi++] = *p;
                }
            }
            safe_workdir[wi] = '\0';
        }
        int n = snprintf(full_cmd, sizeof(full_cmd), "cd '%s' && %s", safe_workdir, cmd);
        if (n < 0 || (size_t)n >= sizeof(full_cmd)) {
            lua_newtable(L);
            lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
            lua_pushliteral(L, "command too long"); lua_setfield(L, -2, "out");
            lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
            return 1;
        }
    } else {
        strncpy(full_cmd, cmd, sizeof(full_cmd) - 1);
        full_cmd[sizeof(full_cmd) - 1] = '\0';
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
    if (!out) {
        pclose(fp);
        lua_newtable(L);
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushliteral(L, "out of memory"); lua_setfield(L, -2, "out");
        lua_pushinteger(L, -1); lua_setfield(L, -2, "code");
        return 1;
    }
    out[0] = '\0';

    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        char *tmp = realloc(out, out_total + n + 1);
        if (!tmp) { free(out); out = NULL; break; }
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

    free(out);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * JSON Parser (minimal, for parsing node responses)
 * ═══════════════════════════════════════════════════════════════════ */

static const char *json_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *json_parse_string(const char *p, const char *end, char *buf, size_t bufsize) {
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t i = 0;
    while (p < end && *p != '"' && i < bufsize - 1) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case '"':  buf[i++] = '"';  break;
                case '\\': buf[i++] = '\\'; break;
                case '/':  buf[i++] = '/';  break;
                case 'n':  buf[i++] = '\n'; break;
                case 'r':  buf[i++] = '\r'; break;
                case 't':  buf[i++] = '\t'; break;
                case 'b':  buf[i++] = '\b'; break;
                case 'f':  buf[i++] = '\f'; break;
                default:   buf[i++] = *p;   break;
            }
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    if (p < end && *p == '"') p++;
    buf[i] = '\0';
    return p;
}

static const char *json_parse_value(lua_State *L, const char *p, const char *end);

static const char *json_parse_object(lua_State *L, const char *p, const char *end) {
    p = json_skip_ws(p, end);
    if (p >= end || *p != '{') return NULL;
    p++;
    lua_newtable(L);
    p = json_skip_ws(p, end);
    if (p < end && *p == '}') return p + 1;
    while (p < end) {
        p = json_skip_ws(p, end);
        char key[256];
        p = json_parse_string(p, end, key, sizeof(key));
        if (!p) { lua_pop(L, 1); return NULL; }
        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') { lua_pop(L, 1); return NULL; }
        p++;
        p = json_skip_ws(p, end);
        p = json_parse_value(L, p, end);
        if (!p) { lua_pop(L, 1); return NULL; }
        lua_setfield(L, -2, key);
        p = json_skip_ws(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') return p + 1;
        lua_pop(L, 1);
        return NULL;
    }
    lua_pop(L, 1);
    return NULL;
}

static const char *json_parse_value(lua_State *L, const char *p, const char *end) {
    if (p >= end) return NULL;
    if (*p == '"') {
        char buf[8192];
        p = json_parse_string(p, end, buf, sizeof(buf));
        if (!p) return NULL;
        lua_pushstring(L, buf);
        return p;
    }
    if (*p == '{') return json_parse_object(L, p, end);
    if (p + 4 <= end && memcmp(p, "true", 4) == 0) {
        lua_pushboolean(L, 1);
        return p + 4;
    }
    if (p + 5 <= end && memcmp(p, "false", 5) == 0) {
        lua_pushboolean(L, 0);
        return p + 5;
    }
    if (p + 4 <= end && memcmp(p, "null", 4) == 0) {
        lua_pushnil(L);
        return p + 4;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *endptr = NULL;
        double val = strtod(p, &endptr);
        if (endptr == p) return NULL;
        lua_pushnumber(L, val);
        return endptr;
    }
    return NULL;
}

static int n_json_parse(lua_State *L) {
    size_t len;
    const char *json = luaL_checklstring(L, 1, &len);
    int top = lua_gettop(L);
    const char *p = json_parse_value(L, json, json + len);
    if (!p) {
        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushstring(L, "json parse error");
        return 2;
    }
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
    {"find_sharp_std",   n_find_sharp_std},
    {"find_zigcc",       n_find_zigcc},
    {"home_dir",         n_home_dir},
    {"cwd",              n_cwd},
    {"chdir",            n_chdir},
    {"get_mtime",        n_get_mtime},
    {"current_platform", n_current_platform},
    {"start_cmd",        n_start_cmd},
    {"wait_task",        n_wait_task},
    {"remove",           n_remove},
    {"fingerprint",      n_fingerprint},
    {"self_fingerprint", n_self_fingerprint},
    {"cache_init",       n_cache_init},
    {"cache_get",        n_cache_get},
    {"cache_put",        n_cache_put},
    {"cache_stats",      n_cache_stats},
    {"cache_clear",      n_cache_clear},
    {"custom_needs_run", n_custom_needs_run},
    {"custom_exec",      n_custom_exec},
    {"http_post",        n_http_post},
    {"http_get",         n_http_get},
    {"colorize",         n_colorize},
    {"is_tty",           n_is_tty},
    {"tty_width",        n_tty_width},
    {"json_parse",       n_json_parse},
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
