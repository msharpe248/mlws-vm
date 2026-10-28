/* text.c - std.text and std.json: formatting, string utilities, UTF-8, and
 * JSON. See docs/STDLIB.md. These are ordinary built-in natives: they run on
 * the calling worker thread and raise errors directly.
 *
 * Strings are UTF-8 by convention. Offsets (find, slice) are in bytes;
 * character-based work goes through text.next, text.chars and text.length,
 * which treat each invalid byte as one U+FFFD. Case mapping, normalization,
 * segmentation and character properties are Unicode's (src/core/unicode.c). */
#include "../heap/heap.h"
#include "../heap/numeric.h"
#include "../core/unicode.h"

#include <math.h>

/* ---- helpers ------------------------------------------------------------------------ */

static const char *str_arg(Process *p, Word a, uint32_t *len) {
    Obj *o = a.r;
    const char *fn = p->cur_native ? p->cur_native->name : "text";
    if (!o) vm_trap(p, "E_NULL", "%s: null where a string was expected", fn);
    if (o->kind == OBJ_STRING) {
        *len = ((ObjString *)o)->len;
        return ((ObjString *)o)->data;
    }
    if (o->kind == OBJ_BYTES) {
        *len = ((ObjBytes *)o)->len;
        return (const char *)((ObjBytes *)o)->data;
    }
    vm_trap(p, "E_TYPE", "%s: expected a string or byte buffer, got %s", fn, obj_kind_name(o->kind));
}

static ObjList *list_arg(Process *p, Word a) {
    Obj *o = a.r;
    if (o && o->kind == OBJ_LIST) return (ObjList *)o;
    vm_trap(p, "E_TYPE", "%s: expected a list", p->cur_native ? p->cur_native->name : "text");
}

static Word wstr(Process *p, const char *s, size_t n) { return (Word){.r = str_new(p, s, n)}; }

static Word sb_string(Process *p, StrBuf *b) {
    Word w = wstr(p, b->s ? b->s : "", b->n);
    sb_free(b);
    return w;
}

/* Decode one UTF-8 character at s[i]; invalid input gives U+FFFD for one byte. */
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *i) { return uni_utf8_next(s, n, i); }

static bool utf8_valid(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        size_t at = i;
        if (utf8_next(s, n, &i) == 0xFFFD && !(i - at == 3 && s[at] == 0xEF && s[at + 1] == 0xBF && s[at + 2] == 0xBD))
            return false;
    }
    return true;
}

static void utf8_put(StrBuf *b, uint32_t cp) { uni_utf8_put(b, cp); }

static size_t utf8_count(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; c++) utf8_next((const unsigned char *)s, n, &i);
    return c;
}

/* The length of the White_Space character at s[i], or 0 if it is not one. */
static size_t space_at(const char *s, size_t n, size_t i) {
    if ((unsigned char)s[i] < 0x80) return s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r');
    size_t j = i;
    uint32_t cp = utf8_next((const unsigned char *)s, n, &j);
    return cp != 0xFFFD && uni_has(cp, UNI_P_WHITE) ? j - i : 0;
}

/* The length of the White_Space character that ends s[0, n), or 0. */
static size_t space_before(const char *s, size_t n) {
    size_t st = n;
    while (st > 0 && n - st < 4 && ((unsigned char)s[st - 1] & 0xC0) == 0x80) st--; /* the start of the last character */
    if (st > 0) st--;
    return space_at(s, n, st) == n - st ? n - st : 0;
}

/* The default text of a typed value (as tostr gives it, but strings unquoted). */
static void put_value(Process *p, StrBuf *b, Word v, uint8_t tag) {
    if (tag == T_R && v.r && ((Obj *)v.r)->kind == OBJ_STRING) {
        sb_putn(b, ((ObjString *)v.r)->data, ((ObjString *)v.r)->len);
        return;
    }
    if (tag == T_R && !v.r) {
        sb_puts(b, "null");
        return;
    }
    ObjString *s = value_to_string(p, v, (MlwsType)tag);
    sb_putn(b, s->data, s->len);
}

/* ---- text.format ---------------------------------------------------------------------- */

static void put_quoted(StrBuf *b, const char *s, size_t n) {
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\r': sb_puts(b, "\\r"); break;
        default:
            if (c < 0x20 || c == 0x7F) sb_printf(b, "\\x%02x", c);
            else sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

typedef struct {
    char fill, align, type;
    bool plus, zero;
    int width, prec;
} Spec;

static void pad_out(StrBuf *out, const Spec *sp, const char *s, size_t n, bool numeric) {
    size_t chars = utf8_count(s, n);
    size_t padn = sp->width > 0 && (size_t)sp->width > chars ? (size_t)sp->width - chars : 0;
    char align = sp->align ? sp->align : numeric ? '>' : '<';
    if (sp->zero && numeric && !sp->align) { /* zero padding goes after the sign */
        size_t sign = n && (s[0] == '-' || s[0] == '+') ? 1 : 0;
        sb_putn(out, s, sign);
        for (size_t i = 0; i < padn; i++) sb_putc(out, '0');
        sb_putn(out, s + sign, n - sign);
        return;
    }
    size_t left = align == '>' ? padn : align == '^' ? padn / 2 : 0, right = padn - left;
    for (size_t i = 0; i < left; i++) sb_putc(out, sp->fill);
    sb_putn(out, s, n);
    for (size_t i = 0; i < right; i++) sb_putc(out, sp->fill);
}

static bool tag_is_int(uint8_t t) { return t >= T_B && t <= T_U64; }

static void format_one(Process *p, StrBuf *out, const Spec *sp, Word v, uint8_t tag) {
    StrBuf t = {0};
#define FAIL(...) (sb_free(&t), sb_free(out), vm_trap(p, __VA_ARGS__)) /* nothing may leak past a trap */
    bool numeric = tag_is_int(tag) && tag != T_B && tag != T_C;
    char ty = sp->type;
    if (tag == T_F32 || tag == T_F64) numeric = true;
    if (ty == 'x' || ty == 'X' || ty == 'o' || ty == 'b' || ty == 'd') {
        if (!tag_is_int(tag)) FAIL("E_TYPE", "text.format: {:%c} needs an integer", ty);
        bool sgn = tag >= T_I8 && tag <= T_I64;
        uint64_t u = sgn && v.i < 0 ? (uint64_t)0 - v.u : v.u;
        if (sgn && v.i < 0) sb_putc(&t, '-');
        else if (sp->plus) sb_putc(&t, '+');
        if (ty == 'd') sb_printf(&t, "%llu", (unsigned long long)u);
        else if (ty == 'x') sb_printf(&t, "%llx", (unsigned long long)u);
        else if (ty == 'X') sb_printf(&t, "%llX", (unsigned long long)u);
        else if (ty == 'o') sb_printf(&t, "%llo", (unsigned long long)u);
        else {
            char bits[65];
            int k = 64;
            bits[k] = 0;
            do bits[--k] = (char)('0' + (u & 1)), u >>= 1;
            while (u);
            sb_puts(&t, bits + k);
        }
        numeric = true;
    } else if (ty == 'f' || ty == 'e' || ty == 'g') {
        double d;
        if (tag == T_F32 || tag == T_F64) d = v.f;
        else if (tag >= T_I8 && tag <= T_I64) d = (double)v.i;
        else if (tag_is_int(tag)) d = (double)v.u;
        else if (tag == T_R && v.r && (((Obj *)v.r)->kind == OBJ_BIG || ((Obj *)v.r)->kind == OBJ_DEC)) d = num_to_f64(v.r);
        else FAIL("E_TYPE", "text.format: {:%c} needs a number", ty);
        char fmt[16];
        snprintf(fmt, sizeof fmt, "%%%s.%d%c", sp->plus ? "+" : "", sp->prec >= 0 ? sp->prec : 6, ty);
        sb_printf(&t, fmt, d);
        numeric = true;
    } else if (ty == 'q') {
        Obj *o = tag == T_R ? v.r : NULL;
        if (!o || (o->kind != OBJ_STRING && o->kind != OBJ_BYTES)) FAIL("E_TYPE", "text.format: {:q} needs a string");
        if (o->kind == OBJ_STRING) put_quoted(&t, ((ObjString *)o)->data, ((ObjString *)o)->len);
        else put_quoted(&t, (const char *)((ObjBytes *)o)->data, ((ObjBytes *)o)->len);
        numeric = false;
    } else if (ty == 0 || ty == 's') {
        if (sp->prec >= 0 && (tag == T_F32 || tag == T_F64)) sb_printf(&t, "%.*f", sp->prec, v.f);
        else put_value(p, &t, v, tag);
        if (sp->plus && numeric && t.n && t.s[0] != '-') {
            StrBuf u = {0};
            sb_putc(&u, '+');
            sb_putn(&u, t.s, t.n);
            sb_free(&t);
            t = u;
        }
        if (ty == 's' || !numeric) {
            if (sp->prec >= 0 && !(tag == T_F32 || tag == T_F64)) { /* precision truncates text, in characters */
                size_t i = 0, c = 0;
                while (i < t.n && c < (size_t)sp->prec) utf8_next((const unsigned char *)t.s, t.n, &i), c++;
                t.n = i;
            }
        }
    } else {
        FAIL("E_TYPE", "text.format: unknown format type '%c'", ty);
    }
    pad_out(out, sp, t.s ? t.s : "", t.n, numeric);
    sb_free(&t);
#undef FAIL
}

/* text.format(fmt, args): {} takes the next argument, {2} a given one;
 * :[[fill]align][+][0][width][.precision][type] as in Python and Rust;
 * {{ and }} are literal braces. */
static void n_format(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *f = str_arg(p, a[0], &n);
    ObjList *args = a[1].r ? list_arg(p, a[1]) : NULL;
    uint32_t nargs = args ? args->len : 0, next = 0;
    StrBuf out = {0};
#define FAIL(...) (sb_free(&out), vm_trap(p, __VA_ARGS__))
    for (uint32_t i = 0; i < n; i++) {
        char c = f[i];
        if (c == '}') {
            if (i + 1 < n && f[i + 1] == '}') i++;
            else FAIL("E_TYPE", "text.format: unmatched '}' at %u", i);
            sb_putc(&out, '}');
            continue;
        }
        if (c != '{') {
            sb_putc(&out, c);
            continue;
        }
        if (i + 1 < n && f[i + 1] == '{') {
            sb_putc(&out, '{');
            i++;
            continue;
        }
        uint32_t j = i + 1, argi = next;
        bool indexed = false;
        if (j < n && f[j] >= '0' && f[j] <= '9') {
            argi = 0;
            while (j < n && f[j] >= '0' && f[j] <= '9') argi = argi * 10 + (uint32_t)(f[j++] - '0');
            indexed = true;
        }
        Spec sp = {' ', 0, 0, false, false, -1, -1};
        if (j < n && f[j] == ':') {
            j++;
            if (j + 1 < n && (f[j + 1] == '<' || f[j + 1] == '>' || f[j + 1] == '^')) sp.fill = f[j], sp.align = f[j + 1], j += 2;
            else if (j < n && (f[j] == '<' || f[j] == '>' || f[j] == '^')) sp.align = f[j++];
            if (j < n && f[j] == '+') sp.plus = true, j++;
            if (j < n && f[j] == '0') sp.zero = true, j++;
            if (j < n && f[j] >= '0' && f[j] <= '9') {
                sp.width = 0;
                while (j < n && f[j] >= '0' && f[j] <= '9' && sp.width < 10000) sp.width = sp.width * 10 + (f[j++] - '0');
            }
            if (j < n && f[j] == '.') {
                j++;
                sp.prec = 0;
                while (j < n && f[j] >= '0' && f[j] <= '9' && sp.prec < 1000) sp.prec = sp.prec * 10 + (f[j++] - '0');
            }
            if (j < n && f[j] != '}') sp.type = f[j++];
        }
        if (j >= n || f[j] != '}') FAIL("E_TYPE", "text.format: bad placeholder at %u", i);
        if (argi >= nargs)
            FAIL("E_BOUNDS", "text.format: placeholder %u needs argument %u, but only %u were given", i, argi,
                    nargs);
        format_one(p, &out, &sp, args->items[argi], args->tags[argi]);
        if (!indexed) next++;
        i = j;
    }
    r[0] = sb_string(p, &out);
#undef FAIL
}

/* ---- strings ------------------------------------------------------------------------------ */

static void n_split(Process *p, Word *a, Word *r) {
    uint32_t n, sn;
    const char *s = str_arg(p, a[0], &n), *sep = str_arg(p, a[1], &sn);
    ObjList *l = list_new(p, 0);
    r[0].r = l;
    if (!sn) { /* on runs of whitespace, ignoring it at the ends */
        for (uint32_t i = 0; i < n;) {
            size_t k;
            while (i < n && (k = space_at(s, n, i))) i += (uint32_t)k;
            uint32_t st = i;
            while (i < n && !space_at(s, n, i)) i++;
            if (i > st) list_push(p, l, wstr(p, s + st, i - st), T_R);
        }
        return;
    }
    uint32_t st = 0;
    for (uint32_t i = 0; i + sn <= n;) {
        if (!memcmp(s + i, sep, sn)) {
            list_push(p, l, wstr(p, s + st, i - st), T_R);
            i += sn;
            st = i;
        } else {
            i++;
        }
    }
    list_push(p, l, wstr(p, s + st, n - st), T_R);
}

static void n_join(Process *p, Word *a, Word *r) {
    ObjList *l = list_arg(p, a[0]);
    uint32_t sn;
    const char *sep = str_arg(p, a[1], &sn);
    StrBuf b = {0};
    for (uint32_t i = 0; i < l->len; i++) {
        if (i) sb_putn(&b, sep, sn);
        put_value(p, &b, l->items[i], l->tags[i]);
    }
    r[0] = sb_string(p, &b);
}

#define TRIM(NAME, FRONT, BACK)                                                \
    static void NAME(Process *p, Word *a, Word *r) {                           \
        uint32_t n, st = 0;                                                    \
        size_t k;                                                              \
        const char *s = str_arg(p, a[0], &n);                                  \
        if (FRONT)                                                             \
            while (st < n && (k = space_at(s, n, st))) st += (uint32_t)k;      \
        if (BACK)                                                              \
            while (n > st && (k = space_before(s + st, n - st))) n -= (uint32_t)k; \
        r[0] = wstr(p, s + st, n - st);                                        \
    }
TRIM(n_trim, 1, 1)
TRIM(n_trim_start, 1, 0)
TRIM(n_trim_end, 0, 1)

static void n_find(Process *p, Word *a, Word *r) {
    uint32_t n, sn;
    const char *s = str_arg(p, a[0], &n), *sub = str_arg(p, a[1], &sn);
    int64_t from = a[2].i < 0 ? 0 : a[2].i;
    r[0].i = -1;
    for (int64_t i = from; i + (int64_t)sn <= (int64_t)n; i++)
        if (!memcmp(s + i, sub, sn)) {
            r[0].i = i;
            return;
        }
}

static void n_replace(Process *p, Word *a, Word *r) {
    uint32_t n, on, nn;
    const char *s = str_arg(p, a[0], &n), *old = str_arg(p, a[1], &on), *new_ = str_arg(p, a[2], &nn);
    if (!on) vm_trap(p, "E_TYPE", "text.replace: the text to replace is empty");
    StrBuf b = {0};
    for (uint32_t i = 0; i < n;) {
        if (i + on <= n && !memcmp(s + i, old, on)) {
            sb_putn(&b, new_, nn);
            i += on;
        } else {
            sb_putc(&b, s[i++]);
        }
    }
    r[0] = sb_string(p, &b);
}

static void n_starts_with(Process *p, Word *a, Word *r) {
    uint32_t n, pn;
    const char *s = str_arg(p, a[0], &n), *pre = str_arg(p, a[1], &pn);
    r[0].i = pn <= n && !memcmp(s, pre, pn);
}

static void n_ends_with(Process *p, Word *a, Word *r) {
    uint32_t n, pn;
    const char *s = str_arg(p, a[0], &n), *suf = str_arg(p, a[1], &pn);
    r[0].i = pn <= n && !memcmp(s + n - pn, suf, pn);
}

static void n_slice(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    int64_t st = a[1].i, en = a[2].i;
    if (st < 0) st += n; /* negative: from the end */
    if (en < 0) en += n;
    if (st < 0) st = 0;
    if (en > (int64_t)n) en = n;
    r[0] = wstr(p, s + st, en > st ? (size_t)(en - st) : 0);
}

/* ---- Unicode (src/core/unicode.c) ----------------------------------------------------- */

#define CASE(NAME, KIND)                                                       \
    static void NAME(Process *p, Word *a, Word *r) {                           \
        uint32_t n;                                                            \
        const char *s = str_arg(p, a[0], &n);                                  \
        StrBuf b = {0};                                                        \
        uni_case(s, n, KIND, &b);                                              \
        r[0] = sb_string(p, &b);                                               \
    }
/* ASCII letters only, byte for byte (protocols, keywords, C's toupper in the "C" locale). */
#define ASCII_CASE(NAME, LO, HI, DELTA)                                        \
    static void NAME(Process *p, Word *a, Word *r) {                           \
        uint32_t n;                                                            \
        const char *s = str_arg(p, a[0], &n);                                  \
        ObjString *o = str_new(p, s, n);                                       \
        for (uint32_t i = 0; i < n; i++)                                       \
            if (o->data[i] >= LO && o->data[i] <= HI) o->data[i] = (char)(o->data[i] + (DELTA)); \
        o->hash = hash_bytes(o->data, o->len);                                 \
        r[0].r = o;                                                            \
    }
ASCII_CASE(n_ascii_upper, 'a', 'z', 'A' - 'a')
ASCII_CASE(n_ascii_lower, 'A', 'Z', 'a' - 'A')

CASE(n_upper, UNI_UPPER)
CASE(n_lower, UNI_LOWER)
CASE(n_title, UNI_TITLE)
CASE(n_fold, UNI_FOLD)

static int form_arg(Process *p, Word a) {
    uint32_t n;
    const char *s = str_arg(p, a, &n);
    static const char *const NAMES[] = {"NFC", "NFD", "NFKC", "NFKD"};
    for (int k = 0; k < 4; k++)
        if (n == strlen(NAMES[k]) && !memcmp(s, NAMES[k], n)) return k;
    vm_trap(p, "E_RANGE", "%s: the form is \"NFC\", \"NFD\", \"NFKC\" or \"NFKD\", not \"%.*s\"",
            p->cur_native ? p->cur_native->name : "text.normalize", (int)(n < 40 ? n : 40), s);
}

static void n_normalize(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    int form = form_arg(p, a[1]);
    StrBuf b = {0};
    uni_normalize(s, n, form, &b);
    r[0] = sb_string(p, &b);
}

static void n_is_normalized(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    r[0].i = uni_is_normalized(s, n, form_arg(p, a[1]));
}

static void n_equal_fold(Process *p, Word *a, Word *r) {
    uint32_t an, bn;
    const char *x = str_arg(p, a[0], &an), *y = str_arg(p, a[1], &bn);
    r[0].i = uni_equal_fold(x, an, y, bn);
}

static void n_graphemes(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    ObjList *l = list_new(p, 0);
    r[0].r = l;
    for (size_t off = 0; off < n;) {
        size_t end = uni_next_grapheme(s, n, off);
        list_push(p, l, wstr(p, s + off, end - off), T_R);
        off = end;
    }
}

static void n_next_grapheme(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    int64_t off = a[1].i;
    if (off < 0 || off > (int64_t)n) vm_trap(p, "E_BOUNDS", "text.next_grapheme: offset %lld is outside the string", (long long)off);
    r[0].i = off == (int64_t)n ? -1 : (int64_t)uni_next_grapheme(s, n, (size_t)off);
}

/* The word-boundary segments of a string: all of them, or only the words. */
static void segments(Process *p, Word *a, Word *r, bool words_only) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    size_t nb;
    size_t *bounds = uni_word_bounds(s, n, &nb);
    ObjList *l = list_new(p, 0);
    r[0].r = l;
    for (size_t k = 0; k + 1 < nb; k++) {
        if (words_only && !uni_is_word(s, bounds[k], bounds[k + 1])) continue;
        list_push(p, l, wstr(p, s + bounds[k], bounds[k + 1] - bounds[k]), T_R);
    }
    free(bounds);
}

static void n_words(Process *p, Word *a, Word *r) { segments(p, a, r, true); }
static void n_word_segments(Process *p, Word *a, Word *r) { segments(p, a, r, false); }

static void n_width(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    r[0].i = uni_width(s, n);
}

static uint32_t char_arg(Word a) { return a.i < 0 || a.i > 0x10FFFF ? 0x10FFFF : (uint32_t)a.i; }

#define CHAR_PRED(NAME, EXPR)                                                  \
    static void NAME(Process *p, Word *a, Word *r) {                           \
        (void)p;                                                               \
        uint32_t c = char_arg(a[0]);                                           \
        r[0].i = (EXPR);                                                       \
    }
CHAR_PRED(n_is_letter, uni_is_letter(c))
CHAR_PRED(n_is_alpha, uni_has(c, UNI_P_ALPHA))
CHAR_PRED(n_is_digit, uni_digit(c) >= 0)
CHAR_PRED(n_is_number, uni_is_number(c))
CHAR_PRED(n_is_space, uni_has(c, UNI_P_WHITE))
CHAR_PRED(n_is_upper, uni_has(c, UNI_P_UPPER))
CHAR_PRED(n_is_lower, uni_has(c, UNI_P_LOWER))
CHAR_PRED(n_is_punct, uni_is_punct(c))
CHAR_PRED(n_is_id_start, uni_has(c, UNI_P_XID_START))
CHAR_PRED(n_is_id_continue, uni_has(c, UNI_P_XID_CONTINUE))
CHAR_PRED(n_is_emoji, uni_has(c, UNI_P_EXTPICT))
CHAR_PRED(n_digit_value, uni_digit(c))
CHAR_PRED(n_char_width, uni_char_width(c))

static void n_category(Process *p, Word *a, Word *r) {
    const char *gc = uni_category(char_arg(a[0]));
    r[0] = wstr(p, gc, strlen(gc));
}

static void n_unicode_version(Process *p, Word *a, Word *r) {
    (void)a;
    r[0] = wstr(p, uni_version(), strlen(uni_version()));
}

static void n_repeat(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    int64_t k = a[1].i;
    if (k < 0 || (n && k > (int64_t)(((uint64_t)1 << 31) / n)))
        vm_trap(p, "E_BAD_ALLOC", "text.repeat: %lld copies is too many", (long long)k);
    StrBuf b = {0};
    for (int64_t i = 0; i < k; i++) sb_putn(&b, s, n);
    r[0] = sb_string(p, &b);
}

static void n_lines(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    ObjList *l = list_new(p, 0);
    r[0].r = l;
    uint32_t st = 0;
    for (uint32_t i = 0; i < n; i++)
        if (s[i] == '\n') {
            uint32_t e = i > st && s[i - 1] == '\r' ? i - 1 : i;
            list_push(p, l, wstr(p, s + st, e - st), T_R);
            st = i + 1;
        }
    if (st < n) list_push(p, l, wstr(p, s + st, n - st), T_R);
}

/* ---- UTF-8 ------------------------------------------------------------------------------- */

static void n_valid(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    r[0].i = utf8_valid((const unsigned char *)s, n);
}

static void n_length(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    r[0].i = (int64_t)utf8_count(s, n);
}

static void n_chars(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    ObjList *l = list_new(p, 0);
    for (size_t i = 0; i < n;) list_push(p, l, (Word){.u = utf8_next((const unsigned char *)s, n, &i)}, T_C);
    r[0].r = l;
}

/* text.next(s, i) -> (char, next index): iterate characters; next is -1 at the end. */
static void n_next(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = str_arg(p, a[0], &n);
    if (a[1].i < 0 || a[1].i >= (int64_t)n) {
        r[0].u = 0;
        r[1].i = -1;
        return;
    }
    size_t i = (size_t)a[1].i;
    r[0].u = utf8_next((const unsigned char *)s, n, &i);
    r[1].i = (int64_t)i;
}

static void n_from_chars(Process *p, Word *a, Word *r) {
    ObjList *l = list_arg(p, a[0]);
    StrBuf b = {0};
    for (uint32_t i = 0; i < l->len; i++) {
        uint8_t t = l->tags[i];
        uint64_t cp = l->items[i].u;
        if (!tag_is_int(t) || (t >= T_I8 && t <= T_I64 && l->items[i].i < 0) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            sb_free(&b);
            vm_trap(p, "E_TYPE", "text.from_chars: element %u is not a Unicode character", i);
        }
        utf8_put(&b, (uint32_t)cp);
    }
    r[0] = sb_string(p, &b);
}

/* ---- JSON: parsing ---------------------------------------------------------------------------- */

#define JSON_DEPTH 256

typedef struct {
    Process *p;
    const char *s;
    size_t n, i;
} JP;

MLWS_NORETURN static void jerr(JP *j, const char *what) {
    size_t line = 1, col = 1;
    for (size_t k = 0; k < j->i && k < j->n; k++) {
        if (j->s[k] == '\n') line++, col = 1;
        else col++;
    }
    vm_trap(j->p, "E_JSON", "json.parse: line %zu column %zu: %s", line, col, what);
}

static void jws(JP *j) {
    while (j->i < j->n && (j->s[j->i] == ' ' || j->s[j->i] == '\t' || j->s[j->i] == '\n' || j->s[j->i] == '\r')) j->i++;
}

static bool jlit(JP *j, const char *lit) {
    size_t k = strlen(lit);
    if (j->i + k > j->n || memcmp(j->s + j->i, lit, k)) return false;
    j->i += k;
    return true;
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static uint32_t jhex4(JP *j) {
    if (j->i + 4 > j->n) jerr(j, "truncated \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexv(j->s[j->i++]);
        if (h < 0) jerr(j, "bad \\u escape");
        v = v << 4 | (uint32_t)h;
    }
    return v;
}

static ObjString *jstring(JP *j) {
    j->i++; /* the opening quote */
    StrBuf b = {0};
    for (;;) {
        if (j->i >= j->n) {
            sb_free(&b);
            jerr(j, "unterminated string");
        }
        unsigned char c = (unsigned char)j->s[j->i++];
        if (c == '"') break;
        if (c < 0x20) {
            sb_free(&b);
            j->i--;
            jerr(j, "control character in a string");
        }
        if (c != '\\') {
            sb_putc(&b, (char)c);
            continue;
        }
        if (j->i >= j->n) break;
        char e = j->s[j->i++];
        switch (e) {
        case '"': sb_putc(&b, '"'); break;
        case '\\': sb_putc(&b, '\\'); break;
        case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'u': {
            uint32_t cp = jhex4(j);
            if (cp >= 0xD800 && cp <= 0xDBFF && j->i + 6 <= j->n && j->s[j->i] == '\\' && j->s[j->i + 1] == 'u') {
                size_t save = j->i;
                j->i += 2;
                uint32_t lo = jhex4(j);
                if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else j->i = save;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* a lone surrogate */
            utf8_put(&b, cp);
            break;
        }
        default:
            sb_free(&b);
            j->i--;
            jerr(j, "bad escape");
        }
    }
    ObjString *s = str_new(j->p, b.s ? b.s : "", b.n);
    sb_free(&b);
    return s;
}

static void jvalue(JP *j, int depth, Word *v, uint8_t *tag);

static void jnumber(JP *j, Word *v, uint8_t *tag) {
    size_t st = j->i;
    bool integral = true;
    if (j->s[j->i] == '-') j->i++;
    if (j->i >= j->n || !(j->s[j->i] >= '0' && j->s[j->i] <= '9')) jerr(j, "bad number");
    if (j->s[j->i] == '0') j->i++;
    else
        while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++;
    if (j->i < j->n && j->s[j->i] == '.') {
        integral = false;
        j->i++;
        if (j->i >= j->n || !(j->s[j->i] >= '0' && j->s[j->i] <= '9')) jerr(j, "bad number");
        while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++;
    }
    if (j->i < j->n && (j->s[j->i] == 'e' || j->s[j->i] == 'E')) {
        integral = false;
        j->i++;
        if (j->i < j->n && (j->s[j->i] == '+' || j->s[j->i] == '-')) j->i++;
        if (j->i >= j->n || !(j->s[j->i] >= '0' && j->s[j->i] <= '9')) jerr(j, "bad number");
        while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++;
    }
    char buf[64];
    size_t len = j->i - st;
    if (integral && len < 19) { /* fits in an i64 for certain */
        memcpy(buf, j->s + st, len);
        buf[len] = 0;
        v->i = strtoll(buf, NULL, 10);
        *tag = T_I64;
        return;
    }
    if (integral) { /* exact: an i64 if it fits, else a BigInt */
        Obj *big = num_parse(j->p, T_BIG, j->s + st, len);
        Word w;
        if (big && num_to_int(big, T_I64, &w)) {
            *v = w;
            *tag = T_I64;
        } else {
            v->r = big;
            *tag = T_R;
        }
        return;
    }
    char *tmp = len < sizeof buf ? buf : xmalloc(len + 1);
    memcpy(tmp, j->s + st, len);
    tmp[len] = 0;
    v->f = strtod(tmp, NULL);
    if (tmp != buf) free(tmp);
    *tag = T_F64;
}

static void jvalue(JP *j, int depth, Word *v, uint8_t *tag) {
    if (depth > JSON_DEPTH) jerr(j, "nested too deeply");
    jws(j);
    if (j->i >= j->n) jerr(j, "unexpected end of input");
    char c = j->s[j->i];
    if (c == '{') {
        j->i++;
        ObjMap *m = map_new(j->p);
        v->r = m;
        *tag = T_R;
        jws(j);
        if (j->i < j->n && j->s[j->i] == '}') {
            j->i++;
            return;
        }
        for (;;) {
            jws(j);
            if (j->i >= j->n || j->s[j->i] != '"') jerr(j, "expected a string key");
            Word k = {.r = jstring(j)}, val;
            uint8_t vt;
            jws(j);
            if (j->i >= j->n || j->s[j->i] != ':') jerr(j, "expected ':'");
            j->i++;
            jvalue(j, depth + 1, &val, &vt);
            map_set(j->p, m, k, T_R, val, vt); /* a repeated key: the last wins */
            jws(j);
            if (j->i < j->n && j->s[j->i] == ',') {
                j->i++;
                continue;
            }
            if (j->i < j->n && j->s[j->i] == '}') {
                j->i++;
                return;
            }
            jerr(j, "expected ',' or '}'");
        }
    }
    if (c == '[') {
        j->i++;
        ObjList *l = list_new(j->p, 0);
        v->r = l;
        *tag = T_R;
        jws(j);
        if (j->i < j->n && j->s[j->i] == ']') {
            j->i++;
            return;
        }
        for (;;) {
            Word e;
            uint8_t et;
            jvalue(j, depth + 1, &e, &et);
            list_push(j->p, l, e, et);
            jws(j);
            if (j->i < j->n && j->s[j->i] == ',') {
                j->i++;
                continue;
            }
            if (j->i < j->n && j->s[j->i] == ']') {
                j->i++;
                return;
            }
            jerr(j, "expected ',' or ']'");
        }
    }
    if (c == '"') {
        v->r = jstring(j);
        *tag = T_R;
        return;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        jnumber(j, v, tag);
        return;
    }
    if (jlit(j, "true") || jlit(j, "false")) {
        v->u = j->s[j->i - 1] == 'e' && j->s[j->i - 2] == 'u'; /* true */
        *tag = T_B;
        return;
    }
    if (jlit(j, "null")) {
        v->r = NULL;
        *tag = T_R;
        return;
    }
    jerr(j, "unexpected character");
}

static void jparse(Process *p, Word a, Word *v, uint8_t *tag) {
    uint32_t n;
    const char *s = str_arg(p, a, &n);
    JP j = {p, s, n, 0};
    jvalue(&j, 0, v, tag);
    jws(&j);
    if (j.i != j.n) jerr(&j, "unexpected text after the value");
}

/* json.parse(text): an object (a map) or array (a list) */
static void n_json_parse(Process *p, Word *a, Word *r) {
    Word v;
    uint8_t tag;
    jparse(p, a[0], &v, &tag);
    Obj *o = tag == T_R ? v.r : NULL;
    if (!o || (o->kind != OBJ_MAP && o->kind != OBJ_LIST))
        vm_trap(p, "E_JSON", "json.parse: the text is not an object or array (use json.parse_any)");
    r[0] = v;
}

/* json.parse_any(text): any JSON value, as the only element of a list (so
 * numbers and booleans keep their types) */
static void n_json_parse_any(Process *p, Word *a, Word *r) {
    Word v;
    uint8_t tag;
    jparse(p, a[0], &v, &tag);
    ObjList *l = list_new(p, 1);
    list_push(p, l, v, tag);
    r[0].r = l;
}

/* ---- JSON: writing ----------------------------------------------------------------------------- */

typedef struct {
    Process *p;
    StrBuf b;
    int64_t indent;
} JW;

MLWS_NORETURN static void jwerr(JW *w, const char *fmt, const char *arg) {
    sb_free(&w->b);
    vm_trap(w->p, "E_JSON", fmt, arg);
}

static void jw_string(JW *w, const char *s, size_t n) {
    if (!utf8_valid((const unsigned char *)s, n)) jwerr(w, "json.stringify: %s is not valid UTF-8", "a string");
    sb_putc(&w->b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') sb_puts(&w->b, "\\\"");
        else if (c == '\\') sb_puts(&w->b, "\\\\");
        else if (c == '\n') sb_puts(&w->b, "\\n");
        else if (c == '\r') sb_puts(&w->b, "\\r");
        else if (c == '\t') sb_puts(&w->b, "\\t");
        else if (c < 0x20) sb_printf(&w->b, "\\u%04x", c);
        else sb_putc(&w->b, (char)c);
    }
    sb_putc(&w->b, '"');
}

static void jw_newline(JW *w, int depth) {
    if (w->indent <= 0) return;
    sb_putc(&w->b, '\n');
    for (int64_t i = 0; i < w->indent * depth; i++) sb_putc(&w->b, ' ');
}

static void jw_value(JW *w, Word v, uint8_t tag, int depth);

static void jw_key(JW *w, Word k, uint8_t kt) {
    if (kt == T_R && k.r && ((Obj *)k.r)->kind == OBJ_STRING) {
        jw_string(w, ((ObjString *)k.r)->data, ((ObjString *)k.r)->len);
    } else if (tag_is_int(kt) && kt != T_B && kt != T_C) { /* integer keys become strings */
        sb_putc(&w->b, '"');
        put_value(w->p, &w->b, k, kt);
        sb_putc(&w->b, '"');
    } else {
        jwerr(w, "json.stringify: %s", "map keys must be strings or integers");
    }
}

static void jw_value(JW *w, Word v, uint8_t tag, int depth) {
    if (depth > JSON_DEPTH) jwerr(w, "json.stringify: %s", "nested too deeply (is there a cycle?)");
    if (tag == T_B) {
        sb_puts(&w->b, v.u ? "true" : "false");
        return;
    }
    if (tag == T_C) {
        StrBuf c = {0};
        utf8_put(&c, (uint32_t)v.u);
        jw_string(w, c.s, c.n);
        sb_free(&c);
        return;
    }
    if (tag_is_int(tag)) {
        put_value(w->p, &w->b, v, tag);
        return;
    }
    if (tag == T_F32 || tag == T_F64) {
        if (isnan(v.f) || isinf(v.f)) jwerr(w, "json.stringify: %s", "NaN and infinity have no JSON form");
        char buf[64];
        fmt_double(v.f, buf, sizeof buf);
        sb_puts(&w->b, buf);
        return;
    }
    if (tag != T_R) jwerr(w, "json.stringify: %s", "pointers have no JSON form");
    Obj *o = v.r;
    if (!o) {
        sb_puts(&w->b, "null");
        return;
    }
    switch (o->kind) {
    case OBJ_STRING: jw_string(w, ((ObjString *)o)->data, ((ObjString *)o)->len); return;
    case OBJ_BYTES: jw_string(w, (const char *)((ObjBytes *)o)->data, ((ObjBytes *)o)->len); return;
    case OBJ_BIG:
    case OBJ_DEC: num_format(&w->b, o); return;
    case OBJ_LIST:
    case OBJ_ARRAY: {
        bool isl = o->kind == OBJ_LIST;
        uint32_t n = isl ? ((ObjList *)o)->len : ((ObjArray *)o)->len;
        sb_putc(&w->b, '[');
        for (uint32_t i = 0; i < n; i++) {
            if (i) sb_putc(&w->b, ',');
            jw_newline(w, depth + 1);
            if (isl) jw_value(w, ((ObjList *)o)->items[i], ((ObjList *)o)->tags[i], depth + 1);
            else jw_value(w, ((ObjArray *)o)->data[i], ((ObjArray *)o)->elem, depth + 1);
        }
        if (n) jw_newline(w, depth);
        sb_putc(&w->b, ']');
        return;
    }
    case OBJ_MAP: {
        ObjMap *m = (ObjMap *)o;
        bool first = true;
        sb_putc(&w->b, '{');
        for (uint32_t i = 0; i < m->nentries; i++) {
            MapEntry *e = &m->entries[i];
            if (!e->live) continue;
            if (!first) sb_putc(&w->b, ',');
            first = false;
            jw_newline(w, depth + 1);
            jw_key(w, e->key, e->ktag);
            sb_puts(&w->b, w->indent > 0 ? ": " : ":");
            jw_value(w, e->val, e->vtag, depth + 1);
        }
        if (!first) jw_newline(w, depth);
        sb_putc(&w->b, '}');
        return;
    }
    case OBJ_STRUCT: { /* an object, with the struct's field names */
        ObjStruct *s = (ObjStruct *)o;
        sb_putc(&w->b, '{');
        for (uint32_t i = 0; i < s->def->nfields; i++) {
            if (i) sb_putc(&w->b, ',');
            jw_newline(w, depth + 1);
            jw_string(w, s->def->fields[i], strlen(s->def->fields[i]));
            sb_puts(&w->b, w->indent > 0 ? ": " : ":");
            jw_value(w, s->fields[i], s->def->types[i], depth + 1);
        }
        if (s->def->nfields) jw_newline(w, depth);
        sb_putc(&w->b, '}');
        return;
    }
    default: jwerr(w, "json.stringify: a %s has no JSON form", obj_kind_name(o->kind));
    }
}

/* json.stringify(value, indent): compact for indent 0, else pretty-printed */
static void n_json_stringify(Process *p, Word *a, Word *r) {
    JW w = {p, {0}, a[1].i > 16 ? 16 : a[1].i};
    jw_value(&w, a[0], T_R, 0);
    r[0] = sb_string(p, &w.b);
}

/* ---- the table ------------------------------------------------------------------------------- */

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}

static const NativeDef TEXT_NATIVES[] = {
    N("text.format", 2, T(T_R, T_R), 1, T(T_R), n_format,
      "Format with {} placeholders: text.format(\"{} is {:>5.1f}\", [name, x]). Spec: "
      "[[fill]align][+][0][width][.precision][type]; types d x X o b f e g s q (quoted)."),
    N("text.split", 2, T(T_R, T_R), 1, T(T_R), n_split,
      "Split on a separator, as a list of strings; an empty separator splits on runs of whitespace "
      "(White_Space)."),
    N("text.join", 2, T(T_R, T_R), 1, T(T_R), n_join, "Join a list's elements (any values) with a separator."),
    N("text.trim", 1, T(T_R), 1, T(T_R), n_trim, "Remove whitespace (White_Space) from both ends."),
    N("text.trim_start", 1, T(T_R), 1, T(T_R), n_trim_start, "Remove whitespace from the start."),
    N("text.trim_end", 1, T(T_R), 1, T(T_R), n_trim_end, "Remove whitespace from the end."),
    N("text.find", 3, T(T_R, T_R, T_I64), 1, T(T_I64), n_find,
      "The byte offset of the first occurrence at or after an offset, or -1."),
    N("text.replace", 3, T(T_R, T_R, T_R), 1, T(T_R), n_replace, "Replace every occurrence."),
    N("text.starts_with", 2, T(T_R, T_R), 1, T(T_B), n_starts_with, "Whether a string starts with a prefix."),
    N("text.ends_with", 2, T(T_R, T_R), 1, T(T_B), n_ends_with, "Whether a string ends with a suffix."),
    N("text.slice", 3, T(T_R, T_I64, T_I64), 1, T(T_R), n_slice,
      "The bytes [start, end) as a string; negative offsets count from the end; clamped to the string."),
    N("text.upper", 1, T(T_R), 1, T(T_R), n_upper, "Upper case, by Unicode's full mappings (ß becomes SS)."),
    N("text.lower", 1, T(T_R), 1, T(T_R), n_lower,
      "Lower case, by Unicode's full mappings (a final Σ becomes ς)."),
    N("text.ascii_upper", 1, T(T_R), 1, T(T_R), n_ascii_upper, "ASCII letters to upper case; every other byte unchanged."),
    N("text.ascii_lower", 1, T(T_R), 1, T(T_R), n_ascii_lower, "ASCII letters to lower case; every other byte unchanged."),
    N("text.title", 1, T(T_R), 1, T(T_R), n_title,
      "Title case: in each word, the first cased character to title case and the rest to lower case."),
    N("text.fold", 1, T(T_R), 1, T(T_R), n_fold, "Unicode case folding (full), for caseless comparison."),
    N("text.equal_fold", 2, T(T_R, T_R), 1, T(T_B), n_equal_fold,
      "Whether two strings are equal ignoring case and canonical equivalence (canonical caseless match)."),
    N("text.normalize", 2, T(T_R, T_R), 1, T(T_R), n_normalize,
      "Unicode normalization: the form is \"NFC\", \"NFD\", \"NFKC\" or \"NFKD\"."),
    N("text.is_normalized", 2, T(T_R, T_R), 1, T(T_B), n_is_normalized,
      "Whether a string is already in a normalization form (\"NFC\", \"NFD\", \"NFKC\", \"NFKD\")."),
    N("text.graphemes", 1, T(T_R), 1, T(T_R), n_graphemes,
      "The user-perceived characters (extended grapheme clusters), as a list of strings."),
    N("text.next_grapheme", 2, T(T_R, T_I64), 1, T(T_I64), n_next_grapheme,
      "The end of the grapheme cluster that starts at a byte offset (-1 at the end), for iterating."),
    N("text.words", 1, T(T_R), 1, T(T_R), n_words,
      "The words, as a list of strings: the segments between word boundaries that hold a letter, digit or "
      "ideograph."),
    N("text.word_segments", 1, T(T_R), 1, T(T_R), n_word_segments,
      "Every segment between word boundaries (words, spaces, punctuation), as a list of strings."),
    N("text.width", 1, T(T_R), 1, T(T_I64), n_width,
      "Columns on a terminal: 2 for wide (East Asian) characters and emoji, 0 for marks and controls."),
    N("text.category", 1, T(T_C), 1, T(T_R), n_category, "A character's general category: \"Lu\", \"Nd\", ..."),
    N("text.is_letter", 1, T(T_C), 1, T(T_B), n_is_letter, "Whether a character is a letter (category L*)."),
    N("text.is_alpha", 1, T(T_C), 1, T(T_B), n_is_alpha, "Whether a character is Alphabetic."),
    N("text.is_digit", 1, T(T_C), 1, T(T_B), n_is_digit, "Whether a character is a decimal digit (category Nd)."),
    N("text.is_number", 1, T(T_C), 1, T(T_B), n_is_number, "Whether a character is a number (category N*)."),
    N("text.is_space", 1, T(T_C), 1, T(T_B), n_is_space, "Whether a character is White_Space."),
    N("text.is_upper", 1, T(T_C), 1, T(T_B), n_is_upper, "Whether a character is Uppercase."),
    N("text.is_lower", 1, T(T_C), 1, T(T_B), n_is_lower, "Whether a character is Lowercase."),
    N("text.is_punct", 1, T(T_C), 1, T(T_B), n_is_punct, "Whether a character is punctuation (category P*)."),
    N("text.is_id_start", 1, T(T_C), 1, T(T_B), n_is_id_start,
      "Whether a character can start an identifier (XID_Start, UAX #31)."),
    N("text.is_id_continue", 1, T(T_C), 1, T(T_B), n_is_id_continue,
      "Whether a character can continue an identifier (XID_Continue, UAX #31)."),
    N("text.is_emoji", 1, T(T_C), 1, T(T_B), n_is_emoji, "Whether a character is Extended_Pictographic."),
    N("text.digit_value", 1, T(T_C), 1, T(T_I64), n_digit_value, "A decimal digit's value, or -1."),
    N("text.char_width", 1, T(T_C), 1, T(T_I64), n_char_width, "A character's columns on a terminal: 0, 1 or 2."),
    N("text.unicode_version", 0, T(0), 1, T(T_R), n_unicode_version,
      "The version of Unicode the tables come from, e.g. \"17.0.0\"."),
    N("text.repeat", 2, T(T_R, T_I64), 1, T(T_R), n_repeat, "A string repeated n times."),
    N("text.lines", 1, T(T_R), 1, T(T_R), n_lines, "Split into lines (\\n or \\r\\n), as a list of strings."),
    N("text.valid", 1, T(T_R), 1, T(T_B), n_valid, "Whether a string or byte buffer is valid UTF-8."),
    N("text.length", 1, T(T_R), 1, T(T_I64), n_length, "The number of characters (an invalid byte counts as one)."),
    N("text.chars", 1, T(T_R), 1, T(T_R), n_chars, "The characters, as a list of chars (invalid bytes: U+FFFD)."),
    N("text.next", 2, T(T_R, T_I64), 2, T(T_C, T_I64), n_next,
      "The character at a byte offset and the offset of the next one (-1 at the end), for iterating."),
    N("text.from_chars", 1, T(T_R), 1, T(T_R), n_from_chars, "A string from a list of characters (or code points)."),
    N("json.parse", 1, T(T_R), 1, T(T_R), n_json_parse,
      "Parse JSON whose top level is an object (a map) or array (a list). Integers are i64 (BigInt if larger), "
      "fractions f64, true/false bool, null null."),
    N("json.parse_any", 1, T(T_R), 1, T(T_R), n_json_parse_any,
      "Parse any JSON value, returned as the only element of a list (so numbers and booleans keep their types)."),
    N("json.stringify", 2, T(T_R, T_I64), 1, T(T_R), n_json_stringify,
      "JSON for a map, list, array, struct (an object of its fields), string, number or null; indent 0 is "
      "compact."),
};

void natives_register_text(VM *vm) {
    for (size_t i = 0; i < sizeof TEXT_NATIVES / sizeof TEXT_NATIVES[0]; i++) vm_register_native(vm, &TEXT_NATIVES[i]);
}

const NativeDef *natives_text_table(size_t *n) {
    *n = sizeof TEXT_NATIVES / sizeof TEXT_NATIVES[0];
    return TEXT_NATIVES;
}
