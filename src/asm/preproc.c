/* preproc.c - see preproc.h and docs/ASSEMBLY.md ("Preprocessor"). */
#include "preproc.h"

#include <ctype.h>

#include "../core/opcodes.h"
#include "../platform/platform.h"

#define MAX_MACRO_DEPTH 64
#define MAX_COND_DEPTH 64
#define MAX_REPT 1000000

typedef VEC(PLine) PLineVec;

static bool id_start(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static bool id_char(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

void pp_init(PP *pp, const char *const *libpaths, size_t nlib, MlwsError *err) {
    memset(pp, 0, sizeof *pp);
    pp->libpaths = libpaths;
    pp->nlib = nlib;
    pp->err = err;
    pp->asof = SIZE_MAX;
    op_init();
}

static void free_lines(PLine *lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i].text);
    free(lines);
}

void pp_free(PP *pp) {
    for (size_t i = 0; i < pp->macros.cap; i++) {
        Macro *m = pp->macros.vals ? pp->macros.vals[i] : NULL;
        if (!pp->macros.keys[i] || !m) continue;
        for (int k = 0; k < m->nparams; k++) {
            free(m->params[k]);
            free(m->defaults[k]);
        }
        free(m->params);
        free(m->defaults);
        free_lines(m->body, m->nbody);
        free(m->name);
        free(m);
    }
    smap_free(&pp->macros);
    for (size_t i = 0; i < pp->consts.cap; i++)
        if (pp->consts.keys[i]) {
            free(((ConstDef *)pp->consts.vals[i])->vals);
            free(pp->consts.vals[i]);
        }
    smap_free(&pp->consts);
    smap_free(&pp->readonly);
    smap_free(&pp->included);
    for (size_t i = 0; i < pp->out.n; i++) free(pp->out.v[i].text);
    VEC_FREE(pp->out);
    for (size_t i = 0; i < pp->files.n; i++) free(pp->files.v[i]);
    VEC_FREE(pp->files);
}

bool pp_line_error(MlwsError *err, const PLine *ln, int col, const char *code, const char *fmt, ...) {
    char msg[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    StrBuf b = {0};
    if (col > 0) sb_printf(&b, "%s:%d:%d: %s", ln->loc.file, ln->loc.line, col, msg);
    else sb_printf(&b, "%s:%d: %s", ln->loc.file, ln->loc.line, msg);
    if (ln->macro)
        sb_printf(&b, " (in macro '%s' expanded at %s:%d)", ln->macro, ln->origin.file, ln->origin.line);
    mlws_err(err, code, "%s", b.s);
    sb_free(&b);
    return false;
}

bool pp_const_lookup(void *ctx, const char *name, size_t len, ExprVal *out) {
    PP *pp = ctx;
    char buf[256];
    if (len >= sizeof buf) return false;
    memcpy(buf, name, len);
    buf[len] = 0;
    void *v;
    if (!smap_get(&pp->consts, buf, &v)) return false;
    /* The value in force at line asof: the last one set at or before it, or
     * the first (a use before the definition) */
    const ConstDef *d = v;
    size_t k = d->n - 1;
    if (pp->asof != SIZE_MAX)
        while (k > 0 && d->vals[k].from > pp->asof) k--;
    *out = d->vals[k].v;
    return true;
}

bool pp_defined(void *ctx, const char *name, size_t len) {
    PP *pp = ctx;
    char buf[256];
    if (len >= sizeof buf) return false;
    memcpy(buf, name, len);
    buf[len] = 0;
    return smap_has(&pp->consts, buf) || smap_has(&pp->macros, buf);
}

static const char *intern_file(PP *pp, const char *path) {
    char *s = xstrdup(path);
    VEC_PUSH(pp->files, s);
    return s;
}

static char *trim_dup(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static void split_lines(PP *pp, const char *file, const char *text, PLineVec *out) {
    const char *fname = intern_file(pp, file);
    int line = 1;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *s = xstrndup(p, len);
        s[comment_start(s)] = 0;
        size_t sl = strlen(s);
        while (sl && isspace((unsigned char)s[sl - 1])) s[--sl] = 0;
        PLine pl = {s, {fname, line}, {fname, line}, NULL};
        VEC_PUSH(*out, pl);
        line++;
        p = e ? e + 1 : p + len;
    }
}

/* Leading `label:` prefixes and the first word of a line. */
typedef struct {
    size_t label_end; /* offset just past the last label colon (0 = none) */
    size_t word, wlen; /* first word after labels */
    size_t rest;       /* offset just past the word */
} LineHead;

static void line_head(const char *t, LineHead *h) {
    size_t p = 0, label_end = 0;
    for (;;) {
        size_t q = p;
        while (isspace((unsigned char)t[q])) q++;
        size_t s = q;
        if (id_start((unsigned char)t[q]) || isdigit((unsigned char)t[q]))
            while (id_char((unsigned char)t[q])) q++;
        if (q > s && t[q] == ':') {
            p = q + 1;
            label_end = p;
            continue;
        }
        break;
    }
    size_t q = p;
    while (isspace((unsigned char)t[q])) q++;
    size_t s = q;
    if (id_start((unsigned char)t[q]))
        while (id_char((unsigned char)t[q])) q++;
    h->label_end = label_end;
    h->word = s;
    h->wlen = q - s;
    h->rest = q;
}

static bool word_is(const char *t, const LineHead *h, const char *s) {
    return h->wlen == strlen(s) && memcmp(t + h->word, s, h->wlen) == 0;
}

/* Split a macro argument list at top-level commas. */
static void split_args(const char *s, StrVec *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return;
    int depth = 0;
    char q = 0;
    const char *start = p;
    for (;; p++) {
        char c = *p;
        if (q) {
            if (c == '\\' && p[1]) p++;
            else if (c == q) q = 0;
            if (c) continue;
        }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
        else if ((c == ',' && depth == 0) || c == 0) {
            VEC_PUSH(*out, trim_dup(start, (size_t)(p - start)));
            start = p + 1;
        }
        if (!c) break;
    }
}

static void free_args(StrVec *a) {
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    VEC_FREE(*a);
}

/* Replace \param, \@ (unique id) and \() (separator) in a body line. */
static char *subst(const char *body, char *const *names, char *const *vals, int n, int uniq) {
    StrBuf b = {0};
    for (const char *p = body; *p;) {
        if (*p != '\\') {
            sb_putc(&b, *p++);
            continue;
        }
        if (p[1] == '@') {
            sb_printf(&b, "%d", uniq);
            p += 2;
            continue;
        }
        if (p[1] == '(' && p[2] == ')') {
            p += 3;
            continue;
        }
        int best = -1;
        size_t bestlen = 0;
        for (int k = 0; k < n; k++) {
            size_t l = strlen(names[k]);
            if (l > bestlen && strncmp(p + 1, names[k], l) == 0) {
                best = k;
                bestlen = l;
            }
        }
        if (best >= 0) {
            sb_puts(&b, vals[best]);
            p += 1 + bestlen;
        } else {
            sb_putc(&b, *p++);
        }
    }
    if (!b.s) b.s = xcalloc(1, 1);
    return b.s;
}

static bool pp_lines(PP *pp, const PLine *lines, size_t n);

/* Emit a line for the assembler. Leading whitespace is kept so that error
 * columns match the source. */
static void emit(PP *pp, const PLine *ln, const char *text, size_t len) {
    while (len && isspace((unsigned char)text[len - 1])) len--;
    size_t lead = 0;
    while (lead < len && isspace((unsigned char)text[lead])) lead++;
    if (lead == len) return;
    char *t = xstrndup(text, len);
    PLine o = {t, ln->loc, ln->origin, ln->macro};
    VEC_PUSH(pp->out, o);
}

/* Find the line index of the closer matching the opener at lines[i]. */
static bool collect(PP *pp, const PLine *lines, size_t n, size_t i, const char *const *openers,
                    const char *closer, size_t *end) {
    int depth = 1;
    for (size_t j = i + 1; j < n; j++) {
        LineHead h;
        line_head(lines[j].text, &h);
        for (const char *const *o = openers; *o; o++)
            if (word_is(lines[j].text, &h, *o)) depth++;
        if (word_is(lines[j].text, &h, closer) && --depth == 0) {
            *end = j;
            return true;
        }
    }
    LineHead h;
    line_head(lines[i].text, &h);
    return pp_line_error(pp->err, &lines[i], (int)h.word + 1, "E_ASM_SYNTAX", "missing %s for this %.*s", closer,
                         (int)h.wlen, lines[i].text + h.word);
}

static bool lex_rest(PP *pp, const PLine *ln, size_t off, TokVec *toks) {
    char msg[256];
    int col = 0;
    if (!lex_line(ln->text + off, toks, msg, sizeof msg, &col))
        return pp_line_error(pp->err, ln, (int)off + col, "E_ASM_SYNTAX", "%s", msg);
    for (size_t i = 0; i < toks->n; i++) toks->v[i].col += (int)off;
    return true;
}

static bool eval_rest(PP *pp, const PLine *ln, size_t off, ExprVal *v) {
    TokVec toks = {0};
    if (!lex_rest(pp, ln, off, &toks)) return false;
    ExprParser ep = {toks.v, toks.n, 0, pp_const_lookup, pp_defined, pp, {0}, 0};
    bool ok = expr_eval(&ep, v);
    if (!ok) {
        pp_line_error(pp->err, ln, ep.errcol, "E_ASM_EXPR", "%s", ep.err);
    } else if (toks.v[ep.pos].kind != TK_EOF) {
        ok = pp_line_error(pp->err, ln, toks.v[ep.pos].col, "E_ASM_SYNTAX", "unexpected text after expression");
    }
    tokvec_free(&toks);
    return ok;
}

static bool eval_cond(PP *pp, const PLine *ln, const LineHead *h, bool *out) {
    const char *t = ln->text;
    if (word_is(t, h, ".if") || word_is(t, h, ".elif")) {
        ExprVal v;
        if (!eval_rest(pp, ln, h->rest, &v)) return false;
        *out = v.is_float ? v.f != 0 : v.i != 0;
        return true;
    }
    if (word_is(t, h, ".ifb") || word_is(t, h, ".ifnb")) {
        const char *p = t + h->rest;
        while (isspace((unsigned char)*p)) p++;
        *out = (*p == 0) == word_is(t, h, ".ifb");
        return true;
    }
    /* .ifdef / .ifndef NAME */
    TokVec toks = {0};
    if (!lex_rest(pp, ln, h->rest, &toks)) return false;
    bool ok = toks.v[0].kind == TK_IDENT && toks.v[1].kind == TK_EOF;
    if (!ok) {
        pp_line_error(pp->err, ln, toks.v[0].col, "E_ASM_SYNTAX", "expected a single name");
    } else {
        bool d = pp_defined(pp, toks.v[0].start, (size_t)toks.v[0].len);
        *out = word_is(t, h, ".ifdef") ? d : !d;
    }
    tokvec_free(&toks);
    return ok;
}

static bool define_macro(PP *pp, const PLine *ln, const LineHead *h, const PLine *body, size_t nbody) {
    const char *p = ln->text + h->rest;
    while (isspace((unsigned char)*p)) p++;
    const char *ns = p;
    if (!id_start((unsigned char)*p))
        return pp_line_error(pp->err, ln, (int)(p - ln->text) + 1, "E_ASM_SYNTAX", "expected a macro name");
    while (id_char((unsigned char)*p)) p++;
    char *name = xstrndup(ns, (size_t)(p - ns));
    if (smap_has(&pp->macros, name)) {
        pp_line_error(pp->err, ln, (int)(ns - ln->text) + 1, "E_ASM_DUP_SYM", "macro '%s' is already defined", name);
        free(name);
        return false;
    }
    if (op_lookup(name, strlen(name)) >= 0) {
        pp_line_error(pp->err, ln, (int)(ns - ln->text) + 1, "E_ASM_DUP_SYM",
                      "macro '%s' would hide the instruction of the same name", name);
        free(name);
        return false;
    }
    Macro *m = xcalloc(1, sizeof *m);
    m->name = name;
    m->loc = ln->loc;
    StrVec ps = {0};
    if (*p == ',') p++;
    split_args(p, &ps);
    m->params = xcalloc(ps.n + 1, sizeof(char *));
    m->defaults = xcalloc(ps.n + 1, sizeof(char *));
    for (size_t k = 0; k < ps.n; k++) {
        char *s = ps.v[k];
        char *eq = strchr(s, '=');
        if (eq) {
            m->defaults[k] = trim_dup(eq + 1, strlen(eq + 1));
            *eq = 0;
        }
        size_t l = strlen(s);
        while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
        if (l > 3 && strcmp(s + l - 3, "...") == 0) {
            if (k != ps.n - 1) {
                free_args(&ps);
                return pp_line_error(pp->err, ln, 0, "E_ASM_SYNTAX", "only the last macro parameter can be variadic");
            }
            s[l - 3] = 0;
            m->varargs = true;
        }
        bool okname = id_start((unsigned char)s[0]);
        for (char *c = s; *c; c++) okname = okname && id_char((unsigned char)*c);
        if (!okname) {
            pp_line_error(pp->err, ln, 0, "E_ASM_SYNTAX", "bad macro parameter name '%s'", s);
            free_args(&ps);
            return false;
        }
        m->params[k] = xstrdup(s);
    }
    m->nparams = (int)ps.n;
    free_args(&ps);
    m->body = xcalloc(nbody + 1, sizeof(PLine));
    for (size_t k = 0; k < nbody; k++) {
        m->body[k] = body[k];
        m->body[k].text = xstrdup(body[k].text);
    }
    m->nbody = nbody;
    smap_put(&pp->macros, m->name, m);
    return true;
}

/* Expand body lines with parameter substitution, then preprocess them. */
static bool expand_body(PP *pp, const PLine *site, const char *macro, const PLine *body, size_t nbody,
                        char *const *names, char *const *vals, int n) {
    if (pp->depth >= MAX_MACRO_DEPTH)
        return pp_line_error(pp->err, site, 0, "E_ASM_MACRO", "macro expansion nested too deeply (recursive macro?)");
    int uniq = ++pp->uniq;
    PLine *nl = xcalloc(nbody + 1, sizeof(PLine));
    for (size_t k = 0; k < nbody; k++) {
        nl[k].text = subst(body[k].text, names, vals, n, uniq);
        nl[k].loc = body[k].loc;
        nl[k].origin = site->origin;
        nl[k].macro = macro ? macro : body[k].macro;
    }
    pp->depth++;
    bool ok = pp_lines(pp, nl, nbody);
    pp->depth--;
    free_lines(nl, nbody);
    return ok;
}

static bool expand_macro(PP *pp, const PLine *ln, Macro *m, const char *argtext) {
    StrVec args = {0};
    split_args(argtext, &args);
    if (!m->varargs && (int)args.n > m->nparams) {
        free_args(&args);
        return pp_line_error(pp->err, ln, 0, "E_ASM_MACRO", "macro '%s' takes %d argument(s), got %zu", m->name,
                             m->nparams, args.n);
    }
    char **vals = xcalloc((size_t)m->nparams + 1, sizeof(char *));
    for (int k = 0; k < m->nparams; k++) {
        if (m->varargs && k == m->nparams - 1) {
            StrBuf b = {0};
            for (size_t j = (size_t)k; j < args.n; j++) {
                if (j > (size_t)k) sb_puts(&b, ", ");
                sb_puts(&b, args.v[j]);
            }
            vals[k] = b.s ? b.s : xstrdup(m->defaults[k] ? m->defaults[k] : "");
        } else if ((size_t)k < args.n && args.v[k][0]) {
            vals[k] = xstrdup(args.v[k]);
        } else {
            vals[k] = xstrdup(m->defaults[k] ? m->defaults[k] : "");
        }
    }
    free_args(&args);
    bool ok = expand_body(pp, ln, m->name, m->body, m->nbody, m->params, vals, m->nparams);
    for (int k = 0; k < m->nparams; k++) free(vals[k]);
    free(vals);
    return ok;
}

static bool expand_rept(PP *pp, const PLine *ln, const LineHead *h, const PLine *body, size_t nbody) {
    const char *t = ln->text;
    if (word_is(t, h, ".irp")) {
        StrVec a = {0};
        split_args(t + h->rest, &a);
        if (a.n == 0 || !id_start((unsigned char)a.v[0][0])) {
            free_args(&a);
            return pp_line_error(pp->err, ln, 0, "E_ASM_SYNTAX", ".irp needs a variable name and values");
        }
        bool ok = true;
        for (size_t k = 1; k < a.n && ok; k++) ok = expand_body(pp, ln, NULL, body, nbody, &a.v[0], &a.v[k], 1);
        free_args(&a);
        return ok;
    }
    /* .rept count [, var] */
    const char *rest = t + h->rest;
    const char *comma = NULL;
    for (const char *c = rest; *c; c++)
        if (*c == ',') comma = c;
    char *var = NULL;
    char *countline;
    if (comma) {
        var = trim_dup(comma + 1, strlen(comma + 1));
        countline = xstrndup(t, (size_t)(comma - t));
    } else {
        countline = xstrdup(t);
    }
    PLine tmp = *ln;
    tmp.text = countline;
    ExprVal v;
    bool ok = eval_rest(pp, &tmp, h->rest, &v);
    free(countline);
    if (ok && (v.is_float || v.i < 0 || v.i > MAX_REPT))
        ok = pp_line_error(pp->err, ln, 0, "E_ASM_RANGE", ".rept count must be an integer in 0..%d", MAX_REPT);
    for (int64_t k = 0; ok && k < v.i; k++) {
        char num[32];
        snprintf(num, sizeof num, "%lld", (long long)k);
        char *val = num;
        ok = expand_body(pp, ln, NULL, body, nbody, var ? &var : NULL, &val, var ? 1 : 0);
    }
    free(var);
    return ok;
}

static bool run_file(PP *pp, const char *path, const PLine *site) {
    char *real = plat_realpath(path);
    if (!real) {
        if (site) return pp_line_error(pp->err, site, 0, "E_ASM_INCLUDE", "cannot open '%s'", path);
        return mlws_err(pp->err, "E_LOAD_NOT_FOUND", "cannot open '%s'", path);
    }
    if (smap_has(&pp->included, real)) { /* every file is included at most once */
        free(real);
        return true;
    }
    smap_put(&pp->included, real, NULL);
    free(real);
    char *text = read_file(path, NULL);
    if (!text) {
        if (site) return pp_line_error(pp->err, site, 0, "E_ASM_INCLUDE", "cannot read '%s'", path);
        return mlws_err(pp->err, "E_LOAD_IO", "cannot read '%s'", path);
    }
    PLineVec lines = {0};
    split_lines(pp, path, text, &lines);
    free(text);
    bool ok = pp_lines(pp, lines.v, lines.n);
    free_lines(lines.v, lines.n);
    return ok;
}

static bool do_include(PP *pp, const PLine *ln, const LineHead *h) {
    TokVec toks = {0};
    if (!lex_rest(pp, ln, h->rest, &toks)) return false;
    if (toks.v[0].kind != TK_STRING || toks.v[1].kind != TK_EOF) {
        tokvec_free(&toks);
        return pp_line_error(pp->err, ln, 0, "E_ASM_SYNTAX", ".include expects a quoted path");
    }
    char *want = xstrdup(toks.v[0].sval);
    tokvec_free(&toks);
    char *found = NULL;
    if (want[0] == '/') {
        if (file_exists(want)) found = xstrdup(want);
    } else {
        char *dir = path_dirname(ln->loc.file);
        char *cand = path_join(dir, want);
        free(dir);
        if (file_exists(cand)) found = cand;
        else free(cand);
        for (size_t i = 0; !found && i < pp->nlib; i++) {
            cand = path_join(pp->libpaths[i], want);
            if (file_exists(cand)) found = cand;
            else free(cand);
        }
    }
    if (!found) {
        pp_line_error(pp->err, ln, 0, "E_ASM_INCLUDE", "include file '%s' not found (searched the including file's directory and the library path)", want);
        free(want);
        return false;
    }
    free(want);
    bool ok = run_file(pp, found, ln);
    free(found);
    return ok;
}

static bool do_const(PP *pp, const PLine *ln, const LineHead *h, bool readonly) {
    TokVec toks = {0};
    if (!lex_rest(pp, ln, h->rest, &toks)) return false;
    bool ok = true;
    if (toks.v[0].kind != TK_IDENT || !tok_is(&toks.v[1], "=")) {
        ok = pp_line_error(pp->err, ln, toks.v[0].col, "E_ASM_SYNTAX", "expected NAME = expression");
        goto done;
    }
    ExprParser ep = {toks.v, toks.n, 2, pp_const_lookup, pp_defined, pp, {0}, 0};
    ExprVal v;
    if (!expr_eval(&ep, &v)) {
        ok = pp_line_error(pp->err, ln, ep.errcol, "E_ASM_EXPR", "%s", ep.err);
        goto done;
    }
    if (toks.v[ep.pos].kind != TK_EOF) {
        ok = pp_line_error(pp->err, ln, toks.v[ep.pos].col, "E_ASM_SYNTAX", "unexpected text after expression");
        goto done;
    }
    char *name = xstrndup(toks.v[0].start, (size_t)toks.v[0].len);
    void *old;
    ConstDef *d;
    if (smap_get(&pp->consts, name, &old)) {
        d = old;
        const ExprVal *o = &d->vals[d->n - 1].v;
        bool same = o->is_float == v.is_float && (v.is_float ? o->f == v.f : o->i == v.i);
        if ((readonly || smap_has(&pp->readonly, name)) && !same) {
            ok = pp_line_error(pp->err, ln, toks.v[0].col, "E_ASM_DUP_SYM",
                               "constant '%s' is already defined with a different value", name);
            free(name);
            goto done;
        }
        if (same) goto defined;
    } else {
        d = xcalloc(1, sizeof *d);
        smap_put(&pp->consts, name, d);
    }
    /* lines from here on see the new value; several assignments between two
     * lines leave only the last */
    if (d->n && d->vals[d->n - 1].from == pp->out.n) d->n--;
    d->vals = xrealloc(d->vals, (d->n + 1) * sizeof *d->vals);
    d->vals[d->n].from = pp->out.n;
    d->vals[d->n].v = v;
    d->n++;
defined:
    if (readonly) smap_put(&pp->readonly, name, NULL);
    free(name);
done:
    tokvec_free(&toks);
    return ok;
}

static bool is_pp_directive(const char *t, const LineHead *h) {
    static const char *const d[] = {".macro", ".endm", ".rept", ".irp", ".endr", ".include", ".const",
                                    ".set",   ".equ",  ".error", ".if", ".ifdef", ".ifndef", ".ifb",
                                    ".ifnb",  ".elif", ".else", ".endif", NULL};
    for (int i = 0; d[i]; i++)
        if (word_is(t, h, d[i])) return true;
    return false;
}

typedef struct {
    bool parent_active, active, taken, seen_else;
    const PLine *ln;
} Cond;

static bool pp_lines(PP *pp, const PLine *lines, size_t n) {
    static const char *const MACRO_OPEN[] = {".macro", NULL};
    static const char *const REPT_OPEN[] = {".rept", ".irp", NULL};
    Cond conds[MAX_COND_DEPTH];
    int nc = 0;
    for (size_t i = 0; i < n; i++) {
        const PLine *ln = &lines[i];
        const char *t = ln->text;
        LineHead h;
        line_head(t, &h);
        bool active = nc == 0 || conds[nc - 1].active;
        int col = (int)h.word + 1;

        if (word_is(t, &h, ".if") || word_is(t, &h, ".ifdef") || word_is(t, &h, ".ifndef") ||
            word_is(t, &h, ".ifb") || word_is(t, &h, ".ifnb")) {
            if (nc == MAX_COND_DEPTH) return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", ".if nested too deeply");
            Cond c = {active, false, false, false, ln};
            if (active) {
                bool v;
                if (!eval_cond(pp, ln, &h, &v)) return false;
                c.active = c.taken = v;
            }
            conds[nc++] = c;
            continue;
        }
        if (word_is(t, &h, ".elif")) {
            if (nc == 0 || conds[nc - 1].seen_else)
                return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", ".elif without matching .if");
            Cond *c = &conds[nc - 1];
            if (c->parent_active && !c->taken) {
                bool v;
                if (!eval_cond(pp, ln, &h, &v)) return false;
                c->active = c->taken = v;
            } else {
                c->active = false;
            }
            continue;
        }
        if (word_is(t, &h, ".else")) {
            if (nc == 0 || conds[nc - 1].seen_else)
                return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", ".else without matching .if");
            Cond *c = &conds[nc - 1];
            c->seen_else = true;
            c->active = c->parent_active && !c->taken;
            c->taken = true;
            continue;
        }
        if (word_is(t, &h, ".endif")) {
            if (nc == 0) return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", ".endif without matching .if");
            nc--;
            continue;
        }
        if (!active) continue;

        if (h.label_end && is_pp_directive(t, &h))
            return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", "a label cannot precede %.*s", (int)h.wlen, t + h.word);
        if (word_is(t, &h, ".macro")) {
            size_t end;
            if (!collect(pp, lines, n, i, MACRO_OPEN, ".endm", &end)) return false;
            if (!define_macro(pp, ln, &h, lines + i + 1, end - i - 1)) return false;
            i = end;
            continue;
        }
        if (word_is(t, &h, ".rept") || word_is(t, &h, ".irp")) {
            size_t end;
            if (!collect(pp, lines, n, i, REPT_OPEN, ".endr", &end)) return false;
            if (!expand_rept(pp, ln, &h, lines + i + 1, end - i - 1)) return false;
            i = end;
            continue;
        }
        if (word_is(t, &h, ".endm") || word_is(t, &h, ".endr"))
            return pp_line_error(pp->err, ln, col, "E_ASM_SYNTAX", "%.*s without a matching opener", (int)h.wlen,
                                 t + h.word);
        if (word_is(t, &h, ".include")) {
            if (!do_include(pp, ln, &h)) return false;
            continue;
        }
        if (word_is(t, &h, ".const") || word_is(t, &h, ".equ")) {
            if (!do_const(pp, ln, &h, true)) return false;
            continue;
        }
        if (word_is(t, &h, ".set")) {
            if (!do_const(pp, ln, &h, false)) return false;
            continue;
        }
        if (word_is(t, &h, ".error")) {
            TokVec toks = {0};
            if (!lex_rest(pp, ln, h.rest, &toks)) return false;
            const char *msg = toks.v[0].kind == TK_STRING ? toks.v[0].sval : "(.error)";
            pp_line_error(pp->err, ln, col, "E_ASM_USER", "%s", msg);
            tokvec_free(&toks);
            return false;
        }
        if (h.wlen) {
            char *name = xstrndup(t + h.word, h.wlen);
            void *mv;
            bool is_macro = smap_get(&pp->macros, name, &mv);
            free(name);
            if (is_macro) {
                if (h.label_end) emit(pp, ln, t, h.label_end);
                if (!expand_macro(pp, ln, mv, t + h.rest)) return false;
                continue;
            }
        }
        emit(pp, ln, t, strlen(t));
    }
    if (nc) return pp_line_error(pp->err, conds[nc - 1].ln, 0, "E_ASM_SYNTAX", "unterminated .if (missing .endif)");
    return true;
}

bool pp_run_file(PP *pp, const char *path) { return run_file(pp, path, NULL); }

bool pp_run_text(PP *pp, const char *name, const char *text) {
    PLineVec lines = {0};
    split_lines(pp, name, text, &lines);
    bool ok = pp_lines(pp, lines.v, lines.n);
    free_lines(lines.v, lines.n);
    return ok;
}
