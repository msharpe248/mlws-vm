/* top.c - `mlwsvm top [HOST:PORT]`: a terminal view of a running VM's
 * processes. It fetches the plain-text table the monitor renders at /top (the
 * server computes %CPU between samples), so the client stays tiny and needs no
 * JSON parser. */
#include "../platform/platform.h"
#include "tools.h"

static bool fetch(const char *host, int port, const char *path, StrBuf *body) {
    int fd = plat_tcp_connect(host, port);
    if (fd < 0) return false;
    StrBuf req = {0};
    sb_printf(&req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    bool ok = plat_sock_write_all(fd, req.s, req.n);
    sb_free(&req);
    StrBuf resp = {0};
    char buf[8192];
    long n;
    while (ok && (n = plat_sock_read(fd, buf, sizeof buf, 2000000000ull)) > 0) sb_putn(&resp, buf, (size_t)n);
    plat_sock_close(fd);
    const char *start = resp.s ? strstr(resp.s, "\r\n\r\n") : NULL;
    ok = ok && start && !strncmp(resp.s, "HTTP/1.0 200", 12);
    if (ok) sb_puts(body, start + 4);
    sb_free(&resp);
    return ok;
}

int tool_top(int argc, char **argv) {
    const char *addr = "127.0.0.1:7070";
    bool once = false;
    long interval_ms = 1000;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atol(argv[++i]);
        else addr = argv[i];
    }
    char host[64] = "127.0.0.1";
    const char *colon = strrchr(addr, ':');
    int port = atoi(colon ? colon + 1 : addr);
    if (colon && colon != addr) snprintf(host, sizeof host, "%.*s", (int)(colon - addr), addr);
    if (port <= 0) {
        fprintf(stderr, "mlwsvm top: bad address '%s' (expected HOST:PORT)\n", addr);
        return 2;
    }
    bool tty = plat_is_tty() && !once, seen = false;
    for (;;) {
        StrBuf body = {0};
        if (!fetch(host, port, "/top", &body)) {
            sb_free(&body);
            if (seen) {
                printf("\nThe program has finished.\n");
                return 0;
            }
            fprintf(stderr, "mlwsvm top: cannot connect to %s:%d (start a program with `mlwsvm run --monitor %s:%d ...`)\n",
                    host, port, host, port);
            return 1;
        }
        seen = true;
        if (tty) fputs("\x1b[H\x1b[2J", stdout); /* home + clear */
        fwrite(body.s, 1, body.n, stdout);
        if (tty) printf("\n(refreshing every %ld ms; Ctrl-C to quit)\n", interval_ms);
        fflush(stdout);
        sb_free(&body);
        if (once) return 0;
        plat_sleep_ns((uint64_t)interval_ms * 1000000ull);
    }
}
