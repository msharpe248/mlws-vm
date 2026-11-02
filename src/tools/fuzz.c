/* fuzz.c - `mlwsvm fuzz`: differential testing of the VM's ways of running a
 * program. It writes random programs that always end, runs each in the
 * interpreter and in the other modes (the JIT, the optimizer, GC stress and
 * verification, and combinations of them), and reports every program whose
 * output or exit status differs from the interpreter's, or that crashes or
 * hangs in some mode. `--reduce` shrinks such a program to the lines that
 * still fail. See docs/TOOLING.md ("Differential fuzzing").
 *
 * A program is a few functions of one signature, (ref list, ref flist, ref
 * box, ref map, ref strings, i64, i64, f64) -> (i64, f64), and a main that
 * calls each of them three times (so `--jit on` compiles some in the middle)
 * and prints their results, then everything they can have written. Each
 * function calls only functions after it, and loops count to constants, so
 * every program ends (a coroutine's function too: it yields only a bounded
 * number of times, and its resumer resumes it until it is dead); a budget of instructions and allocations, counted
 * through loops and calls, keeps each one short. Registers have fixed roles
 * so that each instruction finds the type it expects:
 *   R0  a list of 16 integers      R5  a string (the function's own)
 *   R1  a list of 16 floats        R6  a Box: R2, or a new one
 *   R2  a Box (struct)             R7  a list of integers (the function's own)
 *   R3  a map: keys integers 0..31 and strings, values integers
 *   R4  a list of 8 strings        R8  a closure; R9 anything, briefly
 *   R10 a byte buffer of 32 or more  R11, R12 arrays of 16 i64 and f64
 *   I0..I11, F0..F7 values (I0, I1, F0 the arguments); I12..I14 loop
 *   counters; I15 the checksum; P0 a raw block of 128 bytes.
 * Indexes are masked into range, except in trapping statements, which are
 * mostly placed inside .try regions so that the handler's view of the
 * registers is compared too. */
#include "../platform/platform.h"
#include "tools.h"

/* ---- the program generator ------------------------------------------------ */

#define NI 12             /* I0..I11 hold values */
#define NF 8              /* F0..F7 */
#define CALL_BUDGET 40000 /* instructions one call may run, with what it calls */
#define ALLOC_BUDGET 1500 /* and allocations */
#define MAX_MULT 4000     /* iterations of the innermost of nested loops */

typedef struct {
    uint64_t cost, allocs; /* per call, with what it calls */
    bool up;               /* reads and writes UP[0]: called only through a closure */
} GenFunc;

typedef struct {
    StrBuf *o;
    uint64_t rng;
    int nfuncs, cur;
    GenFunc *fn;
    int label;
    int loops, tries, nest; /* enclosing loops, .try regions and compound statements */
    int left;               /* statements this function may still have */
    uint64_t mult;          /* how often the current statement runs per call */
    uint64_t spent, allocs;
    int act[5], nact;  /* the I registers the innermost loop works on (0: any) */
    int fact[3], nfact;
    int brk, cont;     /* the innermost loop's labels, or -1 */
    char ring[16][48]; /* operand text */
    int nring;
} Gen;

static uint64_t rnd(Gen *g) { /* splitmix64 */
    uint64_t z = (g->rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static int pick(Gen *g, int n) { return (int)(rnd(g) % (uint64_t)n); }
static bool chance(Gen *g, int pct) { return pick(g, 100) < pct; }
static char *slot(Gen *g) { return g->ring[g->nring++ & 15]; }

static void emit(Gen *g, const char *fmt, ...) MLWS_PRINTF(2, 3);
static void emit(Gen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_puts(g->o, "    ");
    sb_vprintf(g->o, fmt, ap);
    sb_putc(g->o, '\n');
    va_end(ap);
    g->spent += g->mult;
}
static int newlabel(Gen *g) { return g->label++; }
static void place(Gen *g, int l) { sb_printf(g->o, ".L%d:\n", l); }
static void alloc_cost(Gen *g, uint64_t n) { g->allocs += g->mult * n; }
static bool can_alloc(Gen *g) { return g->allocs + g->mult <= ALLOC_BUDGET; }

static int ireg_n(Gen *g) { return g->nact ? g->act[pick(g, g->nact)] : pick(g, NI); }
static int freg_n(Gen *g) { return g->nfact ? g->fact[pick(g, g->nfact)] : pick(g, NF); }
static const char *ri(Gen *g) {
    char *s = slot(g);
    snprintf(s, 48, "I%d", ireg_n(g));
    return s;
}
static const char *rf(Gen *g) {
    char *s = slot(g);
    snprintf(s, 48, "F%d", freg_n(g));
    return s;
}

static const char *ilit(Gen *g) {
    static const char *const L[] = {"0", "1", "-1", "2", "3", "5", "7", "8", "15", "16", "31", "32", "33", "63",
                                    "64", "65", "100", "127", "128", "255", "256", "1000", "65535", "65536",
                                    "2147483647", "-2147483648", "4294967295", "4294967296",
                                    "9223372036854775807", "-9223372036854775807", "-9223372036854775808"};
    char *s = slot(g);
    switch (pick(g, 4)) {
    case 0: snprintf(s, 48, "%lld", (long long)(int64_t)rnd(g)); break;
    case 1: snprintf(s, 48, "%d", pick(g, 2001) - 1000); break;
    default: snprintf(s, 48, "%s", L[pick(g, (int)(sizeof L / sizeof *L))]);
    }
    return s;
}
static const char *flit(Gen *g) {
    static const char *const L[] = {"0.0", "-0.0", "1.0", "-1.0", "0.5", "2.0", "3.25", "-7.75", "0.1", "1e10",
                                    "1e-10", "1e300", "-1e300", "1e-310", "inf", "-inf", "1000000.5", "-3.0"};
    char *s = slot(g);
    if (chance(g, 30)) snprintf(s, 48, "%.17g", (double)(int64_t)(rnd(g) % 2000001) / 1000.0 - 1000.0);
    else snprintf(s, 48, "%s", L[pick(g, (int)(sizeof L / sizeof *L))]);
    if (!strchr(s, '.') && !strchr(s, 'e') && !strchr(s, 'n')) strcat(s, ".0");
    return s;
}
static const char *oi(Gen *g) { return chance(g, 25) ? ilit(g) : ri(g); }
static const char *of(Gen *g) { return chance(g, 20) ? flit(g) : rf(g); }

/* An integer type suffix: mostly none (i64). */
static const char *ity(Gen *g) {
    static const char *const T[] = {".i64", ".i32", ".u32", ".i16", ".u16", ".i8", ".u8", ".u64"};
    return chance(g, 70) ? "" : T[pick(g, 8)];
}

/* An index register masked into 0..mask. */
static const char *masked(Gen *g, int mask) {
    const char *t = ri(g);
    emit(g, "and %s, %s, %d", t, ri(g), mask);
    return t;
}

static void stmts(Gen *g, int n);
static void stmt(Gen *g);

static void s_arith(Gen *g) {
    static const char *const OPS[] = {"add", "sub", "mul", "and", "or",  "xor", "shl",
                                      "shr", "sar", "rol", "ror", "min", "max"};
    int k = pick(g, 13);
    const char *op = OPS[k], *t = ity(g);
    bool many = k == 0 || k == 2 || k == 3 || k == 4 || k == 5 || k == 11 || k == 12;
    switch (pick(g, 5)) {
    case 0: emit(g, "%s%s %s, %s", op, t, ri(g), oi(g)); break;
    case 1:
        if (many) {
            emit(g, "%s%s %s, %s, %s, %s", op, t, ri(g), ri(g), oi(g), oi(g));
            break;
        }
        /* fall through */
    default: emit(g, "%s%s %s, %s, %s", op, t, ri(g), ri(g), oi(g));
    }
}

static void s_unary(Gen *g) {
    static const char *const OPS[] = {"neg", "abs", "not", "popcnt", "clz", "ctz", "bswap", "inc", "dec"};
    int k = pick(g, 9);
    if (k >= 7 || chance(g, 30)) emit(g, "%s%s %s", OPS[k], ity(g), ri(g));
    else emit(g, "%s%s %s, %s", OPS[k], ity(g), ri(g), oi(g));
}

static void s_div(Gen *g) {
    const char *op = chance(g, 50) ? "div" : "rem", *t = ity(g);
    if (g->tries && chance(g, 40)) {
        emit(g, "%s%s %s, %s, %s", op, t, ri(g), oi(g), ri(g)); /* may trap */
    } else if (chance(g, 50)) {
        const char *d = ri(g);
        emit(g, "or %s, %s, 1", d, ri(g));
        /* a narrow type reads the low bits, still odd: never 0 */
        emit(g, "%s%s %s, %s, %s", op, t, ri(g), oi(g), d);
    } else {
        static const char *const D[] = {"1", "-1", "2", "3", "7", "10", "16", "-5", "255", "1000000007"};
        emit(g, "%s%s %s, %s, %s", op, t, ri(g), oi(g), D[pick(g, 10)]);
    }
}

static const char *cmpname(Gen *g) {
    static const char *const C[] = {"eq", "ne", "lt", "le", "gt", "ge"};
    return C[pick(g, 6)];
}

static void s_cmp(Gen *g) {
    if (chance(g, 30)) emit(g, "s%s.f64 %s, %s, %s", cmpname(g), ri(g), rf(g), of(g));
    else {
        const char *t = ity(g);
        emit(g, "s%s%s %s, %s, %s", cmpname(g), *t ? t : ".i64", ri(g), ri(g), oi(g));
    }
}

static void s_sel(Gen *g) {
    if (chance(g, 30)) emit(g, "sel.f64 %s, %s, %s, %s", rf(g), ri(g), of(g), of(g));
    else emit(g, "sel %s, %s, %s, %s", ri(g), ri(g), oi(g), oi(g));
}

static void s_float(Gen *g) {
    static const char *const OPS[] = {"add", "sub", "mul", "div", "min", "max", "rem"};
    const char *op = OPS[pick(g, 7)], *t = chance(g, 12) ? ".f32" : ".f64";
    switch (pick(g, 6)) {
    case 0: emit(g, "%s%s %s, %s", op, t, rf(g), of(g)); break;
    case 1: emit(g, "fma%s %s, %s, %s, %s", t, rf(g), rf(g), of(g), of(g)); break;
    case 2: emit(g, "add%s %s, %s, %s, %s", t, rf(g), rf(g), of(g), of(g)); break;
    default: emit(g, "%s%s %s, %s, %s", op, t, rf(g), rf(g), of(g));
    }
}

static void s_ffun(Gen *g) {
    static const char *const OPS[] = {"sqrt", "floor", "ceil", "round", "trunc", "abs", "neg", "inc", "dec"};
    int k = pick(g, 9);
    if (k >= 7 || chance(g, 30)) emit(g, "%s.f64 %s", OPS[k], rf(g));
    else emit(g, "%s.f64 %s, %s", OPS[k], rf(g), of(g));
}

static void s_cvt(Gen *g) {
    switch (pick(g, 9)) {
    case 0: emit(g, "cvt.f64.i64 %s, %s", rf(g), oi(g)); break;
    case 1: emit(g, "cvt.i64.f64 %s, %s", ri(g), of(g)); break;
    case 2: emit(g, "cvt.f64.u64 %s, %s", rf(g), oi(g)); break;
    case 3: emit(g, "cvt.u64.f64 %s, %s", ri(g), of(g)); break;
    case 4: emit(g, "cvt.i32.f64 %s, %s", ri(g), of(g)); break;
    case 5: emit(g, "cvt.f32.f64 %s, %s", rf(g), of(g)); break;
    case 6: emit(g, "cvt.i32.i64 %s, %s", ri(g), oi(g)); break;
    case 7: emit(g, "cvt.u8.i64 %s, %s", ri(g), oi(g)); break;
    default: emit(g, "bitcast.f64.i64 %s, %s", rf(g), ri(g));
    }
}

static void s_mov(Gen *g) {
    switch (pick(g, 7)) {
    case 0: case 1: emit(g, "mov %s, %s", ri(g), oi(g)); break;
    case 2: emit(g, "mov %s, %s", rf(g), of(g)); break;
    case 3: emit(g, "swap %s, %s", ri(g), ri(g)); break;
    case 4: emit(g, "swap %s, %s", rf(g), rf(g)); break;
    case 5: emit(g, "mov%s %s, %s", ity(g), ri(g), ri(g)); break;
    default: emit(g, "ldv %s, %s, R0[%s]", ri(g), rf(g), masked(g, 15));
    }
}

/* An element operand of a list of 16: R[i], R[i+k] or R[k] (or, trapping,
 * R[any]). */
static const char *elem(Gen *g, int r) {
    char *s = slot(g);
    int k = pick(g, 8);
    if (k == 0) snprintf(s, 48, "R%d[%d]", r, pick(g, 16));
    else if (k == 1) {
        int d = 1 + pick(g, 8);
        const char *i = masked(g, 7);
        snprintf(s, 48, "R%d[%s+%d]", r, i, d);
    } else if (k == 2 && (g->tries || chance(g, 3))) snprintf(s, 48, "R%d[%s]", r, ri(g));
    else snprintf(s, 48, "R%d[%s]", r, masked(g, 15));
    return s;
}

static void s_list(Gen *g) {
    switch (pick(g, 6)) {
    case 0: case 1: { const char *e = elem(g, 0); emit(g, "mov %s, %s", ri(g), e); break; }
    case 2: { const char *e = elem(g, 0); emit(g, "mov %s, %s", e, oi(g)); break; }
    case 3: { const char *e = elem(g, 0); const char *d = ri(g); emit(g, "add %s, %s, %s", d, d, e); break; }
    case 4: { const char *e = elem(g, 0); emit(g, "add %s, %s", e, oi(g)); break; }
    default: { const char *e = elem(g, 0); emit(g, "tagof %s, %s", ri(g), e); }
    }
}

static void s_flist(Gen *g) {
    switch (pick(g, 4)) {
    case 0: case 1: { const char *e = elem(g, 1); emit(g, "mov %s, %s", rf(g), e); break; }
    case 2: { const char *e = elem(g, 1); emit(g, "mov %s, %s", e, of(g)); break; }
    default: { const char *e = elem(g, 1); const char *d = rf(g); emit(g, "mul.f64 %s, %s, %s", d, d, e); }
    }
}

static void s_box(Gen *g) {
    const char *b = chance(g, 60) ? "R2" : "R6";
    switch (pick(g, 10)) {
    case 0: emit(g, "mov %s, %s->Box.i", ri(g), b); break;
    case 1: emit(g, "mov %s->Box.i, %s", b, oi(g)); break;
    case 2: emit(g, "add %s->Box.i, %s", b, oi(g)); break;
    case 3: emit(g, "mov %s, %s->Box.f", rf(g), b); break;
    case 4: emit(g, "mov %s->Box.f, %s", b, of(g)); break;
    case 5: emit(g, "mov %s->Box.s, R5", b); break;
    case 6: {
        int skip = newlabel(g);
        emit(g, "mov R9, %s->Box.s", b);
        emit(g, "bz R9, .L%d", skip);
        emit(g, "len %s, R9", ri(g));
        place(g, skip);
        break;
    }
    case 7: { /* a new box, linked in */
        if (!can_alloc(g)) {
            s_arith(g);
            return;
        }
        alloc_cost(g, 1);
        emit(g, "new R6, Box");
        emit(g, "mov R6->Box.i, %s", oi(g));
        if (chance(g, 50)) emit(g, "mov R6->Box.s, R5");
        if (chance(g, 50)) emit(g, "mov R6->Box.n, R2->Box.n");
        emit(g, "mov R2->Box.n, R6");
        break;
    }
    case 8: { /* down the chain */
        int skip = newlabel(g);
        emit(g, "mov R9, %s->Box.n", b);
        emit(g, "bz R9, .L%d", skip);
        emit(g, "add %s, %s, R9->Box.i", ri(g), ri(g));
        emit(g, "bnisa R9, Box, .L%d", skip);
        emit(g, "mov R6, R9");
        place(g, skip);
        break;
    }
    default: emit(g, "mov R6, R2");
    }
}

static void s_map(Gen *g) {
    switch (pick(g, 8)) {
    case 0: case 1: { const char *k = masked(g, 31); emit(g, "mset R3, %s, %s", k, oi(g)); break; }
    case 2: { const char *k = masked(g, 31); emit(g, "mget %s, R3, %s, %s", ri(g), k, oi(g)); break; }
    case 3: { const char *k = masked(g, 31); emit(g, "mhas %s, R3, %s", ri(g), k); break; }
    case 4: { const char *k = masked(g, 31); emit(g, "mdel R3, %s", k); break; }
    case 5: emit(g, "len %s, R3", ri(g)); break;
    case 6:
        if (g->tries) {
            const char *k = masked(g, 63);
            emit(g, "mget %s, R3, %s", ri(g), k); /* E_KEY if absent */
        } else emit(g, "mget %s, R3, R5, -1", ri(g));
        break;
    default:
        if (!can_alloc(g)) return;
        alloc_cost(g, 1); /* a map that grows by a key may allocate */
        emit(g, "mset R3, R5, %s", oi(g));
    }
}

static void s_str(Gen *g) {
    int k = pick(g, 11);
    if (k <= 3 && !can_alloc(g)) k = 4;
    switch (k) {
    case 0: alloc_cost(g, 1); emit(g, "tostr R5, %s", ri(g)); break;
    case 1: alloc_cost(g, 1); emit(g, "tostr.f64 R5, %s", rf(g)); break;
    case 2: alloc_cost(g, 1); emit(g, "strcat R5, R5, \"%c%d\"", 'a' + pick(g, 26), pick(g, 100)); break;
    case 3: {
        alloc_cost(g, 1);
        const char *i = masked(g, 7);
        int skip = newlabel(g);
        emit(g, "mov R9, R4[%s]", i);
        emit(g, "strcat R5, R9, \"-\", R5");
        emit(g, "len I15, R5"); /* R9 may be R5 itself: it would double each time */
        emit(g, "blt I15, 200, .L%d", skip);
        emit(g, "substr R5, R5, 0, 100");
        place(g, skip);
        break;
    }
    case 4: emit(g, "len %s, R5", ri(g)); break;
    case 5: { /* a byte of R5 (never empty) */
        const char *n = ri(g), *i = ri(g);
        emit(g, "len %s, R5", n);
        emit(g, "and %s, %s, 255", i, ri(g));
        emit(g, "rem %s, %s, %s", i, i, n);
        emit(g, "mov %s, R5[%s]", ri(g), i);
        break;
    }
    case 6: emit(g, "mov R4[%d], R5", pick(g, 8)); break;
    case 7: {
        const char *i = masked(g, 7);
        emit(g, "mov R9, R4[%s]", i);
        emit(g, "strcmp %s, R5, R9", ri(g));
        break;
    }
    case 8: emit(g, "strfind %s, R5, \"%d\"", ri(g), pick(g, 10)); break;
    case 9: emit(g, "typeof %s, R%d", ri(g), pick(g, 8)); break;
    default:
        if (g->tries) emit(g, "mov %s, R5[%s]", ri(g), ri(g)); /* E_BOUNDS, mostly */
        else emit(g, "mov %s, R5[0]", ri(g));
    }
}

static void s_slist(Gen *g) {
    int skip;
    switch (pick(g, 8)) {
    case 0: case 1:
        if (!can_alloc(g)) return;
        alloc_cost(g, 1);
        emit(g, "lpush R7, %s", oi(g));
        break;
    case 2:
        skip = newlabel(g);
        emit(g, "len I15, R7");
        emit(g, "bz I15, .L%d", skip);
        emit(g, "lpop %s, R7", ri(g));
        place(g, skip);
        break;
    case 3:
        if (!can_alloc(g)) return;
        alloc_cost(g, 1);
        emit(g, "lins R7, 0, %s", oi(g));
        break;
    case 4:
        skip = newlabel(g);
        emit(g, "len I15, R7");
        emit(g, "bz I15, .L%d", skip);
        emit(g, "mov %s, R7[0]", ri(g));
        emit(g, "ldel R7, 0");
        place(g, skip);
        break;
    case 5: emit(g, "len %s, R7", ri(g)); break;
    case 6:
        if (!can_alloc(g)) return;
        alloc_cost(g, 1);
        emit(g, "lnew R7");
        break;
    default:
        skip = newlabel(g);
        emit(g, "len I15, R7");
        emit(g, "bz I15, .L%d", skip);
        emit(g, "dec I15");
        emit(g, "add %s, %s, R7[I15]", ri(g), ri(g));
        place(g, skip);
    }
}

static void s_global(Gen *g) {
    int k = pick(g, 9);
    switch (k) {
    case 0: emit(g, "mov %s, @g%d", ri(g), pick(g, 4)); break;
    case 1: emit(g, "mov @g%d, %s", pick(g, 4), oi(g)); break;
    case 2: emit(g, "add @g%d, %s", pick(g, 4), oi(g)); break;
    case 3: emit(g, "mov %s, @gf%d", rf(g), pick(g, 2)); break;
    case 4: emit(g, "mov @gf%d, %s", pick(g, 2), of(g)); break;
    case 5: { const char *i = masked(g, 15); emit(g, "mov @gt[%s], %s", i, oi(g)); break; }
    case 6: { const char *i = masked(g, 15); emit(g, "mov %s, @gt[%s]", ri(g), i); break; }
    case 7: { const char *i = masked(g, 31); emit(g, "mov.i32 %s, @gt{%s*4}", ri(g), i); break; }
    default: { const char *i = masked(g, 31); emit(g, "mov.u32 @gt{%s*4}, %s", i, oi(g)); }
    }
}

static void s_raw(Gen *g) {
    switch (pick(g, 6)) {
    case 0: case 1: { const char *i = masked(g, 15); emit(g, "mov P0[%s], %s", i, oi(g)); break; }
    case 2: { const char *i = masked(g, 15); emit(g, "mov %s, P0[%s]", ri(g), i); break; }
    case 3: { const char *i = masked(g, 31); emit(g, "mov.i32 %s, P0{%s*4}", ri(g), i); break; }
    case 4: { const char *i = masked(g, 127); emit(g, "mov.u8 P0{%s}, %s", i, oi(g)); break; }
    default: { const char *i = masked(g, 15); emit(g, "add %s, %s, P0[%s]", ri(g), ri(g), i); }
    }
}

static void s_stack(Gen *g) {
    const char *a = oi(g), *b = of(g);
    emit(g, "push %s, %s", a, b);
    if (chance(g, 50)) emit(g, "add SP[1], %s", oi(g)); /* the integer, under the float */
    emit(g, "pop %s, %s", rf(g), ri(g));
}

static void s_bytes(Gen *g) {
    switch (pick(g, 7)) {
    case 0: case 1: { const char *i = masked(g, 31); emit(g, "mov.u8 R10[%s], %s", i, oi(g)); break; }
    case 2: { const char *i = masked(g, 31); emit(g, "mov %s, R10[%s]", ri(g), i); break; }
    case 3:
        if (!can_alloc(g)) return;
        alloc_cost(g, 1);
        emit(g, "bufpush R10, %s, %s", oi(g), oi(g));
        break;
    case 4: emit(g, "len %s, R10", ri(g)); break;
    case 5: /* a small buffer, maybe empty (no storage yet), as text */
        if (!can_alloc(g)) return;
        alloc_cost(g, 2);
        emit(g, "bufnew R9, %d", pick(g, 3));
        emit(g, "strcat R5, R5, R9");
        break;
    default: { const char *i = masked(g, 31); emit(g, "add %s, %s, R10[%s+%d]", ri(g), ri(g), i, pick(g, 2)); }
    }
}

static void s_array(Gen *g) {
    switch (pick(g, 6)) {
    case 0: { const char *e = elem(g, 11); emit(g, "mov %s, %s", ri(g), e); break; }
    case 1: { const char *e = elem(g, 11); emit(g, "mov %s, %s", e, oi(g)); break; }
    case 2: { const char *e = elem(g, 11); emit(g, "add %s, %s", e, oi(g)); break; }
    case 3: { const char *e = elem(g, 12); emit(g, "mov %s, %s", rf(g), e); break; }
    case 4: { const char *e = elem(g, 12); emit(g, "mov %s, %s", e, of(g)); break; }
    default: { /* the narrow fields: a store truncates */
        const char *b = chance(g, 60) ? "R2" : "R6", *fld = chance(g, 50) ? "w" : "u";
        if (chance(g, 50)) emit(g, "mov %s->Box.%s, %s", b, fld, oi(g));
        else emit(g, "add %s, %s, %s->Box.%s", ri(g), ri(g), b, fld);
    }
    }
}

/* Inside a coroutine (cocur is not null), yield a value and take one back. */
static void s_yield(Gen *g) {
    int skip = newlabel(g);
    emit(g, "cocur R9");
    emit(g, "bz R9, .L%d", skip);
    emit(g, "coyield (%s, %s) -> (%s, %s)", oi(g), of(g), ri(g), rf(g));
    place(g, skip);
}

static void s_up(Gen *g) {
    switch (pick(g, 3)) {
    case 0: emit(g, "add %s, %s, UP[0]", ri(g), ri(g)); break;
    case 1: emit(g, "mov UP[0], %s", oi(g)); break;
    default: emit(g, "add UP[0], %s", oi(g));
    }
}

/* The arguments every function takes, with the box R2 or R6. */
static void call_args(Gen *g, StrBuf *s) {
    sb_printf(s, "(R0, R1, %s, R3, R4, %s, %s, %s)", chance(g, 70) ? "R2" : "R6", oi(g), oi(g), of(g));
}

static bool s_call(Gen *g) {
    int n = g->nfuncs - g->cur - 1;
    if (n <= 0) return false;
    int j = g->cur + 1 + pick(g, n);
    GenFunc *f = &g->fn[j];
    uint64_t closure = f->up || chance(g, 15);
    if (g->spent + g->mult * (f->cost + 20) > CALL_BUDGET || g->allocs + g->mult * (f->allocs + closure) > ALLOC_BUDGET)
        return false;
    StrBuf args = {0};
    call_args(g, &args);
    const char *ri0 = ri(g), *rf0 = rf(g);
    bool coro = closure && chance(g, 30) && g->allocs + g->mult * (f->allocs + 2) <= ALLOC_BUDGET;
    if (closure) {
        if (f->up) emit(g, "closure f%d (%s) -> (R8)", j, oi(g));
        else emit(g, "closure f%d -> (R8)", j);
    }
    if (coro) { /* run it in a coroutine, resumed until it is dead */
        int again = newlabel(g), done = newlabel(g);
        alloc_cost(g, 1);
        emit(g, "conew R9, R8, %d", 64 + pick(g, 200));
        emit(g, "coresume R9 %s -> (%s, %s)", args.s, ri0, rf0);
        place(g, again);
        emit(g, "costatus I15, R9");
        emit(g, "beq I15, 3, .L%d", done);
        emit(g, "coresume R9 (%s, %s) -> (%s, %s)", oi(g), of(g), ri0, rf0);
        emit(g, "jmp .L%d", again);
        place(g, done);
    } else if (closure) emit(g, "calli R8 %s -> (%s, %s)", args.s, ri0, rf0);
    else emit(g, "call f%d %s -> (%s, %s)", j, args.s, ri0, rf0);
    sb_free(&args);
    g->spent += g->mult * f->cost;
    g->allocs += g->mult * (f->allocs + closure);
    return true;
}

/* A condition that branches to label l. */
static void branch(Gen *g, int l) {
    switch (pick(g, 6)) {
    case 0: emit(g, "b%s.f64 %s, %s, .L%d", cmpname(g), rf(g), of(g), l); break;
    case 1: emit(g, "%s %s, .L%d", chance(g, 50) ? "bz" : "bnz", ri(g), l); break;
    case 2: {
        const char *t = ri(g);
        emit(g, "and %s, %s, %d", t, ri(g), 1 << pick(g, 4));
        emit(g, "bnz %s, .L%d", t, l);
        break;
    }
    default: emit(g, "b%s%s %s, %s, .L%d", cmpname(g), ity(g), ri(g), oi(g), l);
    }
}

static void s_if(Gen *g) {
    int els = newlabel(g), end = newlabel(g);
    branch(g, els);
    g->nest++;
    stmts(g, 1 + pick(g, 3));
    if (chance(g, 60)) {
        emit(g, "jmp .L%d", end);
        place(g, els);
        stmts(g, 1 + pick(g, 3));
        place(g, end);
    } else place(g, els);
    g->nest--;
}

static void s_jtab(Gen *g) {
    int n = 2 + pick(g, 3), end = newlabel(g), l[5];
    for (int k = 0; k < n; k++) l[k] = newlabel(g);
    const char *i = masked(g, 3);
    StrBuf s = {0};
    for (int k = 0; k < n; k++) sb_printf(&s, ", .L%d", l[k]);
    emit(g, "jtab %s%s", i, s.s);
    sb_free(&s);
    g->nest++;
    stmts(g, 1); /* an index past the table falls through */
    for (int k = 0; k < n; k++) {
        emit(g, "jmp .L%d", end);
        place(g, l[k]);
        stmts(g, 1 + pick(g, 2));
    }
    place(g, end);
    g->nest--;
}

static void s_loop(Gen *g) {
    static const int TRIPS[] = {1, 2, 3, 4, 5, 7, 10, 16, 33, 64, 100, 250, 1000};
    int n = TRIPS[pick(g, 13)];
    while (n > 1 && g->mult * (uint64_t)n > MAX_MULT) n /= 4;
    if (n < 1) n = 1;
    int c = 12 + g->loops, head = newlabel(g), cont = newlabel(g), out = newlabel(g);
    int save_act[5], save_nact = g->nact, save_fact[3], save_nfact = g->nfact, save_brk = g->brk, save_cont = g->cont;
    memcpy(save_act, g->act, sizeof save_act);
    memcpy(save_fact, g->fact, sizeof save_fact);
    if (chance(g, 85)) { /* a few registers, as many as x86-64 keeps in a loop */
        g->nact = 2 + pick(g, 4);
        for (int k = 0; k < g->nact; k++) g->act[k] = pick(g, NI);
        g->nfact = 1 + pick(g, 3);
        for (int k = 0; k < g->nfact; k++) g->fact[k] = pick(g, NF);
    } else g->nact = g->nfact = 0;
    int shape = pick(g, 3);
    if (shape == 2) emit(g, "mov I%d, %d", c, n);
    else emit(g, "mov I%d, 0", c);
    place(g, head);
    uint64_t mult = g->mult;
    g->mult *= (uint64_t)n;
    if (shape == 1) emit(g, "bge I%d, %d, .L%d", c, n, out);
    g->loops++, g->nest++;
    g->brk = out, g->cont = cont;
    stmts(g, 1 + pick(g, 5));
    g->loops--, g->nest--;
    place(g, cont);
    if (shape == 0) {
        emit(g, "inc I%d", c);
        emit(g, "blt I%d, %d, .L%d", c, n, head);
    } else if (shape == 1) {
        emit(g, "inc I%d", c);
        emit(g, "jmp .L%d", head);
    } else {
        emit(g, "dec I%d", c);
        emit(g, "bnz I%d, .L%d", c, head);
    }
    place(g, out);
    g->mult = mult;
    memcpy(g->act, save_act, sizeof save_act);
    memcpy(g->fact, save_fact, sizeof save_fact);
    g->nact = save_nact, g->nfact = save_nfact, g->brk = save_brk, g->cont = save_cont;
}

static void s_try(Gen *g) {
    int h = newlabel(g), end = newlabel(g);
    sb_printf(g->o, ".try .L%d\n", h);
    g->tries++, g->nest++;
    stmts(g, 1 + pick(g, 4));
    g->tries--;
    sb_puts(g->o, ".endtry\n");
    emit(g, "jmp .L%d", end);
    place(g, h);
    emit(g, "catch R9");
    if (chance(g, 70)) {
        emit(g, "mov R9, R9->Error.code");
        emit(g, "len %s, R9", ri(g));
    }
    stmts(g, pick(g, 3));
    place(g, end);
    g->nest--;
}

static void s_throw(Gen *g) {
    if (!can_alloc(g)) return;
    int skip = newlabel(g);
    alloc_cost(g, 1);
    branch(g, skip);
    emit(g, "new R9, Error");
    emit(g, "mov R9->Error.code, \"E_FUZZ\"");
    emit(g, "throw R9");
    place(g, skip);
}

static void s_jump(Gen *g) { /* break or continue */
    if (g->brk < 0) {
        s_arith(g);
        return;
    }
    branch(g, chance(g, 50) ? g->brk : g->cont);
}

static void s_ret(Gen *g) {
    int skip = newlabel(g);
    branch(g, skip);
    emit(g, "ret %s, %s", oi(g), of(g));
    place(g, skip);
}

static void stmt(Gen *g) {
    if (g->left <= 0) return;
    g->left--;
    int w = pick(g, 1000);
    bool compound = g->nest < 4;
#define W(n) if ((w -= (n)) < 0)
    W(110) {
        s_arith(g);
        return;
    }
    W(40) {
        s_unary(g);
        return;
    }
    W(30) {
        s_div(g);
        return;
    }
    W(35) {
        s_cmp(g);
        return;
    }
    W(20) {
        s_sel(g);
        return;
    }
    W(60) {
        s_float(g);
        return;
    }
    W(25) {
        s_ffun(g);
        return;
    }
    W(30) {
        s_cvt(g);
        return;
    }
    W(50) {
        s_mov(g);
        return;
    }
    W(60) {
        s_list(g);
        return;
    }
    W(30) {
        s_flist(g);
        return;
    }
    W(50) {
        s_box(g);
        return;
    }
    W(35) {
        s_map(g);
        return;
    }
    W(40) {
        s_str(g);
        return;
    }
    W(30) {
        s_slist(g);
        return;
    }
    W(30) {
        s_global(g);
        return;
    }
    W(25) {
        s_raw(g);
        return;
    }
    W(30) {
        s_bytes(g);
        return;
    }
    W(30) {
        s_array(g);
        return;
    }
    W(12) {
        s_yield(g);
        return;
    }
    W(10) {
        s_stack(g);
        return;
    }
    W(g->fn[g->cur].up ? 30 : 0) {
        s_up(g);
        return;
    }
    W(45) {
        if (!s_call(g)) s_arith(g);
        return;
    }
    W(g->loops ? 25 : 0) {
        s_jump(g);
        return;
    }
    W(compound ? 60 : 0) {
        s_if(g);
        return;
    }
    W(compound && g->loops < 3 ? 70 : 0) {
        s_loop(g);
        return;
    }
    W(compound ? 15 : 0) {
        s_jtab(g);
        return;
    }
    W(compound ? 25 : 0) {
        s_try(g);
        return;
    }
    W(g->tries ? 30 : 1) {
        s_throw(g);
        return;
    }
    W(4) {
        s_ret(g);
        return;
    }
#undef W
    s_arith(g);
}

static void stmts(Gen *g, int n) {
    for (int i = 0; i < n; i++) stmt(g);
}

static void gen_func(Gen *g, int k, int size) {
    GenFunc *f = &g->fn[k];
    g->cur = k;
    g->spent = g->allocs = 0;
    g->mult = 1;
    g->loops = g->tries = g->nest = 0;
    g->nact = g->nfact = 0;
    g->brk = g->cont = -1;
    g->left = size;
    sb_printf(g->o, "\n.func f%d(ref l, ref fl, ref b, ref m, ref ss, i64 x, i64 y, f64 z) -> (i64, f64)%s\n", k,
              f->up ? "  ; through a closure, with an i64 capture" : "");
    for (int r = 2; r < NI; r++) {
        if (chance(g, 50)) emit(g, "mov I%d, %s", r, ilit(g));
        else emit(g, "add I%d, I%d, %d", r, pick(g, 2), pick(g, 100));
    }
    for (int r = 1; r < NF; r++) emit(g, "mov F%d, %s", r, flit(g));
    emit(g, "tostr R5, I0");
    emit(g, "mov R6, R2");
    emit(g, "lnew R7");
    emit(g, "alloc P0, 128");
    emit(g, "bufnew R10, 32");
    emit(g, "anew.i64 R11, 16");
    emit(g, "anew.f64 R12, 16");
    alloc_cost(g, 6);
    while (g->left > 0) stmt(g);
    /* the checksum */
    emit(g, "add I15, I0, I1, I2, I3, I4, I5, I6, I7, I8, I9, I10, I11");
    emit(g, "xor I14, I0, I1, I2, I3, I4, I5, I6, I7, I8, I9, I10, I11");
    emit(g, "rol I14, 17");
    emit(g, "xor I15, I14");
    emit(g, "len I14, R7");
    emit(g, "add I15, I14");
    emit(g, "len I14, R5");
    emit(g, "add I15, I14");
    emit(g, "add I14, R11[%d], R11[%d], R10[%d]", pick(g, 16), pick(g, 16), pick(g, 32));
    emit(g, "add I15, I14");
    emit(g, "add.f64 F8, F0, F1, F2, F3, F4, F5, F6, F7");
    emit(g, "free P0");
    int j = k + 1 + (k + 1 < g->nfuncs ? pick(g, g->nfuncs - k - 1) : 0);
    if (j < g->nfuncs && !g->fn[j].up && chance(g, 15) && g->spent + g->fn[j].cost <= CALL_BUDGET &&
        g->allocs + g->fn[j].allocs <= ALLOC_BUDGET) {
        emit(g, "tcall f%d (R0, R1, R2, R3, R4, I15, I3, F8)", j);
        g->spent += g->fn[j].cost;
        g->allocs += g->fn[j].allocs;
    } else emit(g, "ret I15, F8");
    sb_puts(g->o, ".endf\n");
    f->cost = g->spent;
    f->allocs = g->allocs;
}

/* Print an integer, or a float's bits (any NaN as one), then a space. */
static void print_i(StrBuf *o, const char *v) { sb_printf(o, "    ncall \"io.print_i\" (%s)\n    ncall \"io.print_c\" (' ')\n", v); }
static void print_f(StrBuf *o, Gen *g, const char *v) {
    int nan = newlabel(g), done = newlabel(g);
    sb_printf(o, "    mov F9, %s\n    bne.f64 F9, F9, .L%d\n    bitcast.i64.f64 I9, F9\n    jmp .L%d\n", v, nan, done);
    sb_printf(o, ".L%d:\n    mov I9, 9221120237041090560\n.L%d:\n", nan, done);
    print_i(o, "I9");
}
static void nl(StrBuf *o) { sb_puts(o, "    ncall \"io.nl\" ()\n"); }

static void gen_main(Gen *g, int ncalls) {
    StrBuf *o = g->o;
    g->cur = -1;
    g->mult = 1;
    sb_puts(o, "\n.func main\n"
               "    lnew R0, 16\n    resize R0, 16\n    lnew R1, 16\n    resize R1, 16\n"
               "    new R2, Box\n    mov R2->Box.i, 7\n    mov R2->Box.f, 0.5\n    mov R2->Box.s, \"box\"\n"
               "    mnew R3\n    lnew R4, 8\n"
               "    lpush R4, \"s0\", \"s1\", \"s2\", \"s3\", \"s4\", \"s5\", \"s6\", \"s7\"\n"
               "    mov I10, 0\n");
    int round = newlabel(g);
    place(g, round);
    for (int c = 0; c < ncalls; c++) {
        int j = c < g->nfuncs ? c : pick(g, g->nfuncs), h = newlabel(g), next = newlabel(g);
        char callee[16];
        snprintf(callee, sizeof callee, g->fn[j].up ? "calli R8" : "call f%d", j);
        sb_printf(o, ".try .L%d\n", h);
        if (g->fn[j].up) sb_printf(o, "    closure f%d (I10) -> (R8)\n", j);
        sb_printf(o, "    %s (R0, R1, R2, R3, R4, %s, I10, %s) -> (I1, F1)\n.endtry\n", callee, ilit(g), flit(g));
        sb_printf(o, "    ncall \"io.print_s\" (\"call %d f%d: \")\n", c, j);
        print_i(o, "I1");
        print_f(o, g, "F1");
        nl(o);
        sb_printf(o, "    jmp .L%d\n.L%d:\n    catch R9\n    mov R9, R9->Error.code\n", next, h);
        sb_printf(o, "    ncall \"io.print_s\" (\"call %d f%d: \")\n    ncall \"io.print_s\" (R9)\n", c, j);
        nl(o);
        place(g, next);
    }
    sb_puts(o, "    inc I10\n");
    sb_printf(o, "    blt I10, 3, .L%d\n", round);
    /* what the calls can have written */
    int l1 = newlabel(g), l2 = newlabel(g), l3 = newlabel(g), l4 = newlabel(g), l5 = newlabel(g);
    sb_puts(o, "    ncall \"io.print_s\" (\"list: \")\n    mov I2, 0\n");
    place(g, l1);
    sb_puts(o, "    mov I3, R0[I2]\n");
    print_i(o, "I3");
    sb_printf(o, "    inc I2\n    blt I2, 16, .L%d\n", l1);
    nl(o);
    sb_puts(o, "    ncall \"io.print_s\" (\"floats: \")\n    mov I2, 0\n");
    place(g, l2);
    print_f(o, g, "R1[I2]");
    sb_printf(o, "    inc I2\n    blt I2, 16, .L%d\n", l2);
    nl(o);
    sb_puts(o, "    ncall \"io.print_s\" (\"boxes: \")\n    mov R9, R2\n    mov I2, 0\n");
    place(g, l3);
    sb_printf(o, "    bz R9, .L%d\n", l4);
    print_i(o, "R9->Box.i");
    print_f(o, g, "R9->Box.f");
    int nos = newlabel(g);
    sb_printf(o, "    mov R8, R9->Box.s\n    bz R8, .L%d\n    ncall \"io.print_s\" (R8)\n", nos);
    place(g, nos);
    sb_puts(o, "    ncall \"io.print_c\" (' ')\n    mov R9, R9->Box.n\n    inc I2\n");
    sb_printf(o, "    blt I2, 6, .L%d\n", l3);
    place(g, l4);
    nl(o);
    sb_puts(o, "    ncall \"io.print_s\" (\"map: \")\n    len I3, R3\n");
    print_i(o, "I3");
    sb_puts(o, "    mkeys R9, R3\n    mov I2, 0\n");
    place(g, l5);
    int l6 = newlabel(g);
    sb_printf(o, "    bge I2, I3, .L%d\n    tostr R8, R9[I2]\n    ncall \"io.print_s\" (R8)\n"
                 "    ncall \"io.print_c\" ('=')\n    mget I4, R3, R9[I2]\n",
              l6);
    print_i(o, "I4");
    sb_printf(o, "    inc I2\n    jmp .L%d\n", l5);
    place(g, l6);
    nl(o);
    sb_puts(o, "    ncall \"io.print_s\" (\"strings: \")\n");
    for (int k = 0; k < 8; k++)
        sb_printf(o, "    mov R8, R4[%d]\n    ncall \"io.print_s\" (R8)\n    ncall \"io.print_c\" (' ')\n", k);
    nl(o);
    sb_puts(o, "    ncall \"io.print_s\" (\"globals: \")\n");
    for (int k = 0; k < 4; k++) {
        char v[16];
        snprintf(v, sizeof v, "@g%d", k);
        print_i(o, v);
    }
    print_f(o, g, "@gf0");
    print_f(o, g, "@gf1");
    for (int k = 0; k < 16; k++) {
        char v[16];
        snprintf(v, sizeof v, "@gt[%d]", k);
        print_i(o, v);
    }
    nl(o);
    sb_puts(o, "    ret 0\n.endf\n");
}

/* The program for a seed: deterministic, and the same on every host. */
static void gen_program(uint64_t seed, int size, StrBuf *o) {
    Gen g = {0};
    g.o = o;
    g.rng = seed * 0x2545f4914f6cdd1dULL + 1;
    g.nfuncs = 2 + pick(&g, 5);
    g.fn = xcalloc((size_t)g.nfuncs, sizeof *g.fn);
    for (int k = 1; k < g.nfuncs; k++) g.fn[k].up = chance(&g, 25);
    sb_printf(o, "; mlwsvm fuzz --emit %llu --size %d\n", (unsigned long long)seed, size);
    sb_puts(o, ".struct Box\n    i: i64\n    f: f64\n    s: ref\n    n: ref\n    w: i32\n    u: u8\n.ends\n\n");
    sb_printf(o, "g0: .i64 %s\ng1: .i64 %s\ng2: .i64 0\ng3: .i64 %s\n", ilit(&g), ilit(&g), ilit(&g));
    sb_printf(o, "gf0: .f64 %s\ngf1: .f64 %s\n", flit(&g), flit(&g));
    sb_puts(o, "gt: .i64 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0\n");
    /* the last function first: a call's cost is known when it is written */
    StrBuf *parts = xcalloc((size_t)g.nfuncs, sizeof *parts);
    for (int k = g.nfuncs - 1; k >= 0; k--) {
        int n = size / 2 + pick(&g, size + 1);
        if (chance(&g, 10)) n *= 8; /* a large function: a lot of machine code */
        g.o = &parts[k];
        gen_func(&g, k, n);
    }
    for (int k = 0; k < g.nfuncs; k++) {
        sb_putn(o, parts[k].s, parts[k].n);
        sb_free(&parts[k]);
    }
    free(parts);
    g.o = o;
    gen_main(&g, g.nfuncs + pick(&g, 3));
    free(g.fn);
}

/* ---- running a program in each mode ----------------------------------------- */

typedef struct {
    const char *name, *args, *what;
} FuzzMode;

/* The interpreter first: the reference the others are compared with. */
static const FuzzMode MODES[] = {
    {"interp", "", "the interpreter, the reference"},
    {"jit", "--jit all", "every function compiled as it loads, entered at every block"},
    {"hot", "--jit on --jit-hot 2", "functions compiled once hot, in the middle of a run"},
    {"first", "--jit on --jit-hot 1", "functions compiled at their first call"},
    {"slice", "--jit all --slice 7", "the JIT with a time slice of 7 instructions"},
    {"opt", "--opt all", "every optimizer pass"},
    {"jit-opt", "--jit all --opt all", "the JIT on optimized code"},
    {"stress", "--gc-stress", "a collection at every allocation"},
    {"jit-stress", "--jit all --gc-stress", "the JIT with a collection at every allocation"},
    {"verify", "--gc-verify", "each minor collection checked against the whole heap"},
    {"jit-verify", "--jit all --gc-verify", "the JIT, each minor collection checked"},
};
#define NMODES ((int)(sizeof MODES / sizeof *MODES))

typedef struct {
    StrBuf out; /* stdout and stderr */
    int code;   /* exit status; 128+N for signal N */
    bool timeout;
    uint64_t ns;
} RunOut;

typedef struct Fuzz Fuzz;
typedef struct {
    Fuzz *f;
    int id;
    int64_t pid;        /* the child running, or 0 (under f->mu) */
    uint64_t deadline;  /* when the watchdog kills it */
    bool killed;
    char *file;         /* this worker's program */
} Worker;

struct Fuzz {
    const char *vm, *out;
    int size, jobs;
    uint64_t seed0, timeout_ns, stop_ns;
    int64_t count; /* programs to try; 0: until stop_ns */
    int modes[NMODES], nmodes;
    bool verbose;
    PlatMutex *mu;
    int64_t next, done, failed, skipped, runs;
    bool stop_watch;
    Worker *w;
};

static void run_mode(Worker *w, int mode, const char *file, RunOut *r) {
    Fuzz *f = w->f;
    char *argv[16], args[64];
    int n = 0;
    argv[n++] = (char *)f->vm;
    argv[n++] = "run";
    argv[n++] = "--no-cache";
    argv[n++] = "--threads";
    argv[n++] = "1";
    snprintf(args, sizeof args, "%s", MODES[mode].args);
    for (char *t = args; *t && n < 14;) { /* (not strtok: the workers run at once) */
        argv[n++] = t;
        while (*t && *t != ' ') t++;
        if (*t) *t++ = 0;
    }
    argv[n++] = (char *)file;
    argv[n] = NULL;
    memset(r, 0, sizeof *r);
    uint64_t t0 = plat_now_ns();
    PlatChild c;
    int err;
    if (!plat_spawn(argv, PLAT_PIPE_OUT | PLAT_MERGE_ERR, &c, &err)) {
        sb_printf(&r->out, "mlwsvm fuzz: cannot run %s: %s\n", f->vm, plat_err_text(err));
        r->code = 127;
        return;
    }
    plat_mutex_lock(f->mu);
    w->pid = c.pid;
    w->deadline = t0 + f->timeout_ns;
    w->killed = false;
    plat_mutex_unlock(f->mu);
    char buf[8192];
    long k;
    while ((k = plat_fd_read(c.out_fd, buf, sizeof buf, &err)) > 0)
        if (r->out.n < (4u << 20)) sb_putn(&r->out, buf, (size_t)k);
    plat_fd_close(c.out_fd);
    plat_mutex_lock(f->mu);
    w->pid = 0;
    r->timeout = w->killed;
    plat_mutex_unlock(f->mu);
    if (!plat_wait_child(c.pid, &r->code, &err)) r->code = 255;
    r->ns = plat_now_ns() - t0;
    if (!r->out.s) sb_puts(&r->out, "");
}

static void watchdog(void *arg) {
    Fuzz *f = arg;
    for (;;) {
        plat_mutex_lock(f->mu);
        bool stop = f->stop_watch;
        uint64_t now = plat_now_ns();
        for (int i = 0; i < f->jobs && !stop; i++) {
            Worker *w = &f->w[i];
            if (w->pid && !w->killed && now > w->deadline) {
                plat_signal_child(w->pid, 9);
                w->killed = true;
            }
        }
        plat_mutex_unlock(f->mu);
        if (stop) return;
        plat_sleep_ns(20 * 1000000ull);
    }
}

/* How a mode's run went against the interpreter's: NULL if the same. */
static const char *verdict(const RunOut *base, const RunOut *r, char *buf, size_t n) {
    if (r->timeout) return "hangs";
    if (r->code >= 128 && r->code < 255) {
        snprintf(buf, n, "crashes (signal %d)", r->code - 128);
        return buf;
    }
    if (r->code != base->code) {
        snprintf(buf, n, "exits with %d, not %d", r->code, base->code);
        return buf;
    }
    if (r->out.n != base->out.n || memcmp(r->out.s, base->out.s, r->out.n)) return "prints something else";
    return NULL;
}

/* The first line where two outputs differ, for the report. */
static void first_diff(const StrBuf *a, const StrBuf *b, StrBuf *o) {
    size_t i = 0, line = 1, start = 0;
    while (i < a->n && i < b->n && a->s[i] == b->s[i])
        if (a->s[i++] == '\n') line++, start = i;
    size_t ea = start, eb = start;
    while (ea < a->n && a->s[ea] != '\n' && ea - start < 300) ea++;
    while (eb < b->n && b->s[eb] != '\n' && eb - start < 300) eb++;
    sb_printf(o, ";   line %zu, interp: %.*s\n;   line %zu, here:   %.*s\n", line, (int)(ea - start), a->s + start, line,
              (int)(eb - start), b->s + start);
}

static void save_failure(Fuzz *f, uint64_t seed, const StrBuf *prog, RunOut *outs, const char *const *why) {
    StrBuf path = {0}, s = {0};
    sb_printf(&path, "%s/fuzz-%llu.masm", f->out, (unsigned long long)seed);
    sb_printf(&s, "; mlwsvm fuzz found this program (seed %llu, size %d). Against the interpreter:\n",
              (unsigned long long)seed, f->size);
    StrBuf modes = {0};
    for (int i = 1; i < f->nmodes; i++) {
        int m = f->modes[i];
        if (!why[i]) continue;
        sb_printf(&s, ";   %-10s %s   (mlwsvm run --no-cache %s)\n", MODES[m].name, why[i], MODES[m].args);
        if (!strcmp(why[i], "prints something else")) first_diff(&outs[0].out, &outs[i].out, &s);
        sb_printf(&modes, "%s%s", modes.n ? "," : "", MODES[m].name);
        StrBuf op = {0};
        sb_printf(&op, "%s/fuzz-%llu.%s.out", f->out, (unsigned long long)seed, MODES[m].name);
        write_file(op.s, outs[i].out.s, outs[i].out.n);
        sb_free(&op);
    }
    sb_printf(&s, "; Shrink it with: mlwsvm fuzz --reduce %s --modes %s\n", path.s, modes.s);
    StrBuf op = {0};
    sb_printf(&op, "%s/fuzz-%llu.interp.out", f->out, (unsigned long long)seed);
    write_file(op.s, outs[0].out.s, outs[0].out.n);
    sb_free(&op);
    sb_putn(&s, prog->s, prog->n);
    write_file(path.s, s.s, s.n);
    plat_mutex_lock(f->mu);
    fprintf(stderr, "seed %llu: %s -> %s\n", (unsigned long long)seed, modes.s, path.s);
    plat_mutex_unlock(f->mu);
    sb_free(&path);
    sb_free(&s);
    sb_free(&modes);
}

static void worker(void *arg) {
    Worker *w = arg;
    Fuzz *f = w->f;
    RunOut outs[NMODES];
    const char *why[NMODES];
    char bufs[NMODES][64];
    for (;;) {
        plat_mutex_lock(f->mu);
        bool more = f->count ? f->next < f->count : plat_now_ns() < f->stop_ns;
        int64_t i = f->next++;
        plat_mutex_unlock(f->mu);
        if (!more) return;
        uint64_t seed = f->seed0 + (uint64_t)i;
        StrBuf prog = {0};
        gen_program(seed, f->size, &prog);
        write_file(w->file, prog.s, prog.n);
        int bad = 0;
        bool skip = false;
        for (int k = 0; k < f->nmodes; k++) {
            run_mode(w, f->modes[k], w->file, &outs[k]);
            why[k] = NULL;
            if (k == 0) {
                if (outs[0].timeout) skip = true; /* too slow to be worth comparing */
                else if (outs[0].code) why[0] = "fails in the interpreter", bad++;
                if (skip || bad) break;
            } else if ((why[k] = verdict(&outs[0], &outs[k], bufs[k], sizeof bufs[k]))) bad++;
        }
        int ran = 0;
        for (int k = 0; k < f->nmodes; k++) {
            if (k && (skip || why[0])) break;
            ran++;
        }
        if (why[0]) { /* a program the generator should not have written */
            plat_mutex_lock(f->mu);
            fprintf(stderr, "seed %llu: the interpreter exits with %d:\n%.*s\n", (unsigned long long)seed, outs[0].code,
                    (int)(outs[0].out.n > 2000 ? 2000 : outs[0].out.n), outs[0].out.s);
            plat_mutex_unlock(f->mu);
            StrBuf path = {0};
            sb_printf(&path, "%s/fuzz-%llu.masm", f->out, (unsigned long long)seed);
            write_file(path.s, prog.s, prog.n);
            sb_free(&path);
        } else if (bad) save_failure(f, seed, &prog, outs, why);
        if (f->verbose) {
            int slow = 0;
            for (int k = 1; k < ran; k++)
                if (outs[k].ns > outs[slow].ns) slow = k;
            plat_mutex_lock(f->mu);
            fprintf(stderr, "seed %llu: %s, %.1f ms in the interpreter, %.1f ms in %s\n", (unsigned long long)seed,
                    skip ? "skipped (too slow)" : bad ? "FAILED" : "ok", (double)outs[0].ns / 1e6,
                    (double)outs[slow].ns / 1e6, MODES[f->modes[slow]].name);
            plat_mutex_unlock(f->mu);
        }
        for (int k = 0; k < ran; k++) sb_free(&outs[k].out);
        sb_free(&prog);
        plat_mutex_lock(f->mu);
        f->done++;
        f->runs += ran;
        if (skip) f->skipped++;
        if (bad) f->failed++;
        plat_mutex_unlock(f->mu);
    }
}

/* ---- shrinking a failing program -------------------------------------------- */

typedef struct {
    Worker *w;
    int modes[NMODES], nmodes; /* the interpreter, then the modes that failed */
    const char *kinds[NMODES];  /* how each failed: a run must fail the same way */
    int base_code;
    int64_t tests;
} Reducer;

static const char *kind_of(const char *why) {
    if (!why) return NULL;
    if (!strcmp(why, "hangs")) return "hangs";
    if (!strncmp(why, "crashes", 7)) return "crashes";
    return "differs";
}

/* Does this candidate still fail the way the original did, in some mode? */
static bool still_fails(Reducer *rd, const StrBuf *prog) {
    rd->tests++;
    write_file(rd->w->file, prog->s, prog->n);
    RunOut base, r;
    run_mode(rd->w, rd->modes[0], rd->w->file, &base);
    bool ok = !base.timeout && base.code == rd->base_code, fails = false;
    for (int k = 1; ok && k < rd->nmodes && !fails; k++) {
        char buf[64];
        run_mode(rd->w, rd->modes[k], rd->w->file, &r);
        const char *kind = kind_of(verdict(&base, &r, buf, sizeof buf));
        fails = kind && !strcmp(kind, rd->kinds[k]);
        sb_free(&r.out);
    }
    sb_free(&base.out);
    return fails;
}

static void join_lines(char **lines, const bool *keep, size_t n, StrBuf *o) {
    o->n = 0;
    for (size_t i = 0; i < n; i++)
        if (keep[i]) sb_printf(o, "%s\n", lines[i]);
    if (!o->s) sb_puts(o, "");
}

/* The state of a reduction: which lines are kept so far. */
typedef struct {
    Reducer *rd;
    char **lines;
    size_t n;
    bool *keep, *trial;
    size_t kept;
    const char *out; /* the best so far is written here */
    StrBuf *prog;
} Shrink;

/* Try the program without the next `chunk` kept lines from i; *end is where
 * they end (i if there are none). */
static bool try_remove(Shrink *sh, size_t i, size_t chunk, size_t *end) {
    size_t j = i, got = 0;
    while (j < sh->n && got < chunk) got += sh->keep[j++];
    *end = got ? j : i;
    if (got < chunk && chunk > 1) return false; /* (a shorter run is tried with a smaller chunk) */
    if (!got) return false;
    memcpy(sh->trial, sh->keep, sh->n * sizeof *sh->keep);
    for (size_t k = i; k < j; k++) sh->trial[k] = false;
    join_lines(sh->lines, sh->trial, sh->n, sh->prog);
    if (!still_fails(sh->rd, sh->prog)) return false;
    memcpy(sh->keep, sh->trial, sh->n * sizeof *sh->keep);
    sh->kept -= got;
    write_file(sh->out, sh->prog->s, sh->prog->n);
    fprintf(stderr, "\r%zu lines (%lld tests)   ", sh->kept, (long long)sh->rd->tests);
    return true;
}

static int reduce(Fuzz *f, const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        fprintf(stderr, "mlwsvm fuzz: cannot read %s\n", path);
        return 2;
    }
    VEC(char *) lines = {0};
    for (char *s = text, *e; *s; s = e + 1) {
        e = strchr(s, '\n');
        if (!e) e = s + strlen(s);
        bool last = !*e;
        *e = 0;
        if (s[0] != ';') VEC_PUSH(lines, s); /* comments go */
        if (last) break;
    }
    Worker w = {f, 0, 0, 0, false, NULL};
    StrBuf wf = {0};
    sb_printf(&wf, "%s/reduce.masm", f->out);
    w.file = wf.s;
    f->w = &w;
    f->jobs = 1;
    PlatThread *wd = plat_thread_start(watchdog, f);
    Reducer rd = {&w, {0}, 0, {0}, 0, 0};
    bool *keep = xmalloc(lines.n * sizeof *keep), *trial = xmalloc(lines.n * sizeof *trial);
    for (size_t i = 0; i < lines.n; i++) keep[i] = true;
    StrBuf prog = {0};
    join_lines(lines.v, keep, lines.n, &prog);
    /* how the original fails */
    write_file(w.file, prog.s, prog.n);
    RunOut base, r;
    run_mode(&w, 0, w.file, &base);
    rd.base_code = base.code;
    rd.modes[rd.nmodes++] = 0;
    uint64_t slowest = base.ns;
    for (int k = 1; k < f->nmodes; k++) {
        char buf[64];
        run_mode(&w, f->modes[k], w.file, &r);
        const char *kind = kind_of(verdict(&base, &r, buf, sizeof buf));
        if (kind) {
            rd.kinds[rd.nmodes] = kind;
            rd.modes[rd.nmodes++] = f->modes[k];
            fprintf(stderr, "%s: %s %s\n", path, MODES[f->modes[k]].name, kind);
            if (!r.timeout && r.ns > slowest) slowest = r.ns;
        }
        sb_free(&r.out);
    }
    sb_free(&base.out);
    int rc = 0;
    if (rd.nmodes == 1) {
        fprintf(stderr, "%s: no mode fails\n", path);
        rc = 1;
        goto done;
    }
    /* candidates that loop forever are cut off at a few times the original's
     * time (of the runs that ended: a hang is cut off sooner, then) */
    f->timeout_ns = slowest * 4 + 1000000000ull;
    StrBuf out = {0};
    sb_printf(&out, "%.*s.reduced.masm", (int)(strlen(path) > 5 && !strcmp(path + strlen(path) - 5, ".masm") ? strlen(path) - 5 : strlen(path)), path);
    Shrink sh = {&rd, lines.v, lines.n, keep, trial, lines.n, out.s, &prog};
    /* halves, quarters, ... of what is left, then every run of up to 4 lines
     * (a .struct and its .ends go together, or not at all) */
    for (size_t chunk = lines.n / 2 ? lines.n / 2 : 1;;) {
        bool progress = false;
        for (size_t i = 0; i < lines.n;) {
            size_t j;
            progress |= try_remove(&sh, i, chunk, &j);
            if (j == i) break;
            i = j;
        }
        if (!progress) {
            if (chunk == 1) break;
            chunk /= 2;
        } else if (chunk > sh.kept / 2 && chunk > 1) chunk = sh.kept / 2;
    }
    for (bool again = true; again;) {
        again = false;
        for (size_t chunk = 4; chunk >= 1; chunk--)
            for (size_t i = 0, j; i < lines.n; i++)
                if (keep[i]) again |= try_remove(&sh, i, chunk, &j);
    }
    size_t kept = sh.kept;
    join_lines(lines.v, keep, lines.n, &prog);
    write_file(out.s, prog.s, prog.n);
    fprintf(stderr, "\r%s: %zu lines, from %zu (%lld tests): %s\n", path, kept, lines.n, (long long)rd.tests, out.s);
    sb_free(&out);
done:
    plat_mutex_lock(f->mu);
    f->stop_watch = true;
    plat_mutex_unlock(f->mu);
    plat_thread_join(wd);
    sb_free(&prog);
    free(keep);
    free(trial);
    VEC_FREE(lines);
    free(text);
    sb_free(&wf);
    return rc;
}

/* ---- the command ------------------------------------------------------------ */

static int usage_fuzz(void) {
    fprintf(stderr,
            "usage: mlwsvm fuzz [-n N | --time SECONDS] [--seed S] [--size N] [-j N] [--modes LIST] [--vm PATH]\n"
            "                   [--out DIR] [--timeout SECONDS] [-v]\n"
            "       mlwsvm fuzz --emit SEED [--size N]     print the program for a seed\n"
            "       mlwsvm fuzz --reduce FILE [--modes LIST] [--vm PATH]   shrink a failing program\n"
            "       mlwsvm fuzz --list                     the modes\n");
    return 2;
}

static bool parse_modes(Fuzz *f, const char *list) {
    f->nmodes = 0;
    f->modes[f->nmodes++] = 0;
    char *s = xstrdup(list);
    for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
        int m = -1;
        for (int k = 0; k < NMODES; k++)
            if (!strcmp(t, MODES[k].name)) m = k;
        if (m < 0) {
            fprintf(stderr, "mlwsvm fuzz: unknown mode '%s' (mlwsvm fuzz --list)\n", t);
            free(s);
            return false;
        }
        bool have = false;
        for (int k = 0; k < f->nmodes; k++) have |= f->modes[k] == m;
        if (!have) f->modes[f->nmodes++] = m;
    }
    free(s);
    return true;
}

/* The modes set every option themselves: no MLWSVM_ variable may change them. */
static void clear_env(void) {
    StrVec names = {0};
    for (char *const *e = plat_environ(); *e; e++)
        if (!strncmp(*e, "MLWSVM_", 7) && strchr(*e, '=')) VEC_PUSH(names, xstrndup(*e, (size_t)(strchr(*e, '=') - *e)));
    for (size_t i = 0; i < names.n; i++) {
        plat_unsetenv(names.v[i]);
        free(names.v[i]);
    }
    VEC_FREE(names);
}

int tool_fuzz(int argc, char **argv) {
    Fuzz f = {0};
    f.size = 40;
    f.timeout_ns = 60 * 1000000000ull;
    f.seed0 = (uint64_t)plat_wall_ns() / 1000 % 1000000000;
    f.jobs = plat_cpu_count();
    f.out = "fuzz-out";
    const char *emit_seed = NULL, *reduce_file = NULL, *vm = NULL;
    double seconds = 0;
    bool counted = false;
    parse_modes(&f, "jit,hot,first,slice,opt,jit-opt,stress,jit-stress,verify,jit-verify");
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--list")) {
            for (int k = 0; k < NMODES; k++) printf("%-11s %-24s %s\n", MODES[k].name, MODES[k].args, MODES[k].what);
            return 0;
        } else if (!strcmp(a, "-v")) f.verbose = true;
        else if (!v) return usage_fuzz();
        else if (!strcmp(a, "-n")) f.count = strtoll(v, NULL, 10), counted = true, i++;
        else if (!strcmp(a, "--time")) seconds = strtod(v, NULL), i++;
        else if (!strcmp(a, "--seed")) f.seed0 = strtoull(v, NULL, 10), i++;
        else if (!strcmp(a, "--size")) f.size = atoi(v), i++;
        else if (!strcmp(a, "-j")) f.jobs = atoi(v), i++;
        else if (!strcmp(a, "--vm")) vm = v, i++;
        else if (!strcmp(a, "--out")) f.out = v, i++;
        else if (!strcmp(a, "--timeout")) f.timeout_ns = (uint64_t)(strtod(v, NULL) * 1e9), i++;
        else if (!strcmp(a, "--emit")) emit_seed = v, i++;
        else if (!strcmp(a, "--reduce")) reduce_file = v, i++;
        else if (!strcmp(a, "--modes")) {
            if (!parse_modes(&f, v)) return 2;
            i++;
        } else return usage_fuzz();
    }
    if (f.size < 1 || f.jobs < 1 || f.count < 0) return usage_fuzz();
    if (emit_seed) {
        StrBuf prog = {0};
        gen_program(strtoull(emit_seed, NULL, 10), f.size, &prog);
        fwrite(prog.s, 1, prog.n, stdout);
        sb_free(&prog);
        return 0;
    }
    char *self = vm ? plat_realpath(vm) : plat_exe_path(NULL);
    if (!self) {
        fprintf(stderr, "mlwsvm fuzz: cannot find %s\n", vm ? vm : "the mlwsvm executable");
        return 2;
    }
    f.vm = self;
    if (!plat_mkdir(f.out)) {
        fprintf(stderr, "mlwsvm fuzz: cannot make %s\n", f.out);
        free(self);
        return 2;
    }
    clear_env();
    f.mu = plat_mutex_new();
    int rc;
    if (reduce_file) rc = reduce(&f, reduce_file);
    else {
        if (!counted && seconds <= 0) f.count = 100;
        if (seconds > 0 && !counted) f.stop_ns = plat_now_ns() + (uint64_t)(seconds * 1e9);
        if (f.count && f.jobs > f.count) f.jobs = (int)f.count;
        StrBuf names = {0};
        for (int k = 1; k < f.nmodes; k++) sb_printf(&names, "%s%s", k > 1 ? "," : "", MODES[f.modes[k]].name);
        fprintf(stderr, "mlwsvm fuzz: seeds from %llu, size %d, %d jobs; against the interpreter: %s\n",
                (unsigned long long)f.seed0, f.size, f.jobs, names.s ? names.s : "nothing");
        sb_free(&names);
        uint64_t t0 = plat_now_ns();
        f.w = xcalloc((size_t)f.jobs, sizeof *f.w);
        PlatThread **th = xcalloc((size_t)f.jobs, sizeof *th);
        for (int k = 0; k < f.jobs; k++) {
            StrBuf p = {0};
            sb_printf(&p, "%s/work-%d.masm", f.out, k);
            f.w[k] = (Worker){&f, k, 0, 0, false, p.s};
        }
        PlatThread *wd = plat_thread_start(watchdog, &f);
        for (int k = 0; k < f.jobs; k++) th[k] = plat_thread_start(worker, &f.w[k]);
        for (int k = 0; k < f.jobs; k++) plat_thread_join(th[k]);
        plat_mutex_lock(f.mu);
        f.stop_watch = true;
        plat_mutex_unlock(f.mu);
        plat_thread_join(wd);
        for (int k = 0; k < f.jobs; k++) {
            int err;
            plat_remove(f.w[k].file, &err);
            free(f.w[k].file);
        }
        double s = (double)(plat_now_ns() - t0) / 1e9;
        fprintf(stderr, "mlwsvm fuzz: %lld programs (seeds %llu-%llu), %lld runs, %lld skipped, %lld failed, %.1f s\n",
                (long long)f.done, (unsigned long long)f.seed0, (unsigned long long)(f.seed0 + (uint64_t)f.done - 1),
                (long long)f.runs, (long long)f.skipped, (long long)f.failed, s);
        if (f.failed) fprintf(stderr, "mlwsvm fuzz: the failing programs are in %s/\n", f.out);
        rc = f.failed ? 1 : 0;
        free(th);
        free(f.w);
    }
    plat_mutex_free(f.mu);
    free(self);
    return rc;
}
