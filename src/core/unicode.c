/* unicode.c - see unicode.h. The algorithms are those of the Unicode
 * Standard: full case mapping with Final_Sigma (§3.13), normalization
 * (UAX #15), grapheme cluster and word boundaries (UAX #29), and East Asian
 * width (UAX #11) as terminals use it. */
#include "unicode.h"

typedef struct {
    uint32_t cp, off;
    uint8_t n;
} UniSeq;

typedef struct {
    uint32_t a, b, c; /* a + b compose to c */
} UniPair;

#include "unicode_tables.inc"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const char *uni_version(void) { return UNI_VERSION; }

const UniRecord *uni_record(uint32_t cp) {
    if (cp >= 0x110000) cp = 0x10FFFF; /* a noncharacter: unassigned, like every value past it */
    return &UNI_RECORDS[UNI_STAGE2[(size_t)UNI_STAGE1[cp / UNI_BLOCK] * UNI_BLOCK + cp % UNI_BLOCK]];
}

const char *uni_category(uint32_t cp) { return UNI_GC_NAMES[uni_record(cp)->gc]; }
bool uni_has(uint32_t cp, unsigned prop) { return (uni_record(cp)->flags & prop) != 0; }
bool uni_is_letter(uint32_t cp) { return uni_record(cp)->gc >= UNI_GC_Lu && uni_record(cp)->gc <= UNI_GC_Lo; }
bool uni_is_number(uint32_t cp) { return uni_record(cp)->gc >= UNI_GC_Nd && uni_record(cp)->gc <= UNI_GC_No; }
bool uni_is_punct(uint32_t cp) { return uni_record(cp)->gc >= UNI_GC_Pc && uni_record(cp)->gc <= UNI_GC_Po; }
int uni_digit(uint32_t cp) { return uni_record(cp)->digit; }
int uni_char_width(uint32_t cp) { return uni_record(cp)->width; }

/* ---- UTF-8 ------------------------------------------------------------------------ */

uint32_t uni_utf8_next(const unsigned char *s, size_t n, size_t *i) {
    unsigned char c = s[*i];
    if (c < 0x80) {
        (*i)++;
        return c;
    }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (len == 0 || c > 0xF4 || *i + (size_t)len > n) {
        (*i)++;
        return 0xFFFD;
    }
    uint32_t cp = c & (0xFF >> (len + 1));
    for (int k = 1; k < len; k++) {
        unsigned char d = s[*i + (size_t)k];
        if ((d & 0xC0) != 0x80) {
            (*i)++;
            return 0xFFFD;
        }
        cp = cp << 6 | (d & 0x3F);
    }
    static const uint32_t MIN[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < MIN[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { /* overlong, too big, surrogate */
        (*i)++;
        return 0xFFFD;
    }
    *i += (size_t)len;
    return cp;
}

void uni_utf8_put(StrBuf *b, uint32_t cp) {
    char t[4];
    size_t k;
    if (cp < 0x80) t[0] = (char)cp, k = 1;
    else if (cp < 0x800) t[0] = (char)(0xC0 | cp >> 6), t[1] = (char)(0x80 | (cp & 0x3F)), k = 2;
    else if (cp < 0x10000)
        t[0] = (char)(0xE0 | cp >> 12), t[1] = (char)(0x80 | (cp >> 6 & 0x3F)), t[2] = (char)(0x80 | (cp & 0x3F)), k = 3;
    else
        t[0] = (char)(0xF0 | cp >> 18), t[1] = (char)(0x80 | (cp >> 12 & 0x3F)), t[2] = (char)(0x80 | (cp >> 6 & 0x3F)),
        t[3] = (char)(0x80 | (cp & 0x3F)), k = 4;
    sb_putn(b, t, k);
}

/* A string's code points, in a growable array. */
typedef VEC(uint32_t) CpVec;

static void decode(const char *s, size_t n, CpVec *v) {
    for (size_t i = 0; i < n;) VEC_PUSH(*v, uni_utf8_next((const unsigned char *)s, n, &i));
}

static void encode(const uint32_t *cps, size_t n, StrBuf *out) {
    for (size_t i = 0; i < n; i++) uni_utf8_put(out, cps[i]);
}

/* ---- sorted tables ---------------------------------------------------------------- */

static const UniSeq *seq_find(const UniSeq *idx, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (idx[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && idx[lo].cp == cp ? &idx[lo] : NULL;
}

/* ---- case ------------------------------------------------------------------------- */

/* The full mapping of one code point: its length, the code points in out. */
static int case_one(uint32_t cp, int kind, uint32_t out[3]) {
    const UniRecord *r = uni_record(cp);
    if (r->flags & UNI_F_SPECIAL) {
        static const UniSeq *const IDX[] = {UNI_UPPER_IDX, UNI_LOWER_IDX, UNI_TITLE_IDX, UNI_FOLD_IDX};
        static const size_t NIDX[] = {COUNT(UNI_UPPER_IDX), COUNT(UNI_LOWER_IDX), COUNT(UNI_TITLE_IDX),
                                      COUNT(UNI_FOLD_IDX)};
        static const uint32_t *const POOL[] = {UNI_UPPER_POOL, UNI_LOWER_POOL, UNI_TITLE_POOL, UNI_FOLD_POOL};
        const UniSeq *e = seq_find(IDX[kind], NIDX[kind], cp);
        if (e) {
            for (int k = 0; k < e->n; k++) out[k] = POOL[kind][e->off + (uint32_t)k];
            return e->n;
        }
    }
    int32_t d = kind == UNI_UPPER ? r->d_upper : kind == UNI_LOWER ? r->d_lower : kind == UNI_TITLE ? r->d_title : r->d_fold;
    out[0] = (uint32_t)((int32_t)cp + d);
    return 1;
}

/* Final_Sigma (§3.13): a capital sigma at i ends a word, so lowercases to ς:
 * a cased letter comes before it, and none after, case-ignorables aside. */
static bool final_sigma(const uint32_t *c, size_t n, size_t i) {
    size_t j = i;
    bool before = false;
    while (j-- > 0) {
        if (uni_has(c[j], UNI_P_CASE_IGNORABLE)) continue;
        before = uni_has(c[j], UNI_P_CASED);
        break;
    }
    if (!before) return false;
    for (j = i + 1; j < n; j++) {
        if (uni_has(c[j], UNI_P_CASE_IGNORABLE)) continue;
        return !uni_has(c[j], UNI_P_CASED);
    }
    return true;
}

static void put_mapped(StrBuf *out, uint32_t cp, int kind) {
    uint32_t m[3];
    int k = case_one(cp, kind, m);
    for (int j = 0; j < k; j++) uni_utf8_put(out, m[j]);
}

void uni_case(const char *s, size_t n, int kind, StrBuf *out) {
    CpVec c = {0};
    decode(s, n, &c);
    if (kind == UNI_TITLE) { /* each word: its first cased character to title case, the rest to lower */
        size_t nb;
        size_t *bounds = uni_word_bounds(s, n, &nb);
        size_t ci = 0; /* code point index of the byte offset bounds[w] */
        size_t bi = 0;
        for (size_t w = 0; w + 1 < nb; w++) {
            bool seen = false;
            while (bi < bounds[w + 1]) {
                uint32_t cp = uni_utf8_next((const unsigned char *)s, n, &bi);
                if (seen) {
                    if (cp == 0x03A3 && final_sigma(c.v, c.n, ci)) uni_utf8_put(out, 0x03C2);
                    else put_mapped(out, cp, UNI_LOWER);
                } else if (uni_has(cp, UNI_P_CASED)) {
                    put_mapped(out, cp, UNI_TITLE);
                    seen = true;
                } else {
                    uni_utf8_put(out, cp);
                }
                ci++;
            }
        }
        free(bounds);
    } else {
        for (size_t i = 0; i < c.n; i++) {
            if (kind == UNI_LOWER && c.v[i] == 0x03A3 && final_sigma(c.v, c.n, i)) uni_utf8_put(out, 0x03C2);
            else put_mapped(out, c.v[i], kind);
        }
    }
    VEC_FREE(c);
}

/* ---- normalization (UAX #15) ---------------------------------------------------------- */

#define S_BASE 0xAC00
#define L_BASE 0x1100
#define V_BASE 0x1161
#define T_BASE 0x11A7
#define L_COUNT 19
#define V_COUNT 21
#define T_COUNT 28
#define N_COUNT (V_COUNT * T_COUNT)
#define S_COUNT (L_COUNT * N_COUNT)

static int ccc_of(uint32_t cp) { return uni_record(cp)->ccc; }

static void decompose_one(uint32_t cp, bool compat, CpVec *out) {
    if (cp >= S_BASE && cp < S_BASE + S_COUNT) { /* Hangul syllables, by arithmetic */
        uint32_t si = cp - S_BASE;
        VEC_PUSH(*out, L_BASE + si / N_COUNT);
        VEC_PUSH(*out, V_BASE + si % N_COUNT / T_COUNT);
        if (si % T_COUNT) VEC_PUSH(*out, T_BASE + si % T_COUNT);
        return;
    }
    const UniSeq *e = compat ? seq_find(UNI_COMPAT_IDX, COUNT(UNI_COMPAT_IDX), cp) : NULL;
    const uint32_t *pool = UNI_COMPAT_POOL;
    if (!e) {
        e = seq_find(UNI_CANON_IDX, COUNT(UNI_CANON_IDX), cp);
        pool = UNI_CANON_POOL;
    }
    if (!e) {
        VEC_PUSH(*out, cp);
        return;
    }
    for (int k = 0; k < e->n; k++) VEC_PUSH(*out, pool[e->off + (uint32_t)k]);
}

/* The canonical ordering algorithm: a stable sort of each run of non-starters
 * by combining class (runs are short, so insertion sort). */
static void reorder(uint32_t *c, size_t n) {
    for (size_t i = 1; i < n; i++) {
        int k = ccc_of(c[i]);
        if (!k) continue;
        uint32_t x = c[i];
        size_t j = i;
        while (j > 0 && ccc_of(c[j - 1]) > k) {
            c[j] = c[j - 1];
            j--;
        }
        c[j] = x;
    }
}

static uint32_t compose_pair(uint32_t a, uint32_t b) {
    if (a >= L_BASE && a < L_BASE + L_COUNT && b >= V_BASE && b < V_BASE + V_COUNT)
        return S_BASE + ((a - L_BASE) * V_COUNT + (b - V_BASE)) * T_COUNT;
    if (a >= S_BASE && a < S_BASE + S_COUNT && (a - S_BASE) % T_COUNT == 0 && b > T_BASE && b < T_BASE + T_COUNT)
        return a + (b - T_BASE);
    size_t lo = 0, hi = COUNT(UNI_COMPOSE);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const UniPair *p = &UNI_COMPOSE[mid];
        if (p->a < a || (p->a == a && p->b < b)) lo = mid + 1;
        else hi = mid;
    }
    return lo < COUNT(UNI_COMPOSE) && UNI_COMPOSE[lo].a == a && UNI_COMPOSE[lo].b == b ? UNI_COMPOSE[lo].c : 0;
}

/* The canonical composition algorithm, in place: returns the new length. */
static size_t compose(uint32_t *c, size_t n) {
    if (!n) return 0;
    size_t starter = 0, out = 1;
    bool have = ccc_of(c[0]) == 0;
    int last = have ? 0 : 256; /* the class of the last character kept since the starter */
    for (size_t i = 1; i < n; i++) {
        int k = ccc_of(c[i]);
        uint32_t comp = have && (last < k || (last == 0 && out == starter + 1)) ? compose_pair(c[starter], c[i]) : 0;
        if (comp) {
            c[starter] = comp;
            continue;
        }
        if (k == 0) {
            starter = out;
            have = true;
            last = 0;
        } else {
            last = k;
        }
        c[out++] = c[i];
    }
    return out;
}

static void normalize_cps(const char *s, size_t n, int form, CpVec *res) {
    bool compat = form == UNI_NFKC || form == UNI_NFKD;
    for (size_t i = 0; i < n;) decompose_one(uni_utf8_next((const unsigned char *)s, n, &i), compat, res);
    reorder(res->v, res->n);
    if (form == UNI_NFC || form == UNI_NFKC) res->n = compose(res->v, res->n);
}

void uni_normalize(const char *s, size_t n, int form, StrBuf *out) {
    CpVec r = {0};
    normalize_cps(s, n, form, &r);
    encode(r.v, r.n, out);
    VEC_FREE(r);
}

bool uni_is_normalized(const char *s, size_t n, int form) {
    StrBuf b = {0};
    uni_normalize(s, n, form, &b);
    bool same = b.n == n && (!n || !memcmp(b.s, s, n));
    sb_free(&b);
    return same;
}

bool uni_equal_fold(const char *a, size_t an, const char *b, size_t bn) {
    StrBuf x[2] = {{0}, {0}};
    const char *src[2] = {a, b};
    size_t len[2] = {an, bn};
    for (int k = 0; k < 2; k++) {
        StrBuf d = {0}, f = {0};
        uni_normalize(src[k], len[k], UNI_NFD, &d);
        uni_case(d.s ? d.s : "", d.n, UNI_FOLD, &f);
        uni_normalize(f.s ? f.s : "", f.n, UNI_NFD, &x[k]);
        sb_free(&d);
        sb_free(&f);
    }
    bool eq = x[0].n == x[1].n && (!x[0].n || !memcmp(x[0].s, x[1].s, x[0].n));
    sb_free(&x[0]);
    sb_free(&x[1]);
    return eq;
}

/* ---- grapheme clusters (UAX #29) -------------------------------------------------- */

static bool gcb_control(int g) { return g == UNI_GCB_CONTROL || g == UNI_GCB_CR || g == UNI_GCB_LF; }

size_t uni_next_grapheme(const char *s, size_t n, size_t off) {
    const unsigned char *u = (const unsigned char *)s;
    size_t i = off;
    if (i >= n) return n;
    const UniRecord *p = uni_record(uni_utf8_next(u, n, &i));
    /* What the rules that look further back need: regional indicators in a
     * row, an emoji followed by Extend* (1) and then a ZWJ (2), and an Indic
     * consonant followed by extenders (1) including a linker (2). */
    int ri = p->gcb == UNI_GCB_REGIONAL_INDICATOR;
    int emoji = (p->flags & UNI_F_EXTPICT) ? 1 : 0;
    int incb = p->incb == UNI_INCB_CONSONANT ? 1 : 0;
    while (i < n) {
        size_t at = i;
        const UniRecord *c = uni_record(uni_utf8_next(u, n, &i));
        int a = p->gcb, b = c->gcb;
        bool join;
        if (a == UNI_GCB_CR && b == UNI_GCB_LF) join = true;                     /* GB3 */
        else if (gcb_control(a) || gcb_control(b)) join = false;                 /* GB4, GB5 */
        else if (a == UNI_GCB_L && (b == UNI_GCB_L || b == UNI_GCB_V || b == UNI_GCB_LV || b == UNI_GCB_LVT))
            join = true;                                                         /* GB6 */
        else if ((a == UNI_GCB_LV || a == UNI_GCB_V) && (b == UNI_GCB_V || b == UNI_GCB_T)) join = true; /* GB7 */
        else if ((a == UNI_GCB_LVT || a == UNI_GCB_T) && b == UNI_GCB_T) join = true;                     /* GB8 */
        else if (b == UNI_GCB_EXTEND || b == UNI_GCB_ZWJ) join = true;          /* GB9 */
        else if (b == UNI_GCB_SPACINGMARK) join = true;                          /* GB9a */
        else if (a == UNI_GCB_PREPEND) join = true;                              /* GB9b */
        else if (incb == 2 && c->incb == UNI_INCB_CONSONANT) join = true;        /* GB9c */
        else if (emoji == 2 && (c->flags & UNI_F_EXTPICT)) join = true;          /* GB11 */
        else if (a == UNI_GCB_REGIONAL_INDICATOR && b == UNI_GCB_REGIONAL_INDICATOR) join = ri % 2; /* GB12, GB13 */
        else join = false;                                                       /* GB999 */
        if (!join) return at;
        ri = b == UNI_GCB_REGIONAL_INDICATOR ? ri + 1 : 0;
        if (c->flags & UNI_F_EXTPICT) emoji = 1;
        else if (emoji == 1 && b == UNI_GCB_EXTEND) emoji = 1;
        else if (emoji == 1 && b == UNI_GCB_ZWJ) emoji = 2;
        else emoji = 0;
        if (c->incb == UNI_INCB_CONSONANT) incb = 1;
        else if (incb && c->incb == UNI_INCB_LINKER) incb = 2;
        else if (incb && c->incb == UNI_INCB_EXTEND) { /* keeps the state */
        } else incb = 0;
        p = c;
    }
    return n;
}

/* ---- words (UAX #29) -------------------------------------------------------------------- */

static bool wb_ignorable(int w) { return w == UNI_WB_EXTEND || w == UNI_WB_FORMAT || w == UNI_WB_ZWJ; }
static bool ahletter(int w) { return w == UNI_WB_ALETTER || w == UNI_WB_HEBREW_LETTER; }
static bool midnumletq(int w) { return w == UNI_WB_MIDNUMLET || w == UNI_WB_SINGLE_QUOTE; }
static bool wb_newline(int w) { return w == UNI_WB_NEWLINE || w == UNI_WB_CR || w == UNI_WB_LF; }

/* Whether there is a boundary before code point i (0 < i < n), by rules
 * WB3 to WB999. w holds each code point's class; ext whether it is
 * Extended_Pictographic. Classes ignored by WB4 are skipped looking back and
 * forward from i. */
static bool word_break_at(const uint8_t *w, const bool *ext, size_t n, size_t i) {
    int a = w[i - 1], b = w[i];
    if (a == UNI_WB_CR && b == UNI_WB_LF) return false; /* WB3 */
    if (wb_newline(a) || wb_newline(b)) return true;     /* WB3a, WB3b */
    if (a == UNI_WB_ZWJ && ext[i]) return false;          /* WB3c */
    if (a == UNI_WB_WSEGSPACE && b == UNI_WB_WSEGSPACE) return false; /* WB3d */
    if (wb_ignorable(b)) return false;                    /* WB4 */
    /* the classes around i with WB4's characters skipped: before (p, pp) and after (b, nn) */
    long j = (long)i - 1;
    while (j >= 0 && wb_ignorable(w[j])) j--;
    if (j < 0) return true; /* only ignorables before: sot (WB1) did not join them to anything */
    int p = w[j];
    long k = j - 1;
    while (k >= 0 && wb_ignorable(w[k])) k--;
    int pp = k >= 0 ? w[k] : -1;
    size_t m = i + 1;
    while (m < n && wb_ignorable(w[m])) m++;
    int nn = m < n ? w[m] : -1;
    if (ahletter(p) && ahletter(b)) return false;                                     /* WB5 */
    if (ahletter(p) && (b == UNI_WB_MIDLETTER || midnumletq(b)) && nn >= 0 && ahletter(nn)) return false; /* WB6 */
    if (pp >= 0 && ahletter(pp) && (p == UNI_WB_MIDLETTER || midnumletq(p)) && ahletter(b)) return false; /* WB7 */
    if (p == UNI_WB_HEBREW_LETTER && b == UNI_WB_SINGLE_QUOTE) return false;         /* WB7a */
    if (p == UNI_WB_HEBREW_LETTER && b == UNI_WB_DOUBLE_QUOTE && nn == UNI_WB_HEBREW_LETTER) return false; /* WB7b */
    if (pp == UNI_WB_HEBREW_LETTER && p == UNI_WB_DOUBLE_QUOTE && b == UNI_WB_HEBREW_LETTER) return false; /* WB7c */
    if (p == UNI_WB_NUMERIC && b == UNI_WB_NUMERIC) return false;                     /* WB8 */
    if (ahletter(p) && b == UNI_WB_NUMERIC) return false;                             /* WB9 */
    if (p == UNI_WB_NUMERIC && ahletter(b)) return false;                             /* WB10 */
    if (pp == UNI_WB_NUMERIC && (p == UNI_WB_MIDNUM || midnumletq(p)) && b == UNI_WB_NUMERIC) return false; /* WB11 */
    if (p == UNI_WB_NUMERIC && (b == UNI_WB_MIDNUM || midnumletq(b)) && nn == UNI_WB_NUMERIC) return false; /* WB12 */
    if (p == UNI_WB_KATAKANA && b == UNI_WB_KATAKANA) return false;                   /* WB13 */
    if ((ahletter(p) || p == UNI_WB_NUMERIC || p == UNI_WB_KATAKANA || p == UNI_WB_EXTENDNUMLET) &&
        b == UNI_WB_EXTENDNUMLET)
        return false; /* WB13a */
    if (p == UNI_WB_EXTENDNUMLET && (ahletter(b) || b == UNI_WB_NUMERIC || b == UNI_WB_KATAKANA)) return false; /* WB13b */
    if (p == UNI_WB_REGIONAL_INDICATOR && b == UNI_WB_REGIONAL_INDICATOR) { /* WB15, WB16: pairs */
        size_t run = 0;
        for (long q = j; q >= 0; q--) {
            if (wb_ignorable(w[q])) continue;
            if (w[q] != UNI_WB_REGIONAL_INDICATOR) break;
            run++;
        }
        return run % 2 == 0;
    }
    return true; /* WB999 */
}

size_t *uni_word_bounds(const char *s, size_t n, size_t *count) {
    VEC(size_t) offs = {0};
    VEC(uint8_t) w = {0};
    VEC(bool) ext = {0};
    for (size_t i = 0; i < n;) {
        VEC_PUSH(offs, i);
        const UniRecord *r = uni_record(uni_utf8_next((const unsigned char *)s, n, &i));
        VEC_PUSH(w, r->wb);
        VEC_PUSH(ext, (r->flags & UNI_F_EXTPICT) != 0);
    }
    size_t *b = xcalloc(offs.n + 2, sizeof *b), k = 0;
    b[k++] = 0;
    for (size_t i = 1; i < offs.n; i++)
        if (word_break_at(w.v, ext.v, offs.n, i)) b[k++] = offs.v[i];
    if (n) b[k++] = n;
    *count = k;
    VEC_FREE(offs);
    VEC_FREE(w);
    VEC_FREE(ext);
    return b;
}

bool uni_is_word(const char *s, size_t a, size_t b) {
    for (size_t i = a; i < b;) {
        uint32_t cp = uni_utf8_next((const unsigned char *)s, b, &i);
        const UniRecord *r = uni_record(cp);
        if (ahletter(r->wb) || r->wb == UNI_WB_NUMERIC || r->wb == UNI_WB_KATAKANA || r->gc == UNI_GC_Lo ||
            (r->flags & UNI_F_ALPHA))
            return true;
    }
    return false;
}

/* ---- width ---------------------------------------------------------------------------- */

int64_t uni_width(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    int64_t total = 0;
    for (size_t off = 0; off < n;) {
        size_t end = uni_next_grapheme(s, n, off), i = off;
        uint32_t first = uni_utf8_next(u, end, &i);
        const UniRecord *r = uni_record(first);
        int w = r->width;
        if (r->flags & UNI_F_EXTPICT) { /* an emoji: wide if shown as one */
            bool vs16 = false;
            while (i < end) vs16 |= uni_utf8_next(u, end, &i) == 0xFE0F;
            if (vs16 || (r->flags & UNI_F_EMOJIPRES)) w = 2;
        }
        total += w;
        off = end;
    }
    return total;
}
