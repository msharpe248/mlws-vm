/* keys.c - the monitor's keys (docs/TOOLING.md, "Keys"). A key is a token,
 * kept only as its SHA-256, with a scope (read, or admin) and a label. A
 * tokens file holds one to a line, "SCOPE LABEL SHA256", so that reading the
 * file gives no one access. `mlwsvm token` adds, revokes and lists them, the
 * monitor's admins do the same through /api/keys, and the monitor reads the
 * file again whenever it changes. */
#include <ctype.h>

#include "../core/sha256.h"
#include "../platform/platform.h"
#include "tools.h"
#include "tools_internal.h"

static const char *const SCOPE_NAMES[] = {"none", "read", "admin"};

const char *key_scope_name(int scope) { return scope >= 0 && scope <= SCOPE_ADMIN ? SCOPE_NAMES[scope] : "none"; }

int key_scope_of(const char *name) {
    if (!strcmp(name, "read")) return SCOPE_READ;
    if (!strcmp(name, "admin")) return SCOPE_ADMIN;
    return SCOPE_NONE;
}

/* A label: 1 to 63 letters, digits, '_', '.', '-' or '@'. */
bool key_label_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= sizeof ((MonKey *)0)->label) return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || strchr("_.-@", s[i]))) return false;
    return true;
}

void key_hash(const char *token, size_t n, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, token, n);
    sha256_final(&s, out);
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static bool unhex(const char *s, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1)
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* 128 random bits, as 32 hex digits. */
void key_random(char out[33]) {
    uint8_t r[16];
    if (!plat_random_bytes(r, sizeof r))
        for (int i = 0; i < 16; i++) r[i] = (uint8_t)(plat_now_ns() >> (i % 8 * 8)); /* (never: the OS has entropy) */
    hex(r, sizeof r, out);
}

/* One line's three fields, or false (a blank line or comment: true, with no label). */
static bool parse_line(const char *s, const char *z, MonKey *k, const char **why) {
    memset(k, 0, sizeof *k);
    while (s < z && isspace((unsigned char)*s)) s++;
    while (z > s && isspace((unsigned char)z[-1])) z--;
    if (s == z || *s == '#') return true;
    char f[3][80];
    int nf = 0;
    while (s < z && nf < 4) {
        const char *e = s;
        while (e < z && !isspace((unsigned char)*e)) e++;
        if (nf < 3) {
            size_t n = (size_t)(e - s) < sizeof f[0] - 1 ? (size_t)(e - s) : sizeof f[0] - 1;
            memcpy(f[nf], s, n);
            f[nf][n] = 0;
        }
        nf++;
        s = e;
        while (s < z && isspace((unsigned char)*s)) s++;
    }
    if (nf != 3) return *why = "expected SCOPE LABEL SHA256", false;
    if (!(k->scope = key_scope_of(f[0]))) return *why = "a scope is read or admin", false;
    if (!key_label_ok(f[1])) return *why = "a label is 1 to 63 letters, digits, _, ., - or @", false;
    if (strlen(f[2]) != 64 || !unhex(f[2], 32, k->hash)) return *why = "a hash is 64 hex digits (SHA-256)", false;
    snprintf(k->label, sizeof k->label, "%s", f[1]);
    k->from = 'f';
    return true;
}

/* Each line of text, in turn: fn(line start, end, ctx); false stops. */
static bool each_line(const char *text, size_t len, bool (*fn)(const char *, const char *, size_t, void *), void *ctx) {
    size_t line = 0;
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        if (!fn(text + at, text + end, ++line, ctx)) return false;
        at = end + 1;
    }
    return true;
}

typedef struct {
    MonKeyVec *out;
    const char *path;
    char *err;
    size_t errlen;
} ReadCtx;

static bool read_one(const char *s, const char *z, size_t line, void *c) {
    ReadCtx *r = c;
    MonKey k;
    const char *why = NULL;
    if (!parse_line(s, z, &k, &why)) return snprintf(r->err, r->errlen, "%s:%zu: %s", r->path, line, why), false;
    if (!k.label[0]) return true;
    for (size_t i = 0; i < r->out->n; i++)
        if (!strcmp(r->out->v[i].label, k.label))
            return snprintf(r->err, r->errlen, "%s:%zu: the label %s is used twice", r->path, line, k.label), false;
    VEC_PUSH(*r->out, k);
    return true;
}

bool keyfile_read(const char *path, MonKeyVec *out, char *err, size_t errlen) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        PlatStat st;
        int e;
        if (!plat_stat(path, true, &st, &e)) return true; /* (none yet: no keys) */
        return snprintf(err, errlen, "%s: cannot read it", path), false;
    }
    ReadCtx r = {out, path, err, errlen};
    bool ok = each_line(text, len, read_one, &r);
    free(text);
    return ok;
}

/* text written over path, through a file beside it renamed over it. */
static bool replace_file(const char *path, const StrBuf *text, char *err, size_t errlen) {
    StrBuf tmp = {0};
    sb_printf(&tmp, "%s.new", path);
    int e = 0;
    bool ok = write_file(tmp.s, text->s ? text->s : "", text->n) && plat_rename(tmp.s, path, &e);
    if (!ok) {
        plat_remove(tmp.s, &e);
        snprintf(err, errlen, "%s: cannot write it", path);
    }
    sb_free(&tmp);
    return ok;
}

bool keyfile_add(const char *path, int scope, const char *label, const uint8_t hash[32], char *err, size_t errlen) {
    MonKeyVec have = {0};
    if (!keyfile_read(path, &have, err, errlen)) {
        VEC_FREE(have);
        return false;
    }
    for (size_t i = 0; i < have.n; i++)
        if (!strcmp(have.v[i].label, label)) {
            VEC_FREE(have);
            return snprintf(err, errlen, "%s: there is a key labelled %s already", path, label), false;
        }
    VEC_FREE(have);
    size_t len = 0;
    char *text = read_file(path, &len);
    StrBuf b = {0};
    if (text && len) {
        sb_putn(&b, text, len);
        if (text[len - 1] != '\n') sb_putc(&b, '\n');
    } else {
        sb_puts(&b, "# The monitor's keys (mlwsvm token): SCOPE LABEL SHA256-of-the-token\n");
    }
    free(text);
    char h[65];
    hex(hash, 32, h);
    sb_printf(&b, "%s %s %s\n", key_scope_name(scope), label, h);
    bool ok = replace_file(path, &b, err, errlen);
    sb_free(&b);
    return ok;
}

typedef struct {
    const char *label;
    StrBuf *b;
    bool found;
} DropCtx;

static bool drop_one(const char *s, const char *z, size_t line, void *c) {
    (void)line;
    DropCtx *d = c;
    MonKey k;
    const char *why;
    if (parse_line(s, z, &k, &why) && k.label[0] && !strcmp(k.label, d->label)) d->found = true;
    else sb_putn(d->b, s, (size_t)(z - s)), sb_putc(d->b, '\n');
    return true;
}

bool keyfile_remove(const char *path, const char *label, char *err, size_t errlen) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return snprintf(err, errlen, "%s: cannot read it", path), false;
    StrBuf b = {0};
    DropCtx d = {label, &b, false};
    each_line(text, len, drop_one, &d);
    free(text);
    bool ok = d.found;
    if (!ok) snprintf(err, errlen, "%s: no key labelled %s", path, label);
    else ok = replace_file(path, &b, err, errlen);
    sb_free(&b);
    return ok;
}

/* ---- mlwsvm token ---------------------------------------------------------------------- */

static int token_usage(void) {
    fprintf(stderr, "usage: mlwsvm token new --label LABEL [--scope read|admin] [--file FILE]\n"
                    "       mlwsvm token revoke --label LABEL --file FILE\n"
                    "       mlwsvm token list --file FILE\n");
    return 2;
}

int tool_token(int argc, char **argv) {
    if (argc < 1) return token_usage();
    const char *cmd = argv[0], *label = NULL, *file = NULL, *scope_name = "read";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--scope") && i + 1 < argc) scope_name = argv[++i];
        else return token_usage();
    }
    char err[600];
    if (!strcmp(cmd, "new")) {
        int scope = key_scope_of(scope_name);
        if (!label || !scope) return token_usage();
        if (!key_label_ok(label)) {
            fprintf(stderr, "mlwsvm token: a label is 1 to 63 letters, digits, _, ., - or @\n");
            return 2;
        }
        char token[33], h[65];
        uint8_t hash[32];
        key_random(token);
        key_hash(token, strlen(token), hash);
        if (file && !keyfile_add(file, scope, label, hash, err, sizeof err)) {
            fprintf(stderr, "mlwsvm token: %s\n", err);
            return 1;
        }
        printf("%s\n", token);
        if (file) {
            fprintf(stderr, "mlwsvm token: a %s key labelled %s, added to %s (the token is shown only now)\n", scope_name, label, file);
        } else {
            hex(hash, 32, h);
            fprintf(stderr, "mlwsvm token: for the tokens file (the token is shown only now):\n%s %s %s\n", scope_name, label, h);
        }
        return 0;
    }
    if (!file) return token_usage();
    if (!strcmp(cmd, "revoke")) {
        if (!label) return token_usage();
        if (!keyfile_remove(file, label, err, sizeof err)) {
            fprintf(stderr, "mlwsvm token: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "list")) {
        MonKeyVec ks = {0};
        if (!keyfile_read(file, &ks, err, sizeof err)) {
            fprintf(stderr, "mlwsvm token: %s\n", err);
            VEC_FREE(ks);
            return 1;
        }
        for (size_t i = 0; i < ks.n; i++) printf("%-6s %s\n", key_scope_name(ks.v[i].scope), ks.v[i].label);
        VEC_FREE(ks);
        return 0;
    }
    return token_usage();
}
