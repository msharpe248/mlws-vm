/* top.c - `mlwsvm top [http[s]://]HOST:PORT [--ca FILE]`: a terminal view of a
 * running VM's processes. It fetches the plain-text table the monitor renders
 * at /top (the server computes %CPU between samples), so the client stays tiny
 * and needs no JSON parser. With --log it follows the monitor's log instead,
 * as `tail -f` a file: /log?since=N is the lines after number N as text, under
 * a first line with the number to ask from next. Over HTTPS (a monitor with --monitor-cert) it
 * checks the monitor's certificate against the host, with the authorities in
 * --ca's file or else the system's, as std.tls does. A monitor with a tokens
 * file needs a key: --token's, the address's #token=, or
 * $MLWSVM_MONITOR_TOKEN. */
#include "../native/tls.h"
#include "../platform/platform.h"
#include "tools.h"

typedef struct {
    char host[300]; /* (as long as an address parse_target takes) */
    int port;
    bool tls;
    const char *ca;
    char token[200]; /* "": none */
} Target;

#define TOP_WAIT_NS 2000000000ull

/* A TLS call's wait: true to call again. */
static bool tls_again(int fd, long r) {
    if (r == TLS_WANT_READ) return plat_wait_readable(fd, TOP_WAIT_NS);
    if (r == TLS_WANT_WRITE) return plat_sleep_ns(1000000), true;
    return false;
}

/* GET path into body. False with why ("": the monitor could not be reached). */
static bool fetch(const Target *t, const char *path, StrBuf *body, char *why, size_t whylen) {
    why[0] = 0;
    int fd = plat_tcp_connect(t->host, t->port);
    if (fd < 0) return false;
    StrBuf req = {0}, resp = {0};
    sb_printf(&req, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, t->host);
    if (t->token[0]) sb_printf(&req, "Authorization: Bearer %s\r\n", t->token);
    sb_puts(&req, "\r\n");
    bool ok;
    char buf[8192];
    long n;
    if (t->tls) {
        Tls *s = tls_client(fd, t->host, t->ca ? t->ca : "", why, whylen);
        if (!s) {
            plat_sock_close(fd);
            sb_free(&req);
            if (!why[0]) snprintf(why, whylen, "TLS failed");
            return false;
        }
        long r;
        while ((r = tls_handshake(s, why, whylen)) != TLS_OK && tls_again(fd, r)) {}
        ok = r == TLS_OK;
        for (size_t off = 0; ok && off < req.n;) {
            r = tls_write(s, req.s + off, req.n - off, why, whylen);
            if (r > 0) off += (size_t)r;
            else ok = tls_again(fd, r);
        }
        while (ok) {
            r = tls_read(s, buf, sizeof buf, why, whylen);
            if (r > 0) sb_putn(&resp, buf, (size_t)r);
            else if (!r || !tls_again(fd, r)) break;
        }
        if (ok && !why[0] && !resp.n) snprintf(why, whylen, "the monitor sent nothing");
        tls_release(s); /* (and the socket) */
        if (!ok && !why[0]) snprintf(why, whylen, "TLS failed");
    } else {
        ok = plat_sock_write_all(fd, req.s, req.n);
        while (ok && (n = plat_sock_read(fd, buf, sizeof buf, TOP_WAIT_NS)) > 0) sb_putn(&resp, buf, (size_t)n);
        plat_sock_close(fd);
    }
    sb_free(&req);
    const char *start = resp.s ? strstr(resp.s, "\r\n\r\n") : NULL;
    bool good = ok && start && !strncmp(resp.s, "HTTP/1.0 200", 12);
    if (good) sb_puts(body, start + 4);
    else if (ok && !why[0] && resp.s && !strncmp(resp.s, "HTTP/1.0 401", 12))
        snprintf(why, whylen, t->token[0] ? "the monitor does not know that key" : "the monitor needs a key: --token KEY");
    else if (ok && !why[0])
        snprintf(why, whylen, "the monitor did not answer %.*s%s", (int)strcspn(path, "?"), path,
                 !t->tls && (!resp.s || strncmp(resp.s, "HTTP/", 5)) ? " (one over TLS is https://HOST:PORT)" : "");
    sb_free(&resp);
    return good;
}

/* [http[s]://]HOST:PORT[/...][#...], or PORT; [::1]:PORT for an IPv6 address. */
static bool parse_target(const char *addr, Target *t) {
    if (!strncmp(addr, "https://", 8)) t->tls = true, addr += 8;
    else if (!strncmp(addr, "http://", 7)) addr += 7;
    const char *frag = strstr(addr, "#token=");
    if (frag && !t->token[0]) {
        size_t n = strcspn(frag + 7, "&");
        if (n < sizeof t->token) memcpy(t->token, frag + 7, n), t->token[n] = 0;
    }
    size_t len = strcspn(addr, "/#?");
    char a[sizeof t->host];
    if (len >= sizeof a) return false;
    memcpy(a, addr, len);
    a[len] = 0;
    char *colon = strrchr(a, ':');
    if (colon && strchr(a, ']') > colon) colon = NULL; /* ([::1] with no port) */
    t->port = atoi(colon ? colon + 1 : a);
    if (colon && colon != a) {
        *colon = 0;
        char *h = a;
        size_t hl = strlen(h);
        if (h[0] == '[' && hl > 1 && h[hl - 1] == ']') h[hl - 1] = 0, h++;
        snprintf(t->host, sizeof t->host, "%s", h);
    }
    return t->port > 0 && t->port <= 65535;
}

int tool_top(int argc, char **argv) {
    const char *addr = "127.0.0.1:7070";
    bool once = false, log = false;
    long interval_ms = 1000;
    Target t = {.host = "127.0.0.1"};
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--log")) log = true;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ca") && i + 1 < argc) t.ca = argv[++i];
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) snprintf(t.token, sizeof t.token, "%s", argv[++i]);
        else addr = argv[i];
    }
    if (!parse_target(addr, &t)) {
        fprintf(stderr, "mlwsvm top: bad address '%s' (expected [https://]HOST:PORT)\n", addr);
        return 2;
    }
    const char *env_token = getenv("MLWSVM_MONITOR_TOKEN");
    if (!t.token[0] && env_token) snprintf(t.token, sizeof t.token, "%s", env_token);
    if (t.tls && !tls_available()) {
        fprintf(stderr, "mlwsvm top: HTTPS needs OpenSSL or LibreSSL, and neither was found\n");
        return 1;
    }
    bool tty = plat_is_tty() && !once && !log, seen = false;
    unsigned long long since = 0; /* (--log) the number of the last line printed */
    for (;;) {
        StrBuf body = {0};
        char why[256], path[64] = "/top";
        if (log) snprintf(path, sizeof path, "/log?since=%llu", since);
        if (!fetch(&t, path, &body, why, sizeof why)) {
            sb_free(&body);
            if (seen && !why[0]) {
                if (!log) printf("\nThe program has finished.\n");
                return 0;
            }
            if (why[0]) fprintf(stderr, "mlwsvm top: %s%s:%d: %s\n", t.tls ? "https://" : "", t.host, t.port, why);
            else
                fprintf(stderr, "mlwsvm top: cannot connect to %s:%d (start a program with `mlwsvm run --monitor %s:%d ...`)\n",
                        t.host, t.port, t.host, t.port);
            return 1;
        }
        seen = true;
        if (tty) fputs("\x1b[H\x1b[2J", stdout); /* home + clear */
        size_t skip = 0;
        if (log && body.s) { /* the first line: the number to ask from next */
            since = strtoull(body.s, NULL, 10);
            skip = strcspn(body.s, "\n");
            if (skip < body.n) skip++;
        }
        if (body.n > skip) fwrite(body.s + skip, 1, body.n - skip, stdout);
        if (tty) printf("\n(refreshing every %ld ms; Ctrl-C to quit)\n", interval_ms);
        fflush(stdout);
        sb_free(&body);
        if (once) return 0;
        plat_sleep_ns((uint64_t)interval_ms * 1000000ull);
    }
}
