/* vec.c - vector maths on arrays of floats (docs/STDLIB.md#vector-maths):
 * inner products, norms and distances, element-wise sums and scaling, and the
 * same over the rows of a matrix kept as one flat array, with the best k rows.
 * For embeddings, numerics and anything else that does the same arithmetic
 * on many floats: the JIT compiles such a loop one element at a time, and
 * these do it several at once.
 *
 * An f32 or f64 array keeps each element as a double (ObjArray's Words, an
 * f32 one rounded to float), so both are read the same way, and a result
 * stored in an f32 array is rounded as `mov` would round it.
 *
 * The sums keep several partial sums, which the compiler can put in vector
 * registers, and add them in an order fixed here; no compiler is let fuse a
 * multiply and an add (FP_CONTRACT off for clang; GCC's ISO C mode fuses none). So a result is the same on every
 * platform, compiler and mode, though it can differ in its last bits from a
 * sum taken left to right.
 *
 * A native cannot be preempted, so the work is charged to the process's
 * instructions, one for every VEC_PER_INSN elements. A call that would pass
 * the instruction quota does nothing and raises E_QUOTA, as a regex match
 * does, with the quota then used up: the process stops at its next slice,
 * whether or not it catches it. */
#include "../heap/heap.h"

#include <math.h>
#include <string.h>

#ifdef __clang__ /* (GCC fuses none under -std=c11, and warns of the pragma) */
#pragma STDC FP_CONTRACT OFF
#endif

#define VEC_PER_INSN 8
#define LANES 8

/* ---- arguments ---------------------------------------------------------------------- */

static ObjArray *floats(Process *p, Word w, const char *fn, const char *what) {
    ObjArray *a = w.r;
    if (!a || a->h.kind != OBJ_ARRAY || (a->elem != T_F32 && a->elem != T_F64))
        vm_trap(p, "E_TYPE", "%s: %s is not an f32 or f64 array", fn, what);
    return a;
}

static void same_type(Process *p, const ObjArray *a, const ObjArray *b, const char *fn) {
    if (a->elem != b->elem) vm_trap(p, "E_TYPE", "%s: an %s array with an %s one", fn, type_name((MlwsType)a->elem), type_name((MlwsType)b->elem));
}

static void same_len(Process *p, const ObjArray *a, const ObjArray *b, const char *fn) {
    if (a->len != b->len) vm_trap(p, "E_RANGE", "%s: lengths %u and %u differ", fn, a->len, b->len);
}

/* Two arrays to combine: both floats, of one type and one length. */
static void pair(Process *p, Word *a, ObjArray **x, ObjArray **y, const char *fn) {
    *x = floats(p, a[0], fn, "the first");
    *y = floats(p, a[1], fn, "the second");
    same_type(p, *x, *y, fn);
    same_len(p, *x, *y, fn);
}

/* Charges `elems` elements of work, or raises E_QUOTA if that passes the
 * instruction quota. */
static void charge(Process *p, uint64_t elems, const char *fn) {
    uint64_t cost = elems / VEC_PER_INSN, q = p->quota[QUOTA_INSNS];
    if (p->insns >= q || cost >= q - p->insns) {
        p->insns = q;
        vm_trap(p, "E_QUOTA", "instruction quota of %llu exceeded in %s", (unsigned long long)q, fn);
    }
    p->insns += cost;
}

/* ---- kernels ------------------------------------------------------------------------ */

/* LANES partial sums, added pairwise at the end. */
static double total(const double *s) {
    return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
}

static double k_dot(const Word *a, const Word *b, size_t n) {
    double s[LANES] = {0};
    size_t i = 0;
    for (; i + LANES <= n; i += LANES)
        for (int l = 0; l < LANES; l++) s[l] += a[i + l].f * b[i + l].f;
    for (int l = 0; i < n; i++, l++) s[l] += a[i].f * b[i].f;
    return total(s);
}

static double k_sq(const Word *a, size_t n) {
    double s[LANES] = {0};
    size_t i = 0;
    for (; i + LANES <= n; i += LANES)
        for (int l = 0; l < LANES; l++) s[l] += a[i + l].f * a[i + l].f;
    for (int l = 0; i < n; i++, l++) s[l] += a[i].f * a[i].f;
    return total(s);
}

static double k_dist2(const Word *a, const Word *b, size_t n) {
    double s[LANES] = {0};
    size_t i = 0;
    for (; i + LANES <= n; i += LANES)
        for (int l = 0; l < LANES; l++) {
            double d = a[i + l].f - b[i + l].f;
            s[l] += d * d;
        }
    for (int l = 0; i < n; i++, l++) {
        double d = a[i].f - b[i].f;
        s[l] += d * d;
    }
    return total(s);
}

/* The dot product and the squared norm of a, in one pass (b's is known). */
static void k_dot_sq(const Word *a, const Word *b, size_t n, double *dot, double *sq) {
    double s[LANES] = {0}, t[LANES] = {0};
    size_t i = 0;
    for (; i + LANES <= n; i += LANES)
        for (int l = 0; l < LANES; l++) {
            s[l] += a[i + l].f * b[i + l].f;
            t[l] += a[i + l].f * a[i + l].f;
        }
    for (int l = 0; i < n; i++, l++) {
        s[l] += a[i].f * b[i].f;
        t[l] += a[i].f * a[i].f;
    }
    *dot = total(s);
    *sq = total(t);
}

static double cosine(double dot, double sqa, double sqb) {
    double d = sqrt(sqa) * sqrt(sqb);
    return d == 0.0 ? 0.0 : dot / d;
}

/* A result stored in an array of this type. */
static double store(uint8_t elem, double v) { return elem == T_F32 ? (double)(float)v : v; }

/* ---- one or two vectors ------------------------------------------------------------- */

static void n_dot(Process *p, Word *a, Word *r) {
    ObjArray *x, *y;
    pair(p, a, &x, &y, "vec.dot");
    charge(p, x->len, "vec.dot");
    r[0].f = k_dot(x->data, y->data, x->len);
}

static void n_norm(Process *p, Word *a, Word *r) {
    ObjArray *x = floats(p, a[0], "vec.norm", "the vector");
    charge(p, x->len, "vec.norm");
    r[0].f = sqrt(k_sq(x->data, x->len));
}

static void n_cosine(Process *p, Word *a, Word *r) {
    ObjArray *x, *y;
    pair(p, a, &x, &y, "vec.cosine");
    charge(p, 3 * (uint64_t)x->len, "vec.cosine");
    double dot, sq;
    k_dot_sq(x->data, y->data, x->len, &dot, &sq);
    r[0].f = cosine(dot, sq, k_sq(y->data, y->len));
}

static void n_l2(Process *p, Word *a, Word *r) {
    ObjArray *x, *y;
    pair(p, a, &x, &y, "vec.l2");
    charge(p, x->len, "vec.l2");
    r[0].f = sqrt(k_dist2(x->data, y->data, x->len));
}

/* dst = x / |x| (zeros for a zero vector); dst may be x. */
static void normalize(ObjArray *dst, const ObjArray *x) {
    double n = sqrt(k_sq(x->data, x->len));
    for (uint32_t i = 0; i < x->len; i++) dst->data[i].f = n == 0.0 ? 0.0 : store(dst->elem, x->data[i].f / n);
}

static void add(ObjArray *dst, const ObjArray *x, const ObjArray *y) {
    for (uint32_t i = 0; i < x->len; i++) dst->data[i].f = store(dst->elem, x->data[i].f + y->data[i].f);
}

static void scale(ObjArray *dst, const ObjArray *x, double s) {
    for (uint32_t i = 0; i < x->len; i++) dst->data[i].f = store(dst->elem, x->data[i].f * s);
}

/* The array a result goes in, given: of the type and length of x. */
static ObjArray *into(Process *p, Word w, const ObjArray *x, const char *fn) {
    ObjArray *d = floats(p, w, fn, "the destination");
    same_type(p, d, x, fn);
    same_len(p, d, x, fn);
    return d;
}

static void n_normalize(Process *p, Word *a, Word *r) {
    ObjArray *x = floats(p, a[0], "vec.normalize", "the vector");
    charge(p, 2 * (uint64_t)x->len, "vec.normalize");
    ObjArray *d = array_new(p, (MlwsType)x->elem, x->len);
    normalize(d, x);
    r[0].r = d;
}

static void n_add(Process *p, Word *a, Word *r) {
    ObjArray *x, *y;
    pair(p, a, &x, &y, "vec.add");
    charge(p, x->len, "vec.add");
    ObjArray *d = array_new(p, (MlwsType)x->elem, x->len);
    add(d, x, y);
    r[0].r = d;
}

static void n_scale(Process *p, Word *a, Word *r) {
    ObjArray *x = floats(p, a[0], "vec.scale", "the vector");
    charge(p, x->len, "vec.scale");
    ObjArray *d = array_new(p, (MlwsType)x->elem, x->len);
    scale(d, x, a[1].f);
    r[0].r = d;
}

static void n_normalize_into(Process *p, Word *a, Word *r) {
    (void)r;
    ObjArray *x = floats(p, a[1], "vec.normalize_into", "the vector");
    ObjArray *d = into(p, a[0], x, "vec.normalize_into");
    charge(p, 2 * (uint64_t)x->len, "vec.normalize_into");
    normalize(d, x);
}

static void n_add_into(Process *p, Word *a, Word *r) {
    (void)r;
    ObjArray *x, *y;
    pair(p, a + 1, &x, &y, "vec.add_into");
    ObjArray *d = into(p, a[0], x, "vec.add_into");
    charge(p, x->len, "vec.add_into");
    add(d, x, y);
}

static void n_scale_into(Process *p, Word *a, Word *r) {
    (void)r;
    ObjArray *x = floats(p, a[1], "vec.scale_into", "the vector");
    ObjArray *d = into(p, a[0], x, "vec.scale_into");
    charge(p, x->len, "vec.scale_into");
    scale(d, x, a[2].f);
}

/* ---- the rows of a matrix ----------------------------------------------------------- */

enum { M_DOT, M_COSINE, M_L2 };

/* A matrix of rows of dim elements, a query of dim, and which rows: all
 * (rows null) or those an i64 array names. */
typedef struct {
    ObjArray *m, *q, *rows;
    uint32_t dim, nrows, count; /* rows in the matrix, rows to score */
    double qsq;                 /* the query's squared norm (cosine) */
} Rows;

static void rows_args(Process *p, Word *a, Word rows, Rows *s, const char *fn) {
    s->m = floats(p, a[0], fn, "the matrix");
    s->q = floats(p, a[2], fn, "the query");
    same_type(p, s->m, s->q, fn);
    if (a[1].i <= 0 || a[1].i > UINT32_MAX) vm_trap(p, "E_RANGE", "%s: a row of %lld elements", fn, (long long)a[1].i);
    s->dim = (uint32_t)a[1].i;
    if (s->m->len % s->dim)
        vm_trap(p, "E_RANGE", "%s: a matrix of %u elements is not rows of %u", fn, s->m->len, s->dim);
    if (s->q->len != s->dim) vm_trap(p, "E_RANGE", "%s: a query of %u elements for rows of %u", fn, s->q->len, s->dim);
    s->nrows = s->m->len / s->dim;
    s->rows = rows.r;
    s->count = s->nrows;
    if (s->rows) {
        if (s->rows->h.kind != OBJ_ARRAY || s->rows->elem != T_I64)
            vm_trap(p, "E_TYPE", "%s: the rows to score are not an i64 array (or null, for all)", fn);
        s->count = s->rows->len;
        for (uint32_t i = 0; i < s->count; i++)
            if (s->rows->data[i].i < 0 || s->rows->data[i].i >= s->nrows)
                vm_trap(p, "E_RANGE", "%s: row %lld of a matrix of %u", fn, (long long)s->rows->data[i].i, s->nrows);
    }
}

/* The index of the i-th row scored. */
static uint32_t row_at(const Rows *s, uint32_t i) { return s->rows ? (uint32_t)s->rows->data[i].i : i; }

static double score(const Rows *s, int metric, uint32_t row) {
    const Word *v = s->m->data + (size_t)row * s->dim;
    switch (metric) {
    case M_DOT: return k_dot(v, s->q->data, s->dim);
    case M_L2: return sqrt(k_dist2(v, s->q->data, s->dim));
    default: {
        double dot, sq;
        k_dot_sq(v, s->q->data, s->dim, &dot, &sq);
        return cosine(dot, sq, s->qsq);
    }
    }
}

/* What scoring the chosen rows costs, charged. */
static void charge_rows(Process *p, const Rows *s, int metric, const char *fn) {
    charge(p, (uint64_t)s->count * s->dim * (metric == M_COSINE ? 2 : 1), fn);
}

static void rows_into(Process *p, Word *a, int metric, const char *fn) {
    Rows s;
    rows_args(p, a, a[3], &s, fn);
    ObjArray *out = floats(p, a[4], fn, "the scores' array");
    if (out->len != s.count) vm_trap(p, "E_RANGE", "%s: %u rows to score into %u scores", fn, s.count, out->len);
    charge_rows(p, &s, metric, fn);
    if (metric == M_COSINE) s.qsq = k_sq(s.q->data, s.dim);
    for (uint32_t i = 0; i < s.count; i++) out->data[i].f = store(out->elem, score(&s, metric, row_at(&s, i)));
}

static void n_dot_rows(Process *p, Word *a, Word *r) {
    (void)r;
    rows_into(p, a, M_DOT, "vec.dot_rows");
}

static void n_cosine_rows(Process *p, Word *a, Word *r) {
    (void)r;
    rows_into(p, a, M_COSINE, "vec.cosine_rows");
}

static void n_l2_rows(Process *p, Word *a, Word *r) {
    (void)r;
    rows_into(p, a, M_L2, "vec.l2_rows");
}

/* ---- the best k --------------------------------------------------------------------- */

/* Whether (sa, ra) is better than (sb, rb): the higher score, or for l2 the
 * lower; NaN worst; between equals, the lower row. */
static bool better(int metric, double sa, int64_t ra, double sb, int64_t rb) {
    if (isnan(sa) || isnan(sb)) return isnan(sb) && (!isnan(sa) || ra < rb);
    if (sa != sb) return metric == M_L2 ? sa < sb : sa > sb;
    return ra < rb;
}

/* A heap of n entries with the worst at the root. */
static void sift_down(int metric, Word *ix, Word *sc, uint32_t n, uint32_t i) {
    for (;;) {
        uint32_t w = i, l = 2 * i + 1, rt = l + 1;
        if (l < n && better(metric, sc[w].f, ix[w].i, sc[l].f, ix[l].i)) w = l;
        if (rt < n && better(metric, sc[w].f, ix[w].i, sc[rt].f, ix[rt].i)) w = rt;
        if (w == i) return;
        Word t = ix[i];
        ix[i] = ix[w], ix[w] = t;
        t = sc[i];
        sc[i] = sc[w], sc[w] = t;
        i = w;
    }
}

static void sift_up(int metric, Word *ix, Word *sc, uint32_t i) {
    while (i) {
        uint32_t up = (i - 1) / 2;
        if (!better(metric, sc[up].f, ix[up].i, sc[i].f, ix[i].i)) return;
        Word t = ix[i];
        ix[i] = ix[up], ix[up] = t;
        t = sc[i];
        sc[i] = sc[up], sc[up] = t;
        i = up;
    }
}

static void n_top_k(Process *p, Word *a, Word *r) {
    const char *fn = "vec.top_k";
    Rows s;
    rows_args(p, a, a[5], &s, fn);
    if (a[3].i < 0) vm_trap(p, "E_RANGE", "%s: k is %lld", fn, (long long)a[3].i);
    const ObjString *name = a[4].r;
    if (!name || name->h.kind != OBJ_STRING) vm_trap(p, "E_TYPE", "%s: the metric is not a string", fn);
    int metric;
    if (!strcmp(name->data, "dot")) metric = M_DOT;
    else if (!strcmp(name->data, "cosine")) metric = M_COSINE;
    else if (!strcmp(name->data, "l2")) metric = M_L2;
    else vm_trap(p, "E_RANGE", "%s: the metric is \"dot\", \"cosine\" or \"l2\", not \"%s\"", fn, name->data);
    uint32_t k = (uint64_t)a[3].i < s.count ? (uint32_t)a[3].i : s.count;
    if (k) charge_rows(p, &s, metric, fn);
    ObjArray *ix = array_new(p, T_I64, k), *sc = array_new(p, T_F64, k); /* (no collection runs inside a native) */
    r[0].r = ix;
    r[1].r = sc;
    if (!k) return;
    if (metric == M_COSINE) s.qsq = k_sq(s.q->data, s.dim);
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.count; i++) {
        int64_t row = row_at(&s, i);
        double v = score(&s, metric, (uint32_t)row);
        if (n < k) {
            ix->data[n].i = row, sc->data[n].f = v;
            sift_up(metric, ix->data, sc->data, n++);
        } else if (better(metric, v, row, sc->data[0].f, ix->data[0].i)) {
            ix->data[0].i = row, sc->data[0].f = v;
            sift_down(metric, ix->data, sc->data, n, 0);
        }
    }
    while (n > 1) { /* the worst to the end, each in turn: best first */
        n--;
        Word t = ix->data[0];
        ix->data[0] = ix->data[n], ix->data[n] = t;
        t = sc->data[0];
        sc->data[0] = sc->data[n], sc->data[n] = t;
        sift_down(metric, ix->data, sc->data, n, 0);
    }
}

/* ---- the table ---------------------------------------------------------------------- */

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}

static const NativeDef VEC_NATIVES[] = {
    N("vec.dot", 2, T(T_R, T_R), 1, T(T_F64), n_dot, "The inner product of two f32 or f64 arrays of one type and length."),
    N("vec.norm", 1, T(T_R), 1, T(T_F64), n_norm, "The Euclidean (L2) norm of an f32 or f64 array."),
    N("vec.cosine", 2, T(T_R, T_R), 1, T(T_F64), n_cosine,
      "The cosine of the angle between two arrays: a . b / (|a| |b|), or 0 if either is all zeros."),
    N("vec.l2", 2, T(T_R, T_R), 1, T(T_F64), n_l2, "The Euclidean distance between two arrays."),
    N("vec.normalize", 1, T(T_R), 1, T(T_R), n_normalize,
      "A new array of the same type, scaled to a norm of 1 (all zeros stay zeros)."),
    N("vec.add", 2, T(T_R, T_R), 1, T(T_R), n_add, "A new array of the same type: a + b, element by element."),
    N("vec.scale", 2, T(T_R, T_F64), 1, T(T_R), n_scale, "A new array of the same type: each element times s."),
    N("vec.normalize_into", 2, T(T_R, T_R), 0, T(0), n_normalize_into,
      "normalize(a) into dst, an array of a's type and length (which may be a)."),
    N("vec.add_into", 3, T(T_R, T_R, T_R), 0, T(0), n_add_into, "a + b into dst (which may be a or b)."),
    N("vec.scale_into", 3, T(T_R, T_R, T_F64), 0, T(0), n_scale_into, "a times s into dst (which may be a)."),
    N("vec.dot_rows", 5, T(T_R, T_I64, T_R, T_R, T_R), 0, T(0), n_dot_rows,
      "(matrix, dim, query, rows, out): the inner product of the query with each row of a matrix kept as one array "
      "of rows of dim elements, into out (a float array): every row if rows is null, else those an i64 array "
      "names, in its order."),
    N("vec.cosine_rows", 5, T(T_R, T_I64, T_R, T_R, T_R), 0, T(0), n_cosine_rows,
      "As dot_rows, the cosine with each row."),
    N("vec.l2_rows", 5, T(T_R, T_I64, T_R, T_R, T_R), 0, T(0), n_l2_rows,
      "As dot_rows, the Euclidean distance to each row."),
    N("vec.top_k", 6, T(T_R, T_I64, T_R, T_I64, T_R, T_R), 2, T(T_R, T_R), n_top_k,
      "(matrix, dim, query, k, metric, rows) -> (indices, scores): the k rows closest to the query, best first, "
      "by \"dot\" or \"cosine\" (highest) or \"l2\" (lowest), among all rows or those rows names; an i64 array of "
      "their indices and an f64 array of their scores. Equal scores: the lower row first."),
};

void natives_register_vec(VM *vm) {
    for (size_t i = 0; i < sizeof VEC_NATIVES / sizeof VEC_NATIVES[0]; i++) vm_register_native(vm, &VEC_NATIVES[i]);
}

const NativeDef *natives_vec_table(size_t *n) {
    *n = sizeof VEC_NATIVES / sizeof VEC_NATIVES[0];
    return VEC_NATIVES;
}
