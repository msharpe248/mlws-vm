/* jit.inc - a prototype JIT: arm64 machine code for a function's common
 * quickened forms, run in mixed mode with the interpreter (MLWSVM_JIT=1).
 * Included by interp.c in the tail-call build, before the handler table.
 *
 * Blocks. A function's compiled instructions fall into basic blocks: one
 * starts at the first instruction, at a compiled branch's target, and after a
 * block's end (a branch, call or return) or an instruction the interpreter
 * runs. The first instruction of each block gets the execution opcode Q_JIT
 * (its own quickened form is kept in Function.jit_xop), whose handler enters
 * the machine code there; the rest keep their forms, so the interpreter can
 * run any of them, as it does after a slice ends inside a block. The machine
 * code works on the same register windows as the handlers, and stops at an
 * instruction the interpreter must run, returning it to the interpreter.
 *
 * Registers. Inside a block, I and F registers live in machine registers once
 * read or written (a small cache), and the changed ones are written back to
 * the window at the block's end, so a block's instructions pass values
 * without a store and a load each.
 *
 * The slice. A block's header takes its length from `left` (w23), or, if
 * that would go below zero, stops at the block's first instruction, not yet
 * counted, which the interpreter counts as it dispatches it. So Process.insns
 * and preemption are exactly the interpreter's.
 *
 * Machine registers: x19 the Ctx, x20/x21/x22 the I/F/R windows, w23 the
 * slice's instructions left, x24 the current Frame, x25 the Process, x26 the
 * code memory's start. x9-x11, x16, x17, d0, d1 are scratch; x0-x8, x12-x15
 * and d16-d28 hold cached registers. The Ctx's frame and windows are brought
 * up to date only when the interpreter takes over (h_Q_JIT, ctx_load). */
#if MLWS_TAILCALL && defined(__aarch64__) && defined(__APPLE__)
#define MLWS_JIT 1
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

typedef struct {
    const Insn *in; /* where the interpreter goes on: not yet counted */
    uint64_t left;
} JitExit;

static int jit_mode = -1; /* MLWSVM_JIT: 0 off, 1 on, 2 on with a summary */
static uint8_t *jit_mem, *jit_top, *jit_end;
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;
static JitExit (*jit_enter)(Ctx *c, uint32_t left, void *target);
static uint8_t *jit_exit_code;
static uint64_t jit_nfuncs, jit_ninsns, jit_nnative, jit_bytes;
static uint64_t jit_entries, jit_ran; /* MLWSVM_JIT=2: entries into machine code, instructions run there */
static uint64_t jit_stops[Q__COUNT]; /* MLWSVM_JIT=2: what the machine code stopped at, by form */
static uint16_t jit_stop_op[Q__COUNT];

enum { XC = 19, XI = 20, XF = 21, XR = 22, WL = 23, XFR = 24, XP = 25, XM = 26, T0 = 9, T1 = 10, T2 = 11 };
#define XZR 31

typedef struct {
    uint32_t *v;
    size_t n, cap;
} JBuf;

static void jw(JBuf *b, uint32_t w) {
    if (b->n == b->cap) {
        b->cap = b->cap ? 2 * b->cap : 256;
        b->v = xrealloc(b->v, b->cap * sizeof *b->v);
    }
    b->v[b->n++] = w;
}

/* ---- arm64 encodings ---- */
static uint32_t a_ldr_x(int t, int n, unsigned off) { return 0xF9400000u | (off / 8) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_str_x(int t, int n, unsigned off) { return 0xF9000000u | (off / 8) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_ldr_d(int t, int n, unsigned off) { return 0xFD400000u | (off / 8) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_str_d(int t, int n, unsigned off) { return 0xFD000000u | (off / 8) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_ldr_w(int t, int n, unsigned off) { return 0xB9400000u | (off / 4) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_str_w(int t, int n, unsigned off) { return 0xB9000000u | (off / 4) << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_ldrb(int t, int n, unsigned off) { return 0x39400000u | off << 10 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_strb(int t, int n, unsigned off) { return 0x39000000u | off << 10 | (unsigned)n << 5 | (unsigned)t; }
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
static uint32_t a_rrr(uint32_t op, int d, int n, int m) { return op | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)d; }
#define A_ADD 0x8B000000u
#define A_SUB 0xCB000000u
#define A_AND 0x8A000000u
#define A_ORR 0xAA000000u
#define A_EOR 0xCA000000u
#define A_MUL 0x9B007C00u
#define A_SDIV 0x9AC00C00u
#define A_FADD 0x1E602800u
#define A_FSUB 0x1E603800u
#define A_FMUL 0x1E600800u
#define A_FDIV 0x1E601800u
static uint32_t a_addi(int d, int n, unsigned imm) { return 0x91000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_subi(int d, int n, unsigned imm) { return 0xD1000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_msub(int d, int n, int m, int a) {
    return 0x9B008000u | (unsigned)m << 16 | (unsigned)a << 10 | (unsigned)n << 5 | (unsigned)d;
}
static uint32_t a_cmp(int n, int m) { return 0xEB00001Fu | (unsigned)m << 16 | (unsigned)n << 5; }
static uint32_t a_cmpi(int n, unsigned imm) { return 0xF100001Fu | imm << 10 | (unsigned)n << 5; }
static uint32_t a_cmni(int n, unsigned imm) { return 0xB100001Fu | imm << 10 | (unsigned)n << 5; }
static uint32_t a_fcmp(int n, int m) { return 0x1E602000u | (unsigned)m << 16 | (unsigned)n << 5; }
static uint32_t a_fmov_dx(int d, int n) { return 0x9E670000u | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_fmov_xd(int d, int n) { return 0x9E660000u | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_scvtf(int d, int n) { return 0x9E620000u | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_ldr_xr(int t, int n, int m) { return 0xF8607800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_ldr_dr(int t, int n, int m) { return 0xFC607800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_str_xr(int t, int n, int m) { return 0xF8207800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_str_dr(int t, int n, int m) { return 0xFC207800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_ldrb_r(int t, int n, int m) { return 0x38606800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
static uint32_t a_strb_r(int t, int n, int m) { return 0x38206800u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)t; }
/* a forward branch inside one form's code, patched once its target is known */
static void a_patch(JBuf *b, size_t at) {
    uint32_t off = (uint32_t)(b->n - at);
    if ((b->v[at] & 0xFC000000u) == 0x14000000u) b->v[at] |= off & 0x3FFFFFF;
    else b->v[at] |= (off & 0x7FFFF) << 5;
}
static uint32_t a_cmp_wi(int n, unsigned imm) { return 0x7100001Fu | imm << 10 | (unsigned)n << 5; }
static uint32_t a_fmov_dd(int d, int n) { return 0x1E604000u | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_subs_wi(int d, int n, unsigned imm) { return 0x71000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_add_wi(int d, int n, unsigned imm) { return 0x11000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_sub_wi(int d, int n, unsigned imm) { return 0x51000000u | imm << 10 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_cmp_w(int n, int m) { return 0x6B00001Fu | (unsigned)m << 16 | (unsigned)n << 5; }
static uint32_t a_add_lsl3(int d, int n, int m) { return 0x8B000C00u | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)d; }
static uint32_t a_mov_x(int d, int m) { return 0xAA0003E0u | (unsigned)m << 16 | (unsigned)d; }
static uint32_t a_mov_w(int d, int m) { return 0x2A0003E0u | (unsigned)m << 16 | (unsigned)d; }
static uint32_t a_br(int n) { return 0xD61F0000u | (unsigned)n << 5; }
static uint32_t a_blr(int n) { return 0xD63F0000u | (unsigned)n << 5; }
#define A_RET 0xD65F03C0u
#define A_BRK 0xD4200000u
enum { C_EQ = 0, C_NE = 1, C_HS = 2, C_LO = 3, C_MI = 4, C_HI = 8, C_LS = 9, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13 };

/* x = v, in as few movz/movk as its 16-bit chunks need */
static void a_movk64(JBuf *b, int d, uint64_t v) {
    jw(b, 0xD2800000u | (uint32_t)(v & 0xFFFF) << 5 | (unsigned)d);
    for (unsigned hw = 1; hw < 4; hw++) {
        uint32_t ch = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        if (ch) jw(b, 0xF2800000u | hw << 21 | ch << 5 | (unsigned)d);
    }
}

/* Branch fix-ups: to an instruction's code, a stub, or the exit, by kind. */
enum { FX_B, FX_BL, FX_BCOND, FX_CB };
enum { TO_BODY, TO_UNDO, TO_EXIT, TO_SIDE };
typedef struct {
    uint32_t at;
    uint8_t kind, to;
    uint32_t pc;
} JFix;
typedef struct {
    JFix *v;
    size_t n, cap;
} JFixes;
static void jbranch(JBuf *b, JFixes *f, int kind, uint32_t bits, int to, uint32_t pc) {
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 64;
        f->v = xrealloc(f->v, f->cap * sizeof *f->v);
    }
    f->v[f->n++] = (JFix){(uint32_t)b->n, (uint8_t)kind, (uint8_t)to, pc};
    jw(b, kind == FX_B ? 0x14000000u : kind == FX_BL ? 0x94000000u : kind == FX_BCOND ? 0x54000000u | bits : bits);
}

/* ---- the register cache ----
 * Bank 0 is I (x registers), bank 1 F (d registers). of[b][r]: the slot
 * holding register r, or -1; vm[b][s]: the register in slot s, or -1. */
#define JC_SLOTS 13
static const uint8_t jc_reg[2][JC_SLOTS] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14, 15},
                                            {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28}};
typedef struct {
    int8_t of[2][32], vm[2][JC_SLOTS];
    bool dirty[2][JC_SLOTS];
    uint32_t pin[2]; /* slots this instruction uses */
    uint8_t next[2];
} JCache;

static void jc_reset(JCache *c) {
    memset(c->of, -1, sizeof c->of);
    memset(c->vm, -1, sizeof c->vm);
    memset(c->dirty, 0, sizeof c->dirty);
    c->pin[0] = c->pin[1] = 0;
}

static void jc_store(JBuf *b, int bank, int s, int r) {
    jw(b, bank ? a_str_d(jc_reg[1][s], XF, (unsigned)r * 8) : a_str_x(jc_reg[0][s], XI, (unsigned)r * 8));
}

/* Write the changed registers back, and forget them all (a block's end). */
static void jc_flush(JBuf *b, JCache *c) {
    for (int bank = 0; bank < 2; bank++)
        for (int s = 0; s < JC_SLOTS; s++)
            if (c->vm[bank][s] >= 0 && c->dirty[bank][s]) jc_store(b, bank, s, c->vm[bank][s]);
    jc_reset(c);
}

static int jc_alloc(JBuf *b, JCache *c, int bank) {
    for (int s = 0; s < JC_SLOTS; s++)
        if (c->vm[bank][s] < 0) return s;
    for (;;) { /* evict, in turn, a slot this instruction does not use */
        int s = c->next[bank];
        c->next[bank] = (uint8_t)((s + 1) % JC_SLOTS);
        if (c->pin[bank] & 1u << s) continue;
        if (c->dirty[bank][s]) jc_store(b, bank, s, c->vm[bank][s]);
        c->of[bank][c->vm[bank][s]] = -1;
        c->vm[bank][s] = -1;
        c->dirty[bank][s] = false;
        return s;
    }
}

/* The machine register holding register r, loaded if it is not yet. */
static int jc_get(JBuf *b, JCache *c, int bank, int r) {
    int s = c->of[bank][r];
    if (s < 0) {
        s = jc_alloc(b, c, bank);
        jw(b, bank ? a_ldr_d(jc_reg[1][s], XF, (unsigned)r * 8) : a_ldr_x(jc_reg[0][s], XI, (unsigned)r * 8));
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int8_t)r;
    }
    c->pin[bank] |= 1u << s;
    return jc_reg[bank][s];
}

/* The machine register to write register r to (its value to be written back). */
static int jc_def(JBuf *b, JCache *c, int bank, int r) {
    int s = c->of[bank][r];
    if (s < 0) {
        s = jc_alloc(b, c, bank);
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int8_t)r;
    }
    c->dirty[bank][s] = true;
    c->pin[bank] |= 1u << s;
    return jc_reg[bank][s];
}

/* ---- side exits ----
 * A form whose check fails (another struct type, an index out of range, ...)
 * stops before its instruction, for the interpreter to run it, as a block's
 * header does: a stub out of the way writes back the registers changed so
 * far in the block, gives back the count of the block's rest, and stops. */
typedef struct {
    uint32_t pc;
    uint8_t n;
    uint8_t bank[2 * JC_SLOTS], mreg[2 * JC_SLOTS], vreg[2 * JC_SLOTS];
} JSide;
typedef struct {
    JSide *v;
    size_t n, cap;
} JSides;
static JSides *jit_sides;     /* the function being compiled's */
static const uint32_t *jit_rest; /* its instructions from each to its block's end */

static void jside(JBuf *b, JFixes *fx, const JCache *c, int kind, uint32_t bits, uint32_t pc) {
    JSides *ss = jit_sides;
    if (ss->n == ss->cap) {
        ss->cap = ss->cap ? 2 * ss->cap : 16;
        ss->v = xrealloc(ss->v, ss->cap * sizeof *ss->v);
    }
    JSide *sd = &ss->v[ss->n];
    sd->pc = pc;
    sd->n = 0;
    for (int bank = 0; bank < 2; bank++)
        for (int k = 0; k < JC_SLOTS; k++)
            if (c->vm[bank][k] >= 0 && c->dirty[bank][k]) {
                sd->bank[sd->n] = (uint8_t)bank, sd->mreg[sd->n] = jc_reg[bank][k];
                sd->vreg[sd->n++] = (uint8_t)c->vm[bank][k];
            }
    jbranch(b, fx, kind, bits, TO_SIDE, (uint32_t)ss->n++);
}

/* ---- the forms it compiles ---- */
static bool jit_ends_block(uint16_t x) {
    switch (x) {
#define X(N, o) case Q_##N##_II: case Q_##N##_IK: case Q_##N##_FFL: case Q_##N##_FKL:
        QBRANCH_OPS(X)
#undef X
#define X(N, o) case Q_##N##_UU: case Q_##N##_UK:
        QUBRANCH_OPS(X)
#undef X
    case Q_BZ_I: case Q_BNZ_I: case Q_JMP: case Q_CALL_R: case Q_RET_R: case Q_RET_RK:
    case Q_BZ_R: case Q_BNZ_R: case Q_BEQ_RR: case Q_BNE_RR: case Q_BEQ_RK: case Q_BNE_RK:
    case Q_BEQ_EKK: case Q_BNE_EKK: case Q_TCALL_R: return true;
    default: return false;
    }
}

static bool jit_handles(uint16_t x) {
    if (jit_ends_block(x)) return true;
    switch (x) {
    case Q_MOV_II: case Q_MOV_IK: case Q_MOV_FF: case Q_MOV_FK: case Q_MOV_RR:
#define X(N, o) case Q_##N##_III: case Q_##N##_IIK: case Q_##N##_II: case Q_##N##_IK:
        QARITH_OPS(X)
#undef X
#define X(N, o) case Q_F##N##_FFF: case Q_F##N##_FFK: case Q_F##N##_FF: case Q_F##N##_FK: case Q_F##N##_FKF:
        QFARITH_OPS(X)
#undef X
    case Q_REM_IIK: case Q_REM_IK: case Q_DIV_IIK: case Q_DIV_IK: case Q_INC_I: case Q_DEC_I:
    case Q_MOV_RK: case Q_BITCAST_FI: case Q_BITCAST_IF: case Q_CVT_FI: case Q_TYPEOF: case Q_NEW:
    case Q_LDF_I: case Q_LDF_F: case Q_LDF_R: case Q_STF_I: case Q_STF_F: case Q_STF_R:
    case Q_RET_RK: case Q_LEN: case Q_LNEW:
    case Q_LDE_I: case Q_LDE_F: case Q_LDE_R: case Q_STE_I: case Q_STE_F: case Q_STE_R:
    case Q_LDEK_I: case Q_LDEK_F: case Q_LDEK_R: case Q_STEK_I: case Q_STEK_F: case Q_STEK_R:
        return true;
    default: return false;
    }
}

/* ---- set-up: the code memory, and the entry and exit ---- */
static uint32_t *jit_alloc(size_t words) {
    size_t bytes = (words * 4 + 15) & ~(size_t)15;
    if (!jit_mem || (size_t)(jit_end - jit_top) < bytes) return NULL;
    uint32_t *at = (uint32_t *)jit_top;
    jit_top += bytes;
    return at;
}

/* MLWSVM_JIT_DUMP=file: each piece of code as .inst lines, for an assembler
 * and a disassembler to check (cc -c dump.s && objdump -d dump.o). */
static void jit_dump(const char *name, const JBuf *b) {
    static FILE *df;
    static bool tried;
    if (!tried) {
        tried = true;
        const char *path = getenv("MLWSVM_JIT_DUMP");
        if (path) df = fopen(path, "w");
    }
    if (!df) return;
    fprintf(df, "_%s:\n", name);
    for (size_t k = 0; k < b->n; k++) fprintf(df, "    .inst 0x%08x\n", b->v[k]);
    fflush(df);
}

static void jit_install(uint32_t *at, const JBuf *b) {
    pthread_jit_write_protect_np(0);
    memcpy(at, b->v, b->n * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(at, b->n * 4);
}

static void jit_summary(void) {
    fprintf(stderr, "jit: %llu functions, %llu of %llu instructions compiled, %llu bytes\n",
            (unsigned long long)jit_nfuncs, (unsigned long long)jit_nnative, (unsigned long long)jit_ninsns,
            (unsigned long long)jit_bytes);
    fprintf(stderr, "jit: %llu entries, %llu instructions run in machine code (%.1f per entry)\n",
            (unsigned long long)jit_entries, (unsigned long long)jit_ran,
            jit_entries ? (double)jit_ran / (double)jit_entries : 0.0);
    for (int k = 0; k < 15; k++) {
        int best = -1;
        for (int x = 0; x < Q__COUNT; x++)
            if (jit_stops[x] && (best < 0 || jit_stops[x] > jit_stops[best])) best = x;
        if (best < 0) break;
        fprintf(stderr, "  stopped %10llu times at %s (form %d)\n", (unsigned long long)jit_stops[best],
                op_info[jit_stop_op[best]].mnem, best);
        jit_stops[best] = 0;
    }
}

#define P_OFF(f) ((unsigned)offsetof(Process, f))
#define FR_OFF(f) ((unsigned)offsetof(Frame, f))
#define C_OFF(f) ((unsigned)offsetof(Ctx, f))

static bool jit_init(void) {
    const char *e = getenv("MLWSVM_JIT");
    jit_mode = e && *e && *e != '0' ? (*e == '2' ? 2 : 1) : 0;
    if (!jit_mode) return false;
    size_t size = (size_t)64 << 20;
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) {
        jit_mode = 0;
        return false;
    }
    jit_mem = jit_top = m;
    jit_end = jit_mem + size;
    /* jit_enter(c, left, target): save what it uses, load the state, go */
    JBuf b = {0};
    jw(&b, a_stp_pre(29, 30, 31, -80));
    jw(&b, a_addi(29, 31, 0)); /* mov x29, sp */
    jw(&b, a_stp(19, 20, 31, 16));
    jw(&b, a_stp(21, 22, 31, 32));
    jw(&b, a_stp(23, 24, 31, 48));
    jw(&b, a_stp(25, 26, 31, 64));
    jw(&b, a_mov_x(XC, 0));
    jw(&b, a_ldr_x(XI, XC, C_OFF(I)));
    jw(&b, a_ldr_x(XF, XC, C_OFF(F)));
    jw(&b, a_ldr_x(XR, XC, C_OFF(R)));
    jw(&b, a_ldr_x(XFR, XC, C_OFF(fr)));
    jw(&b, a_ldr_x(XP, XC, C_OFF(p)));
    a_movk64(&b, XM, (uint64_t)(uintptr_t)jit_mem);
    jw(&b, a_mov_w(WL, 1));
    jw(&b, a_br(2));
    size_t exit_at = b.n; /* x0: the instruction to stop at; returns it with left */
    jw(&b, a_mov_w(1, WL));
    jw(&b, a_ldp(25, 26, 31, 64));
    jw(&b, a_ldp(23, 24, 31, 48));
    jw(&b, a_ldp(21, 22, 31, 32));
    jw(&b, a_ldp(19, 20, 31, 16));
    jw(&b, a_ldp_post(29, 30, 31, 80));
    jw(&b, A_RET);
    uint32_t *at = jit_alloc(b.n);
    jit_dump("jit_enter", &b);
    jit_install(at, &b);
    jit_enter = (JitExit(*)(Ctx *, uint32_t, void *))(void *)at;
    jit_exit_code = (uint8_t *)(at + exit_at);
    free(b.v);
    if (jit_mode == 2) atexit(jit_summary);
    return true;
}

/* ---- calls and returns, in the machine code ----
 * A call does what Q_CALL_R does: the caller's pc, a frame pushed as
 * proc_push_frame_fitting pushes one (or, without room, the interpreter's
 * call, which grows the stacks), the callee's window cleared and the
 * arguments copied by the call's plan; then the callee's machine code, or the
 * interpreter at its first instruction. The call is a `bl`/`blr`, followed
 * by its continuation, whose offset in the code memory the call's Insn.target
 * keeps (calls do not use it). A return to a compiled call puts its values in
 * Ctx.retv, pops the frame and returns there with `ret`, so the CPU's return
 * predictor knows where it goes; the continuation takes the caller's windows
 * and copies the results. Any other return is the interpreter's. */
static void jit_windows(JBuf *b, const Function *fn) { /* x21, x22 from x20 */
    jw(b, a_addi(XF, XI, fn->roff[BANK_F] * 8u));
    jw(b, a_addi(XR, XI, fn->roff[BANK_R] * 8u));
}

/* Returns the continuation's offset in b. */
static uint32_t jit_call(JBuf *b, JFixes *fx, const Function *caller, const Insn *in, uint32_t pc) {
    const QPlan *pl = in->k.p;
    const Function *fn = pl->fn;
    int argc = pl->argc;
    a_movk64(b, T0, pc + 1);
    jw(b, a_str_w(T0, XFR, FR_OFF(pc)));
    jw(b, a_ldr_w(13, XP, P_OFF(nframes)));
    jw(b, a_ldr_w(14, XP, P_OFF(flimit)));
    jw(b, a_cmp_w(13, 14));
    jbranch(b, fx, FX_BCOND, C_HS, TO_UNDO, pc);
    jw(b, a_ldr_w(15, XP, P_OFF(rtop)));
    jw(b, a_add_wi(16, 15, fn->rsize));
    jw(b, a_ldr_w(14, XP, P_OFF(rcap)));
    jw(b, a_cmp_w(16, 14));
    jbranch(b, fx, FX_BCOND, C_HI, TO_UNDO, pc);
    jw(b, a_addi(17, XFR, sizeof(Frame))); /* the frame above this one */
    jw(b, a_add_wi(13, 13, 1));
    jw(b, a_str_w(13, XP, P_OFF(nframes)));
    a_movk64(b, 14, (uint64_t)(uintptr_t)fn);
    jw(b, a_str_x(14, 17, FR_OFF(fn)));
    jw(b, a_str_w(XZR, 17, FR_OFF(pc)));
    a_movk64(b, 14, (uint64_t)(uintptr_t)in);
    jw(b, a_str_x(14, 17, FR_OFF(ret_insn)));
    jw(b, a_str_x(XZR, 17, FR_OFF(closure)));
    a_movk64(b, 14, 1);
    jw(b, a_strb(14, 17, FR_OFF(qret)));
    jw(b, a_ldr_x(14, XP, P_OFF(sp)));
    jw(b, a_str_x(14, 17, FR_OFF(entry_sp)));
    jw(b, a_ldr_x(T0, XP, P_OFF(fp)));
    jw(b, a_str_x(T0, 17, FR_OFF(saved_fp)));
    jw(b, a_str_x(14, XP, P_OFF(fp)));
    jw(b, a_str_w(15, 17, FR_OFF(base)));
    jw(b, a_str_w(16, XP, P_OFF(rtop)));
    jw(b, a_ldr_w(14, XP, P_OFF(max_depth)));
    jw(b, a_cmp_w(13, 14));
    jw(b, 0x54000000u | 2u << 5 | C_LS); /* b.ls past the store */
    jw(b, a_str_w(13, XP, P_OFF(max_depth)));
    jw(b, a_ldr_x(14, XP, P_OFF(regs)));
    jw(b, a_add_lsl3(14, 14, 15));
    for (unsigned k = 0; k < fn->zlen; k++) jw(b, a_str_x(XZR, 14, (fn->zoff + k) * 8u));
    for (int k = 0; k < argc; k++) {
        jw(b, a_ldr_x(T0, XI, pl->slot[k] * 8u));
        jw(b, a_str_x(T0, 14, pl->slot[argc + k] * 8u));
    }
    jw(b, a_mov_x(XFR, 17));
    jw(b, a_mov_x(XI, 14));
    jit_windows(b, fn);
    size_t z1 = 0, z2 = 0;
    if (fn == caller) { /* (its code is being made: its first instruction's) */
        jbranch(b, fx, FX_BL, 0, TO_BODY, 0);
    } else { /* the callee's machine code for its first instruction, if it has some */
        a_movk64(b, T0, (uint64_t)(uintptr_t)&fn->jit_entry);
        jw(b, a_ldr_x(T0, T0, 0));
        z1 = b->n;
        jw(b, 0xB4000000u | T0); /* cbz: to the interpreter */
        jw(b, a_ldr_x(T0, T0, 0));
        z2 = b->n;
        jw(b, 0xB4000000u | T0);
        jw(b, a_blr(T0));
    }
    /* The continuation: in the caller's windows (x24 is its frame again), with the results. */
    uint32_t c = (uint32_t)b->n;
    jw(b, a_ldr_w(T0, XFR, FR_OFF(base)));
    jw(b, a_ldr_x(14, XP, P_OFF(regs)));
    jw(b, a_add_lsl3(XI, 14, T0));
    jit_windows(b, caller);
    for (int k = 0; k < pl->nres; k++) {
        jw(b, a_ldr_x(T0, XC, C_OFF(retv) + 8u * (unsigned)k));
        jw(b, a_str_x(T0, XI, pl->slot[2 * argc + k] * 8u));
    }
    jbranch(b, fx, FX_B, 0, TO_BODY, pc + 1);
    if (z1) {
        b->v[z1] |= (uint32_t)(b->n - z1) << 5;
        b->v[z2] |= (uint32_t)(b->n - z2) << 5;
        a_movk64(b, 0, (uint64_t)(uintptr_t)fn->code);
        jbranch(b, fx, FX_B, 0, TO_EXIT, 0);
    }
    return c;
}

static void jit_ret(JBuf *b, JFixes *fx, const Insn *in, uint32_t pc) {
    const QPlan *rp = in->k.p;
    jw(b, a_ldrb(T0, XFR, FR_OFF(qret)));
    jbranch(b, fx, FX_CB, 0xB4000000u | T0, TO_UNDO, pc);
    jw(b, a_ldr_x(T1, XFR, FR_OFF(ret_insn)));
    jw(b, a_ldr_w(T2, T1, (unsigned)offsetof(Insn, target)));
    jbranch(b, fx, FX_CB, 0xB4000000u | T2, TO_UNDO, pc);
    for (int k = 0; k < rp->nres; k++) {
        if (rp->slot[k] == QPLAN_K) a_movk64(b, T0, in->ops[k].kw); /* Q_RET_RK's constant */
        else jw(b, a_ldr_x(T0, XI, rp->slot[k] * 8u));
        jw(b, a_str_x(T0, XC, C_OFF(retv) + 8u * (unsigned)k));
    }
    jw(b, a_ldr_x(T0, XFR, FR_OFF(entry_sp)));
    jw(b, a_str_x(T0, XP, P_OFF(sp)));
    jw(b, a_ldr_x(T0, XFR, FR_OFF(saved_fp)));
    jw(b, a_str_x(T0, XP, P_OFF(fp)));
    jw(b, a_ldr_w(T0, XFR, FR_OFF(base)));
    jw(b, a_str_w(T0, XP, P_OFF(rtop)));
    jw(b, a_ldr_w(T0, XP, P_OFF(nframes)));
    jw(b, a_sub_wi(T0, T0, 1));
    jw(b, a_str_w(T0, XP, P_OFF(nframes)));
    jw(b, a_subi(XFR, XFR, sizeof(Frame)));
    jw(b, a_rrr(A_ADD, 30, XM, T2));
    jw(b, A_RET);
}

/* A direct tail call (Q_TCALL_R): the callee's frame replaces this one at
 * the same base, as in the handler. The arguments go through Ctx.retv, since
 * the callee's window is this one's. */
static uint8_t jit_tail_qret(const Frame *fr, const Function *fn) { return tail_qret(fr->ret_insn, fr->qret, fn->mf); }

static void jit_tcall(JBuf *b, JFixes *fx, const Function *caller, const Insn *in, uint32_t pc) {
    const Operand *qo = in->ops;
    const Function *fn = qo[0].rv.ptr;
    const MFunc *mf = fn->mf;
    int argc = (int)qo[1].imm;
    jw(b, a_ldr_w(15, XFR, FR_OFF(base)));
    jw(b, a_add_wi(16, 15, fn->rsize));
    jw(b, a_ldr_w(14, XP, P_OFF(rcap)));
    jw(b, a_cmp_w(16, 14));
    jbranch(b, fx, FX_BCOND, C_HI, TO_UNDO, pc); /* the stacks grow on the generic path */
    for (int k = 0; k < argc; k++) {
        if (qo[2 + k].mode == OM_REG) jw(b, a_ldr_x(T0, XI, qo[2 + k].pad * 8u));
        else a_movk64(b, T0, qo[2 + k].kw);
        jw(b, a_str_x(T0, XC, C_OFF(retv) + 8u * (unsigned)k));
    }
    jw(b, a_mov_x(0, XFR));
    a_movk64(b, 1, (uint64_t)(uintptr_t)fn);
    a_movk64(b, 16, (uint64_t)(uintptr_t)jit_tail_qret);
    jw(b, a_blr(16));
    jw(b, a_ldr_w(15, XFR, FR_OFF(base))); /* (the call kept nothing in x15, x16) */
    jw(b, a_add_wi(16, 15, fn->rsize));
    jw(b, a_ldr_x(T0, XFR, FR_OFF(entry_sp))); /* popped and pushed: sp and fp are the entry sp */
    jw(b, a_str_x(T0, XP, P_OFF(sp)));
    jw(b, a_str_x(T0, XP, P_OFF(fp)));
    a_movk64(b, T1, (uint64_t)(uintptr_t)fn);
    jw(b, a_str_x(T1, XFR, FR_OFF(fn)));
    jw(b, a_str_w(XZR, XFR, FR_OFF(pc)));
    jw(b, a_str_x(XZR, XFR, FR_OFF(closure)));
    jw(b, a_strb(0, XFR, FR_OFF(qret)));
    jw(b, a_str_w(16, XP, P_OFF(rtop)));
    for (unsigned k = 0; k < fn->zlen; k++) jw(b, a_str_x(XZR, XI, (fn->zoff + k) * 8u));
    uint32_t cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        int bk = (int)type_bank((MlwsType)mf->params[k]);
        jw(b, a_ldr_x(T0, XC, C_OFF(retv) + 8u * (unsigned)k));
        jw(b, a_str_x(T0, XI, (fn->roff[bk] + cnt[bk]++) * 8u));
    }
    jit_windows(b, fn);
    if (fn == caller) {
        jbranch(b, fx, FX_B, 0, TO_BODY, 0);
        return;
    }
    a_movk64(b, T0, (uint64_t)(uintptr_t)&fn->jit_entry);
    jw(b, a_ldr_x(T0, T0, 0));
    jw(b, 0xB4000000u | 4u << 5 | T0); /* cbz: to the interpreter */
    jw(b, a_ldr_x(T0, T0, 0));
    jw(b, 0xB4000000u | 2u << 5 | T0);
    jw(b, a_br(T0));
    a_movk64(b, 0, (uint64_t)(uintptr_t)fn->code);
    jbranch(b, fx, FX_B, 0, TO_EXIT, 0);
}

/* ---- elements, as ELEM_LOAD and ELEM_STORE: a list element tagged with the
 * instruction's type, or an element of an array of it; x9 the object, ix the
 * index's register; anything else is a side exit. ---- */
static void jit_elem(JBuf *b, JFixes *fx, const JCache *jc, const Insn *in, uint32_t pc, int ix, bool store, int bank,
                     int val) {
    unsigned type = in->type;
    jside(b, fx, jc, FX_CB, 0xB4000000u | T0, pc);
    jw(b, a_ldrb(T1, T0, (unsigned)offsetof(Obj, kind)));
    jw(b, a_cmp_wi(T1, OBJ_LIST));
    size_t to_arr = b->n;
    jw(b, 0x54000000u | C_NE);
    jw(b, a_ldr_w(T1, T0, (unsigned)offsetof(ObjList, len)));
    jw(b, a_cmp(ix, T1));
    jside(b, fx, jc, FX_BCOND, C_HS, pc);
    jw(b, a_ldr_x(T1, T0, (unsigned)offsetof(ObjList, tags)));
    if (store) {
        a_movk64(b, T2, type);
        jw(b, a_strb_r(T2, T1, ix));
    } else {
        jw(b, a_ldrb_r(T2, T1, ix));
        jw(b, a_cmp_wi(T2, type));
        jside(b, fx, jc, FX_BCOND, C_NE, pc);
    }
    jw(b, a_ldr_x(T1, T0, (unsigned)offsetof(ObjList, items)));
    size_t at = b->n;
    if (store) jw(b, bank == 1 ? a_str_dr(val, T1, ix) : a_str_xr(val, T1, ix));
    else jw(b, bank == 1 ? a_ldr_dr(val, T1, ix) : a_ldr_xr(val, T1, ix));
    size_t to_done = b->n;
    jw(b, 0x14000000u);
    a_patch(b, to_arr);
    jw(b, a_cmp_wi(T1, OBJ_ARRAY));
    jside(b, fx, jc, FX_BCOND, C_NE, pc);
    jw(b, a_ldr_w(T1, T0, (unsigned)offsetof(ObjArray, len)));
    jw(b, a_cmp(ix, T1));
    jside(b, fx, jc, FX_BCOND, C_HS, pc);
    jw(b, a_ldrb(T1, T0, (unsigned)offsetof(ObjArray, elem)));
    jw(b, a_cmp_wi(T1, type));
    jside(b, fx, jc, FX_BCOND, C_NE, pc);
    jw(b, a_addi(T1, T0, (unsigned)offsetof(ObjArray, data)));
    jw(b, b->v[at]); /* the same access, from the array's data */
    a_patch(b, to_done);
}

/* ---- the other forms ---- */
static uint32_t jit_arith_op(uint16_t x, int *shape) {
    switch (x) {
#define A_OR A_ORR
#define A_XOR A_EOR
#define X(N, o)                                                                \
    case Q_##N##_III: *shape = 0; return A_##N;                                \
    case Q_##N##_IIK: *shape = 1; return A_##N;                                \
    case Q_##N##_II: *shape = 2; return A_##N;                                 \
    case Q_##N##_IK: *shape = 3; return A_##N;
        QARITH_OPS(X)
#undef X
#undef A_OR
#undef A_XOR
    default: return 0;
    }
}

static uint32_t jit_farith_op(uint16_t x, int *shape) {
    switch (x) {
#define X(N, o)                                                                \
    case Q_F##N##_FFF: *shape = 0; return A_F##N;                             \
    case Q_F##N##_FFK: *shape = 1; return A_F##N;                             \
    case Q_F##N##_FF: *shape = 2; return A_F##N;                              \
    case Q_F##N##_FK: *shape = 3; return A_F##N;                              \
    case Q_F##N##_FKF: *shape = 4; return A_F##N;
        QFARITH_OPS(X)
#undef X
    default: return 0;
    }
}

/* the condition of a compare-and-branch form, and its kind (0 int, 1 unsigned, 2 f64; +4: constant) */
static int jit_branch_cond(uint16_t x, int *kind) {
    switch (x) {
#define X(N, c, cf)                                                            \
    case Q_##N##_II: *kind = 0; return c;                                      \
    case Q_##N##_IK: *kind = 4; return c;                                      \
    case Q_##N##_FFL: *kind = 2; return cf;                                    \
    case Q_##N##_FKL: *kind = 6; return cf;
        X(BEQ, C_EQ, C_EQ) X(BNE, C_NE, C_NE) X(BLT, C_LT, C_MI)
        X(BLE, C_LE, C_LS) X(BGT, C_GT, C_GT) X(BGE, C_GE, C_GE)
#undef X
    case Q_BLT_UU: *kind = 1; return C_LO;
    case Q_BLE_UU: *kind = 1; return C_LS;
    case Q_BGT_UU: *kind = 1; return C_HI;
    case Q_BGE_UU: *kind = 1; return C_HS;
    case Q_BLT_UK: *kind = 5; return C_LO;
    case Q_BLE_UK: *kind = 5; return C_LS;
    case Q_BGT_UK: *kind = 5; return C_HI;
    case Q_BGE_UK: *kind = 5; return C_HS;
    default: return -1;
    }
}

static void jit_const_f(JBuf *b, int d, double f) {
    uint64_t bits;
    memcpy(&bits, &f, 8);
    a_movk64(b, T0, bits);
    jw(b, a_fmov_dx(d, T0));
}

static void jit_insn(JBuf *b, JFixes *fx, JCache *jc, const Function *f, const Insn *in, uint16_t x, uint32_t pc,
                     uint32_t *cont) {
    int shape, kind, s1, s2, d;
    uint32_t op;
    uint32_t t = pc + (uint32_t)(int32_t)in->target; /* a branch's target (relative in this build) */
    jc->pin[0] = jc->pin[1] = 0;
    switch (x) {
    case Q_MOV_II:
        s1 = jc_get(b, jc, 0, in->r[1]);
        d = jc_def(b, jc, 0, in->r[0]);
        if (d != s1) jw(b, a_mov_x(d, s1));
        return;
    case Q_MOV_IK: d = jc_def(b, jc, 0, in->r[0]); a_movk64(b, d, (uint64_t)in->k.i); return;
    case Q_MOV_FF:
        s1 = jc_get(b, jc, 1, in->r[1]);
        d = jc_def(b, jc, 1, in->r[0]);
        if (d != s1) jw(b, a_fmov_dd(d, s1));
        return;
    case Q_MOV_FK: d = jc_def(b, jc, 1, in->r[0]); jit_const_f(b, d, in->k.f); return;
    case Q_MOV_RR: jw(b, a_ldr_x(T0, XR, in->r[1] * 8u)); jw(b, a_str_x(T0, XR, in->r[0] * 8u)); return;
    case Q_INC_I: case Q_DEC_I:
        s1 = jc_get(b, jc, 0, in->r[0]);
        d = jc_def(b, jc, 0, in->r[0]);
        jw(b, x == Q_INC_I ? a_addi(d, s1, 1) : a_subi(d, s1, 1));
        return;
    case Q_DIV_IIK: case Q_DIV_IK: case Q_REM_IIK: case Q_REM_IK: {
        bool three = x == Q_DIV_IIK || x == Q_REM_IIK;
        s1 = jc_get(b, jc, 0, in->r[three ? 1 : 0]);
        a_movk64(b, T1, (uint64_t)in->k.i);
        d = jc_def(b, jc, 0, in->r[0]);
        jw(b, a_rrr(A_SDIV, T2, s1, T1));
        if (x == Q_REM_IIK || x == Q_REM_IK) jw(b, a_msub(d, T2, T1, s1));
        else jw(b, a_mov_x(d, T2));
        return;
    }
    case Q_JMP: jc_flush(b, jc); jbranch(b, fx, FX_B, 0, TO_BODY, t); return;
    case Q_BZ_I: case Q_BNZ_I:
        s1 = jc_get(b, jc, 0, in->r[0]);
        jc_flush(b, jc);
        jbranch(b, fx, FX_CB, (x == Q_BZ_I ? 0xB4000000u : 0xB5000000u) | (unsigned)s1, TO_BODY, t);
        return;
    case Q_CALL_R: jc_flush(b, jc); *cont = jit_call(b, fx, f, in, pc); return;
    case Q_MOV_RK: a_movk64(b, T0, (uint64_t)(uintptr_t)in->k.p); jw(b, a_str_x(T0, XR, in->r[0] * 8u)); return;
    case Q_BITCAST_FI: s1 = jc_get(b, jc, 0, in->r[1]); d = jc_def(b, jc, 1, in->r[0]); jw(b, a_fmov_dx(d, s1)); return;
    case Q_BITCAST_IF: s1 = jc_get(b, jc, 1, in->r[1]); d = jc_def(b, jc, 0, in->r[0]); jw(b, a_fmov_xd(d, s1)); return;
    case Q_CVT_FI: s1 = jc_get(b, jc, 0, in->r[1]); d = jc_def(b, jc, 1, in->r[0]); jw(b, a_scvtf(d, s1)); return;
    case Q_TYPEOF:
        jw(b, a_ldr_x(T0, XR, in->r[1] * 8u));
        d = jc_def(b, jc, 0, in->r[0]);
        a_movk64(b, d, OBJ_NULL);
        jw(b, 0xB4000000u | 2u << 5 | T0); /* cbz: null */
        jw(b, a_ldrb(d, T0, (unsigned)offsetof(Obj, kind)));
        return;
    case Q_BZ_R: case Q_BNZ_R:
        jc_flush(b, jc);
        jw(b, a_ldr_x(T0, XR, in->r[0] * 8u));
        jbranch(b, fx, FX_CB, (x == Q_BZ_R ? 0xB4000000u : 0xB5000000u) | T0, TO_BODY, t);
        return;
    case Q_BEQ_RR: case Q_BNE_RR: case Q_BEQ_RK: case Q_BNE_RK:
        jc_flush(b, jc);
        jw(b, a_ldr_x(T0, XR, in->r[0] * 8u));
        if (x == Q_BEQ_RR || x == Q_BNE_RR) jw(b, a_ldr_x(T1, XR, in->r[1] * 8u));
        else a_movk64(b, T1, (uint64_t)(uintptr_t)in->k.p);
        jw(b, a_cmp(T0, T1));
        jbranch(b, fx, FX_BCOND, x == Q_BEQ_RR || x == Q_BEQ_RK ? C_EQ : C_NE, TO_BODY, t);
        return;
    case Q_BEQ_EKK: case Q_BNE_EKK: { /* R0[i] (a list element tagged r) against a constant */
        uint32_t i = in->r[1] | (uint32_t)in->r[2] << 8;
        jw(b, a_ldr_x(T0, XR, in->r[0] * 8u));
        jside(b, fx, jc, FX_CB, 0xB4000000u | T0, pc);
        jw(b, a_ldrb(T1, T0, (unsigned)offsetof(Obj, kind)));
        jw(b, a_cmp_wi(T1, OBJ_LIST));
        jside(b, fx, jc, FX_BCOND, C_NE, pc);
        jw(b, a_ldr_w(T1, T0, (unsigned)offsetof(ObjList, len)));
        jw(b, a_cmp_wi(T1, i));
        jside(b, fx, jc, FX_BCOND, C_LS, pc); /* len <= i */
        jw(b, a_ldr_x(T1, T0, (unsigned)offsetof(ObjList, tags)));
        jw(b, a_ldrb(T1, T1, i));
        jw(b, a_cmp_wi(T1, T_R));
        jside(b, fx, jc, FX_BCOND, C_NE, pc);
        jw(b, a_ldr_x(T1, T0, (unsigned)offsetof(ObjList, items)));
        jw(b, a_ldr_x(T1, T1, i * 8u));
        a_movk64(b, T2, (uint64_t)(uintptr_t)in->k.p);
        jc_flush(b, jc);
        jw(b, a_cmp(T1, T2));
        jbranch(b, fx, FX_BCOND, x == Q_BEQ_EKK ? C_EQ : C_NE, TO_BODY, t);
        return;
    }
    case Q_LDF_I: case Q_LDF_F: case Q_LDF_R: case Q_STF_I: case Q_STF_F: case Q_STF_R: {
        bool load = x <= Q_LDF_R;
        int bank = load ? x - Q_LDF_I : x - Q_STF_I;
        unsigned off = (unsigned)offsetof(ObjStruct, fields) + in->target * 8u;
        jw(b, a_ldr_x(T0, XR, in->r[load ? 1 : 0] * 8u)); /* the struct, checked as IS_STRUCT checks it */
        jside(b, fx, jc, FX_CB, 0xB4000000u | T0, pc);
        jw(b, a_ldrb(T1, T0, (unsigned)offsetof(Obj, kind)));
        jw(b, a_cmp_wi(T1, OBJ_STRUCT));
        jside(b, fx, jc, FX_BCOND, C_NE, pc);
        jw(b, a_ldr_x(T1, T0, (unsigned)offsetof(ObjStruct, def)));
        a_movk64(b, T2, (uint64_t)(uintptr_t)in->k.p);
        jw(b, a_cmp(T1, T2));
        jside(b, fx, jc, FX_BCOND, C_NE, pc);
        if (bank == 2) {
            if (load) jw(b, a_ldr_x(T1, T0, off)), jw(b, a_str_x(T1, XR, in->r[0] * 8u));
            else jw(b, a_ldr_x(T1, XR, in->r[1] * 8u)), jw(b, a_str_x(T1, T0, off));
        } else if (load) {
            d = jc_def(b, jc, bank, in->r[0]);
            jw(b, bank ? a_ldr_d(d, T0, off) : a_ldr_x(d, T0, off));
        } else {
            s1 = jc_get(b, jc, bank, in->r[1]);
            jw(b, bank ? a_str_d(s1, T0, off) : a_str_x(s1, T0, off));
        }
        return;
    }
    case Q_NEW: case Q_LNEW: /* these may trap (a quota), so the frame's pc and the count are up to date */
        jc_flush(b, jc);
        a_movk64(b, T0, pc + 1);
        jw(b, a_str_w(T0, XFR, FR_OFF(pc)));
        /* Process.insns as the interpreter has it here: the rest of the block not yet run */
        jw(b, a_ldr_x(T0, XC, C_OFF(ibase)));
        jw(b, a_add_wi(T1, WL, jit_rest[pc] - 1)); /* (a w register: the upper half is zero) */
        jw(b, a_rrr(A_SUB, T0, T0, T1));
        jw(b, a_str_x(T0, XP, P_OFF(insns)));
        jw(b, a_mov_x(0, XP));
        a_movk64(b, 1, x == Q_NEW ? (uint64_t)(uintptr_t)in->k.p : (uint64_t)in->k.i);
        a_movk64(b, 16, x == Q_NEW ? (uint64_t)(uintptr_t)struct_new : (uint64_t)(uintptr_t)list_new);
        jw(b, a_blr(16));
        jw(b, a_str_x(0, XR, in->r[0] * 8u));
        return;
    case Q_RET_R: case Q_RET_RK: jc_flush(b, jc); jit_ret(b, fx, in, pc); return;
    case Q_TCALL_R: jc_flush(b, jc); jit_tcall(b, fx, f, in, pc); return;
    case Q_LEN: {
        jw(b, a_ldr_x(T0, XR, in->r[1] * 8u));
        jside(b, fx, jc, FX_CB, 0xB4000000u | T0, pc);
        jw(b, a_ldrb(T1, T0, (unsigned)offsetof(Obj, kind)));
        static const struct { uint8_t kind; uint16_t off; } kinds[] = {
            {OBJ_LIST, offsetof(ObjList, len)}, {OBJ_STRING, offsetof(ObjString, len)}, {OBJ_ARRAY, offsetof(ObjArray, len)}};
        size_t done[3];
        for (int k = 0; k < 3; k++) {
            jw(b, a_cmp_wi(T1, kinds[k].kind));
            size_t skip = b->n;
            jw(b, 0x54000000u | C_NE);
            jw(b, a_ldr_w(T2, T0, kinds[k].off));
            done[k] = b->n;
            jw(b, 0x14000000u);
            a_patch(b, skip);
        }
        jside(b, fx, jc, FX_B, 0, pc); /* a map or bytes: the handler's */
        for (int k = 0; k < 3; k++) a_patch(b, done[k]);
        d = jc_def(b, jc, 0, in->r[0]);
        jw(b, a_mov_x(d, T2));
        return;
    }
    case Q_LDE_I: case Q_LDE_F: case Q_LDE_R: case Q_LDEK_I: case Q_LDEK_F: case Q_LDEK_R: {
        int bank = x >= Q_LDEK_I ? x - Q_LDEK_I : x - Q_LDE_I;
        int ix;
        if (x >= Q_LDEK_I) a_movk64(b, 16, in->target), ix = 16;
        else ix = jc_get(b, jc, 0, in->r[2]);
        jw(b, a_ldr_x(T0, XR, in->r[1] * 8u));
        jit_elem(b, fx, jc, in, pc, ix, false, bank, bank == 1 ? 0 : 17);
        if (bank == 2) jw(b, a_str_x(17, XR, in->r[0] * 8u));
        else if (bank == 1) d = jc_def(b, jc, 1, in->r[0]), jw(b, a_fmov_dd(d, 0));
        else d = jc_def(b, jc, 0, in->r[0]), jw(b, a_mov_x(d, 17));
        return;
    }
    case Q_STE_I: case Q_STE_F: case Q_STE_R: case Q_STEK_I: case Q_STEK_F: case Q_STEK_R: {
        int bank = x >= Q_STEK_I ? x - Q_STEK_I : x - Q_STE_I;
        int ix, val;
        if (x >= Q_STEK_I) a_movk64(b, 16, in->target), ix = 16;
        else ix = jc_get(b, jc, 0, in->r[2]);
        if (bank == 2) jw(b, a_ldr_x(17, XR, in->r[1] * 8u)), val = 17;
        else val = jc_get(b, jc, bank, in->r[1]);
        jw(b, a_ldr_x(T0, XR, in->r[0] * 8u));
        jit_elem(b, fx, jc, in, pc, ix, true, bank, val);
        return;
    }
    default: break;
    }
    if ((op = jit_arith_op(x, &shape))) {
        s1 = jc_get(b, jc, 0, in->r[shape <= 1 ? 1 : 0]);
        if (shape == 0 || shape == 2) {
            s2 = jc_get(b, jc, 0, in->r[shape == 0 ? 2 : 1]);
            d = jc_def(b, jc, 0, in->r[0]);
            jw(b, a_rrr(op, d, s1, s2));
            return;
        }
        int64_t k = in->k.i;
        d = jc_def(b, jc, 0, in->r[0]);
        if (op == A_ADD && k >= 0 && k < 4096) jw(b, a_addi(d, s1, (unsigned)k));
        else if (op == A_SUB && k >= 0 && k < 4096) jw(b, a_subi(d, s1, (unsigned)k));
        else {
            a_movk64(b, T1, (uint64_t)k);
            jw(b, a_rrr(op, d, s1, T1));
        }
        return;
    }
    if ((op = jit_farith_op(x, &shape))) {
        switch (shape) {
        case 0: s1 = jc_get(b, jc, 1, in->r[1]); s2 = jc_get(b, jc, 1, in->r[2]); break;
        case 1: s1 = jc_get(b, jc, 1, in->r[1]); jit_const_f(b, 1, in->k.f); s2 = 1; break;
        case 2: s1 = jc_get(b, jc, 1, in->r[0]); s2 = jc_get(b, jc, 1, in->r[1]); break;
        case 3: s1 = jc_get(b, jc, 1, in->r[0]); jit_const_f(b, 1, in->k.f); s2 = 1; break;
        default: jit_const_f(b, 0, in->k.f); s1 = 0; s2 = jc_get(b, jc, 1, in->r[2]); break;
        }
        d = jc_def(b, jc, 1, in->r[0]);
        jw(b, a_rrr(op, d, s1, s2));
        return;
    }
    int cond = jit_branch_cond(x, &kind);
    if (cond >= 0) {
        if ((kind & 3) == 2) {
            s1 = jc_get(b, jc, 1, in->r[0]);
            if (kind & 4) jit_const_f(b, 1, in->k.f), s2 = 1;
            else s2 = jc_get(b, jc, 1, in->r[1]);
            jc_flush(b, jc);
            jw(b, a_fcmp(s1, s2));
        } else {
            s1 = jc_get(b, jc, 0, in->r[0]);
            int64_t k = in->k.i;
            if (!(kind & 4)) {
                s2 = jc_get(b, jc, 0, in->r[1]);
                jc_flush(b, jc);
                jw(b, a_cmp(s1, s2));
            } else {
                jc_flush(b, jc);
                if (k >= 0 && k < 4096) {
                    jw(b, a_cmpi(s1, (unsigned)k));
                } else if (k < 0 && k > -4096) {
                    jw(b, a_cmni(s1, (unsigned)-k));
                } else {
                    a_movk64(b, T1, (uint64_t)k);
                    jw(b, a_cmp(s1, T1));
                }
            }
        }
        jbranch(b, fx, FX_BCOND, (uint32_t)cond, TO_BODY, t);
        return;
    }
    jw(b, A_BRK); /* not reached: jit_handles agrees with this switch */
}

/* Whether entering the machine code at leader L pays: the code it can run
 * from there before it must stop for the interpreter has a loop or a call,
 * or is long.
 * Each entry and exit costs about as much as a few instructions in the
 * interpreter, so a short run between two instructions the interpreter runs
 * would be slower than the interpreter alone. */
#define JIT_RUN_MIN 16
static bool jit_worth_entering(const Function *f, const uint16_t *xop, uint32_t L, uint8_t *mark, uint32_t *stack) {
    uint32_t n = f->ncode, sp = 0, seen = 0;
    bool loop = false;
    memset(mark, 0, n);
    stack[sp++] = L;
    mark[L] = 1;
    while (sp && !loop) { /* depth first, with 1 on the path and 2 done */
        uint32_t pc = stack[sp - 1];
        uint32_t succ[2], ns = 0;
        uint16_t x = xop[pc];
        if (!jit_ends_block(x)) succ[ns++] = pc + 1;
        else if (x == Q_CALL_R) loop = true; /* (a call goes on in the callee's machine code, if it has some) */
        else if (x != Q_RET_R) {
            succ[ns++] = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (x != Q_JMP) succ[ns++] = pc + 1;
        }
        bool pushed = false;
        for (uint32_t k = 0; k < ns && !pushed; k++) {
            uint32_t t = succ[k];
            if (t >= n || !jit_handles(xop[t])) continue;
            if (mark[t] == 1) loop = true;
            if (mark[t]) continue;
            mark[t] = 1;
            stack[sp++] = t;
            seen++;
            pushed = true;
        }
        if (!pushed) mark[pc] = 2, sp--;
    }
    return loop || seen + 1 >= JIT_RUN_MIN;
}

static void jit_compile(Function *f) {
    uint32_t n = f->ncode;
    uint16_t *xop = xmalloc(n * sizeof *xop);
    bool any = false;
    for (uint32_t pc = 0; pc < n; pc++) {
        xop[pc] = f->code[pc].xop;
        any = any || jit_handles(xop[pc]);
    }
    jit_ninsns += n;
    if (!any) {
        free(xop);
        return;
    }
    /* Blocks, and rest[pc]: the instructions from pc to its block's end. */
    uint8_t *lead = xcalloc(n + 1, 1);
    uint32_t *rest = xcalloc(n + 1, sizeof *rest);
    for (uint32_t pc = 0; pc < n; pc++) {
        if (pc == 0 || !jit_handles(xop[pc - 1]) || jit_ends_block(xop[pc - 1])) lead[pc] = 1;
        if (jit_handles(xop[pc]) && jit_ends_block(xop[pc]) && xop[pc] != Q_CALL_R && xop[pc] != Q_RET_R) {
            uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (t < n) lead[t] = 1;
        }
    }
    for (uint32_t pc = n; pc-- > 0;) {
        if (!jit_handles(xop[pc])) continue;
        bool last = pc + 1 >= n || lead[pc + 1] || !jit_handles(xop[pc + 1]) || jit_ends_block(xop[pc]);
        rest[pc] = last ? 1 : rest[pc + 1] + 1;
        if (rest[pc] > 4000) rest[pc] = 1, lead[pc + 1] = 1; /* (a block's length fits in 12 bits) */
    }
    JBuf b = {0};
    JFixes fx = {0};
    JSides sides = {0};
    jit_sides = &sides;
    jit_rest = rest;
    JCache jc;
    jc_reset(&jc);
    memset(jc.next, 0, sizeof jc.next);
    uint32_t *body = xcalloc(n + 1, sizeof *body), *undo = xcalloc(n + 1, sizeof *undo);
    uint32_t *cont = xcalloc(n + 1, sizeof *cont);
    uint32_t nnat = 0;
    for (uint32_t pc = 0; pc < n; pc++) {
        body[pc] = (uint32_t)b.n;
        if (!jit_handles(xop[pc])) { /* the interpreter's: stop here */
            a_movk64(&b, 0, (uint64_t)(uintptr_t)&f->code[pc]);
            jbranch(&b, &fx, FX_B, 0, TO_EXIT, 0);
            continue;
        }
        nnat++;
        if (lead[pc]) { /* the block's header: count it, or stop before it */
            jw(&b, a_subs_wi(WL, WL, rest[pc]));
            jbranch(&b, &fx, FX_BCOND, C_LO, TO_UNDO, pc);
        }
        jit_insn(&b, &fx, &jc, f, &f->code[pc], xop[pc], pc, &cont[pc]);
        if (rest[pc] == 1) jc_flush(&b, &jc); /* the block ends: back to the window */
    }
    jw(&b, A_BRK); /* past the end: not reached (the verifier ends every path) */
    uint32_t *side = xcalloc(sides.n + 1, sizeof *side);
    for (size_t k = 0; k < sides.n; k++) {
        const JSide *sd = &sides.v[k];
        side[k] = (uint32_t)b.n;
        for (int j = 0; j < sd->n; j++)
            jw(&b, sd->bank[j] ? a_str_d(sd->mreg[j], XF, sd->vreg[j] * 8u) : a_str_x(sd->mreg[j], XI, sd->vreg[j] * 8u));
        jw(&b, a_add_wi(WL, WL, rest[sd->pc]));
        a_movk64(&b, 0, (uint64_t)(uintptr_t)&f->code[sd->pc]);
        jbranch(&b, &fx, FX_B, 0, TO_EXIT, 0);
    }
    for (uint32_t pc = 0; pc < n; pc++) { /* give the count back and stop before pc */
        if (!jit_handles(xop[pc])) continue;
        undo[pc] = (uint32_t)b.n;
        jw(&b, a_add_wi(WL, WL, rest[pc]));
        a_movk64(&b, 0, (uint64_t)(uintptr_t)&f->code[pc]);
        jbranch(&b, &fx, FX_B, 0, TO_EXIT, 0);
    }
    pthread_mutex_lock(&jit_lock);
    uint32_t *at = jit_alloc(b.n);
    if (at) {
        for (size_t k = 0; k < fx.n; k++) {
            const JFix *x = &fx.v[k];
            int64_t dest = x->to == TO_BODY ? body[x->pc] : x->to == TO_SIDE ? side[x->pc] : undo[x->pc];
            int64_t off = x->to == TO_EXIT ? ((uint32_t *)jit_exit_code - (at + x->at)) : dest - (int64_t)x->at;
            if (x->kind == FX_B || x->kind == FX_BL) b.v[x->at] |= (uint32_t)off & 0x3FFFFFF;
            else b.v[x->at] |= ((uint32_t)off & 0x7FFFF) << 5;
        }
        char nm[256];
        snprintf(nm, sizeof nm, "f%llu_%s", (unsigned long long)jit_nfuncs, f->name);
        for (char *q = nm; *q; q++)
            if (!isalnum((unsigned char)*q)) *q = '_';
        jit_dump(nm, &b);
        jit_install(at, &b);
        void **ent = xcalloc(n, sizeof *ent);
        uint8_t *mark = xmalloc(n);
        uint32_t *stack = xmalloc(n * sizeof *stack);
        for (uint32_t pc = 0; pc < n; pc++) {
            if (!jit_handles(xop[pc])) continue;
            if (cont[pc]) f->code[pc].target = (uint32_t)((uint8_t *)(at + cont[pc]) - jit_mem);
            if (!lead[pc]) continue;
            ent[pc] = at + body[pc]; /* for calls from machine code */
            if (jit_worth_entering(f, xop, pc, mark, stack)) f->code[pc].xop = Q_JIT;
        }
        free(mark), free(stack);
        f->jit_xop = xop;
        f->jit_entry = ent;
        xop = NULL;
        jit_nfuncs++;
        jit_nnative += nnat;
        jit_bytes += b.n * 4;
    }
    pthread_mutex_unlock(&jit_lock);
    free(xop);
    free(lead), free(rest), free(body), free(undo), free(cont), free(side), free(sides.v);
    free(b.v), free(fx.v);
}

void interp_jit_function(Function *f) {
    pthread_mutex_lock(&jit_lock);
    bool on = jit_mode < 0 ? jit_init() : jit_mode > 0;
    pthread_mutex_unlock(&jit_lock);
    if (on) jit_compile(f);
}

/* The interpreter reached a block's first instruction: run the machine code
 * from there, then go on with the instruction it stopped at, as NEXT would. */
static QCC int h_Q_JIT(QPARAMS) {
    uint32_t pc = (uint32_t)(in - QCODE);
    const Function *fn = QFRAME->fn;
    if (c->hooked) MUSTTAIL return qhandlers[fn->jit_xop[pc]](QARGS); /* a tool sees each instruction */
    JitExit r = jit_enter(c, left + 1, fn->jit_entry[pc]);
    if (jit_mode == 2) {
        jit_entries++, jit_ran += left + 1 - (uint32_t)r.left;
        const Process *pp = c->p;
        const Function *sf = pp->frames[pp->nframes - 1].fn;
        uint16_t sx = r.in->xop == Q_JIT ? sf->jit_xop[r.in - sf->code] : r.in->xop;
        jit_stops[sx]++, jit_stop_op[sx] = r.in->op;
    }
    left = (uint32_t)r.left;
    ctx_load(c); /* the machine code kept the frame and windows to itself */
    I = c->I, F = c->F, R = c->R;
    QSETFRAME();
    const Insn *nx = r.in;
    if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx));
    left--;
    uint16_t x = nx->xop;
    if (x == Q_JIT) x = QFRAME->fn->jit_xop[nx - QCODE];
    MUSTTAIL return qhandlers[x](QARGSN(nx));
}
#else
void interp_jit_function(Function *f) { (void)f; }
#endif
