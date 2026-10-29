/* common.c - see common.h */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* stat */
#endif
#include "common.h"

#include <sys/stat.h>
#if defined(_WIN32) && !defined(S_ISREG)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

static void oom(void) {
    fputs("mlwsvm: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* ---- string builder ---------------------------------------------------- */

static void sb_reserve(StrBuf *b, size_t extra) {
    if (b->n + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->n + extra + 1) nc *= 2;
        b->s = xrealloc(b->s, nc);
        b->cap = nc;
    }
}

void sb_putc(StrBuf *b, char c) {
    sb_reserve(b, 1);
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}

void sb_putn(StrBuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void sb_puts(StrBuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_vprintf(StrBuf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    sb_reserve(b, (size_t)n);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    b->n += (size_t)n;
}

void sb_printf(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void sb_free(StrBuf *b) {
    free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
}

/* How many bytes of a valid UTF-8 sequence start at s (of n left), or 0. */
static size_t utf8_seq(const unsigned char *s, size_t n) {
    size_t len = s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC2 && s[0] < 0xE0 ? 2 : 0;
    if (!len || len > n) return 0;
    for (size_t k = 1; k < len; k++)
        if ((s[k] & 0xC0) != 0x80) return 0;
    if (len == 3 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))) return 0; /* overlong, surrogate */
    if (len == 4 && ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90))) return 0; /* overlong, > U+10FFFF */
    return len;
}

/* A string literal, as the assembler reads it: valid UTF-8 as itself, and
 * every other byte that is not plain ASCII as an escape, so the text is valid
 * UTF-8 whatever the bytes are. */
void sb_put_quoted(StrBuf *b, const char *s, size_t n) {
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t len = utf8_seq((const unsigned char *)s + i, n - i);
            if (len) {
                sb_putn(b, s + i, len);
                i += len - 1;
            } else {
                sb_printf(b, "\\x%02x", c);
            }
            continue;
        }
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case 0: sb_puts(b, "\\0"); break;
        default:
            if (c < 0x20 || c == 0x7f) sb_printf(b, "\\x%02x", c);
            else sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

/* ---- string map ---------------------------------------------------------- */

uint32_t hash_bytes(const void *p, size_t n) {
    const unsigned char *s = p;
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 16777619u;
    }
    return h;
}

static size_t smap_slot(const StrMap *m, const char *key) {
    size_t mask = m->cap - 1;
    size_t i = hash_bytes(key, strlen(key)) & mask;
    while (m->keys[i] && strcmp(m->keys[i], key) != 0) i = (i + 1) & mask;
    return i;
}

static void smap_grow(StrMap *m) {
    StrMap nm = {0};
    nm.cap = m->cap ? m->cap * 2 : 16;
    nm.keys = xcalloc(nm.cap, sizeof(char *));
    nm.vals = xcalloc(nm.cap, sizeof(void *));
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->keys[i]) continue;
        size_t j = smap_slot(&nm, m->keys[i]);
        nm.keys[j] = m->keys[i];
        nm.vals[j] = m->vals[i];
        nm.n++;
    }
    free(m->keys);
    free(m->vals);
    *m = nm;
}

void smap_put(StrMap *m, const char *key, void *val) {
    if ((m->n + 1) * 2 > m->cap) smap_grow(m);
    size_t i = smap_slot(m, key);
    if (!m->keys[i]) {
        m->keys[i] = xstrdup(key);
        m->n++;
    }
    m->vals[i] = val;
}

bool smap_get(const StrMap *m, const char *key, void **val) {
    if (!m->cap) return false;
    size_t i = smap_slot(m, key);
    if (!m->keys[i]) return false;
    if (val) *val = m->vals[i];
    return true;
}

bool smap_has(const StrMap *m, const char *key) { return smap_get(m, key, NULL); }

void smap_free(StrMap *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

void fmt_double(double d, char *buf, size_t n) {
    if (d != d) {
        snprintf(buf, n, "nan");
        return;
    }
    if (d > 1.7976931348623157e308 || d < -1.7976931348623157e308) {
        snprintf(buf, n, d < 0 ? "-inf" : "inf");
        return;
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, n, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) return;
    }
}

/* ---- errors -------------------------------------------------------------- */

bool mlws_err(MlwsError *e, const char *code, const char *fmt, ...) {
    if (!e) return false;
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return false;
}

void mlws_err_clear(MlwsError *e) {
    if (!e) return;
    e->code[0] = 0;
    e->msg[0] = 0;
}

/* ---- files --------------------------------------------------------------- */

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    StrBuf b = {0};
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) sb_putn(&b, tmp, n);
    fclose(f);
    if (!b.s) b.s = xcalloc(1, 1);
    if (len) *len = b.n;
    return b.s;
}

bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    return slash ? slash + 1 : path;
}

char *path_join(const char *a, const char *b) {
    if (!a || !*a || b[0] == '/') return xstrdup(b);
    StrBuf s = {0};
    sb_puts(&s, a);
    if (s.s[s.n - 1] != '/') sb_putc(&s, '/');
    sb_puts(&s, b);
    return s.s;
}

bool str_ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}
