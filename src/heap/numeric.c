/* numeric.c - BigInt and decimal arithmetic.
 *
 * Magnitudes are little-endian arrays of 32-bit limbs worked on in temporary
 * BN buffers; results become immutable heap objects. Division is Knuth's
 * Algorithm D in the form popularised by Hacker's Delight (`divmnu`).
 * Temporaries are freed before the result is charged to the heap, so a quota
 * trap never leaks them. */
#include "numeric.h"

#include <math.h>

#include "heap.h"

#define MAX_LIMBS (1u << 24) /* 512 Mbit numbers are plenty; beyond that E_BAD_ALLOC */
#define MAX_EXP 1000000      /* decimal exponent magnitude limit */

typedef struct {
    int sign;
    uint32_t n, cap;
    uint32_t *d;
} BN;

static void bn_free(BN *a) {
    free(a->d);
    memset(a, 0, sizeof *a);
}

static void bn_reserve(BN *a, uint32_t n) {
    if (n <= a->cap) return;
    a->d = xrealloc(a->d, (size_t)n * 4);
    memset(a->d + a->cap, 0, (size_t)(n - a->cap) * 4);
    a->cap = n;
}

static void bn_norm(BN *a) {
    while (a->n && !a->d[a->n - 1]) a->n--;
    if (!a->n) a->sign = 0;
}

static void bn_set_u64(BN *a, uint64_t v, int sign) {
    bn_reserve(a, 2);
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> 32);
    a->n = 2;
    a->sign = sign;
    bn_norm(a);
}

static void bn_copy(BN *r, const uint32_t *d, uint32_t n, int sign) {
    bn_reserve(r, n ? n : 1);
    if (n) memcpy(r->d, d, (size_t)n * 4);
    r->n = n;
    r->sign = n ? sign : 0;
}

static int mag_cmp(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (uint32_t i = an; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void mag_add(BN *r, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    if (an < bn) {
        const uint32_t *t = a;
        a = b;
        b = t;
        uint32_t tn = an;
        an = bn;
        bn = tn;
    }
    uint32_t *out = xcalloc((size_t)an + 1, 4);
    uint64_t carry = 0;
    for (uint32_t i = 0; i < an; i++) {
        uint64_t s = (uint64_t)a[i] + (i < bn ? b[i] : 0) + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    out[an] = (uint32_t)carry;
    free(r->d);
    r->d = out;
    r->cap = an + 1;
    r->n = an + 1;
}

/* r = a - b, requires |a| >= |b| */
static void mag_sub(BN *r, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    uint32_t *out = xcalloc((size_t)an + 1, 4);
    int64_t borrow = 0;
    for (uint32_t i = 0; i < an; i++) {
        int64_t s = (int64_t)a[i] - (i < bn ? b[i] : 0) - borrow;
        borrow = s < 0;
        out[i] = (uint32_t)(s + (borrow ? ((int64_t)1 << 32) : 0));
    }
    free(r->d);
    r->d = out;
    r->cap = an + 1;
    r->n = an;
}

static void mag_mul(BN *r, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    uint32_t *out = xcalloc((size_t)an + bn + 1, 4);
    for (uint32_t i = 0; i < an; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < bn; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        out[i + bn] = (uint32_t)carry;
    }
    free(r->d);
    r->d = out;
    r->cap = an + bn + 1;
    r->n = an + bn;
}

static int clz32(uint32_t x) {
    int n = 0;
    while (!(x & 0x80000000u)) x <<= 1, n++;
    return n;
}

/* Knuth Algorithm D. q gets m-n+1 limbs, r gets n limbs. v[n-1] != 0, m >= n. */
static void divmnu(uint32_t *q, uint32_t *r, const uint32_t *u, const uint32_t *v, int m, int n) {
    const uint64_t b = (uint64_t)1 << 32;
    if (n == 1) {
        uint64_t k = 0;
        for (int j = m - 1; j >= 0; j--) {
            uint64_t cur = k * b + u[j];
            q[j] = (uint32_t)(cur / v[0]);
            k = cur - (uint64_t)q[j] * v[0];
        }
        r[0] = (uint32_t)k;
        return;
    }
    int s = clz32(v[n - 1]);
    uint32_t *vn = xmalloc((size_t)n * 4), *un = xmalloc((size_t)(m + 1) * 4);
    for (int i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (s ? (uint32_t)((uint64_t)v[i - 1] >> (32 - s)) : 0);
    vn[0] = v[0] << s;
    un[m] = s ? (uint32_t)((uint64_t)u[m - 1] >> (32 - s)) : 0;
    for (int i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (s ? (uint32_t)((uint64_t)u[i - 1] >> (32 - s)) : 0);
    un[0] = u[0] << s;
    for (int j = m - n; j >= 0; j--) {
        uint64_t num = (uint64_t)un[j + n] * b + un[j + n - 1];
        uint64_t qhat = num / vn[n - 1], rhat = num - qhat * vn[n - 1];
        while (qhat >= b || qhat * vn[n - 2] > b * rhat + un[j + n - 2]) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= b) break;
        }
        int64_t t, k = 0;
        for (int i = 0; i < n; i++) {
            uint64_t prod = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(prod & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (int64_t)(prod >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + n] - k;
        un[j + n] = (uint32_t)t;
        q[j] = (uint32_t)qhat;
        if (t < 0) { /* qhat was one too large: add back */
            q[j]--;
            k = 0;
            for (int i = 0; i < n; i++) {
                t = (int64_t)un[i + j] + vn[i] + k;
                un[i + j] = (uint32_t)t;
                k = t >> 32;
            }
            un[j + n] += (uint32_t)k;
        }
    }
    for (int i = 0; i < n - 1; i++) r[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
    r[n - 1] = un[n - 1] >> s;
    free(vn);
    free(un);
}

/* Truncated division of magnitudes: q = a / b, r = a % b (b != 0). */
static void mag_divmod(BN *q, BN *r, const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    if (mag_cmp(a, an, b, bn) < 0) {
        if (q) q->n = 0;
        if (r) bn_copy(r, a, an, 1);
        return;
    }
    uint32_t *qd = xcalloc((size_t)an - bn + 1, 4), *rd = xcalloc(bn, 4);
    divmnu(qd, rd, a, b, (int)an, (int)bn);
    if (q) {
        free(q->d);
        q->d = qd;
        q->cap = q->n = an - bn + 1;
    } else {
        free(qd);
    }
    if (r) {
        free(r->d);
        r->d = rd;
        r->cap = r->n = bn;
    } else {
        free(rd);
    }
}

static void bn_add_signed(BN *r, const BN *a, const BN *b, bool subtract) {
    int bs = subtract ? -b->sign : b->sign;
    if (!a->sign) {
        bn_copy(r, b->d, b->n, bs);
        return;
    }
    if (!bs) {
        bn_copy(r, a->d, a->n, a->sign);
        return;
    }
    BN t = {0, 0, 0, NULL};
    if (a->sign == bs) {
        mag_add(&t, a->d, a->n, b->d, b->n);
        t.sign = a->sign;
    } else {
        int c = mag_cmp(a->d, a->n, b->d, b->n);
        if (c >= 0) mag_sub(&t, a->d, a->n, b->d, b->n), t.sign = a->sign;
        else mag_sub(&t, b->d, b->n, a->d, a->n), t.sign = bs;
    }
    bn_free(r);
    *r = t;
    bn_norm(r);
}

static void bn_mul_signed(BN *r, const BN *a, const BN *b) {
    BN t = {0, 0, 0, NULL};
    mag_mul(&t, a->d, a->n, b->d, b->n);
    t.sign = a->sign * b->sign;
    bn_free(r);
    *r = t;
    bn_norm(r);
}

/* Multiply in place by a small factor and add a small addend. */
static void bn_muladd_small(BN *a, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (uint32_t i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        bn_reserve(a, a->n + 1);
        a->d[a->n++] = (uint32_t)carry;
    }
    if (a->n && !a->sign) a->sign = 1;
}

static uint32_t bn_divsmall(BN *a, uint32_t d) { /* in place; returns the remainder */
    uint64_t r = 0;
    for (uint32_t i = a->n; i-- > 0;) {
        uint64_t cur = (r << 32) | a->d[i];
        a->d[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    bn_norm(a);
    return (uint32_t)r;
}

static void bn_mul_pow10(BN *a, uint32_t k) {
    while (k >= 9) bn_muladd_small(a, 1000000000u, 0), k -= 9;
    static const uint32_t P[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (k) bn_muladd_small(a, P[k], 0);
}

static void bn_to_dec(StrBuf *b, const BN *a) {
    if (!a->sign) {
        sb_putc(b, '0');
        return;
    }
    BN t = {0, 0, 0, NULL};
    bn_copy(&t, a->d, a->n, 1);
    VEC(uint32_t) chunks = {0};
    while (t.n) VEC_PUSH(chunks, bn_divsmall(&t, 1000000000u));
    if (a->sign < 0) sb_putc(b, '-');
    sb_printf(b, "%u", chunks.v[chunks.n - 1]);
    for (size_t i = chunks.n - 1; i-- > 0;) sb_printf(b, "%09u", chunks.v[i]);
    VEC_FREE(chunks);
    bn_free(&t);
}

/* Parse optional sign + decimal digits (underscores allowed between digits). */
static bool bn_parse_dec(BN *a, const char *s, size_t n) {
    size_t i = 0;
    int sign = 1;
    if (i < n && (s[i] == '-' || s[i] == '+')) sign = s[i++] == '-' ? -1 : 1;
    if (i == n) return false;
    a->n = 0;
    a->sign = 0;
    bool any = false;
    for (; i < n; i++) {
        if (s[i] == '_' && any) continue;
        if (s[i] < '0' || s[i] > '9') return false;
        if (a->n == 0) bn_reserve(a, 1);
        if (a->n == 0 && s[i] != '0') {
            a->d[0] = (uint32_t)(s[i] - '0');
            a->n = 1;
            a->sign = 1;
        } else if (a->n) {
            bn_muladd_small(a, 10, (uint32_t)(s[i] - '0'));
        }
        any = true;
    }
    if (!any) return false;
    if (a->n) a->sign = sign;
    return true;
}

/* ---- objects -------------------------------------------------------------- */

static void check_size(Process *p, uint32_t n) {
    if (n > MAX_LIMBS) vm_trap(p, "E_BAD_ALLOC", "number too large (%u limbs)", n);
}

/* Build an object from a BN, freeing the BN *before* charging the heap. */
static Obj *make_num(Process *p, MlwsType t, BN *a, int32_t exp) {
    bn_norm(a);
    check_size(p, a->n);
    size_t hdr = t == T_BIG ? sizeof(ObjBig) : sizeof(ObjDec);
    size_t size = hdr + (size_t)a->n * 4;
    Obj *o;
    if (p) {
        o = gc_alloc_unaccounted(p, t == T_BIG ? OBJ_BIG : OBJ_DEC, size);
    } else { /* constant: immortal, owned by its module */
        o = xcalloc(1, size);
        o->kind = t == T_BIG ? OBJ_BIG : OBJ_DEC;
        o->flags = OBJF_IMMORTAL;
    }
    if (t == T_BIG) {
        ObjBig *g = (ObjBig *)o;
        g->sign = a->sign;
        g->n = a->n;
        if (a->n) memcpy(g->d, a->d, (size_t)a->n * 4);
    } else {
        ObjDec *dc = (ObjDec *)o;
        dc->sign = a->sign;
        dc->exp = exp; /* zero keeps its scale too: 0.00 + 1.5 = 1.50 */
        dc->n = a->n;
        if (a->n) memcpy(dc->d, a->d, (size_t)a->n * 4);
    }
    bn_free(a);
    if (p) gc_account(p, (int64_t)size); /* may trap E_QUOTA: nothing left to leak */
    return o;
}

size_t num_footprint(const Obj *a) {
    return a->kind == OBJ_BIG ? sizeof(ObjBig) + ((const ObjBig *)a)->n * 4u
                              : sizeof(ObjDec) + ((const ObjDec *)a)->n * 4u;
}

static void as_bn(const Obj *o, BN *out, int32_t *exp) {
    if (o->kind == OBJ_BIG) {
        const ObjBig *g = (const ObjBig *)o;
        bn_copy(out, g->d, g->n, g->sign);
        if (exp) *exp = 0;
    } else {
        const ObjDec *d = (const ObjDec *)o;
        bn_copy(out, d->d, d->n, d->sign);
        if (exp) *exp = d->exp;
    }
}

bool num_kind_matches(const Obj *o, MlwsType t) {
    return o && ((t == T_BIG && o->kind == OBJ_BIG) || (t == T_DEC && o->kind == OBJ_DEC));
}

Obj *num_from_i64(Process *p, MlwsType t, int64_t v, bool is_unsigned) {
    BN a = {0, 0, 0, NULL};
    if (is_unsigned || v >= 0) bn_set_u64(&a, (uint64_t)v, 1);
    else bn_set_u64(&a, 0 - (uint64_t)v, -1);
    return make_num(p, t, &a, 0);
}

Obj *num_parse(Process *p, MlwsType t, const char *s, size_t n) {
    BN a = {0, 0, 0, NULL};
    if (t == T_BIG) {
        if (!bn_parse_dec(&a, s, n)) {
            bn_free(&a);
            return NULL;
        }
        return make_num(p, t, &a, 0);
    }
    /* decimal: [sign] digits [. digits] [e [sign] digits] */
    char *digits = xmalloc(n + 2);
    size_t dn = 0, i = 0;
    long exp = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) digits[dn++] = s[i++];
    bool any = false, dot = false;
    for (; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            digits[dn++] = c;
            any = true;
            if (dot) exp--;
        } else if (c == '.' && !dot) {
            dot = true;
        } else if (c == '_' && any) {
            continue;
        } else {
            break;
        }
    }
    bool ok = any;
    if (ok && i < n && (s[i] == 'e' || s[i] == 'E')) {
        char *end;
        char ebuf[16];
        size_t el = n - i - 1 < sizeof ebuf - 1 ? n - i - 1 : sizeof ebuf - 1;
        memcpy(ebuf, s + i + 1, el);
        ebuf[el] = 0;
        long e = strtol(ebuf, &end, 10);
        ok = el > 0 && *end == 0 && e > -MAX_EXP && e < MAX_EXP;
        exp += e;
        i = n;
    }
    ok = ok && i == n && exp > -MAX_EXP && exp < MAX_EXP;
    if (ok) ok = bn_parse_dec(&a, digits, dn);
    free(digits);
    if (!ok) {
        bn_free(&a);
        return NULL;
    }
    return make_num(p, t, &a, (int32_t)exp);
}

Obj *num_from_f64(Process *p, MlwsType t, double v) {
    if (!isfinite(v)) vm_trap(p, "E_OVERFLOW", "cannot convert %s to %s", isnan(v) ? "nan" : "infinity", type_name(t));
    char buf[400];
    if (t == T_BIG) snprintf(buf, sizeof buf, "%.0f", trunc(v)); /* exact: every double integer prints exactly */
    else fmt_double(v, buf, sizeof buf); /* shortest form: 0.1 becomes 0.1, not 0.1000000000000000055... */
    return num_parse(p, t, buf, strlen(buf));
}

Obj *num_const(MlwsType t, const char *s, size_t n) { return num_parse(NULL, t, s, n); }

/* Bring two decimals to a common exponent (the smaller). */
static void align(BN *a, int32_t *ea, BN *b, int32_t *eb) {
    if (*ea > *eb) bn_mul_pow10(a, (uint32_t)(*ea - *eb)), *ea = *eb;
    else if (*eb > *ea) bn_mul_pow10(b, (uint32_t)(*eb - *ea)), *eb = *ea;
}

static void check_align(Process *p, int32_t ea, int32_t eb) {
    long d = (long)ea - eb;
    if (d > MAX_EXP || d < -MAX_EXP) vm_trap(p, "E_BAD_ALLOC", "decimal exponents too far apart");
}

static bool is_zero(const Obj *o) { return o->kind == OBJ_BIG ? ((const ObjBig *)o)->sign == 0 : ((const ObjDec *)o)->sign == 0; }

Obj *num_arith(Process *p, int op, const Obj *x, const Obj *y) {
    MlwsType t = x->kind == OBJ_BIG ? T_BIG : T_DEC;
    if ((op == OP_DIV || op == OP_REM) && is_zero(y)) vm_trap(p, "E_DIV_ZERO", "%s division by zero", type_name(t));
    if (op == OP_MIN || op == OP_MAX) {
        int c = num_cmp(x, y);
        return (Obj *)((op == OP_MIN) == (c <= 0) ? x : y);
    }
    int32_t ea = 0, eb = 0;
    if (t == T_DEC && op != OP_MUL && op != OP_DIV) check_align(p, ((const ObjDec *)x)->exp, ((const ObjDec *)y)->exp);
    BN a = {0, 0, 0, NULL}, b = {0, 0, 0, NULL}, r = {0, 0, 0, NULL};
    as_bn(x, &a, &ea);
    as_bn(y, &b, &eb);
    int32_t er = 0;
    switch (op) {
    case OP_ADD:
    case OP_SUB:
        align(&a, &ea, &b, &eb);
        bn_add_signed(&r, &a, &b, op == OP_SUB);
        er = ea;
        break;
    case OP_MUL:
        bn_mul_signed(&r, &a, &b);
        er = ea + eb;
        if (er > MAX_EXP || er < -MAX_EXP) {
            bn_free(&a);
            bn_free(&b);
            bn_free(&r);
            vm_trap(p, "E_BAD_ALLOC", "decimal exponent out of range");
        }
        break;
    case OP_REM:
        align(&a, &ea, &b, &eb);
        mag_divmod(NULL, &r, a.d, a.n, b.d, b.n);
        r.sign = a.sign;
        er = ea;
        break;
    default: { /* OP_DIV */
        if (t == T_BIG) {
            mag_divmod(&r, NULL, a.d, a.n, b.d, b.n);
            r.sign = a.sign * b.sign;
            break;
        }
        /* Q = round_half_even(a / b * 10^S), result Q * 10^-S, trailing zeros removed. */
        int32_t S = DEC_DIV_DIGITS;
        if (-ea > S) S = -ea;
        if (-eb > S) S = -eb;
        long k = (long)S + ea - eb;
        if (k > MAX_EXP || k < -MAX_EXP) {
            bn_free(&a);
            bn_free(&b);
            vm_trap(p, "E_BAD_ALLOC", "decimal exponents too far apart");
        }
        if (k >= 0) bn_mul_pow10(&a, (uint32_t)k);
        else bn_mul_pow10(&b, (uint32_t)-k);
        BN rem = {0, 0, 0, NULL};
        mag_divmod(&r, &rem, a.d, a.n, b.d, b.n);
        bn_norm(&r);
        bn_norm(&rem);
        if (rem.n) { /* round half to even: compare 2*rem with |b| */
            bn_muladd_small(&rem, 2, 0);
            int c = mag_cmp(rem.d, rem.n, b.d, b.n);
            if (c > 0 || (c == 0 && r.n && (r.d[0] & 1))) {
                if (!r.n) bn_set_u64(&r, 1, 1);
                else bn_muladd_small(&r, 1, 1);
            }
        }
        bn_free(&rem);
        r.sign = r.n ? a.sign * b.sign : 0;
        er = r.n ? -S : 0; /* a zero quotient is plain 0 */
        while (er < 0 && r.n) { /* strip trailing zeros */
            BN t2 = {0, 0, 0, NULL};
            bn_copy(&t2, r.d, r.n, r.sign);
            if (bn_divsmall(&t2, 10) != 0) {
                bn_free(&t2);
                break;
            }
            bn_free(&r);
            r = t2;
            er++;
        }
        break;
    }
    }
    bn_free(&a);
    bn_free(&b);
    bn_norm(&r);
    return make_num(p, t, &r, er);
}

Obj *num_unary(Process *p, int op, const Obj *x) {
    MlwsType t = x->kind == OBJ_BIG ? T_BIG : T_DEC;
    BN a = {0, 0, 0, NULL};
    int32_t e;
    as_bn(x, &a, &e);
    a.sign = op == OP_NEG ? -a.sign : (a.sign ? 1 : 0);
    return make_num(p, t, &a, e);
}

int num_cmp(const Obj *x, const Obj *y) {
    BN a = {0, 0, 0, NULL}, b = {0, 0, 0, NULL};
    int32_t ea, eb;
    as_bn(x, &a, &ea);
    as_bn(y, &b, &eb);
    int c;
    if (a.sign != b.sign) {
        c = a.sign < b.sign ? -1 : 1;
    } else if (!a.sign) {
        c = 0;
    } else {
        if (x->kind == OBJ_DEC) {
            if ((long)ea - eb > MAX_EXP || (long)eb - ea > MAX_EXP) { /* cannot align: exponents decide */
                int sign = a.sign;
                c = ea > eb ? 1 : -1;
                bn_free(&a);
                bn_free(&b);
                return sign > 0 ? c : -c;
            }
            align(&a, &ea, &b, &eb);
        }
        c = mag_cmp(a.d, a.n, b.d, b.n) * a.sign;
    }
    bn_free(&a);
    bn_free(&b);
    return c;
}

bool num_to_int(const Obj *x, MlwsType to, Word *out) {
    BN a = {0, 0, 0, NULL};
    int32_t e;
    as_bn(x, &a, &e);
    if (e > 0) bn_mul_pow10(&a, (uint32_t)e);
    while (e < 0 && a.n) { /* truncate toward zero */
        bn_divsmall(&a, 10);
        e++;
    }
    bool ok = a.n <= 2;
    uint64_t mag = a.n ? a.d[0] | (a.n > 1 ? (uint64_t)a.d[1] << 32 : 0) : 0;
    int bits = type_bits(to);
    if (ok && type_is_signed(to)) {
        uint64_t lim = (uint64_t)1 << (bits - 1);
        ok = a.sign < 0 ? mag <= lim : mag < lim;
        out->i = a.sign < 0 ? (int64_t)(0 - mag) : (int64_t)mag;
    } else if (ok) {
        ok = a.sign >= 0 && (bits == 64 || mag < ((uint64_t)1 << bits));
        out->u = mag;
    }
    if (to == T_B) out->i = a.sign != 0, ok = true;
    bn_free(&a);
    return ok;
}

void num_format(StrBuf *b, const Obj *x) {
    BN a = {0, 0, 0, NULL};
    int32_t e;
    as_bn(x, &a, &e);
    if (x->kind == OBJ_BIG || e == 0 || (!a.n && e > 0)) { /* zero prints as 0 unless it has a scale */
        bn_to_dec(b, &a);
        bn_free(&a);
        return;
    }
    StrBuf digits = {0};
    int sign = a.sign;
    a.sign = a.n ? 1 : 0;
    bn_to_dec(&digits, &a);
    bn_free(&a);
    if (sign < 0) sb_putc(b, '-');
    if (e > 0) {
        sb_puts(b, digits.s);
        for (int32_t i = 0; i < e; i++) sb_putc(b, '0');
    } else {
        size_t scale = (size_t)-e, len = digits.n;
        if (len <= scale) {
            sb_puts(b, "0.");
            for (size_t i = len; i < scale; i++) sb_putc(b, '0');
            sb_puts(b, digits.s);
        } else {
            sb_putn(b, digits.s, len - scale);
            sb_putc(b, '.');
            sb_puts(b, digits.s + len - scale);
        }
    }
    sb_free(&digits);
}

double num_to_f64(const Obj *x) {
    StrBuf b = {0};
    num_format(&b, x);
    double d = strtod(b.s, NULL);
    sb_free(&b);
    return d;
}

Obj *num_convert(Process *p, MlwsType to, const Obj *from) {
    BN a = {0, 0, 0, NULL};
    int32_t e;
    as_bn(from, &a, &e);
    if (to == T_BIG) {
        if (e > 0) bn_mul_pow10(&a, (uint32_t)e);
        while (e < 0 && a.n) bn_divsmall(&a, 10), e++;
        return make_num(p, T_BIG, &a, 0);
    }
    return make_num(p, T_DEC, &a, e);
}

/* Equal values hash alike: decimals are hashed without trailing zeros. */
uint32_t num_hash(const Obj *x) {
    BN a = {0, 0, 0, NULL};
    int32_t e;
    as_bn(x, &a, &e);
    if (x->kind == OBJ_DEC)
        while (a.n) {
            BN t = {0, 0, 0, NULL};
            bn_copy(&t, a.d, a.n, a.sign);
            if (bn_divsmall(&t, 10)) {
                bn_free(&t);
                break;
            }
            bn_free(&a);
            a = t;
            e++;
        }
    if (!a.n) e = 0; /* every zero is the same value */
    uint32_t h = hash_bytes(a.d, (size_t)a.n * 4) ^ (uint32_t)(a.sign * 0x9e3779b9u) ^ (uint32_t)e * 0x85ebca6bu;
    if (x->kind == OBJ_DEC) h ^= 0x5bd1e995u;
    bn_free(&a);
    return h;
}

bool num_equal(const Obj *a, const Obj *b) { return a->kind == b->kind && num_cmp(a, b) == 0; }
