/*
 * node.c — spkg-node: Distributed compilation server (Phase 3)
 *
 * Lightweight HTTP server that receives compile tasks and returns .o files.
 * Uses mongoose for HTTP handling. Pure C, no TLS, no external dependencies.
 *
 * Usage:
 *   spkg-node --listen 0.0.0.0:10080 --max-jobs 4 --sharpc /path/to/sharpc
 */

#define MG_ENABLE_LINES  1
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

static const char *find_path_sep(const char *p) {
    for (; *p; p++) {
        if (*p == PATH_SEP) return p;
#ifdef _WIN32
        if (*p == '/') return p;
#endif
    }
    return NULL;
}

static int is_executable(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
#ifdef _WIN32
    const char *ext = strrchr(path, '.');
    return (ext && (strcmp(ext, ".exe") == 0 || strcmp(ext, ".bat") == 0 ||
                    strcmp(ext, ".cmd") == 0));
#else
    /* Check execute permission */
    return access(path, X_OK) == 0;
#endif
}

static int resolve_self_exe(char *buf, size_t sz) {
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
    if (n > 0) { buf[n] = '\0'; return 1; }
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)sz;
    if (_NSGetExecutablePath(buf, &n) == 0) return 1;
#elif defined(_WIN32)
    if (GetModuleFileNameA(NULL, buf, (DWORD)sz) > 0) return 1;
#else
    (void)buf; (void)sz;
#endif
    return 0;
}

/* Try to find sharpc relative to node binary */
static const char *find_sharpc_path(char *buf, size_t sz) {
    const char *env = getenv("SHARPC");
    if (env && is_executable(env)) return env;

    char self_exe[PATH_MAX];
    if (!resolve_self_exe(self_exe, sizeof(self_exe))) return NULL;

    /* Find the LAST separator (strrchr), not the first */
    char *slash = strrchr(self_exe, PATH_SEP);
#ifdef _WIN32
    if (!slash) slash = strrchr(self_exe, '/');
#endif
    if (!slash) return NULL;
    *slash = '\0';
    size_t dirlen = strlen(self_exe);

#ifdef _WIN32
    const char *rel[] = {
        "..\\..\\sharpc\\bin\\sharpc.exe",
        "..\\..\\..\\build\\sharpc.exe",
        "..\\..\\build\\sharpc.exe",
        "..\\build\\sharpc.exe",
        "..\\sharpc.exe",
        NULL
    };
#else
    const char *rel[] = {
        "../../sharpc/bin/sharpc",
        "../../../build/sharpc",
        "../../build/sharpc",
        "../build/sharpc",
        "../sharpc",
        NULL
    };
#endif
    for (int i = 0; rel[i]; i++) {
        size_t rlen = strlen(rel[i]);
        size_t need = dirlen + 1 + rlen + 1;
        if (need > sz) continue;
        memcpy(buf, self_exe, dirlen);
        buf[dirlen] = PATH_SEP;
        memcpy(buf + dirlen + 1, rel[i], rlen + 1);
        fprintf(stderr, "[DEBUG] trying sharpc: %s\n", buf);
        if (is_executable(buf)) return buf;
    }
    return NULL;
}

/* ── Configuration ──────────────────────────────────────────────── */

static const char *g_listen     = "http://0.0.0.0:10080";
static const char *g_sharpc     = "sharpc";
static int         g_max_jobs   = 4;
static volatile int g_running   = 1;
static volatile int g_active    = 0;

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

/* ── Compile task execution ─────────────────────────────────────── */

static int compile_task(const char *source_content, const char *cflags_str,
                        const char *optimize, char *out_path, size_t out_path_size,
                        char *depfile_content, size_t depfile_buf_size,
                        char *error_out, size_t error_buf_size) {
#ifdef _WIN32
    char src_path[MAX_PATH];
    char tmp_dir[MAX_PATH];
    DWORD tmp_len = GetTempPathA(sizeof(tmp_dir), tmp_dir);
    if (tmp_len == 0 || tmp_len >= sizeof(tmp_dir)) return -1;
    char tmp_name[MAX_PATH];
    if (GetTempFileNameA(tmp_dir, "spk", 0, tmp_name) == 0) return -1;
    snprintf(src_path, sizeof(src_path), "%s", tmp_name);
    if (GetTempFileNameA(tmp_dir, "spk", 0, tmp_name) == 0) return -1;
    snprintf(out_path, out_path_size, "%s", tmp_name);
#else
    char src_template[] = "/tmp/spkg_src_XXXXXX";
    char out_template[] = "/tmp/spkg_out_XXXXXX";

    /* Create unique temp files using mkstemp (template must end with XXXXXX) */
    int src_fd = mkstemp(src_template);
    if (src_fd < 0) return -1;
    close(src_fd);

    int out_fd = mkstemp(out_template);
    if (out_fd < 0) {
        remove(src_template);
        return -1;
    }
    close(out_fd);

    /* Build actual paths with extensions */
    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s.sp", src_template);
    rename(src_template, src_path);
    snprintf(out_path, out_path_size, "%s.o", out_template);
#endif

    /* Write source to temp file */
    FILE *fp = fopen(src_path, "w");
    if (!fp) { return -1; }
    fputs(source_content, fp);
    fclose(fp);

    /* Build command: -c for object file (gcc compatible) */
    char cmd[4096];
    char depfile[512];
    snprintf(depfile, sizeof(depfile), "%s.d", out_path);

    const char *opt_flag = "-O0";
    if (optimize && optimize[0]) {
        if (strcmp(optimize, "ReleaseSafe") == 0) opt_flag = "-O1";
        else if (strcmp(optimize, "ReleaseFast") == 0) opt_flag = "-O2";
        else if (strcmp(optimize, "ReleaseSmall") == 0) opt_flag = "-Os";
    }

    /* Convert pipe-separated cflags to space-separated for shell execution */
    char cflags_safe[2048];
    if (cflags_str && cflags_str[0]) {
        size_t ci = 0;
        for (size_t i = 0; cflags_str[i] && ci < sizeof(cflags_safe) - 1; i++) {
            cflags_safe[ci++] = (cflags_str[i] == '|') ? ' ' : cflags_str[i];
        }
        cflags_safe[ci] = '\0';
    } else {
        cflags_safe[0] = '\0';
    }

    snprintf(cmd, sizeof(cmd), "%s -c %s %s -MMD -MF \"%s\" \"%s\" -o \"%s\" 2>&1",
             g_sharpc, opt_flag, cflags_safe,
             depfile, src_path, out_path);

    /* Capture output */
    FILE *pfp = popen(cmd, "r");
    int ret = -1;
    if (pfp) {
        if (error_out && error_buf_size > 0) {
            size_t n = fread(error_out, 1, error_buf_size - 1, pfp);
            error_out[n] = '\0';
            /* Trim trailing whitespace */
            while (n > 0 && (error_out[n-1] == '\n' || error_out[n-1] == '\r' || error_out[n-1] == ' ')) {
                error_out[--n] = '\0';
            }
        }
        int status = pclose(pfp);
#ifdef _WIN32
        ret = (status == 0) ? 0 : -1;
#else
        ret = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
#endif
    } else {
        if (error_out && error_buf_size > 0) {
            snprintf(error_out, error_buf_size, "popen failed: %s", strerror(errno));
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
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"active\":%d,\"max_jobs\":%d}",
                 g_active, g_max_jobs);
        send_json(c, 200, body);
        return;
    }

    /* Route: POST /compile */
    if (uri_eq(hm->uri, "/compile")) {
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
        json_get_cflags(hm->body, cflags, sizeof(cflags));
        json_get_str(hm->body, "optimize", optimize, sizeof(optimize));

        /* Execute compilation */
        g_active++;

        char out_path[512];
        char depfile_content[4096];
        char error_out[4096];
        int rc = compile_task(source, cflags,
                              optimize[0] ? optimize : NULL,
                              out_path, sizeof(out_path),
                              depfile_content, sizeof(depfile_content),
                              error_out, sizeof(error_out));

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
            if (!err_resp) { free(src_ptr); return; }
            char *p = err_resp;
            size_t n = strlen(prefix);
            memcpy(p, prefix, n); p += n;
            memcpy(p, err_escaped, ei); p += ei;
            n = strlen(suffix);
            memcpy(p, suffix, n + 1);

            send_json(c, 500, err_resp);
            free(err_resp);
            free(src_ptr);
            return;
        }

        free(src_ptr);

        /* Read .o file */
        FILE *fp = fopen(out_path, "rb");
        if (!fp) {
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":2,"
                      "\"stderr\":\"cannot read output .o\"}");
            remove(out_path);
            return;
        }
        fseek(fp, 0, SEEK_END);
        long osize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        char *odata = (char *)malloc(osize);
        if (!odata) {
            fclose(fp); remove(out_path);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":3,"
                      "\"stderr\":\"out of memory\"}");
            return;
        }
        size_t nread = fread(odata, 1, osize, fp);
        fclose(fp);
        if ((long)nread != osize) {
            free(odata); remove(out_path);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":4,"
                      "\"stderr\":\"read .o file failed\"}");
            return;
        }

        /* Base64 encode */
        char *b64 = NULL;
        size_t b64_len = 0;
        /* Calculate required base64 buffer size: ceil(osize/3)*4 + 1 */
        size_t b64_size = ((osize + 2) / 3) * 4 + 1;
        b64 = (char *)malloc(b64_size);
        if (!b64) {
            free(odata); remove(out_path);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":5,"
                      "\"stderr\":\"out of memory\"}");
            return;
        }
        b64_len = mg_base64_encode((unsigned char *)odata, (size_t)osize, b64, b64_size);
        free(odata);
        if (b64_len == 0) {
            free(b64); remove(out_path);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":6,"
                      "\"stderr\":\"base64 encode failed\"}");
            return;
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
            free(b64); remove(out_path);
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":7,"
                      "\"stderr\":\"out of memory\"}");
            return;
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
        remove(out_path);
        return;
    }

    /* Unknown route */
    send_json(c, 404, "{\"status\":\"error\",\"code\":404,\"stderr\":\"unknown route\"}");
}

/* ── Signal handlers ────────────────────────────────────────────── */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── CLI parsing ─────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            g_listen = argv[++i];
        } else if (strcmp(argv[i], "--max-jobs") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val <= 0) val = 1;
            if (val > 64) val = 64;
            g_max_jobs = val;
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
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Auto-detect sharpc if not explicitly set */
    static char sharpc_buf[PATH_MAX];
    if (strcmp(g_sharpc, "sharpc") == 0) {
        const char *found = find_sharpc_path(sharpc_buf, sizeof(sharpc_buf));
        if (found) g_sharpc = found;
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
    printf("spkg-node: press Ctrl+C to stop\n");

    /* Event loop */
    while (g_running) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nspkg-node: shutting down\n");
    mg_mgr_free(&mgr);
    return 0;
}
