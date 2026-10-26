/* bindgen.c - `mlwsvm bindgen`: FFI declarations for a C header.
 *
 *     mlwsvm bindgen [-l LIB] [-p PREFIX] [-o OUT] [--all] [--no-check] [--cc CLANG]
 *                    [--ast FILE.json] header.h [-- clang options...]
 *
 * Clang parses the header (`clang -Xclang -ast-dump=json -fsyntax-only`), and
 * the JSON syntax tree becomes `.cstruct`, `.callback`, `.extern` and `.const`
 * lines in an include file: the header's functions as natives PREFIX.name from
 * library LIB. Only the header's own declarations are written, not those of
 * the headers it includes, unless --all is given. Anything that cannot be
 * expressed (unions, bitfields, long double, ...) is written as a comment
 * saying why, so the output is a starting point to review, not a black box.
 * --ast reads a saved syntax tree instead of running clang. */
#include "tools.h"

#include <ctype.h>
#include <stdlib.h>

#include "../platform/platform.h"

/* ---- a small JSON reader ------------------------------------------------------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct Json {
    JKind kind;
    char *str;          /* J_STR, and J_NUM's text */
    bool b;
    uint32_t n;         /* elements or members */
    struct Json **items;
    char **keys;        /* J_OBJ */
} Json;

typedef struct {
    const char *s, *end;
    bool bad;
} JReader;

static void jws(JReader *r) {
    while (r->s < r->end && (*r->s == ' ' || *r->s == '\n' || *r->s == '\r' || *r->s == '\t')) r->s++;
}

static char *jstring(JReader *r) {
    StrBuf b = {0};
    r->s++; /* the opening quote */
    while (r->s < r->end && *r->s != '"') {
        char c = *r->s++;
        if (c != '\\') {
            sb_putc(&b, c);
            continue;
        }
        if (r->s >= r->end) break;
        c = *r->s++;
        switch (c) {
        case 'n': sb_putc(&b, '\n'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'u': { /* names in a syntax tree are ASCII: keep others as '?' */
            unsigned v = 0;
            for (int k = 0; k < 4 && r->s < r->end; k++, r->s++) v = v * 16 + (unsigned)(isdigit((unsigned char)*r->s) ? *r->s - '0' : (tolower((unsigned char)*r->s) - 'a' + 10));
            sb_putc(&b, v < 0x80 ? (char)v : '?');
            break;
        }
        default: sb_putc(&b, c); break;
        }
    }
    if (r->s >= r->end) r->bad = true;
    else r->s++;
    if (!b.s) sb_putc(&b, 0), b.n = 0;
    return b.s;
}

static void jfree(Json *j);

static Json *jvalue(JReader *r, int depth) {
    jws(r);
    if (r->s >= r->end || depth > 4000) {
        r->bad = true;
        return NULL;
    }
    Json *j = xcalloc(1, sizeof *j);
    char c = *r->s;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        j->kind = obj ? J_OBJ : J_ARR;
        r->s++;
        size_t cap = 0;
        for (;;) {
            jws(r);
            if (r->s < r->end && *r->s == (obj ? '}' : ']')) {
                r->s++;
                break;
            }
            if (j->n) {
                if (r->s >= r->end || *r->s != ',') {
                    r->bad = true;
                    break;
                }
                r->s++;
                jws(r);
            }
            char *key = NULL;
            if (obj) {
                if (r->s >= r->end || *r->s != '"') {
                    r->bad = true;
                    break;
                }
                key = jstring(r);
                jws(r);
                if (r->s >= r->end || *r->s != ':') {
                    free(key);
                    r->bad = true;
                    break;
                }
                r->s++;
            }
            Json *v = jvalue(r, depth + 1);
            if (r->bad) {
                free(key);
                jfree(v);
                break;
            }
            if (j->n == cap) {
                cap = cap ? cap * 2 : 4;
                j->items = xrealloc(j->items, cap * sizeof *j->items);
                if (obj) j->keys = xrealloc(j->keys, cap * sizeof *j->keys);
            }
            if (obj) j->keys[j->n] = key;
            j->items[j->n++] = v;
        }
    } else if (c == '"') {
        j->kind = J_STR;
        j->str = jstring(r);
    } else if (c == 't' || c == 'f' || c == 'n') {
        size_t len = c == 'f' ? 5 : 4;
        j->kind = c == 'n' ? J_NULL : J_BOOL;
        j->b = c == 't';
        r->s += len;
    } else {
        const char *start = r->s;
        while (r->s < r->end && (isdigit((unsigned char)*r->s) || strchr("+-.eE", *r->s))) r->s++;
        if (r->s == start) r->bad = true;
        j->kind = J_NUM;
        j->str = xstrndup(start, (size_t)(r->s - start));
    }
    return j;
}

static void jfree(Json *j) {
    if (!j) return;
    for (uint32_t i = 0; i < j->n; i++) {
        jfree(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

static Json *jget(const Json *j, const char *key) {
    if (!j || j->kind != J_OBJ) return NULL;
    for (uint32_t i = 0; i < j->n; i++)
        if (!strcmp(j->keys[i], key)) return j->items[i];
    return NULL;
}

static const char *jstr(const Json *j, const char *key) {
    Json *v = jget(j, key);
    return v && v->kind == J_STR ? v->str : NULL;
}

static bool jtrue(const Json *j, const char *key) {
    Json *v = jget(j, key);
    return v && v->kind == J_BOOL && v->b;
}

/* ---- C types, from clang's type strings ----------------------------------------- */

typedef enum { CT_VOID, CT_INT, CT_FLOAT, CT_RECORD, CT_ENUM, CT_PTR, CT_ARRAY, CT_FUNC, CT_BAD } CKind;

typedef struct CType {
    CKind k;
    int size;           /* CT_INT, CT_FLOAT: bytes */
    bool is_signed, is_char, is_bool;
    bool is_const;      /* declared const (what a pointer points to may not be changed) */
    bool is_union;      /* CT_RECORD */
    char *name;         /* CT_RECORD, CT_ENUM; CT_BAD: why */
    struct CType *to;   /* CT_PTR, CT_ARRAY: the element; CT_FUNC: the result */
    uint32_t count;     /* CT_ARRAY */
    VEC(struct CType *) params;
    bool variadic;
} CType;

typedef struct {
    const char *s;
    char tok[256];
    StrMap *typedefs;   /* name -> qualType (for names left in a type) */
    int depth;
} TLex;

static CType *ct_new(CKind k) {
    CType *t = xcalloc(1, sizeof *t);
    t->k = k;
    return t;
}

static CType *ct_bad(const char *why) {
    CType *t = ct_new(CT_BAD);
    t->name = xstrdup(why);
    return t;
}

static void ct_free(CType *t) {
    if (!t) return;
    ct_free(t->to);
    for (size_t i = 0; i < t->params.n; i++) ct_free(t->params.v[i]);
    VEC_FREE(t->params);
    free(t->name);
    free(t);
}

/* The next token: an identifier, a number, "...", or one punctuation character. */
static const char *tnext(TLex *l) {
    while (*l->s == ' ') l->s++;
    size_t n = 0;
    if (!*l->s) l->tok[0] = 0;
    else if (isalnum((unsigned char)*l->s) || *l->s == '_') {
        while ((isalnum((unsigned char)*l->s) || *l->s == '_') && n < sizeof l->tok - 1) l->tok[n++] = *l->s++;
        l->tok[n] = 0;
    } else if (!strncmp(l->s, "...", 3)) {
        strcpy(l->tok, "...");
        l->s += 3;
    } else {
        l->tok[0] = *l->s++;
        l->tok[1] = 0;
    }
    return l->tok;
}

static const char *tpeek(TLex *l) {
    TLex save = *l;
    tnext(l);
    static char peeked[256];
    snprintf(peeked, sizeof peeked, "%s", l->tok);
    *l = save;
    return peeked;
}

static CType *parse_type_string(const char *s, StrMap *typedefs, int depth);

/* Specifiers: builtin type words, struct/union/enum NAME, or a typedef name. */
static CType *specifiers_(TLex *l, bool *is_const_out) {
    int is_unsigned = 0, is_signed = 0, shorts = 0, longs = 0;
    bool is_const = false;
    const char *base = NULL;
    CType *named = NULL;
    for (;;) {
        const char *p = tpeek(l);
        if (!strcmp(p, "const") || !strcmp(p, "volatile") || !strcmp(p, "restrict") || !strcmp(p, "__restrict") ||
            !strcmp(p, "_Nonnull") || !strcmp(p, "_Nullable") || !strcmp(p, "_Null_unspecified") ||
            !strcmp(p, "_Atomic")) {
            tnext(l);
            is_const = is_const || !strcmp(l->tok, "const");
        } else if (!strcmp(p, "unsigned")) tnext(l), is_unsigned = 1;
        else if (!strcmp(p, "signed")) tnext(l), is_signed = 1;
        else if (!strcmp(p, "short")) tnext(l), shorts++;
        else if (!strcmp(p, "long")) tnext(l), longs++;
        else if (!strcmp(p, "int") || !strcmp(p, "char") || !strcmp(p, "float") || !strcmp(p, "double") ||
                 !strcmp(p, "void") || !strcmp(p, "_Bool") || !strcmp(p, "bool") || !strcmp(p, "__int128") ||
                 !strcmp(p, "_Float16") || !strcmp(p, "__fp16")) {
            static const char *const W[] = {"int", "char", "float", "double", "void", "_Bool", "bool", "__int128",
                                            "_Float16", "__fp16"};
            tnext(l);
            for (size_t i = 0; i < sizeof W / sizeof W[0]; i++)
                if (!strcmp(l->tok, W[i])) base = W[i];
        } else if (!strcmp(p, "struct") || !strcmp(p, "union") || !strcmp(p, "enum")) {
            tnext(l);
            bool un = !strcmp(l->tok, "union"), en = !strcmp(l->tok, "enum");
            tnext(l);
            if (!isalpha((unsigned char)l->tok[0]) && l->tok[0] != '_') return ct_bad("an anonymous struct, union or enum");
            named = ct_new(en ? CT_ENUM : CT_RECORD);
            named->is_union = un;
            named->name = xstrdup(l->tok);
        } else if ((isalpha((unsigned char)p[0]) || p[0] == '_') && !base && !named && !shorts && !longs &&
                   !is_unsigned && !is_signed) {
            tnext(l); /* a typedef name that clang left in the type */
            void *v;
            if (l->depth < 32 && smap_get(l->typedefs, l->tok, &v)) named = parse_type_string(v, l->typedefs, l->depth + 1);
            else {
                char why[300];
                snprintf(why, sizeof why, "the type %s", l->tok);
                named = ct_bad(why);
            }
        } else break;
    }
    *is_const_out = is_const;
    if (named) return named;
    if (base && !strcmp(base, "void")) return ct_new(CT_VOID);
    if (base && (!strcmp(base, "float") || !strcmp(base, "double"))) {
        if (longs) return ct_bad("long double");
        CType *t = ct_new(CT_FLOAT);
        t->size = !strcmp(base, "float") ? 4 : 8;
        return t;
    }
    if (base && (!strcmp(base, "__int128") || !strcmp(base, "_Float16") || !strcmp(base, "__fp16")))
        return ct_bad(base);
    CType *t = ct_new(CT_INT);
    if (base && (!strcmp(base, "_Bool") || !strcmp(base, "bool"))) {
        t->size = 1, t->is_bool = true;
        return t;
    }
    if (base && !strcmp(base, "char")) {
        t->size = 1;
        t->is_char = !is_unsigned && !is_signed;
        t->is_signed = !is_unsigned;
        return t;
    }
    if (!base && !shorts && !longs && !is_unsigned && !is_signed) {
        ct_free(t);
        return ct_bad("an unknown type");
    }
    t->size = shorts ? (int)sizeof(short) : longs >= 2 ? (int)sizeof(long long) : longs ? (int)sizeof(long) : (int)sizeof(int);
    t->is_signed = !is_unsigned;
    return t;
}

static CType *specifiers(TLex *l) {
    bool c = false;
    CType *t = specifiers_(l, &c);
    t->is_const = c;
    return t;
}

static CType *declarator(TLex *l, CType *base);

/* Array and function suffixes, applied right to left. */
static CType *suffixes(TLex *l, CType *base) {
    const char *p = tpeek(l);
    if (!strcmp(p, "[")) {
        tnext(l);
        uint32_t n = 0;
        if (isdigit((unsigned char)tpeek(l)[0])) n = (uint32_t)strtoul(tnext(l), NULL, 10);
        if (strcmp(tnext(l), "]")) {
            ct_free(base);
            return ct_bad("a type clang wrote in an unexpected way");
        }
        CType *inner = suffixes(l, base); /* int [2][3]: an array of 2 arrays of 3 */
        CType *t = ct_new(CT_ARRAY);
        t->to = inner;
        t->count = n;
        return t;
    }
    if (!strcmp(p, "(")) {
        tnext(l);
        CType *f = ct_new(CT_FUNC);
        f->to = base;
        if (!strcmp(tpeek(l), "void")) { /* (void): no parameters */
            TLex save = *l;
            tnext(l);
            if (!strcmp(tpeek(l), ")")) tnext(l);
            else {
                *l = save;
                goto params;
            }
            return suffixes(l, f);
        }
    params:
        while (strcmp(tpeek(l), ")") && tpeek(l)[0]) {
            if (!strcmp(tpeek(l), "...")) {
                tnext(l);
                f->variadic = true;
            } else {
                CType *pt = declarator(l, specifiers(l));
                VEC_PUSH(f->params, pt);
            }
            if (!strcmp(tpeek(l), ",")) tnext(l);
            else if (strcmp(tpeek(l), ")")) break;
        }
        if (strcmp(tnext(l), ")")) {
            ct_free(f);
            return ct_bad("a type clang wrote in an unexpected way");
        }
        return suffixes(l, f);
    }
    return base;
}

/* Pointers, a parenthesized declarator, then suffixes. */
static CType *declarator(TLex *l, CType *base) {
    while (!strcmp(tpeek(l), "*")) {
        tnext(l);
        CType *t = ct_new(CT_PTR);
        t->to = base;
        base = t;
        while (!strcmp(tpeek(l), "const") || !strcmp(tpeek(l), "volatile") || !strcmp(tpeek(l), "restrict") ||
               !strcmp(tpeek(l), "__restrict") || !strcmp(tpeek(l), "_Nonnull") || !strcmp(tpeek(l), "_Nullable") ||
               !strcmp(tpeek(l), "_Null_unspecified"))
            tnext(l);
    }
    const char *p = tpeek(l);
    if (!strcmp(p, "(")) {
        TLex save = *l;
        tnext(l);
        const char *q = tpeek(l);
        if (!strcmp(q, "*") || !strcmp(q, "(") || !strcmp(q, "^") || !strcmp(q, "[")) {
            /* (inner)suffixes: the suffixes apply to base first, the inner declarator to the result */
            int level = 1;
            while (level && *l->s) {
                tnext(l);
                if (!strcmp(l->tok, "(")) level++;
                else if (!strcmp(l->tok, ")")) level--;
            }
            CType *outer = suffixes(l, base);
            TLex after = *l;
            *l = save;
            tnext(l);
            CType *t = declarator(l, outer);
            tnext(l); /* its ')' */
            *l = after;
            return t;
        }
        *l = save;
    }
    return suffixes(l, base);
}

static CType *parse_type_string(const char *s, StrMap *typedefs, int depth) {
    TLex l = {s, {0}, typedefs, depth};
    CType *t = declarator(&l, specifiers(&l));
    if (tpeek(&l)[0]) { /* something left over */
        ct_free(t);
        return ct_bad("a type clang wrote in an unexpected way");
    }
    return t;
}

/* ---- the generator ---------------------------------------------------------------- */

typedef struct {
    char *name;
    const Json *node;      /* the RecordDecl with its fields */
    int state;             /* 0 not yet written, 1 being written, 2 written, 3 cannot be */
    char *why;             /* state 3 */
} Record;

typedef struct {
    StrBuf out;
    StrMap typedefs;       /* typedef name -> qualType (desugared when clang gives it) */
    StrMap records;        /* struct name -> Record * */
    StrMap callbacks;      /* .callback name -> its signature, to tell same from different */
    StrVec aliases;        /* "struct NAME" strings for typedef'd anonymous structs */
    StrMap last_decl;      /* function name -> its last declaration (the one in force) */
    StrMap written;        /* functions already written */
    bool check;            /* the library is open: leave out symbols it does not have */
    void *libh;            /* (NULL: the program's own symbols) */
    int missing;
    const char *lib, *prefix, *header;
    bool all;
    const char *cur_file;  /* clang writes a file name only when it changes */
    int skipped;
} Gen;

static const char *int_type(const CType *t) {
    static const char *const S[] = {"i8", "i16", "i32", "i64"}, *const U[] = {"u8", "u16", "u32", "u64"};
    int k = t->size == 1 ? 0 : t->size == 2 ? 1 : t->size == 4 ? 2 : 3;
    return t->is_signed && !t->is_bool ? S[k] : U[k];
}

static bool record_ok(Gen *g, const char *name);

/* A field's type as a .cstruct field type, or NULL (with why). */
static bool field_type(Gen *g, const CType *t, StrBuf *b, const char **why) {
    switch (t->k) {
    case CT_INT: sb_puts(b, int_type(t)); return true;
    case CT_ENUM: sb_puts(b, "i32"); return true;
    case CT_FLOAT: sb_puts(b, t->size == 4 ? "f32" : "f64"); return true;
    case CT_PTR:
        if (t->to->k == CT_INT && t->to->is_char) sb_puts(b, "cstr");
        else if (t->to->k == CT_RECORD && !t->to->is_union) sb_printf(b, "ptr<%s>", t->to->name);
        else sb_puts(b, "ptr");
        return true;
    case CT_RECORD:
        if (t->is_union) return *why = "a union", false;
        if (!record_ok(g, t->name)) return *why = "a struct that cannot be described", false;
        sb_printf(b, "struct %s", t->name);
        return true;
    case CT_ARRAY:
        if (!t->count) return *why = "an array of unknown size", false;
        if (t->to->k == CT_INT || t->to->k == CT_ENUM || t->to->k == CT_FLOAT) {
            if (!field_type(g, t->to, b, why)) return false;
            sb_printf(b, "[%u]", t->count);
            return true;
        }
        return *why = "an array of something other than numbers", false;
    case CT_BAD: return *why = t->name, false;
    default: return *why = "a function or void", false;
    }
}

/* Write a struct's .cstruct (after those it contains), once. */
static bool record_ok(Gen *g, const char *name) {
    void *v;
    if (!smap_get(&g->records, name, &v)) return false; /* opaque: declared, never defined */
    Record *r = v;
    if (r->state == 2) return true;
    if (r->state == 3 || r->state == 1) return false; /* (1: a struct containing itself by value) */
    r->state = 1;
    StrBuf body = {0};
    const char *why = NULL;
    char whybuf[300];
    const Json *inner = jget(r->node, "inner");
    uint32_t nfields = 0;
    for (uint32_t i = 0; inner && i < inner->n && !why; i++) {
        const Json *f = inner->items[i];
        const char *kind = jstr(f, "kind");
        if (!kind || strcmp(kind, "FieldDecl")) continue;
        if (jtrue(f, "isBitfield")) {
            why = "a bitfield";
            break;
        }
        const Json *ty = jget(f, "type");
        const char *qt = jstr(ty, "desugaredQualType") ? jstr(ty, "desugaredQualType") : jstr(ty, "qualType");
        const char *fname = jstr(f, "name");
        if (!qt || !fname || !*fname) {
            why = "an unnamed field";
            break;
        }
        CType *t = parse_type_string(qt, &g->typedefs, 0);
        sb_printf(&body, "    %s: ", fname);
        if (!field_type(g, t, &body, &why)) {
            snprintf(whybuf, sizeof whybuf, "%s", why); /* it may point into t */
            why = whybuf;
            ct_free(t);
            break;
        }
        ct_free(t);
        sb_printf(&body, "    ; %s\n", qt);
        nfields++;
    }
    if (!why && !nfields) why = "no fields";
    if (why) {
        r->state = 3;
        r->why = xstrdup(why);
        sb_printf(&g->out, "; struct %s: not described (%s); passed only as a pointer, ptr<%s>\n", name, why, name);
        sb_free(&body);
        return false;
    }
    sb_printf(&g->out, ".cstruct %s\n%s.ends\n", name, body.s);
    sb_free(&body);
    r->state = 2;
    return true;
}

/* A parameter or result type for .extern, or false (with why). */
static bool ffi_type_of(Gen *g, const CType *t, bool param, const char *cb_name, StrBuf *b, const char **why);

/* Write the .callback for a function-pointer parameter; its name in *name. */
static bool callback_for(Gen *g, const CType *fn, const char *want, char *name, size_t namesz, const char **why) {
    StrBuf sig = {0};
    sb_puts(&sig, "(");
    for (size_t i = 0; i < fn->params.n; i++) {
        const CType *pt = fn->params.v[i];
        if (i) sb_puts(&sig, ", ");
        if (pt->k == CT_VOID && fn->params.n == 1) break;
        bool ok = pt->k == CT_INT || pt->k == CT_ENUM || pt->k == CT_FLOAT || (pt->k == CT_PTR && pt->to->k != CT_FUNC);
        if (!ok || !ffi_type_of(g, pt, false, NULL, &sig, why)) {
            sb_free(&sig);
            *why = "a callback whose parameters are not numbers or pointers";
            return false;
        }
    }
    sb_puts(&sig, ")");
    if (fn->variadic) {
        sb_free(&sig);
        return *why = "a variadic callback", false;
    }
    const CType *rt = fn->to;
    if (rt->k != CT_VOID) {
        sb_puts(&sig, " -> (");
        /* a cstr result could not be kept alive for C: only numbers and plain pointers */
        bool ok = rt->k == CT_INT || rt->k == CT_ENUM || rt->k == CT_FLOAT ||
                  (rt->k == CT_PTR && rt->to->k != CT_FUNC && !(rt->to->k == CT_INT && rt->to->is_char));
        if (!ok || !ffi_type_of(g, rt, false, NULL, &sig, why)) {
            sb_free(&sig);
            *why = "a callback whose result is not a number or pointer";
            return false;
        }
        sb_puts(&sig, ")");
    }
    snprintf(name, namesz, "%s", want);
    for (int n = 2;; n++) {
        void *v;
        if (!smap_get(&g->callbacks, name, &v)) break;
        if (!strcmp(v, sig.s)) { /* the same type: written already */
            sb_free(&sig);
            return true;
        }
        snprintf(name, namesz, "%s_%d", want, n);
    }
    smap_put(&g->callbacks, name, xstrdup(sig.s));
    sb_printf(&g->out, ".callback %s %s\n", name, sig.s);
    sb_free(&sig);
    return true;
}

static bool ffi_type_of(Gen *g, const CType *t, bool param, const char *cb_name, StrBuf *b, const char **why) {
    switch (t->k) {
    case CT_INT: sb_puts(b, int_type(t)); return true;
    case CT_ENUM: sb_puts(b, "i32"); return true;
    case CT_FLOAT: sb_puts(b, t->size == 4 ? "f32" : "f64"); return true;
    case CT_RECORD:
        if (t->is_union) return *why = "a union passed by value", false;
        if (!record_ok(g, t->name)) return *why = "a struct that cannot be described, passed by value", false;
        sb_printf(b, "struct %s", t->name);
        return true;
    case CT_PTR: {
        const CType *to = t->to;
        if (to->k == CT_INT && to->is_char && param && cb_name && !to->is_const)
            sb_puts(b, "buf"); /* char *: C writes into it (const char *: a string in) */
        else if (to->k == CT_INT && to->is_char) sb_puts(b, "cstr");
        else if (to->k == CT_FUNC) {
            char name[200];
            if (!param || !cb_name) sb_puts(b, "ptr");
            else if (callback_for(g, to, cb_name, name, sizeof name, why)) sb_printf(b, "callback %s", name);
            else return false;
        } else if (to->k == CT_RECORD && !to->is_union) {
            if (param && record_ok(g, to->name)) sb_printf(b, "struct* %s", to->name);
            else sb_printf(b, "ptr<%s>", to->name);
        } else if (to->k == CT_PTR && param && to->to->k == CT_RECORD && !to->to->is_union)
            sb_printf(b, "out ptr<%s>", to->to->name); /* T **: C hands back a pointer */
        else if (param && (to->k == CT_INT || to->k == CT_FLOAT || to->k == CT_ENUM || to->k == CT_VOID))
            sb_puts(b, "buf"); /* memory: or `out T` for a single value, or `ptr` for a handle */
        else sb_puts(b, "ptr");
        return true;
    }
    case CT_BAD: return *why = t->name, false;
    case CT_VOID: return *why = "void", false;
    default: return *why = "a function or array", false;
    }
}

/* The symbol to call: the name, or an asm label's (glibc redirects scanf to
 * __isoc99_scanf), which clang gives only as the mangled name. */
static const char *symbol_of(const Json *n) {
    const Json *inner = jget(n, "inner");
    for (uint32_t i = 0; inner && i < inner->n; i++)
        if (jstr(inner->items[i], "kind") && !strcmp(jstr(inner->items[i], "kind"), "AsmLabelAttr") &&
            jstr(n, "mangledName")) {
            const char *m = jstr(n, "mangledName");
#ifdef __APPLE__
            if (m[0] == '_') m++; /* Mach-O symbols start with an underscore that dlsym adds itself */
#endif
            return m;
        }
    return jstr(n, "name");
}

static void gen_function(Gen *g, const Json *n) {
    const char *name = jstr(n, "name");
    void *v;
    if (!name || smap_get(&g->written, name, &v)) return; /* declared again: written once */
    smap_put(&g->written, name, (void *)name);
    if (smap_get(&g->last_decl, name, &v)) n = v; /* the declaration in force */
    const Json *ty = jget(n, "type");
    const char *qt = jstr(ty, "desugaredQualType") ? jstr(ty, "desugaredQualType") : jstr(ty, "qualType");
    if (!name || !qt) return;
    CType *ft = parse_type_string(qt, &g->typedefs, 0);
    const char *why = NULL;
    StrBuf b = {0};
    if (ft->k != CT_FUNC) why = ft->k == CT_BAD ? ft->name : "not a function type";
    /* Parameter types: from the ParmVarDecls (desugared), which name them too. */
    const Json *inner = jget(n, "inner");
    VEC(const Json *) parms = {0};
    for (uint32_t i = 0; inner && i < inner->n; i++)
        if (jstr(inner->items[i], "kind") && !strcmp(jstr(inner->items[i], "kind"), "ParmVarDecl"))
            VEC_PUSH(parms, inner->items[i]);
    if (!why) {
        if (ft->params.n > FFI_MAX_ARGS) why = "more than 8 parameters";
        sb_printf(&b, ".extern %s.%s, \"%s\", \"%s\" (", g->prefix, name, g->lib, symbol_of(n));
        for (size_t i = 0; i < ft->params.n && !why; i++) {
            const CType *pt = ft->params.v[i];
            if (pt->k == CT_VOID) break; /* (void) */
            if (i) sb_puts(&b, ", ");
            /* a callback is named after its typedef, or after the function and parameter */
            char cb[200];
            const char *pq = i < parms.n ? jstr(jget(parms.v[i], "type"), "qualType") : NULL;
            const char *pname = i < parms.n ? jstr(parms.v[i], "name") : NULL;
            if (pq && (isalpha((unsigned char)pq[0]) || pq[0] == '_') && !strpbrk(pq, " *()[]")) snprintf(cb, sizeof cb, "%s", pq);
            else if (pname) snprintf(cb, sizeof cb, "%s_%s", name, pname);
            else snprintf(cb, sizeof cb, "%s_arg%zu", name, i + 1);
            if (!ffi_type_of(g, pt, true, cb, &b, &why)) break;
        }
        if (!why && ft->variadic) sb_puts(&b, ", ...");
        sb_puts(&b, ")");
        if (!why && ft->to->k != CT_VOID) {
            sb_puts(&b, " -> (");
            if (ft->to->k == CT_PTR && ft->to->to->k == CT_RECORD && !ft->to->to->is_union)
                sb_printf(&b, "ptr<%s>", ft->to->to->name); /* a struct pointer result: the library's */
            else if (!ffi_type_of(g, ft->to, false, NULL, &b, &why)) why = why ? why : "its result";
            sb_puts(&b, ")");
        }
    }
    if (!why && g->check && !plat_dlsym(g->libh, symbol_of(n))) {
        /* a compiler intrinsic (alloca), or a function this platform lacks */
        sb_printf(&g->out, "; %s: skipped (the library has no symbol %s)\n", name, symbol_of(n));
        g->missing++;
    } else if (why) {
        sb_printf(&g->out, "; %s: skipped (%s): %s\n", name, why, qt);
        g->skipped++;
    } else {
        sb_printf(&g->out, "%s\n", b.s);
        if (ft->variadic) sb_printf(&g->out, "; (%s is variadic: add the types it passes after `...`)\n", name);
    }
    VEC_FREE(parms);
    sb_free(&b);
    ct_free(ft);
}

static void gen_enum(Gen *g, const Json *n) {
    const Json *inner = jget(n, "inner");
    const char *ename = jstr(n, "name");
    sb_printf(&g->out, "; enum %s\n", ename ? ename : "(anonymous)");
    int64_t next = 0;
    for (uint32_t i = 0; inner && i < inner->n; i++) {
        const Json *c = inner->items[i];
        if (!jstr(c, "kind") || strcmp(jstr(c, "kind"), "EnumConstantDecl")) continue;
        const Json *ci = jget(c, "inner");
        for (uint32_t k = 0; ci && k < ci->n; k++) { /* an explicit value: clang has computed it */
            const char *v = jstr(ci->items[k], "value");
            if (v) next = strtoll(v, NULL, 10);
        }
        sb_printf(&g->out, ".const %s = %lld\n", jstr(c, "name"), (long long)next);
        next++;
    }
}

/* Follow clang's file names in document order, so each declaration knows its file. */
static void track_files(Gen *g, const Json *n) {
    if (!n || n->kind != J_OBJ) return;
    static const char *const LOCS[] = {"loc", "range"};
    for (int k = 0; k < 2; k++) {
        const Json *l = jget(n, LOCS[k]);
        const Json *parts[6] = {l, jget(l, "spellingLoc"), jget(l, "expansionLoc"), jget(l, "begin"), jget(l, "end"), NULL};
        const Json *be[2] = {jget(l, "begin"), jget(l, "end")};
        for (int i = 0; parts[i]; i++)
            if (jstr(parts[i], "file")) g->cur_file = jstr(parts[i], "file");
        for (int i = 0; i < 2; i++) {
            if (jstr(jget(be[i], "spellingLoc"), "file")) g->cur_file = jstr(jget(be[i], "spellingLoc"), "file");
            if (jstr(jget(be[i], "expansionLoc"), "file")) g->cur_file = jstr(jget(be[i], "expansionLoc"), "file");
        }
    }
    const Json *inner = jget(n, "inner");
    for (uint32_t i = 0; inner && i < inner->n; i++) track_files(g, inner->items[i]);
}

/* The file a declaration's location names (before its range and children move on). */
static const char *decl_file(Gen *g, const Json *n) {
    const Json *loc = jget(n, "loc");
    const char *f = jstr(jget(loc, "expansionLoc"), "file");
    if (!f) f = jstr(jget(loc, "spellingLoc"), "file");
    if (!f) f = jstr(loc, "file");
    if (f) g->cur_file = f;
    return g->cur_file;
}

static bool same_file(const char *a, const char *b) {
    if (!a || !b) return false;
    if (!strcmp(a, b)) return true;
    char *ra = plat_realpath(a), *rb = plat_realpath(b);
    bool same = ra && rb && !strcmp(ra, rb);
    free(ra);
    free(rb);
    return same;
}

/* Collect a struct definition, and those defined inside it. */
static void add_records(Gen *g, const Json *n) {
    if (jtrue(n, "completeDefinition") && jstr(n, "name") && jstr(n, "tagUsed") && !strcmp(jstr(n, "tagUsed"), "struct")) {
        Record *r = xcalloc(1, sizeof *r);
        r->name = xstrdup(jstr(n, "name"));
        r->node = n;
        void *old;
        if (smap_get(&g->records, r->name, &old)) free(((Record *)old)->name), free(old);
        smap_put(&g->records, r->name, r);
    }
    const Json *inner = jget(n, "inner");
    for (uint32_t i = 0; inner && i < inner->n; i++)
        if (jstr(inner->items[i], "kind") && !strcmp(jstr(inner->items[i], "kind"), "RecordDecl")) add_records(g, inner->items[i]);
}

static bool generate(Gen *g, const Json *root) {
    const Json *top = jget(root, "inner");
    if (!top || top->kind != J_ARR) return false;
    /* First every typedef and struct definition, wherever it is (a struct the
     * header uses may be defined in another header); then the header's own. */
    const char *file_before = g->cur_file;
    StrMap anon = {0}; /* id -> an anonymous struct or enum, named by a typedef */
    for (uint32_t i = 0; i < top->n; i++) {
        const Json *n = top->items[i];
        const char *kind = jstr(n, "kind");
        if (!kind) continue;
        if ((!strcmp(kind, "RecordDecl") || !strcmp(kind, "EnumDecl")) && !jstr(n, "name") && jstr(n, "id"))
            smap_put(&anon, jstr(n, "id"), (void *)n);
        if (!strcmp(kind, "TypedefDecl")) {
            const char *name = jstr(n, "name");
            const Json *ty = jget(n, "type");
            const char *qt = jstr(ty, "desugaredQualType") ? jstr(ty, "desugaredQualType") : jstr(ty, "qualType");
            const Json *in = jget(n, "inner");
            const Json *owned = in && in->n ? jget(in->items[0], "ownedTagDecl") : NULL;
            void *v;
            if (name && owned && jstr(owned, "id") && smap_get(&anon, jstr(owned, "id"), &v)) {
                /* typedef struct { ... } NAME: the struct takes the typedef's name */
                const Json *decl = v;
                if (!strcmp(jstr(decl, "kind"), "EnumDecl")) smap_put(&g->typedefs, name, "int");
                else if (jtrue(decl, "completeDefinition") && jstr(decl, "tagUsed") && !strcmp(jstr(decl, "tagUsed"), "struct")) {
                    Record *r = xcalloc(1, sizeof *r);
                    r->name = xstrdup(name);
                    r->node = decl;
                    smap_put(&g->records, r->name, r);
                    size_t an = strlen(name) + 8;
                    char *alias = xmalloc(an);
                    snprintf(alias, an, "struct %s", name);
                    VEC_PUSH(g->aliases, alias);
                    smap_put(&g->typedefs, name, alias);
                }
            } else if (name && qt && strcmp(name, qt)) smap_put(&g->typedefs, name, (void *)qt);
        } else if (!strcmp(kind, "RecordDecl")) {
            add_records(g, n);
        } else if (!strcmp(kind, "FunctionDecl") && jstr(n, "name") && !jtrue(n, "isImplicit")) {
            smap_put(&g->last_decl, jstr(n, "name"), (void *)n);
        }
    }
    smap_free(&anon);
    g->cur_file = file_before;
    for (uint32_t i = 0; i < top->n; i++) {
        const Json *n = top->items[i];
        const char *kind = jstr(n, "kind");
        const char *file = decl_file(g, n);
        track_files(g, n);
        if (!kind || jtrue(n, "isImplicit")) continue;
        if (!g->all && !same_file(file, g->header)) continue;
        if (!strcmp(kind, "FunctionDecl")) {
            const char *sc = jstr(n, "storageClass");
            if (sc && !strcmp(sc, "static")) continue;
            if (jtrue(n, "inline") && !(sc && !strcmp(sc, "extern"))) continue; /* may have no symbol to call */
            gen_function(g, n);
        } else if (!strcmp(kind, "EnumDecl")) {
            gen_enum(g, n);
        } else if (!strcmp(kind, "TypedefDecl") && jstr(n, "name")) {
            void *v; /* typedef struct { ... } NAME, even if unused */
            if (smap_get(&g->records, jstr(n, "name"), &v)) record_ok(g, jstr(n, "name"));
        } else if (!strcmp(kind, "RecordDecl") && jtrue(n, "completeDefinition") && jstr(n, "name") &&
                   jstr(n, "tagUsed") && !strcmp(jstr(n, "tagUsed"), "struct")) {
            record_ok(g, jstr(n, "name")); /* the header's structs, even unused */
        }
    }
    return true;
}

/* ---- running clang ---------------------------------------------------------------- */

static char *run_clang(const char *cc, const char *header, int nextra, char **extra, int *status) {
    VEC(char *) argv = {0};
    VEC_PUSH(argv, (char *)cc);
    VEC_PUSH(argv, "-Xclang");
    VEC_PUSH(argv, "-ast-dump=json");
    VEC_PUSH(argv, "-fsyntax-only");
    VEC_PUSH(argv, "-xc");
    for (int i = 0; i < nextra; i++) VEC_PUSH(argv, extra[i]);
    VEC_PUSH(argv, (char *)header);
    VEC_PUSH(argv, NULL);
    PlatChild child;
    int err;
    if (!plat_spawn(argv.v, PLAT_PIPE_OUT, &child, &err)) {
        VEC_FREE(argv);
        *status = -1;
        return NULL;
    }
    VEC_FREE(argv);
    StrBuf b = {0};
    char buf[65536];
    for (;;) {
        long n = plat_fd_read(child.out_fd, buf, sizeof buf, &err);
        if (n == PLAT_IO_AGAIN) {
            plat_wait_readable(child.out_fd, 1000000000ull);
            continue;
        }
        if (n <= 0) break;
        sb_putn(&b, buf, (size_t)n);
    }
    plat_fd_close(child.out_fd);
    plat_wait_child(child.pid, status, &err);
    return b.s ? b.s : xstrdup("");
}

/* Open the library as .extern would (alternatives separated by '|'; "libz"
 * also as libz.so or libz.dylib), to check that its symbols exist. */
static bool open_lib(const char *spec, void **h) {
    const char *s = spec;
    do {
        const char *bar = strchr(s, '|');
        size_t n = bar ? (size_t)(bar - s) : strlen(s);
        char *alt = xstrndup(s, n);
        if (!*alt) {
            free(alt);
            *h = NULL;
            return true;
        }
        *h = plat_dlopen(alt, NULL, 0);
        if (!*h && !strchr(alt, '/')) {
            char *with = xmalloc(n + 16);
            snprintf(with, n + 16, "%s%s", alt, plat_dl_suffix());
            *h = plat_dlopen(with, NULL, 0);
            free(with);
        }
        free(alt);
        if (*h) return true;
        s = bar ? bar + 1 : NULL;
    } while (s);
    return false;
}

static int usage(void) {
    fprintf(stderr, "usage: mlwsvm bindgen [-l LIB] [-p PREFIX] [-o OUT] [--all] [--no-check] [--cc CLANG]\n"
                    "                      [--ast FILE.json] header.h [-- clang options...]\n");
    return 2;
}

int tool_bindgen(int argc, char **argv) {
    const char *lib = "", *prefix = NULL, *out = NULL, *cc = "clang", *ast = NULL, *header = NULL;
    bool all = false, check = true;
    int i = 0, nextra = 0;
    char **extra = NULL;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--cc") && i + 1 < argc) cc = argv[++i];
        else if (!strcmp(argv[i], "--ast") && i + 1 < argc) ast = argv[++i];
        else if (!strcmp(argv[i], "--all")) all = true;
        else if (!strcmp(argv[i], "--no-check")) check = false;
        else if (!strcmp(argv[i], "--")) {
            extra = argv + i + 1;
            nextra = argc - i - 1;
            break;
        } else if (argv[i][0] == '-') return usage();
        else if (!header) header = argv[i];
        else return usage();
    }
    if (!header) return usage();
    char *text;
    if (ast) {
        text = read_file(ast, NULL);
        if (!text) {
            fprintf(stderr, "mlwsvm bindgen: cannot read %s\n", ast);
            return 1;
        }
    } else {
        int status;
        text = run_clang(cc, header, nextra, extra, &status);
        if (!text || status != 0) {
            fprintf(stderr, "mlwsvm bindgen: %s could not parse %s (is clang installed? --cc names another)\n", cc,
                    header);
            free(text);
            return 1;
        }
    }
    JReader r = {text, text + strlen(text), false};
    Json *root = jvalue(&r, 0);
    if (r.bad || !root) {
        fprintf(stderr, "mlwsvm bindgen: the syntax tree is not valid JSON\n");
        jfree(root);
        free(text);
        return 1;
    }
    /* The prefix: the header's name, as an identifier. */
    char pfx[128];
    if (!prefix) {
        const char *base = strrchr(header, '/') ? strrchr(header, '/') + 1 : header;
        size_t n = 0;
        for (; base[n] && base[n] != '.' && n < sizeof pfx - 1; n++) pfx[n] = isalnum((unsigned char)base[n]) ? base[n] : '_';
        pfx[n] = 0;
        prefix = pfx;
    }
    Gen g;
    memset(&g, 0, sizeof g);
    g.lib = lib;
    g.prefix = prefix;
    g.header = header;
    g.all = all;
    g.check = check && open_lib(lib, &g.libh);
    if (check && !g.check) fprintf(stderr, "mlwsvm bindgen: cannot open %s here, so its symbols are not checked\n", lib);
    sb_printf(&g.out, "; Generated by `mlwsvm bindgen` from %s. Review before use: C declarations\n"
                      "; do not say who owns a pointer, whether an int * is an array or an out\n"
                      "; parameter, or which arguments a variadic function takes.\n",
              header);
    bool ok = generate(&g, root);
    if (!ok) fprintf(stderr, "mlwsvm bindgen: no declarations in the syntax tree\n");
    else {
        FILE *f = out ? fopen(out, "w") : stdout;
        if (!f) {
            fprintf(stderr, "mlwsvm bindgen: cannot write %s\n", out);
            ok = false;
        } else {
            fwrite(g.out.s, 1, g.out.n, f);
            if (out) fclose(f);
            if (g.skipped) fprintf(stderr, "mlwsvm bindgen: %d declaration(s) skipped (see the comments)\n", g.skipped);
            if (g.missing) fprintf(stderr, "mlwsvm bindgen: %d function(s) not in %s left out\n", g.missing, *lib ? lib : "the program");
        }
    }
    for (size_t k = 0; k < g.records.cap; k++)
        if (g.records.keys && g.records.keys[k]) {
            Record *rec = g.records.vals[k];
            free(rec->name);
            free(rec->why);
            free(rec);
        }
    smap_free(&g.records);
    for (size_t k = 0; k < g.callbacks.cap; k++)
        if (g.callbacks.keys && g.callbacks.keys[k]) free(g.callbacks.vals[k]);
    smap_free(&g.callbacks);
    smap_free(&g.typedefs);
    smap_free(&g.last_decl);
    smap_free(&g.written);
    for (size_t k = 0; k < g.aliases.n; k++) free(g.aliases.v[k]);
    VEC_FREE(g.aliases);
    sb_free(&g.out);
    jfree(root);
    free(text);
    return ok ? 0 : 1;
}
