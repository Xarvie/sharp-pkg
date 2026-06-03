/*
 * node.c — spkg-node: Distributed compilation server (Phase 3)
 *
 * Lightweight HTTP server that receives compile tasks and returns .o files.
 * Uses mongoose for HTTP handling. Pure C, no TLS, no external dependencies.
 *
 * Cross-platform: Windows (CreateProcess), Linux/macOS (fork/exec).
 * Any node compiles for any target via the --target flag in the request.
 * A Windows node can compile for Linux; a Linux node can compile for Windows.
 * Target triple in the request controls cross-compilation output format.
 *
 * Usage:
 *   spkg-node --listen 0.0.0.0:10080 --max-jobs 4 --sharpc /path/to/sharpc
 */

#define MG_ENABLE_LINES  1
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <errno.h>

/* ── Platform headers ──────────────────────────────────────────── */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
    #include <process.h>
    #include <direct.h>
    #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
    #endif
    #define popen  _popen
    #define pclose _pclose
    /* Windows lacks these POSIX signals; define sentinels if not already defined */
    #ifndef SIGKILL
    #define SIGKILL 9
    #endif
    #ifndef SIGTERM
    #define SIGTERM 15
    #endif
    #ifndef SIGPIPE
    #define SIGPIPE 13
    #endif
    #ifndef SIGINT
    #define SIGINT  2
    #endif
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <sys/select.h>
    #include <dirent.h>
    #include <signal.h>
#endif

/* ── Host platform detection ───────────────────────────────────── */
/* Compile-time host triple for the /health endpoint */

#ifdef _WIN32
    #ifdef _WIN64
        #ifdef __aarch64__
            #define HOST_TRIPLE "aarch64-windows-gnu"
        #else
            #define HOST_TRIPLE "x86_64-windows-gnu"
        #endif
    #else
        #define HOST_TRIPLE "i386-windows-gnu"
    #endif
#elif defined(__APPLE__)
    #ifdef __aarch64__
        #define HOST_TRIPLE "aarch64-macos-gnu"
    #else
        #define HOST_TRIPLE "x86_64-macos-gnu"
    #endif
#else /* Linux / other Unix */
    #ifdef __aarch64__
        #define HOST_TRIPLE "aarch64-linux-gnu"
    #elif defined(__arm__)
        #define HOST_TRIPLE "arm-linux-gnueabihf"
    #elif defined(__riscv)
        #if __riscv_xlen == 64
            #define HOST_TRIPLE "riscv64-linux-gnu"
        #else
            #define HOST_TRIPLE "riscv32-linux-gnu"
        #endif
    #elif defined(__loongarch64)
        #define HOST_TRIPLE "loongarch64-linux-gnu"
    #else
        #define HOST_TRIPLE "x86_64-linux-gnu"
    #endif
#endif

/* ── is_executable ─────────────────────────────────────────────── */

static int is_executable(const char *path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return 0;
    /* Check for .exe/.bat/.cmd extension as minimal executable test */
    size_t len = strlen(path);
    if (len > 4) {
        const char *ext = path + len - 4;
        if (_stricmp(ext, ".exe") == 0 ||
            _stricmp(ext, ".bat") == 0 ||
            _stricmp(ext, ".cmd") == 0)
            return 1;
    }
    return 1; /* Accept any file that exists and is not a directory */
#else
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return access(path, X_OK) == 0;
#endif
}

/* Find sharpc from SHARP_ROOT */
static const char *find_sharpc_path(char *buf, size_t sz) {
    const char *root = getenv("SHARP_ROOT");
    if (root && root[0]) {
#ifdef _WIN32
        size_t need = strlen(root) + 18; /* "/bin/sharpc.exe" + NUL */
#else
        size_t need = strlen(root) + 13; /* "/bin/sharpc" + NUL */
#endif
        if (need <= sz) {
            memcpy(buf, root, strlen(root));
#ifdef _WIN32
            memcpy(buf + strlen(root), "/bin/sharpc.exe", 18);
#else
            memcpy(buf + strlen(root), "/bin/sharpc", 13);
#endif
            if (is_executable(buf)) return buf;
        }
    }
    return NULL;
}

/* ── Configuration ──────────────────────────────────────────────── */

static const char *g_listen     = "http://0.0.0.0:10080";
static const char *g_sharpc     = "sharpc";
static int         g_max_jobs   = 4;
static volatile int g_running   = 1;
static volatile int g_active    = 0;
#define COMPILE_TIMEOUT_SEC 120

/* ── Minimal JSON extraction helpers ────────────────────────────── */

/* Extract a JSON string value for a given key (e.g. {"source": "hello"} → "hello") */
static int json_get_str(struct mg_str json, const char *key, char *buf, size_t bufsize) {
    char path[128];
    snprintf(path, sizeof(path), "$.%s", key);
    char *val = mg_json_get_str(json, path);
    if (!val) return 0;
    strncpy(buf, val, bufsize - 1);
    buf[bufsize - 1] = '\0';
    free(val);
    return 1;
}

/* Extract a JSON array of strings as pipe-separated value.
 * {"cflags":["-O2","-Iinc"]} → "-O2|-Iinc" */
static int json_get_cflags(struct mg_str json, char *buf, size_t bufsize) {
    buf[0] = '\0';
    size_t off = 0;

    /* Use mg_json_get to find the cflags array */
    int tok = mg_json_get(json, "$.cflags", NULL);
    if (tok < 0) return 0;

    const char *p = json.buf + tok;
    if (*p != '[') return 0;
    p++;
    const char *end = json.buf + json.len;

    while (p < end && *p != ']') {
        if (*p == '"') {
            p++;
            while (p < end && *p != '"' && off < bufsize - 2) {
                if (*p == '\\' && p + 1 < end) { p++; } /* skip escape */
                buf[off++] = *p++;
            }
            buf[off++] = '|';
            if (*p == '"') p++;
        }
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')) p++;
    }
    if (off > 0 && buf[off - 1] == '|') buf[--off] = '\0';
    else buf[off] = '\0';
    return 1;
}

/* ── Filesystem helpers ────────────────────────────────────────── */

/* Recursively create directories for a file path */
static int make_dirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
#ifdef _WIN32
    /* Normalize separators to backslash for Windows API */
    for (char *p = tmp; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') {
            char saved = *p;
            *p = '\0';
            /* Skip drive root (e.g., "C:") */
            if (strlen(tmp) >= 2 && tmp[1] == ':') { *p = saved; continue; }
            CreateDirectoryA(tmp, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(tmp, NULL);
#else
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
#endif
    return 1;
}

/* Recursively remove a directory tree */
static void remove_dir(const char *path) {
#ifdef _WIN32
    char search_path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(path);
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof(sub), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_dir(sub);
        } else {
            SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(sub);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryA(path);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *entry;
    char sub[PATH_MAX];
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
        /* Use stat() instead of d_type for portability (XFS without ftype) */
        struct stat st;
        if (entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN && stat(sub, &st) == 0 && S_ISDIR(st.st_mode))) {
            remove_dir(sub);
        } else {
            remove(sub);
        }
    }
    closedir(d);
    rmdir(path);
#endif
}

/* Parse headers JSON array, write to temp dir, return dir path (caller must free + remove_dir) */
static char *parse_headers(struct mg_str json) {
    int tok = mg_json_get(json, "$.headers", (int *)NULL);
    if (tok < 0) return NULL;  /* No headers field */
    const char *p = json.buf + tok;
    if (*p != '[') return NULL;

    /* Create temp directory */
#ifdef _WIN32
    char temp_path[MAX_PATH];
    char hdr_dir[MAX_PATH];
    if (!GetTempPathA(sizeof(temp_path), temp_path)) {
        strcpy(temp_path, ".");
    }
    /* Generate unique temp dir name */
    snprintf(hdr_dir, sizeof(hdr_dir), "%sspkg_hdr_%u_%u",
             temp_path, (unsigned)GetCurrentProcessId(), (unsigned)GetTickCount());
    if (!CreateDirectoryA(hdr_dir, NULL)) {
        /* Retry with different name */
        snprintf(hdr_dir, sizeof(hdr_dir), "%sspkg_hdr_%u_%u_%u",
                 temp_path, (unsigned)GetCurrentProcessId(),
                 (unsigned)GetTickCount(), (unsigned)rand());
        if (!CreateDirectoryA(hdr_dir, NULL)) return NULL;
    }
    char *hdr_copy = _strdup(hdr_dir);
#else
    char hdr_dir[] = "/tmp/spkg_hdr_XXXXXX";
    if (!mkdtemp(hdr_dir)) return NULL;
    char *hdr_copy = strdup(hdr_dir);
#endif
    if (!hdr_copy) {
#ifdef _WIN32
        RemoveDirectoryA(hdr_dir);
#else
        rmdir(hdr_dir);
#endif
        return NULL;
    }

    /* Parse array entries: {"path":"...","content":"..."} */
    p++;  /* skip '[' */
    const char *end = json.buf + json.len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (p >= end || *p == ']') break;
        if (*p != '{') { p++; continue; }

        int depth = 1;
        const char *obj_start = p;
        p++;
        while (p < end && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        struct mg_str obj = {(char *)obj_start, (size_t)(p - obj_start)};

        char *hpath = mg_json_get_str(obj, "$.path");
        char *hcontent = mg_json_get_str(obj, "$.content");
        if (hpath && hcontent) {
            char full_path[PATH_MAX];
#ifdef _WIN32
            snprintf(full_path, sizeof(full_path), "%s\\%s", hdr_copy, hpath);
            /* Normalize / to \ in the relative path part */
            for (char *cp = full_path; *cp; cp++) {
                if (*cp == '/') *cp = '\\';
            }
#else
            snprintf(full_path, sizeof(full_path), "%s/%s", hdr_copy, hpath);
#endif
            make_dirs(full_path);
            FILE *fp = fopen(full_path, "w");
            if (fp) { fputs(hcontent, fp); fclose(fp); }
        }
        free(hpath);
        free(hcontent);
    }
    return hdr_copy;
}

/* ── Object file validation (ELF / Mach-O / COFF) ─────────────── */

static int is_valid_object(const char *data, long osize) {
    if (osize < 4) return 0;

    /* ELF: starts with \x7fELF */
    if ((unsigned char)data[0] == 0x7f &&
        data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        return 1;
    }

    /* Mach-O (macOS): various magic numbers */
    if (osize >= 4) {
        unsigned char m0 = (unsigned char)data[0];
        unsigned char m1 = (unsigned char)data[1];
        unsigned char m2 = (unsigned char)data[2];
        unsigned char m3 = (unsigned char)data[3];
        /* 32-bit Mach-O */
        if ((m0 == 0xfe && m1 == 0xed && m2 == 0xfa && (m3 == 0xce || m3 == 0xcf)) ||
            (m0 == 0xce && m1 == 0xfa && m2 == 0xed && m3 == 0xfe) ||
            (m0 == 0xcf && m1 == 0xfa && m2 == 0xed && m3 == 0xfe))
            return 1;
        /* 64-bit Mach-O */
        if ((m0 == 0xfe && m1 == 0xed && m2 == 0xfa && m3 == 0xcf) ||
            (m0 == 0xcf && m1 == 0xfa && m2 == 0xed && m3 == 0xfe))
            return 1;
    }

    /* COFF / PE (Windows): check machine type field at offset 0 */
    /* COFF .obj files: IMAGE_FILE_HEADER starts with Machine (2 bytes LE) */
    if (osize >= 20) {
        unsigned short machine = (unsigned char)data[0] | ((unsigned char)data[1] << 8);
        /* Known COFF machine types */
        if (machine == 0x014C ||  /* IMAGE_FILE_MACHINE_I386 */
            machine == 0x8664 ||  /* IMAGE_FILE_MACHINE_AMD64 */
            machine == 0xAA64 ||  /* IMAGE_FILE_MACHINE_ARM64 */
            machine == 0x01C4 ||  /* IMAGE_FILE_MACHINE_ARMNT */
            machine == 0x0200 ||  /* IMAGE_FILE_MACHINE_IA64 */
            machine == 0x01C2 ||  /* IMAGE_FILE_MACHINE_THUMB */
            machine == 0x01A2 ||  /* IMAGE_FILE_MACHINE_SH3 */
            machine == 0x01A4 ||  /* IMAGE_FILE_MACHINE_SH4 */
            machine == 0x0162 ||  /* IMAGE_FILE_MACHINE_R3000 */
            machine == 0x0166 ||  /* IMAGE_FILE_MACHINE_R4000 */
            machine == 0x0168 ||  /* IMAGE_FILE_MACHINE_R10000 */
            machine == 0x0266 ||  /* IMAGE_FILE_MACHINE_MIPS16 */
            machine == 0x0366 ||  /* IMAGE_FILE_MACHINE_MIPSFPU */
            machine == 0x0466 ||  /* IMAGE_FILE_MACHINE_MIPSFPU16 */
            machine == 0x01F0 ||  /* IMAGE_FILE_MACHINE_POWERPC */
            machine == 0x01F1 ||  /* IMAGE_FILE_MACHINE_POWERPCFP */
            machine == 0x00E0)    /* IMAGE_FILE_MACHINE_RISCV32/64... actually 0x5064 for RISCV64 */
            return 1;
        /* RISCV64 = 0x5064, RISCV32 = 0x5032 */
        if (machine == 0x5064 || machine == 0x5032) return 1;
        /* LoongArch = 0x6264 */
        if (machine == 0x6264) return 1;
    }

    return 0;
}

/* ── Compile task execution ─────────────────────────────────────── */

#ifdef _WIN32
/* ── Windows: CreateProcess-based compilation ─────────────────── */

static int compile_task(const char *source_content, const char *cflags_str,
                        const char *optimize, const char *target,
                        char *out_path, size_t out_path_size,
                        char *depfile_content, size_t depfile_buf_size,
                        char *error_out, size_t error_buf_size,
                        const char *src_ext) {
    char temp_path[MAX_PATH];
    if (!GetTempPathA(sizeof(temp_path), temp_path)) {
        strcpy(temp_path, ".");
    }

    /* Create temp paths for source and output */
    char src_base[MAX_PATH], src_path[MAX_PATH];
    char out_base[MAX_PATH];
    unsigned pid = (unsigned)GetCurrentProcessId();
    unsigned tick = (unsigned)GetTickCount();
    static unsigned seq = 0;
    seq++;

    snprintf(src_base, sizeof(src_base), "%sspkg_src_%u_%u_%u",
             temp_path, pid, tick, seq);
    snprintf(src_path, sizeof(src_path), "%s.%s", src_base, src_ext);
    snprintf(out_base, sizeof(out_base), "%sspkg_out_%u_%u_%u",
             temp_path, pid, tick, seq);
    snprintf(out_path, out_path_size, "%s.o", out_base);

    /* Write source to temp file */
    FILE *fp = fopen(src_path, "w");
    if (!fp) { return -1; }
    fputs(source_content, fp);
    fclose(fp);

    /* Build command args directly (no shell wrapping needed) */
    const char *opt_flag = "-O0";
    if (optimize && optimize[0]) {
        if (strcmp(optimize, "ReleaseSafe") == 0) opt_flag = "-O1";
        else if (strcmp(optimize, "ReleaseFast") == 0) opt_flag = "-O2";
        else if (strcmp(optimize, "ReleaseSmall") == 0) opt_flag = "-Os";
    }

    /* Build cflags: split pipe-separated into space-separated args */
    char cflags_safe[4096];
    if (cflags_str && cflags_str[0]) {
        size_t ci = 0;
        size_t token_start = 0;
        for (size_t i = 0; ; i++) {
            if (cflags_str[i] == '|' || cflags_str[i] == '\0') {
                if (i > token_start && ci + 3 < sizeof(cflags_safe)) {
                    if (ci > 0) cflags_safe[ci++] = ' ';
                    if (cflags_str[token_start] == '"') {
                        /* Already quoted token */
                        for (size_t j = token_start; j < i && ci + 2 < sizeof(cflags_safe); j++) {
                            cflags_safe[ci++] = cflags_str[j];
                        }
                    } else {
                        cflags_safe[ci++] = '"';
                        for (size_t j = token_start; j < i && ci + 3 < sizeof(cflags_safe); j++) {
                            if (cflags_str[j] == '"') {
                                cflags_safe[ci++] = '\\'; cflags_safe[ci++] = '"';
                            } else {
                                cflags_safe[ci++] = cflags_str[j];
                            }
                        }
                        cflags_safe[ci++] = '"';
                    }
                }
                token_start = i + 1;
                if (cflags_str[i] == '\0') break;
            }
        }
        cflags_safe[ci] = '\0';
    } else {
        cflags_safe[0] = '\0';
    }

    char depfile[MAX_PATH + 16];
    snprintf(depfile, sizeof(depfile), "%s.d", out_path);

    char target_flag[256] = "";
    if (target && target[0]) {
        snprintf(target_flag, sizeof(target_flag), " --target %s", target);
    }

    /* Build command line for CreateProcess */
    char cmdline[32768];
    int n = snprintf(cmdline, sizeof(cmdline),
                     "\"%s\" -c %s %s%s -MMD -MF \"%s\" \"%s\" -o \"%s\"",
                     g_sharpc, opt_flag, cflags_safe, target_flag,
                     depfile, src_path, out_path);
    if (n < 0 || (size_t)n >= sizeof(cmdline)) {
        DeleteFileA(src_path);
        if (error_out && error_buf_size > 0)
            snprintf(error_out, error_buf_size, "command line too long");
        return -1;
    }

    /* Create pipe for stdout/stderr capture */
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        DeleteFileA(src_path);
        if (error_out && error_buf_size > 0)
            snprintf(error_out, error_buf_size, "CreatePipe failed: %lu", GetLastError());
        return -1;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = { 0 };

    /* CreateProcess needs a mutable command line buffer */
    BOOL created = CreateProcessA(
        NULL,             /* application name (from command line) */
        cmdline,          /* command line (mutable) */
        NULL, NULL,       /* process/thread security */
        TRUE,             /* inherit handles (for pipe) */
        CREATE_NO_WINDOW, /* creation flags */
        NULL, NULL,       /* environment / current directory */
        &si, &pi
    );

    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    if (!created) {
        CloseHandle(hReadPipe);
        DeleteFileA(src_path);
        if (error_out && error_buf_size > 0)
            snprintf(error_out, error_buf_size, "CreateProcess failed: %lu", GetLastError());
        return -1;
    }

    CloseHandle(pi.hThread);

    /* Read stdout/stderr with timeout */
    char err_buf[4096];
    size_t err_total = 0;
    time_t start = time(NULL);
    int ret = -1;

    while (1) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 100); /* 100ms poll */
        if (wait == WAIT_OBJECT_0) {
            /* Process exited */
            DWORD exit_code = 0;
            if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
                ret = (exit_code == 0) ? 0 : -1;
            }
            break;
        }

        /* Read available pipe data */
        DWORD avail = 0;
        if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            char tmp[4096];
            DWORD nread = 0;
            DWORD to_read = (avail < sizeof(tmp) - 1) ? avail : (sizeof(tmp) - 1);
            if (ReadFile(hReadPipe, tmp, to_read, &nread, NULL) && nread > 0) {
                if (err_total + nread < sizeof(err_buf) - 1) {
                    memcpy(err_buf + err_total, tmp, nread);
                    err_total += nread;
                    err_buf[err_total] = '\0';
                }
            }
        }

        if (time(NULL) - start >= COMPILE_TIMEOUT_SEC) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            if (error_out && error_buf_size > 0) {
                snprintf(error_out, error_buf_size,
                         "compilation timed out after %d seconds", COMPILE_TIMEOUT_SEC);
            }
            ret = -1;
            break;
        }
    }

    /* Drain remaining pipe data */
    {
        char tmp[4096];
        DWORD nread;
        while (ReadFile(hReadPipe, tmp, sizeof(tmp) - 1, &nread, NULL) && nread > 0) {
            if (err_total + nread < sizeof(err_buf) - 1) {
                memcpy(err_buf + err_total, tmp, nread);
                err_total += nread;
                err_buf[err_total] = '\0';
            }
        }
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);

    if (error_out && error_buf_size > 0 && err_total > 0) {
        memcpy(error_out, err_buf, err_total);
        error_out[err_total] = '\0';
        while (err_total > 0 && (error_out[err_total-1] == '\n' ||
               error_out[err_total-1] == '\r' || error_out[err_total-1] == ' ')) {
            error_out[--err_total] = '\0';
        }
    }

    /* Clean up source */
    DeleteFileA(src_path);

    if (ret != 0) return -1;

    /* Read depfile if exists */
    depfile_content[0] = '\0';
    FILE *dfp = fopen(depfile, "r");
    if (dfp) {
        size_t n = fread(depfile_content, 1, depfile_buf_size - 1, dfp);
        depfile_content[n] = '\0';
        fclose(dfp);
        DeleteFileA(depfile);
    }

    return 0;
}

#else
/* ── POSIX: fork/exec-based compilation ────────────────────────── */

static int compile_task(const char *source_content, const char *cflags_str,
                        const char *optimize, const char *target,
                        char *out_path, size_t out_path_size,
                        char *depfile_content, size_t depfile_buf_size,
                        char *error_out, size_t error_buf_size,
                        const char *src_ext) {
    char src_template[] = "/tmp/spkg_src_XXXXXX";
    char out_template[] = "/tmp/spkg_out_XXXXXX";

    int src_fd = mkstemp(src_template);
    if (src_fd < 0) return -1;
    close(src_fd);

    int out_fd = mkstemp(out_template);
    if (out_fd < 0) {
        remove(src_template);
        return -1;
    }
    close(out_fd);

    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s.%s", src_template, src_ext);
    rename(src_template, src_path);
    snprintf(out_path, out_path_size, "%s.o", out_template);

    remove(out_template);

    /* Write source to temp file */
    FILE *fp = fopen(src_path, "w");
    if (!fp) { remove(src_path); return -1; }
    fputs(source_content, fp);
    fclose(fp);

    /* Build command: -c for object file (gcc compatible) */
    char cmd[8192];
    char depfile[1024];
    snprintf(depfile, sizeof(depfile), "%s.d", out_path);

    const char *opt_flag = "-O0";
    if (optimize && optimize[0]) {
        if (strcmp(optimize, "ReleaseSafe") == 0) opt_flag = "-O1";
        else if (strcmp(optimize, "ReleaseFast") == 0) opt_flag = "-O2";
        else if (strcmp(optimize, "ReleaseSmall") == 0) opt_flag = "-Os";
    }

    /* Shell-escape pipe-separated cflags tokens into single-quoted args */
    char cflags_safe[4096];
    if (cflags_str && cflags_str[0]) {
        size_t ci = 0;
        size_t token_start = 0;
        for (size_t i = 0; ; i++) {
            if (cflags_str[i] == '|' || cflags_str[i] == '\0') {
                if (i > token_start && ci + 4 < sizeof(cflags_safe)) {
                    if (ci > 0) cflags_safe[ci++] = ' ';
                    cflags_safe[ci++] = '\'';
                    for (size_t j = token_start; j < i && ci + 4 < sizeof(cflags_safe); j++) {
                        if (cflags_str[j] == '\'') {
                            cflags_safe[ci++] = '\''; cflags_safe[ci++] = '\\';
                            cflags_safe[ci++] = '\''; cflags_safe[ci++] = '\'';
                        } else {
                            cflags_safe[ci++] = cflags_str[j];
                        }
                    }
                    cflags_safe[ci++] = '\'';
                }
                token_start = i + 1;
                if (cflags_str[i] == '\0') break;
            }
        }
        cflags_safe[ci] = '\0';
    } else {
        cflags_safe[0] = '\0';
    }

    char target_flag[256] = "";
    if (target && target[0]) {
        snprintf(target_flag, sizeof(target_flag), " --target %s", target);
    }

    snprintf(cmd, sizeof(cmd), "%s -c %s %s%s -MMD -MF \"%s\" \"%s\" -o \"%s\" 2>&1",
             g_sharpc, opt_flag, cflags_safe, target_flag,
             depfile, src_path, out_path);

    /* Capture output with timeout */
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        if (error_out && error_buf_size > 0)
            snprintf(error_out, error_buf_size, "pipe failed: %s", strerror(errno));
        remove(src_path);
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipe_fd[0]); close(pipe_fd[1]);
        if (error_out && error_buf_size > 0)
            snprintf(error_out, error_buf_size, "fork failed: %s", strerror(errno));
        remove(src_path);
        return -1;
    }

    int ret = -1;
    if (child == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipe_fd[1]);

    char err_buf[4096];
    size_t err_total = 0;
    time_t start = time(NULL);

    while (1) {
        int status;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w < 0) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd[0], &rfds);
        struct timeval tv = { 0, 100000 };  /* 100ms */

        if (select(pipe_fd[0] + 1, &rfds, NULL, NULL, &tv) > 0) {
            char tmp[4096];
            ssize_t nr = read(pipe_fd[0], tmp, sizeof(tmp));
            if (nr > 0) {
                size_t space = sizeof(err_buf) - 1 - err_total;
                if ((size_t)nr < space) {
                    memcpy(err_buf + err_total, tmp, (size_t)nr);
                    err_total += (size_t)nr;
                    err_buf[err_total] = '\0';
                }
            }
        }

        if (w > 0) {
            ret = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
            break;
        }

        if (time(NULL) - start >= COMPILE_TIMEOUT_SEC) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            if (error_out && error_buf_size > 0) {
                snprintf(error_out, error_buf_size, "compilation timed out after %d seconds", COMPILE_TIMEOUT_SEC);
            }
            break;
        }
    }

    /* Drain remaining pipe data */
    {
        char tmp[4096];
        ssize_t nr;
        while ((nr = read(pipe_fd[0], tmp, sizeof(tmp))) > 0) {
            if (err_total + (size_t)nr < sizeof(err_buf) - 1) {
                memcpy(err_buf + err_total, tmp, (size_t)nr);
                err_total += (size_t)nr;
                err_buf[err_total] = '\0';
            }
        }
    }
    close(pipe_fd[0]);

    if (error_out && error_buf_size > 0 && err_total > 0) {
        memcpy(error_out, err_buf, err_total);
        error_out[err_total] = '\0';
        while (err_total > 0 && (error_out[err_total-1] == '\n' || error_out[err_total-1] == '\r' || error_out[err_total-1] == ' ')) {
            error_out[--err_total] = '\0';
        }
    }

    /* Clean up source */
    remove(src_path);

    if (ret != 0) return -1;

    /* Read depfile if exists */
    depfile_content[0] = '\0';
    FILE *dfp = fopen(depfile, "r");
    if (dfp) {
        size_t n = fread(depfile_content, 1, depfile_buf_size - 1, dfp);
        depfile_content[n] = '\0';
        fclose(dfp);
        remove(depfile);
    }

    return 0;
}
#endif

/* ── HTTP handler ───────────────────────────────────────────────── */

static int uri_eq(struct mg_str uri, const char *s) {
    return uri.len == strlen(s) && memcmp(uri.buf, s, uri.len) == 0;
}

static void send_json(struct mg_connection *c, int code, const char *body) {
    mg_http_reply(c, code, "Content-Type: application/json\r\n", "%s", body);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    /* Route: GET /health */
    if (uri_eq(hm->uri, "/health")) {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"active\":%d,\"max_jobs\":%d,\"host\":\"%s\"}",
                 g_active, g_max_jobs, HOST_TRIPLE);
        send_json(c, 200, body);
        return;
    }

    /* Route: POST /compile */
    if (uri_eq(hm->uri, "/compile")) {
        if (hm->body.len > 10 * 1024 * 1024) {
            send_json(c, 413,
                      "{\"status\":\"error\",\"code\":413,"
                      "\"stderr\":\"request body too large (max 10 MB)\"}");
            return;
        }
        if (g_active >= g_max_jobs) {
            send_json(c, 503,
                      "{\"status\":\"error\",\"code\":503,"
                      "\"stderr\":\"node busy (max_jobs reached)\"}");
            return;
        }

        /* Parse JSON body - use mg_json_get_str for dynamic allocation */
        char *src_ptr = mg_json_get_str(hm->body, "$.source");
        if (!src_ptr) {
            src_ptr = mg_json_get_str(hm->body, ".source");
        }

        if (!src_ptr) {
            send_json(c, 400,
                      "{\"status\":\"error\",\"code\":400,"
                      "\"stderr\":\"missing 'source' field\"}");
            return;
        }
        const char *source = src_ptr;

        char cflags[2048] = {0};
        char optimize[64] = {0};
        char src_ext[16] = "ce";
        json_get_cflags(hm->body, cflags, sizeof(cflags));
        json_get_str(hm->body, "optimize", optimize, sizeof(optimize));

        {
            char tmp_ext[16];
            if (json_get_str(hm->body, "src_ext", tmp_ext, sizeof(tmp_ext)) && tmp_ext[0]) {
                int valid = 1;
                for (int i = 0; tmp_ext[i]; i++) {
                    if (!((tmp_ext[i] >= 'a' && tmp_ext[i] <= 'z') ||
                          (tmp_ext[i] >= 'A' && tmp_ext[i] <= 'Z') ||
                          (tmp_ext[i] >= '0' && tmp_ext[i] <= '9') ||
                          tmp_ext[i] == '+' || tmp_ext[i] == '_' ||
                          tmp_ext[i] == '-'))
                        valid = 0;
                }
                if (valid) {
                    size_t need = strlen(tmp_ext) + 1;
                    if (need <= sizeof(src_ext)) memcpy(src_ext, tmp_ext, need);
                }
            }
        }

        /* Parse headers and create temp header dir */
        char *hdr_dir = parse_headers(hm->body);
        if (hdr_dir) {
            size_t len = strlen(cflags);
            size_t need = strlen(hdr_dir) + 4;  /* -I + path */
            if (len + need < sizeof(cflags)) {
                if (len > 0) { cflags[len] = '|'; len++; }
                cflags[len++] = '-'; cflags[len++] = 'I';
                memcpy(cflags + len, hdr_dir, need - 3);
                len += need - 3;
                cflags[len] = '\0';
            }
        }

        /* Parse target (already in zig format from client) */
        char target[128] = {0};
        {
            char *tgt = mg_json_get_str(hm->body, "$.target");
            if (!tgt) tgt = mg_json_get_str(hm->body, ".target");
            if (tgt) {
                strncpy(target, tgt, sizeof(target) - 1);
                target[sizeof(target) - 1] = '\0';
                free(tgt);
            }
        }

        /* Execute compilation */
        g_active++;

        char out_path[512];
        char depfile_content[4096];
        char error_out[4096] = {0};
        int rc = compile_task(source, cflags,
                              optimize[0] ? optimize : NULL,
                              target[0] ? target : NULL,
                              out_path, sizeof(out_path),
                              depfile_content, sizeof(depfile_content),
                              error_out, sizeof(error_out),
                              src_ext);

        g_active--;

        if (rc != 0) {
            /* Escape error message for JSON */
            char err_escaped[2048];
            size_t ei = 0;
            const char *err_src = (error_out[0]) ? error_out : "compilation failed";
            for (size_t i = 0; err_src[i] && ei < sizeof(err_escaped) - 4; i++) {
                if (err_src[i] == '\n') { err_escaped[ei++] = '\\'; err_escaped[ei++] = 'n'; }
                else if (err_src[i] == '\r') { err_escaped[ei++] = '\\'; err_escaped[ei++] = 'r'; }
                else if (err_src[i] == '\\') { err_escaped[ei++] = '\\'; err_escaped[ei++] = '\\'; }
                else if (err_src[i] == '"') { err_escaped[ei++] = '\\'; err_escaped[ei++] = '"'; }
                else { err_escaped[ei++] = err_src[i]; }
            }
            err_escaped[ei] = '\0';

            /* Build error response with memcpy to avoid truncation warning */
            const char *prefix = "{\"status\":\"error\",\"code\":1,\"stderr\":\"";
            const char *suffix = "\"}";
            size_t resp_size = strlen(prefix) + ei + strlen(suffix) + 1;
            char *err_resp = (char *)malloc(resp_size);
            if (!err_resp) { goto cleanup; }
            char *p = err_resp;
            size_t n = strlen(prefix);
            memcpy(p, prefix, n); p += n;
            memcpy(p, err_escaped, ei); p += ei;
            n = strlen(suffix);
            memcpy(p, suffix, n + 1);

            send_json(c, 500, err_resp);
            free(err_resp);
            goto cleanup;
        }

        /* Read .o file */
        FILE *fp = fopen(out_path, "rb");
        if (!fp) {
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":2,"
                      "\"stderr\":\"cannot read output .o\"}");
            goto cleanup;
        }
        fseek(fp, 0, SEEK_END);
        long osize = ftell(fp);
        if (osize < 0) {
            fclose(fp);
            goto cleanup;
        }
        fseek(fp, 0, SEEK_SET);

        char *odata = (char *)malloc(osize);
        if (!odata) {
            fclose(fp);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":3,"
                      "\"stderr\":\"out of memory\"}");
            goto cleanup;
        }
        size_t nread = fread(odata, 1, osize, fp);
        fclose(fp);
        if ((long)nread != osize) {
            free(odata);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":4,"
                      "\"stderr\":\"read .o file failed\"}");
            goto cleanup;
        }

        /* Validate output: non-empty and has valid object file magic (ELF/Mach-O/COFF) */
        if (!is_valid_object(odata, osize)) {
            free(odata);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":4,"
                      "\"stderr\":\"compilation produced invalid object file\"}");
            goto cleanup;
        }

        /* Base64 encode */
        char *b64 = NULL;
        size_t b64_len = 0;
        /* Calculate required base64 buffer size: ceil(osize/3)*4 + 1 */
        size_t b64_size = ((osize + 2) / 3) * 4 + 1;
        b64 = (char *)malloc(b64_size);
        if (!b64) {
            free(odata);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":5,"
                      "\"stderr\":\"out of memory\"}");
            goto cleanup;
        }
        b64_len = mg_base64_encode((unsigned char *)odata, (size_t)osize, b64, b64_size);
        free(odata);
        if (b64_len == 0) {
            free(b64);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":6,"
                      "\"stderr\":\"base64 encode failed\"}");
            goto cleanup;
        }

        /* Escape depfile for JSON */
        char dep_escaped[8192];
        size_t di = 0;
        for (size_t i = 0; depfile_content[i] && di < sizeof(dep_escaped) - 2; i++) {
            if (depfile_content[i] == '\n') {
                dep_escaped[di++] = '\\'; dep_escaped[di++] = 'n';
            } else if (depfile_content[i] == '\\') {
                dep_escaped[di++] = '\\'; dep_escaped[di++] = '\\';
            } else if (depfile_content[i] == '"') {
                dep_escaped[di++] = '\\'; dep_escaped[di++] = '"';
            } else {
                dep_escaped[di++] = depfile_content[i];
            }
        }
        dep_escaped[di] = '\0';

        /* Build JSON response manually to avoid snprintf truncation warning */
        const char *prefix = "{\"status\":\"ok\",\"output\":\"";
        const char *mid = "\",\"depfile\":\"";
        const char *suffix = "\",\"cached\":false}";
        size_t resp_size = strlen(prefix) + b64_len + strlen(mid) + di + strlen(suffix) + 1;
        char *resp = (char *)malloc(resp_size);
        if (!resp) {
            free(b64);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":7,"
                      "\"stderr\":\"out of memory\"}");
            goto cleanup;
        }
        char *p = resp;
        size_t n = strlen(prefix);
        memcpy(p, prefix, n); p += n;
        memcpy(p, b64, b64_len); p += b64_len;
        n = strlen(mid);
        memcpy(p, mid, n); p += n;
        memcpy(p, dep_escaped, di); p += di;
        n = strlen(suffix);
        memcpy(p, suffix, n); p += n;
        *p = '\0';

        send_json(c, 200, resp);
        free(b64);
        free(resp);

        /* fall through to cleanup for temp file removal */

cleanup:
        if (out_path[0]) {
#ifdef _WIN32
            char depfile[520];
            DeleteFileA(out_path);
            snprintf(depfile, sizeof(depfile), "%s.d", out_path);
            DeleteFileA(depfile);
            /* Also clean up temp source if it still exists */
            /* src_path is scoped to compile_task, handled there */
#else
            char depfile[520];
            remove(out_path);
            snprintf(depfile, sizeof(depfile), "%s.d", out_path);
            remove(depfile);
#endif
        }
        if (hdr_dir) {
            remove_dir(hdr_dir);
            free(hdr_dir);
        }
        free(src_ptr);
        return;
    }

    /* Unknown route */
    send_json(c, 404, "{\"status\":\"error\",\"code\":404,\"stderr\":\"unknown route\"}");
}

/* ── Signal handlers ────────────────────────────────────────────── */

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    g_running = 0;
    return TRUE;
}
#else
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}
#endif

/* ── CLI parsing ─────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            g_listen = argv[++i];
        } else if (strcmp(argv[i], "--max-jobs") == 0 && i + 1 < argc) {
            char *end = NULL;
            long val = strtol(argv[++i], &end, 10);
            if (*end != '\0' || val <= 0) val = 1;
            if (val > 64) val = 64;
            g_max_jobs = (int)val;
        } else if (strcmp(argv[i], "--sharpc") == 0 && i + 1 < argc) {
            g_sharpc = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("spkg-node — Distributed compilation server\n\n"
                   "Usage: spkg-node [options]\n\n"
                   "  --listen <addr:port>  listen address (default: http://0.0.0.0:10080)\n"
                   "  --max-jobs <N>        max concurrent compilations (default: 4)\n"
                   "  --sharpc <path>       sharpc compiler path (default: sharpc)\n"
                   "  -h, --help            show this help\n");
            return 0;
        } else {
            fprintf(stderr, "spkg-node: unknown option '%s'. Try --help.\n", argv[i]);
            return 1;
        }
    }

    /* Register signal handlers */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
    /* Ignore SIGPIPE-equivalent on Windows */
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
#endif

    /* Resolve sharpc from SHARP_ROOT if not explicitly set */
    static char sharpc_buf[PATH_MAX];
    if (strcmp(g_sharpc, "sharpc") == 0) {
        const char *found = find_sharpc_path(sharpc_buf, sizeof(sharpc_buf));
        if (found) {
            g_sharpc = found;
        } else {
#ifdef _WIN32
            fprintf(stderr,
                "ERROR: cannot find sharpc. Set SHARP_ROOT to your sharp "
                "installation root (containing bin/sharpc.exe, std/, zig/).\n");
#else
            fprintf(stderr,
                "ERROR: cannot find sharpc. Set SHARP_ROOT to your sharp "
                "installation root (containing bin/sharpc, std/, zig/).\n");
#endif
            return 1;
        }
    }

    /* Start mongoose */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection *c = mg_http_listen(&mgr, g_listen, ev_handler, &mgr);
    if (!c) {
        fprintf(stderr, "spkg-node: failed to listen on %s\n", g_listen);
        mg_mgr_free(&mgr);
        return 1;
    }

    printf("spkg-node: listening on %s (max_jobs=%d, sharpc=%s)\n",
           g_listen, g_max_jobs, g_sharpc);
#ifdef _WIN32
    printf("spkg-node: press Ctrl+C or close window to stop\n");
#else
    printf("spkg-node: press Ctrl+C to stop\n");
#endif

    /* Event loop */
    while (g_running) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nspkg-node: shutting down\n");
    mg_mgr_free(&mgr);
    return 0;
}