/* jit_a64.inc - the JIT's arm64 encoder (jit.inc): its machine registers,
 * and the operations jit.inc writes the forms in (jm_*), as arm64 code.
 * jit_x64.inc is the same for x86-64. */

/* Kept across C calls (callee-saved; jit_enter saves them): the Ctx, the I
 * window (the F and R windows follow it, at the function's roff), the slice's
 * instructions left (32 bits), the current Frame, the Process, the code
 * memory's start, and the encoder's own temporary. */
enum { JR_CTX = 19, JR_WIN = 20, JR_LEFT = 23, JR_FR = 24, JR_PROC = 25, JR_CODE = 26, JR_TMP = 27 };
/* Scratch, for a form's own use; C's argument and result registers. */
enum { JR_S0 = 9, JR_S1 = 10, JR_S2 = 11, JR_S3 = 16, JR_S4 = 17 };
enum { JR_A0 = 0, JR_A1 = 1, JR_A2 = 2, JR_R0 = 0, JR_R1 = 1 };
enum { JF_S0 = 0, JF_S1 = 1 };
#define JIT_ARCH "arm64"
/* The register cache's machine registers (none of the above). */
#define JC_NI 16
#define JC_NF 16
static const uint8_t jc_ireg[JC_NI] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14, 15, 21, 22, 28};
static const uint8_t jc_freg[JC_NF] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
#define XZR 31

static void jw(JBuf *b, uint32_t w) {
    jb_need(b, 4);
    memcpy(b->v + b->n, &w, 4);
    b->n += 4;
}
static uint32_t jr32(const uint8_t *p) {
    uint32_t w;
    memcpy(&w, p, 4);
    return w;
}
static void jp32(uint8_t *p, uint32_t w) { memcpy(p, &w, 4); }

/* ---- encodings ---- */
static uint32_t a_rrr(uint32_t op, int d, int n, int m) { return op | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)d; }
#define A_ADD 0x8B000000u
#define A_SUB 0xCB000000u
#define A_AND 0x8A000000u
#define A_ORR 0xAA000000u
#define A_EOR 0xCA000000u
#define A_MUL 0x9B007C00u
#define A_SDIV 0x9AC00C00u
#define A_LSLV 0x9AC02000u
#define A_LSRV 0x9AC02400u
#define A_ASRV 0x9AC02800u
#define A_FADD 0x1E602800u
#define A_FSUB 0x1E603800u
#define A_FMUL 0x1E600800u
#define A_FDIV 0x1E601800u
#define A_FSQRT 0x1E61C000u
#define A_FNEG 0x1E614000u
#define A_FABS 0x1E60C000u
#define A_RET 0xD65F03C0u
#define A_BRK 0xD4200000u
static uint32_t a_addi(int d, int n, unsigned imm) { return 0x91000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_subi(int d, int n, unsigned imm) { return 0xD1000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_mov_x(int d, int m) { return 0xAA0003E0u | (unsigned)m << 16 | (unsigned)d; }
static uint32_t a_mov_w(int d, int m) { return 0x2A0003E0u | (unsigned)m << 16 | (unsigned)d; }
static uint32_t a_stp_pre(int t, int t2, int n, int off) {
    return 0xA9800000u | ((unsigned)(off / 8) & 0x7F) << 15 | (unsigned)t2 << 10 | (unsigned)n << 5 | (unsigned)t;
}
static uint32_t a_stp(int t, int t2, int n, int off) {
    return 0xA9000000u | ((unsigned)(off / 8) & 0x7F) << 15 | (unsigned)t2 << 10 | (unsigned)n << 5 | (unsigned)t;
}
static uint32_t a_ldp(int t, int t2, int n, int off) {
    return 0xA9400000u | ((unsigned)(off / 8) & 0x7F) << 15 | (unsigned)t2 << 10 | (unsigned)n << 5 | (unsigned)t;
}
static uint32_t a_ldp_post(int t, int t2, int n, int off) {
    return 0xA8C00000u | ((unsigned)(off / 8) & 0x7F) << 15 | (unsigned)t2 << 10 | (unsigned)n << 5 | (unsigned)t;
}

/* x = v: movz (or movn, for mostly-ones values) and a movk per other chunk */
static void a_movk64(JBuf *b, int d, uint64_t v) {
    int zeros = 0, ones = 0;
    for (unsigned hw = 0; hw < 4; hw++) {
        uint32_t ch = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        zeros += ch == 0, ones += ch == 0xFFFF;
    }
    bool neg = ones > zeros;
    uint32_t c0 = (uint32_t)v & 0xFFFF;
    jw(b, (neg ? 0x92800000u | (~c0 & 0xFFFF) << 5 : 0xD2800000u | c0 << 5) | (unsigned)d);
    for (unsigned hw = 1; hw < 4; hw++) {
        uint32_t ch = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        if (ch != (neg ? 0xFFFFu : 0)) jw(b, 0xF2800000u | hw << 21 | ch << 5 | (unsigned)d);
    }
}

/* A load or store at [n + off]: the scaled 12-bit form if it fits, else the
 * offset in the temporary and the register form. */
static void a_mem(JBuf *b, uint32_t uop, uint32_t rop, unsigned sz, int t, int n, int64_t off) {
    if (off >= 0 && off % sz == 0 && off / sz < 4096) {
        jw(b, uop | (uint32_t)(off / sz) << 10 | (unsigned)n << 5 | (unsigned)t);
    } else {
        a_movk64(b, JR_TMP, (uint64_t)off);
        jw(b, rop | (unsigned)JR_TMP << 16 | (unsigned)n << 5 | (unsigned)t);
    }
}

/* ---- the operations ---- */
static void jm_ld(JBuf *b, int d, int n, int64_t off) { a_mem(b, 0xF9400000u, 0xF8606800u, 8, d, n, off); }
static void jm_st(JBuf *b, int s, int n, int64_t off) { a_mem(b, 0xF9000000u, 0xF8206800u, 8, s, n, off); }
static void jm_stz(JBuf *b, int n, int64_t off) { jm_st(b, XZR, n, off); }
static void jm_ld32(JBuf *b, int d, int n, int64_t off) { a_mem(b, 0xB9400000u, 0xB8606800u, 4, d, n, off); }
static void jm_st32(JBuf *b, int s, int n, int64_t off) { a_mem(b, 0xB9000000u, 0xB8206800u, 4, s, n, off); }
static void jm_ld8(JBuf *b, int d, int n, int64_t off) { a_mem(b, 0x39400000u, 0x38606800u, 1, d, n, off); }
static void jm_st8(JBuf *b, int s, int n, int64_t off) { a_mem(b, 0x39000000u, 0x38206800u, 1, s, n, off); }
static void jm_fld(JBuf *b, int d, int n, int64_t off) { a_mem(b, 0xFD400000u, 0xFC606800u, 8, d, n, off); }
static void jm_fst(JBuf *b, int s, int n, int64_t off) { a_mem(b, 0xFD000000u, 0xFC206800u, 8, s, n, off); }
/* [n + m*8], and bytes at [n + m] */
static void jm_ldx(JBuf *b, int d, int n, int m) { jw(b, a_rrr(0xF8607800u, d, n, m)); }
static void jm_stx(JBuf *b, int s, int n, int m) { jw(b, a_rrr(0xF8207800u, s, n, m)); }
static void jm_fldx(JBuf *b, int d, int n, int m) { jw(b, a_rrr(0xFC607800u, d, n, m)); }
static void jm_fstx(JBuf *b, int s, int n, int m) { jw(b, a_rrr(0xFC207800u, s, n, m)); }
static void jm_ld8x(JBuf *b, int d, int n, int m) { jw(b, a_rrr(0x38606800u, d, n, m)); }
static void jm_st8x(JBuf *b, int s, int n, int m) { jw(b, a_rrr(0x38206800u, s, n, m)); }

static void jm_movi(JBuf *b, int d, uint64_t v) { a_movk64(b, d, v); }
static void jm_mov(JBuf *b, int d, int s) {
    if (d != s) jw(b, a_mov_x(d, s));
}
static void jm_mov32(JBuf *b, int d, int s) { jw(b, a_mov_w(d, s)); }

enum { JA_ADD, JA_SUB, JA_AND, JA_OR, JA_XOR, JA_MUL, JA_SHL, JA_SHR, JA_SAR };
static void jm_alu(JBuf *b, int op, int d, int a, int c) {
    static const uint32_t ops[] = {A_ADD, A_SUB, A_AND, A_ORR, A_EOR, A_MUL, A_LSLV, A_LSRV, A_ASRV};
    jw(b, a_rrr(ops[op], d, a, c)); /* (a shift's count modulo 64, as shiftop) */
}
/* d = a + v or a - v, for any v */
static void jm_alui(JBuf *b, int op, int d, int a, int64_t v) {
    if (op == JA_SUB) op = JA_ADD, v = (int64_t)(0 - (uint64_t)v);
    if (op == JA_ADD && v >= 0 && v < 4096) jw(b, a_addi(d, a, (unsigned)v));
    else if (op == JA_ADD && v < 0 && v > -4096) jw(b, a_subi(d, a, (unsigned)-v));
    else a_movk64(b, JR_TMP, (uint64_t)v), jm_alu(b, op, d, a, JR_TMP);
}
/* 32-bit: d = a + v (the upper half zero), d = a - c */
static void jm_add32i(JBuf *b, int d, int a, uint32_t v) {
    if (v < 4096) jw(b, 0x11000000u | v << 10 | (unsigned)a << 5 | (unsigned)d);
    else a_movk64(b, JR_TMP, v), jw(b, a_rrr(0x0B000000u, d, a, JR_TMP));
}
static void jm_sub32i(JBuf *b, int d, int a, uint32_t v) {
    if (v < 4096) jw(b, 0x51000000u | v << 10 | (unsigned)a << 5 | (unsigned)d);
    else a_movk64(b, JR_TMP, v), jw(b, a_rrr(0x4B000000u, d, a, JR_TMP));
}
/* the same, setting the flags (a block's header: below zero is JC_LO) */
static void jm_subs32i(JBuf *b, int d, int a, uint32_t v) {
    if (v < 4096) jw(b, 0x71000000u | v << 10 | (unsigned)a << 5 | (unsigned)d);
    else a_movk64(b, JR_TMP, v), jw(b, a_rrr(0x6B000000u, d, a, JR_TMP));
}
static void jm_sub32(JBuf *b, int d, int a, int c) { jw(b, a_rrr(0x4B000000u, d, a, c)); }
/* d = n + m*8 */
static void jm_addsh3(JBuf *b, int d, int n, int m) { jw(b, a_rrr(0x8B000C00u, d, n, m)); }
/* d = a / k or a % k (k a constant, neither 0 nor -1: the quickener's DIV_IK and REM_IK) */
static void jm_divk(JBuf *b, int d, int a, int64_t k, bool rem) {
    a_movk64(b, JR_S1, (uint64_t)k);
    jw(b, a_rrr(A_SDIV, JR_S2, a, JR_S1));
    if (rem) jw(b, 0x9B008000u | (unsigned)JR_S1 << 16 | (unsigned)a << 10 | (unsigned)JR_S2 << 5 | (unsigned)d);
    else jm_mov(b, d, JR_S2);
}

/* Conditions, after jm_cmp: signed, unsigned and equality. */
enum { JC_EQ, JC_NE, JC_LT, JC_LE, JC_GT, JC_GE, JC_LO, JC_LS, JC_HI, JC_HS, JC_AL };
static const uint8_t a_cc[] = {0, 1, 11, 13, 12, 10, 3, 9, 8, 2, 14};
/* Float comparisons (C's: false if either is a NaN, except FNE). */
enum { JC_FEQ, JC_FNE, JC_FLT, JC_FLE, JC_FGT, JC_FGE };
static const uint8_t a_fcc[] = {0, 1, 4, 9, 12, 10};

static void jm_cmp(JBuf *b, int a, int c) { jw(b, 0xEB00001Fu | (unsigned)c << 16 | (unsigned)a << 5); }
static void jm_cmpi(JBuf *b, int a, int64_t v) {
    if (v >= 0 && v < 4096) jw(b, 0xF100001Fu | (uint32_t)v << 10 | (unsigned)a << 5);
    else if (v < 0 && v > -4096) jw(b, 0xB100001Fu | (uint32_t)-v << 10 | (unsigned)a << 5);
    else a_movk64(b, JR_TMP, (uint64_t)v), jm_cmp(b, a, JR_TMP);
}
static void jm_cmp32(JBuf *b, int a, int c) { jw(b, 0x6B00001Fu | (unsigned)c << 16 | (unsigned)a << 5); }
static void jm_cmpi32(JBuf *b, int a, uint32_t v) {
    if (v < 4096) jw(b, 0x7100001Fu | v << 10 | (unsigned)a << 5);
    else a_movk64(b, JR_TMP, v), jm_cmp32(b, a, JR_TMP);
}
static void jm_cset(JBuf *b, int d, int cond) { jw(b, 0x9A9F07E0u | (unsigned)(a_cc[cond] ^ 1) << 12 | (unsigned)d); }
/* d = cond ? a : 0 */
static void jm_cselz(JBuf *b, int d, int a, int cond) {
    jw(b, 0x9A800000u | (unsigned)XZR << 16 | (unsigned)a_cc[cond] << 12 | (unsigned)a << 5 | (unsigned)d);
}

enum { JF_ADD, JF_SUB, JF_MUL, JF_DIV };
static void jm_fop(JBuf *b, int op, int d, int a, int c) {
    static const uint32_t ops[] = {A_FADD, A_FSUB, A_FMUL, A_FDIV};
    jw(b, a_rrr(ops[op], d, a, c));
}
enum { JF_SQRT, JF_NEG, JF_ABS };
static void jm_fun(JBuf *b, int op, int d, int a) {
    static const uint32_t ops[] = {A_FSQRT, A_FNEG, A_FABS};
    jw(b, ops[op] | (unsigned)a << 5 | (unsigned)d);
}
static void jm_fmov(JBuf *b, int d, int s) {
    if (d != s) jw(b, 0x1E604000u | (unsigned)s << 5 | (unsigned)d);
}
static void jm_fmov_from_x(JBuf *b, int d, int x) { jw(b, 0x9E670000u | (unsigned)x << 5 | (unsigned)d); }
static void jm_fmov_to_x(JBuf *b, int x, int d) { jw(b, 0x9E660000u | (unsigned)d << 5 | (unsigned)x); }
static void jm_scvtf(JBuf *b, int d, int x) { jw(b, 0x9E620000u | (unsigned)x << 5 | (unsigned)d); }
static void jm_fconst(JBuf *b, int d, double f) {
    uint64_t bits;
    memcpy(&bits, &f, 8);
    if (!bits) {
        jm_fmov_from_x(b, d, XZR);
        return;
    }
    a_movk64(b, JR_TMP, bits);
    jm_fmov_from_x(b, d, JR_TMP);
}
static void jm_fcmp(JBuf *b, int a, int c) { jw(b, 0x1E602000u | (unsigned)c << 16 | (unsigned)a << 5); }
static void jm_fcset(JBuf *b, int d, int fcond, int a, int c) {
    jm_fcmp(b, a, c);
    jw(b, 0x9A9F07E0u | (unsigned)(a_fcc[fcond] ^ 1) << 12 | (unsigned)d);
}

static void jm_ccall(JBuf *b, const void *fn) {
    a_movk64(b, JR_S3, (uint64_t)(uintptr_t)fn);
    jw(b, 0xD63F0000u | (unsigned)JR_S3 << 5);
}
static void jm_jmpr(JBuf *b, int r) { jw(b, 0xD61F0000u | (unsigned)r << 5); }
static void jm_brk(JBuf *b) { jw(b, A_BRK); }

/* ---- branches ----
 * A branch to an instruction's code, a stub or the exit (jit.inc's fix-ups),
 * and a branch forward inside one form's code: each leaves its place, patched
 * when the target is known. */
enum { FX_B, FX_BL, FX_BCOND, FX_CB };
static uint32_t jm_emit_branch(JBuf *b, int kind, int cond, int reg, bool nz) {
    uint32_t at = (uint32_t)b->n;
    switch (kind) {
    case FX_B: jw(b, 0x14000000u); break;
    case FX_BL: jw(b, 0x94000000u); break;
    case FX_BCOND: jw(b, 0x54000000u | a_cc[cond]); break;
    default: jw(b, (nz ? 0xB5000000u : 0xB4000000u) | (unsigned)reg); break;
    }
    return at;
}
/* The branch at `at` (in code starting at p) goes to `dest`, both offsets in p. */
static void jm_patch_at(uint8_t *p, uint32_t at, int64_t dest) {
    uint32_t w = jr32(p + at);
    int64_t off = (dest - (int64_t)at) / 4;
    if ((w & 0x7C000000u) == 0x14000000u) w |= (uint32_t)off & 0x3FFFFFF; /* b, bl */
    else w |= ((uint32_t)off & 0x7FFFF) << 5;                         /* b.cond, cbz, cbnz */
    jp32(p + at, w);
}
static uint32_t jm_fwd(JBuf *b, int cond) { return jm_emit_branch(b, cond == JC_AL ? FX_B : FX_BCOND, cond, 0, false); }
static uint32_t jm_fwd_cbz(JBuf *b, int reg, bool nz) { return jm_emit_branch(b, FX_CB, 0, reg, nz); }
static void jm_bind(JBuf *b, uint32_t at) { jm_patch_at(b->v, at, (int64_t)b->n); }
/* A float compare and branch, fixed up by the caller: returns the places. */
static int jm_fbranch(JBuf *b, int fcond, int a, int c, uint32_t at[2]) {
    jm_fcmp(b, a, c);
    at[0] = (uint32_t)b->n;
    jw(b, 0x54000000u | a_fcc[fcond]);
    return 1;
}

/* ---- calls and returns between machine code ----
 * A call is a `bl` or `blr`, and its continuation follows it; a return is a
 * `ret` to the continuation of the frame's call (the code memory's start plus
 * the call instruction's Insn.target), which is where the `bl` went from when
 * it was a call from machine code, so the CPU's return predictor is right. No
 * call grows the machine stack. */
#define JM_CONT_SKIP 0 /* where a return that is not to a `bl` goes in a continuation */
static void jm_ncall_reg(JBuf *b, int r) { jw(b, 0xD63F0000u | (unsigned)r << 5); }
static uint32_t jm_ncall_self(JBuf *b) { return jm_emit_branch(b, FX_BL, 0, 0, false); }
static void jm_cont(JBuf *b) { (void)b; }
/* return to the continuation at the code memory's start + off (a 32-bit register) */
static void jm_nret(JBuf *b, int off, const uint8_t *mem) {
    (void)mem;
    jw(b, a_rrr(A_ADD, 30, JR_CODE, off));
    jw(b, A_RET);
}

/* jtab: idx < n goes to the n targets' fix-ups, which the caller adds at the
 * places returned in at[] (each a `b`); otherwise it falls through. */
static void jm_jtab(JBuf *b, int idx, uint32_t n, uint32_t *at, uint32_t *oob) {
    a_movk64(b, JR_TMP, n);
    jm_cmp(b, idx, JR_TMP);
    *oob = jm_emit_branch(b, FX_BCOND, JC_HS, 0, false);
    jw(b, 0x10000000u | 3u << 5 | JR_S0);                                     /* adr x9, the table (3 on) */
    jw(b, 0x8B000800u | (unsigned)idx << 16 | (unsigned)JR_S0 << 5 | JR_S0); /* add x9, x9, idx, lsl #2 */
    jm_jmpr(b, JR_S0);
    for (uint32_t k = 0; k < n; k++) at[k] = jm_emit_branch(b, FX_B, 0, 0, false);
}

/* ---- the entry and the exit ----
 * jit_enter(c, left, target) saves the registers it uses, loads the state and
 * goes to target; the exit (R0: the instruction to stop at) returns
 * {R0, left}. */
static void jm_tramp(JBuf *b, size_t *exit_at, const uint8_t *mem) {
    jw(b, a_stp_pre(29, 30, 31, -96));
    jw(b, a_addi(29, 31, 0)); /* mov x29, sp */
    jw(b, a_stp(19, 20, 31, 16));
    jw(b, a_stp(21, 22, 31, 32));
    jw(b, a_stp(23, 24, 31, 48));
    jw(b, a_stp(25, 26, 31, 64));
    jw(b, a_stp(27, 28, 31, 80));
    jw(b, a_mov_x(JR_CTX, 0));
    jm_ld(b, JR_WIN, JR_CTX, offsetof(Ctx, I));
    jm_ld(b, JR_FR, JR_CTX, offsetof(Ctx, fr));
    jm_ld(b, JR_PROC, JR_CTX, offsetof(Ctx, p));
    a_movk64(b, JR_CODE, (uint64_t)(uintptr_t)mem);
    jw(b, a_mov_w(JR_LEFT, 1));
    jm_jmpr(b, 2);
    *exit_at = b->n;
    jw(b, a_mov_w(1, JR_LEFT));
    jw(b, a_ldp(27, 28, 31, 80));
    jw(b, a_ldp(25, 26, 31, 64));
    jw(b, a_ldp(23, 24, 31, 48));
    jw(b, a_ldp(21, 22, 31, 32));
    jw(b, a_ldp(19, 20, 31, 16));
    jw(b, a_ldp_post(29, 30, 31, 96));
    jw(b, A_RET);
}
/* The Process, in a register (it has its own here). */
static int jm_proc(JBuf *b, int tmp) {
    (void)b, (void)tmp;
    return JR_PROC;
}
/* After a C helper changed the frame or the windows: the Ctx's. */
static void jm_reload(JBuf *b) {
    jm_ld(b, JR_FR, JR_CTX, offsetof(Ctx, fr));
    jm_ld(b, JR_WIN, JR_CTX, offsetof(Ctx, I));
}
/* A stepped instruction that returned or threw may go on in machine code. */
#define JM_STEP_POP_STOPS 0
