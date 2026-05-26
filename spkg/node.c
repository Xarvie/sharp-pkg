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
    size_t off = 0;
    buf[0] = '\0';

    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", "cflags");
    const char *p = memmem(json.buf, json.len, pat, strlen(pat));
    if (!p) return 0;
    p += strlen(pat);
    while (p < json.buf + json.len && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '[') return 0;
    p++;

    while (p < json.buf + json.len && *p != ']') {
        if (*p == '"') {
            p++;
            while (p < json.buf + json.len && *p != '"' && off < bufsize - 2) {
                if (*p == '\\' && p + 1 < json.buf + json.len) { p++; }
                buf[off++] = *p++;
            }
            buf[off++] = '|';
            if (*p == '"') p++;
        }
        p++;
    }
    if (off > 0) off--;
    buf[off] = '\0';
    return (int)off;
}

/* ── Compile task execution ─────────────────────────────────────── */

static int compile_task(const char *source_content, const char *cflags_str,
                        const char *optimize, char *out_path, size_t out_path_size,
                        char *depfile_content, size_t depfile_buf_size) {
#ifdef _WIN32
    char src_path[MAX_PATH];
    char tmp_dir[MAX_PATH];
    GetTempPathA(sizeof(tmp_dir), tmp_dir);
    snprintf(src_path, sizeof(src_path), "%sspkg_src_%d.sp", tmp_dir, rand());
    snprintf(out_path, out_path_size, "%sspkg_out_%d.o", tmp_dir, rand());
#else
    char src_path[256];
    snprintf(src_path, sizeof(src_path), "/tmp/spkg_src_%d.sp", rand());
    snprintf(out_path, out_path_size, "/tmp/spkg_out_%d.o", rand());
#endif

    /* Write source to temp file */
    FILE *fp = fopen(src_path, "w");
    if (!fp) { return -1; }
    fputs(source_content, fp);
    fclose(fp);

    /* Build command */
    char cmd[4096];
    char depfile[512];
    snprintf(depfile, sizeof(depfile), "%s.d", out_path);

    const char *opt_flag = "-O0";
    if (optimize && optimize[0]) {
        if (strcmp(optimize, "ReleaseSafe") == 0) opt_flag = "-O1";
        else if (strcmp(optimize, "ReleaseFast") == 0) opt_flag = "-O2";
        else if (strcmp(optimize, "ReleaseSmall") == 0) opt_flag = "-Os";
    }

    snprintf(cmd, sizeof(cmd), "%s %s %s -MMD -MF \"%s\" \"%s\" -o \"%s\"",
             g_sharpc, opt_flag, cflags_str ? cflags_str : "",
             depfile, src_path, out_path);

    int ret = system(cmd);

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

        /* Parse JSON body */
        char source[65536] = {0};
        char cflags[2048] = {0};
        char optimize[64] = {0};

        if (!json_get_str(hm->body, "source", source, sizeof(source))) {
            send_json(c, 400,
                      "{\"status\":\"error\",\"code\":400,"
                      "\"stderr\":\"missing 'source' field\"}");
            return;
        }

        json_get_cflags(hm->body, cflags, sizeof(cflags));
        json_get_str(hm->body, "optimize", optimize, sizeof(optimize));

        /* Execute compilation */
        g_active++;

        char out_path[512];
        char depfile_content[4096];
        int rc = compile_task(source, cflags,
                              optimize[0] ? optimize : NULL,
                              out_path, sizeof(out_path),
                              depfile_content, sizeof(depfile_content));

        g_active--;

        if (rc != 0) {
            send_json(c, 500,
                      "{\"status\":\"error\",\"code\":1,"
                      "\"stderr\":\"compilation failed\"}");
            return;
        }

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
        if (!odata) { fclose(fp); remove(out_path); return; }
        fread(odata, 1, osize, fp);
        fclose(fp);

        /* Base64 encode */
        char b64[131072];
        size_t b64_len = mg_base64_encode((unsigned char *)odata, (size_t)osize, b64, sizeof(b64));
        b64[b64_len] = '\0';
        free(odata);

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

        char resp[131072];
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\","
                 "\"output\":\"%s\","
                 "\"depfile\":\"%s\","
                 "\"cached\":false}",
                 b64, dep_escaped);

        send_json(c, 200, resp);
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
            g_max_jobs = atoi(argv[++i]);
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
