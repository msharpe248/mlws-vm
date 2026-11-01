/* jit.inc - the JIT: machine code for a function's common quickened forms,
 * run in mixed mode with the interpreter (--jit, docs/JIT.md). Included by
 * interp.c in the tail-call build, before the handler table; the machine
 * code is arm64's (jit_a64.inc) or x86-64's (jit_x64.inc), written here once
 * in the operations both encoders provide (jm_*).
 *
 * When. With --jit on, a function is compiled once hot: its first
 * instruction and each loop's head have the execution opcode Q_JITC, which
 * counts (Function.jit_count) and runs the instruction's own form
 * (Function.jit_xop); the count reaching --jit-hot compiles it. With --jit
 * all, every function is compiled as it is loaded.
 *
 * Blocks. A function's compiled instructions fall into basic blocks: one
 * starts at the first instruction, at a compiled branch's target, and after a
 * block's end (a branch, call or return) or an instruction the interpreter
 * runs. The first instruction of each block where entering pays gets Q_JIT,
 * whose handler enters the machine code there; the rest keep their forms, so
 * the interpreter can run any of them, as it does after a slice ends inside a
 * block. The machine code works on the same register windows and frames as
 * the handlers, and stops at an instruction the interpreter must run,
 * returning it to the interpreter. An instruction it does not compile is run
 * from the machine code by its own handler (`stepped`).
 *
 * Registers. Inside a block, I and F registers live in machine registers once
 * read or written (a small cache), and the changed ones are written back to
 * the window at the block's end, so a block's instructions pass values
 * without a store and a load each.
 *
 * The slice. A block's header takes its length from `left`, or, if that would
 * go below zero, stops at the block's first instruction, not yet counted,
 * which the interpreter counts as it dispatches it. So Process.insns and
 * preemption are exactly the interpreter's.
 *
 * Checks. A form compiled here does what its handler's fast path does, with
 * the same checks; whatever the handler hands to the generic instruction (a
 * struct of another type, an index out of range, a tag that is not the
 * instruction's type) is a side exit: the machine code stops before the
 * instruction, for the interpreter to run it. */
#if MLWS_TAILCALL && (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
#define MLWS_JIT 1
#include "../platform/platform.h"

typedef struct {
    uint8_t *v;
    size_t n, cap;
} JBuf;
static void jb_need(JBuf *b, size_t k) {
    if (b->n + k <= b->cap) return;
    while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 1024;
    b->v = xrealloc(b->v, b->cap);
}
/* A branch to an instruction's code, a stub or the exit, patched once the
 * code is placed: at its place in the code, to `to` (TO_*) for `pc`. */
typedef struct {
    uint32_t at;
    uint8_t to;
    uint32_t pc;
} JFix;
typedef struct {
    JFix *v;
    size_t n, cap;
} JFixes;

#if defined(__aarch64__)
#include "jit_a64.inc"
#else
#include "jit_x64.inc"
#endif

#define JC_MAX 16
typedef struct {
    const Insn *in; /* where the interpreter goes on: not yet counted */
    uint64_t left;
} JitExit;

/* A VM's JIT: its code memory (the entry and exit first), and what the report
 * shows. */
typedef struct Jit Jit;
struct Jit {
    uint8_t *mem, *top, *end;
    size_t align;
    JitExit (*enter)(Ctx *c, uint32_t left, void *target);
    uint8_t *exit;
    bool no_memory;
    bool stats; /* count entries, steps and stops (--jit-report) */
    uint64_t nfuncs, ninsns, nnative, bytes, ns;
    uint64_t entries, ran, nsteps;
    uint64_t stops[Q__COUNT], steps[Q__COUNT];
    uint16_t opof[Q__COUNT];
};
enum { JIT_COMPILED = 1, JIT_NONE = 2 }; /* Function.jit_state */

#define JIT_MEM_SIZE ((size_t)64 << 20)

/* ---- code memory (plat_code_*: written, then executable) ---- */
static bool jit_mem_init(Jit *jt) {
    jt->mem = jt->top = plat_code_reserve(JIT_MEM_SIZE);
    if (!jt->mem) return false;
    jt->end = jt->mem + JIT_MEM_SIZE;
    jt->align = plat_code_align();
    return true;
}

/* Room for n bytes of code. */
static uint8_t *jit_reserve(Jit *jt, size_t n) {
    size_t bytes = (n + jt->align - 1) & ~(jt->align - 1);
    if ((size_t)(jt->end - jt->top) < bytes) return NULL;
    uint8_t *at = jt->top;
    jt->top += bytes;
    return at;
}

static bool jit_install(Jit *jt, uint8_t *at, const JBuf *b) { return (void)jt, plat_code_write(at, b->v, b->n); }

/* MLWSVM_JIT_DUMP=file: each piece of code as .byte lines, for an assembler
 * and a disassembler to check (cc -c dump.s && objdump -d dump.o). */
static void jit_dump(const char *name, const JBuf *b) {
    static FILE *df;
    static int tried;
    if (!__atomic_exchange_n(&tried, 1, __ATOMIC_RELAXED)) {
        const char *path = getenv("MLWSVM_JIT_DUMP");
        if (path) df = fopen(path, "w");
    }
    if (!df) return;
    fprintf(df, "_%s:\n", name);
    for (size_t k = 0; k < b->n; k++) fprintf(df, k % 16 ? ",0x%02x" : "\n    .byte 0x%02x", b->v[k]);
    fprintf(df, "\n");
    fflush(df);
}

static Jit *jit_get(VM *vm) { /* (under vm->jit_lock) */
    if (vm->jit || vm->jit_failed) return vm->jit;
    Jit *jt = xcalloc(1, sizeof *jt);
    if (!jit_mem_init(jt)) {
        free(jt);
        vm->jit_failed = true;
        return NULL;
    }
    jt->stats = vm->jit_report_path != NULL || vm->jit_stats;
    JBuf b = {0};
    size_t exit_at;
    jm_tramp(&b, &exit_at, jt->mem);
    uint8_t *at = jit_reserve(jt, b.n);
    if (!at || !jit_install(jt, at, &b)) {
        free(b.v);
        plat_code_free(jt->mem, JIT_MEM_SIZE);
        free(jt);
        vm->jit_failed = true;
        return NULL;
    }
    jit_dump("jit_enter", &b);
    jt->enter = (JitExit(*)(Ctx *, uint32_t, void *))(void *)at;
    jt->exit = at + exit_at;
    free(b.v);
    vm->jit = jt;
    return jt;
}

void interp_jit_free(VM *vm) {
    Jit *jt = vm->jit;
    if (!jt) return;
    plat_code_free(jt->mem, JIT_MEM_SIZE);
    free(jt);
    vm->jit = NULL;
}

/* ---- a function being compiled ---- */
typedef struct {
    uint32_t pc;
    uint8_t n;
    uint8_t bank[2 * JC_MAX], mreg[2 * JC_MAX], vreg[2 * JC_MAX];
} JSide;

/* The register cache. Bank 0 is I (general registers), bank 1 F (float
 * registers). of[b][r]: the slot holding register r, or -1; vm[b][s]: the
 * register in slot s, or -1. */
typedef struct {
    int8_t of[2][256], vm[2][JC_MAX];
    bool dirty[2][JC_MAX];
    uint32_t pin[2]; /* slots this instruction uses */
    uint8_t next[2];
} JCache;

typedef struct {
    Jit *jt;
    const Function *f;
    JBuf b;
    JFixes fx;
    struct {
        JSide *v;
        size_t n, cap;
    } sides;
    JCache jc;
    const uint32_t *rest; /* the instructions from each to its block's end */
} JComp;

enum { TO_BODY, TO_UNDO, TO_EXIT, TO_SIDE };

static void jfix(JComp *j, uint32_t at, int to, uint32_t pc) {
    JFixes *f = &j->fx;
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 64;
        f->v = xrealloc(f->v, f->cap * sizeof *f->v);
    }
    f->v[f->n++] = (JFix){at, (uint8_t)to, pc};
}
static void jbranch(JComp *j, int kind, int cond, int reg, bool nz, int to, uint32_t pc) {
    jfix(j, jm_emit_branch(&j->b, kind, cond, reg, nz), to, pc);
}
static void jexit(JComp *j, const Insn *in) { /* stop before in, for the interpreter */
    jm_movi(&j->b, JR_R0, (uint64_t)(uintptr_t)in);
    jbranch(j, FX_B, 0, 0, false, TO_EXIT, 0);
}

/* A call stops for the interpreter while the machine stack is near its
 * limit (x86-64's calls push; arm64's do not). */
static void jm_ncall_check(JComp *j, uint32_t pc) {
#if defined(__x86_64__)
    x_mem(&j->b, 0, true, 0x3B, RSP, JR_CTX, offsetof(Ctx, jlimit), false); /* cmp rsp, [rbx + jlimit] */
    jbranch(j, FX_BCOND, JC_LS, 0, false, TO_UNDO, pc);
#else
    (void)j, (void)pc;
#endif
}

/* A register's place in the window: the F and R windows follow I's. */
static int64_t jwoff(const JComp *j, int bank, unsigned r) { return (int64_t)(j->f->roff[bank] + r) * 8; }
static void jld_r(JComp *j, int d, unsigned r) { jm_ld(&j->b, d, JR_WIN, jwoff(j, BANK_R, r)); }
static void jst_r(JComp *j, int s, unsigned r) { jm_st(&j->b, s, JR_WIN, jwoff(j, BANK_R, r)); }

static int jc_mreg(int bank, int s) { return bank ? jc_freg[s] : jc_ireg[s]; }
static int jc_nslots(int bank) { return bank ? JC_NF : JC_NI; }

static void jc_reset(JCache *c) {
    memset(c->of, -1, sizeof c->of);
    memset(c->vm, -1, sizeof c->vm);
    memset(c->dirty, 0, sizeof c->dirty);
    c->pin[0] = c->pin[1] = 0;
}

static void jc_store(JComp *j, int bank, int s, int r) {
    if (bank) jm_fst(&j->b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
    else jm_st(&j->b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
}

/* Write the changed registers back, and forget them all (a block's end). */
static void jc_flush(JComp *j) {
    JCache *c = &j->jc;
    for (int bank = 0; bank < 2; bank++)
        for (int s = 0; s < jc_nslots(bank); s++)
            if (c->vm[bank][s] >= 0 && c->dirty[bank][s]) jc_store(j, bank, s, c->vm[bank][s]);
    jc_reset(c);
}

static int jc_alloc(JComp *j, int bank) {
    JCache *c = &j->jc;
    int ns = jc_nslots(bank);
    for (int s = 0; s < ns; s++)
        if (c->vm[bank][s] < 0) return s;
    for (;;) { /* evict, in turn, a slot this instruction does not use */
        int s = c->next[bank];
        c->next[bank] = (uint8_t)((s + 1) % ns);
        if (c->pin[bank] & 1u << s) continue;
        if (c->dirty[bank][s]) jc_store(j, bank, s, c->vm[bank][s]);
        c->of[bank][c->vm[bank][s]] = -1;
        c->vm[bank][s] = -1;
        c->dirty[bank][s] = false;
        return s;
    }
}

/* The machine register holding register r, loaded if it is not yet. */
static int jc_get(JComp *j, int bank, int r) {
    JCache *c = &j->jc;
    int s = c->of[bank][r];
    if (s < 0) {
        s = jc_alloc(j, bank);
        if (bank) jm_fld(&j->b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
        else jm_ld(&j->b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int8_t)r;
    }
    c->pin[bank] |= 1u << s;
    return jc_mreg(bank, s);
}

/* The machine register to write register r to (its value to be written back). */
static int jc_def(JComp *j, int bank, int r) {
    JCache *c = &j->jc;
    int s = c->of[bank][r];
    if (s < 0) {
        s = jc_alloc(j, bank);
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int8_t)r;
    }
    c->dirty[bank][s] = true;
    c->pin[bank] |= 1u << s;
    return jc_mreg(bank, s);
}

/* ---- side exits ----
 * A form whose check fails stops before its instruction, for the interpreter
 * to run it, as a block's header does: a stub out of the way writes back the
 * registers changed so far in the block, gives back the count of the block's
 * rest, and stops. */
static void jside_at(JComp *j, uint32_t at, uint32_t pc) {
    if (j->sides.n == j->sides.cap) {
        j->sides.cap = j->sides.cap ? 2 * j->sides.cap : 16;
        j->sides.v = xrealloc(j->sides.v, j->sides.cap * sizeof *j->sides.v);
    }
    JSide *sd = &j->sides.v[j->sides.n];
    const JCache *c = &j->jc;
    sd->pc = pc;
    sd->n = 0;
    for (int bank = 0; bank < 2; bank++)
        for (int k = 0; k < jc_nslots(bank); k++)
            if (c->vm[bank][k] >= 0 && c->dirty[bank][k]) {
                sd->bank[sd->n] = (uint8_t)bank, sd->mreg[sd->n] = (uint8_t)jc_mreg(bank, k);
                sd->vreg[sd->n++] = (uint8_t)c->vm[bank][k];
            }
    jfix(j, at, TO_SIDE, (uint32_t)j->sides.n++);
}
static void jside(JComp *j, int kind, int cond, int reg, bool nz, uint32_t pc) {
    jside_at(j, jm_emit_branch(&j->b, kind, cond, reg, nz), pc);
}
#define SIDE_IF(cond) jside(j, FX_BCOND, (cond), 0, false, pc)
#define SIDE_IF_NULL(reg) jside(j, FX_CB, 0, (reg), false, pc)

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
    case Q_BEQ_EKK: case Q_BNE_EKK: case Q_TCALL_R:
    case Q_BISA: case Q_BNISA: case Q_BEQ_RG: case Q_BNE_RG:
    case Q_CALL_RK: case Q_CALLI_R: case Q_CALLI_RK: case Q_JTAB: case Q_TCALLI_R: return true;
    default: return false;
    }
}

/* A call, a return or a tail call: where machine code goes on elsewhere. */
static bool jit_calls(uint16_t x) {
    return x == Q_CALL_R || x == Q_CALL_RK || x == Q_CALLI_R || x == Q_CALLI_RK || x == Q_RET_R || x == Q_RET_RK ||
           x == Q_TCALL_R || x == Q_TCALLI_R;
}

/* A block's end that is a branch, with a target (in Insn.target). */
static bool jit_branches(uint16_t x) { return jit_ends_block(x) && !jit_calls(x) && x != Q_JTAB; }

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
    case Q_LEN: case Q_LNEW: case Q_LPUSH_R: case Q_LPUSH_RK:
    case Q_LDG_I: case Q_LDG_F: case Q_LDG_R: case Q_STG_I: case Q_STG_F: case Q_STG_R:
    case Q_ISA: case Q_LDV_E: case Q_LDV_ED: case Q_LDV_EK:
    case Q_SQRT_F: case Q_SQRT_FF: case Q_NEG_F: case Q_NEG_FF: case Q_ABS_F: case Q_ABS_FF:
    case Q_LDU_I: case Q_LDU_F: case Q_LDU_R: case Q_STU_I: case Q_STU_F: case Q_STU_R: case Q_STFK:
    case Q_SEQ_RR: case Q_SNE_RR: case Q_SEQ_RK: case Q_SNE_RK: case Q_MGET_R:
#define X(N, o) case Q_##N##_FF: case Q_##N##_FK:
        QSET_OPS(X)
#undef X
#define X(N, e) case Q_##N##_III: case Q_##N##_IIK: case Q_##N##_II: case Q_##N##_IK:
        QSHIFT_OPS(X)
#undef X
    case Q_LDED_I: case Q_LDED_F: case Q_LDED_R: case Q_STED_I: case Q_STED_F: case Q_STED_R:
    case Q_LDE_I: case Q_LDE_F: case Q_LDE_R: case Q_STE_I: case Q_STE_F: case Q_STE_R:
    case Q_LDEK_I: case Q_LDEK_F: case Q_LDEK_R: case Q_STEK_I: case Q_STEK_F: case Q_STEK_R:
        return true;
    default: return false;
    }
}

/* An instruction the JIT does not compile is still run from the machine code
 * (`stepped`), by its own handler: called with no slice left, a handler runs
 * its instruction and stops before the next (h_slice). Except yield, sleep
 * and break, whose stop is the point. */
static bool jit_steps(const Insn *in, uint16_t x) {
    return !jit_handles(x) && in->op != OP_YIELD && in->op != OP_SLEEP && in->op != OP_BREAK;
}

/* An instruction's own form, under Q_JIT or Q_JITC. */
static uint16_t jit_form(const Function *fn, const Insn *in) {
    uint16_t x = __atomic_load_n(&in->xop, __ATOMIC_RELAXED);
    return x == Q_JIT || x == Q_JITC ? fn->jit_xop[in - fn->code] : x;
}

typedef struct {
    void *native; /* the machine code to go on in, or NULL: stop (at Ctx.jnext, or with Ctx.jstatus if NULL) */
    uint64_t left;
} JitStep;

static JitStep jit_step(Ctx *c, const Insn *in, uint32_t left) {
    Process *p = c->p;
    ctx_load(c);
    c->code = c->fr->fn->code;
    uint32_t depth = p->nframes;
    uint64_t ib = c->ibase;
    c->ibase = ib - left; /* the handler's count, with no slice left, as the interpreter's */
    uint16_t x = jit_form(c->fr->fn, in);
    Jit *jt = c->vm->jit;
    if (jt->stats) {
        __atomic_fetch_add(&jt->nsteps, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&jt->steps[x], 1, __ATOMIC_RELAXED);
        jt->opof[x] = in->op;
    }
#if QWIDE
    int st = qhandlers[x](c, in, 0, c->I, c->F, c->R, c->fr, c->code);
#else
    int st = qhandlers[x](c, in, 0, c->I, c->F, c->R);
#endif
    c->ibase = ib;
    uint64_t nl = p->insns <= ib ? ib - p->insns : 0;
    if (st != SLICE_YIELD) { /* done, blocked, failed, ...: as the handler says */
        c->jnext = NULL, c->jstatus = st;
        return (JitStep){NULL, nl};
    }
    ctx_load(c); /* on wherever it left off: the next instruction, or a branch's target or a callee */
    const Function *fn = c->fr->fn;
    uint32_t npc = c->fr->pc;
    c->jnext = fn->code + npc;
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    if (JM_STEP_POP_STOPS && p->nframes < depth) ent = NULL;
    return (JitStep){ent ? ent[npc] : NULL, nl};
}

/* ---- calls and returns, in the machine code ----
 * A call does what Q_CALL_R does: the caller's pc, a frame pushed as
 * proc_push_frame_fitting pushes one (or, without room, the interpreter's
 * call, which grows the stacks), the callee's window cleared and the
 * arguments copied by the call's plan; then the callee's machine code, or the
 * interpreter at its first instruction. The call's continuation follows it;
 * its offset in the code memory is the call's Insn.target, which calls do not
 * otherwise use (a call has no label, so it is 0 until then). A return to a
 * compiled call puts its values in Ctx.retv, pops the frame and returns to
 * the continuation, which takes the caller's window and copies the results.
 * Any other return is the interpreter's. */
#define P_OFF(f) ((int64_t)offsetof(Process, f))
#define FR_OFF(f) ((int64_t)offsetof(Frame, f))
#define C_OFF(f) ((int64_t)offsetof(Ctx, f))

/* The continuation of a call with plan pl: the caller's window, the results. */
static void jit_cont(JComp *j, const QPlan *pl, int argc, uint32_t pc) {
    JBuf *b = &j->b;
    jm_cont(b);
    jm_ld32(b, JR_S0, JR_FR, FR_OFF(base));
    int P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S1, P, P_OFF(regs));
    jm_addsh3(b, JR_WIN, JR_S1, JR_S0);
    for (int k = 0; k < pl->nres; k++) {
        jm_ld(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
        jm_st(b, JR_S0, JR_WIN, (int64_t)pl->slot[2 * argc + k] * 8);
    }
    jbranch(j, FX_B, 0, 0, false, TO_BODY, pc + 1);
}

/* Returns the continuation's offset in the code. */
static uint32_t jit_call(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *pl = in->k.p;
    const Function *fn = pl->fn;
    int argc = pl->argc;
    int t = jc_ireg[0], t2 = jc_ireg[1]; /* (the cache is empty at a call) */
    jm_ncall_check(j, pc);
    int P = jm_proc(b, JR_S3);
    jm_movi(b, JR_S0, pc + 1);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    jm_ld32(b, JR_S0, P, P_OFF(nframes));
    jm_ld32(b, JR_S1, P, P_OFF(flimit));
    jm_cmp32(b, JR_S0, JR_S1);
    jbranch(j, FX_BCOND, JC_HS, 0, false, TO_UNDO, pc);
    jm_ld32(b, JR_S1, P, P_OFF(rtop));
    jm_add32i(b, JR_S2, JR_S1, fn->rsize);
    jm_ld32(b, JR_S4, P, P_OFF(rcap));
    jm_cmp32(b, JR_S2, JR_S4);
    jbranch(j, FX_BCOND, JC_HI, 0, false, TO_UNDO, pc);
    jm_alui(b, JA_ADD, JR_S4, JR_FR, sizeof(Frame)); /* the frame above this one */
    jm_add32i(b, JR_S0, JR_S0, 1);
    jm_st32(b, JR_S0, P, P_OFF(nframes));
    jm_movi(b, t, (uint64_t)(uintptr_t)fn);
    jm_st(b, t, JR_S4, FR_OFF(fn));
    jm_movi(b, t2, 0);
    jm_st32(b, t2, JR_S4, FR_OFF(pc));
    jm_movi(b, t, (uint64_t)(uintptr_t)in);
    jm_st(b, t, JR_S4, FR_OFF(ret_insn));
    jm_stz(b, JR_S4, FR_OFF(closure));
    jm_movi(b, t, 1);
    jm_st8(b, t, JR_S4, FR_OFF(qret));
    jm_ld(b, t, P, P_OFF(sp));
    jm_st(b, t, JR_S4, FR_OFF(entry_sp));
    jm_ld(b, t2, P, P_OFF(fp));
    jm_st(b, t2, JR_S4, FR_OFF(saved_fp));
    jm_st(b, t, P, P_OFF(fp));
    jm_st32(b, JR_S1, JR_S4, FR_OFF(base));
    jm_st32(b, JR_S2, P, P_OFF(rtop));
    jm_ld32(b, t, P, P_OFF(max_depth));
    jm_cmp32(b, JR_S0, t);
    uint32_t deep = jm_fwd(b, JC_LS);
    jm_st32(b, JR_S0, P, P_OFF(max_depth));
    jm_bind(b, deep);
    jm_ld(b, t, P, P_OFF(regs));
    jm_addsh3(b, t, t, JR_S1); /* the callee's window */
    for (unsigned k = 0; k < fn->zlen; k++) jm_stz(b, t, (int64_t)(fn->zoff + k) * 8);
    for (int k = 0; k < argc; k++) {
        if (pl->slot[k] == QPLAN_K) jm_movi(b, JR_S0, in->ops[2 + k].kw); /* Q_CALL_RK's constant */
        else jm_ld(b, JR_S0, JR_WIN, (int64_t)pl->slot[k] * 8);
        jm_st(b, JR_S0, t, (int64_t)pl->slot[argc + k] * 8);
    }
    jm_mov(b, JR_FR, JR_S4);
    jm_mov(b, JR_WIN, t);
    uint32_t z1 = 0, z2 = 0;
    if (fn == j->f) { /* (its code is being made: its first instruction's) */
        jfix(j, jm_ncall_self(b), TO_BODY, 0);
    } else { /* the callee's machine code for its first instruction, if it has some */
        jm_movi(b, JR_S0, (uint64_t)(uintptr_t)&fn->jit_entry);
        jm_ld(b, JR_S0, JR_S0, 0);
        z1 = jm_fwd_cbz(b, JR_S0, false);
        jm_ld(b, JR_S0, JR_S0, 0);
        z2 = jm_fwd_cbz(b, JR_S0, false);
        jm_ncall_reg(b, JR_S0);
    }
    uint32_t c = (uint32_t)b->n;
    jit_cont(j, pl, argc, pc);
    if (fn != j->f) { /* pushed, but the callee runs in the interpreter, from its first instruction */
        jm_bind(b, z1), jm_bind(b, z2);
        jexit(j, fn->code);
    }
    return c;
}

/* Process.insns as the interpreter has it before a block's last instruction,
 * for a C helper that may trap: ibase - left (the block is counted). */
static void jit_sync_insns(JComp *j, uint32_t also) {
    JBuf *b = &j->b;
    jm_ld(b, JR_S0, JR_CTX, C_OFF(ibase));
    if (also) jm_add32i(b, JR_S1, JR_LEFT, also);
    else jm_mov32(b, JR_S1, JR_LEFT);
    jm_alu(b, JA_SUB, JR_S0, JR_S0, JR_S1);
    int P = jm_proc(b, JR_S3);
    jm_st(b, JR_S0, P, P_OFF(insns));
}

/* A closure call (Q_CALLI_R, Q_CALLI_RK) from machine code: the closure is
 * checked and its frame pushed as call_rk does it (the stacks may grow); 0:
 * the interpreter's call must do it; otherwise the Ctx has the callee's frame
 * and windows, and the result is its machine code, or 1 if it has none. */
static uintptr_t jit_calli_go(Ctx *c, const Insn *in) {
    Process *p = c->p;
    if (!call_rk(p, in)) return 0;
    ctx_load(c);
    const Function *fn = c->fr->fn;
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    return ent && ent[0] ? (uintptr_t)ent[0] : 1;
}

/* After a helper that pushed or replaced the frame: 1, the interpreter runs
 * the callee from its first instruction. */
static void jit_callee_interp(JComp *j, uint32_t h) {
    JBuf *b = &j->b;
    jm_bind(b, h);
    jm_ld(b, JR_S0, JR_FR, FR_OFF(fn));
    jm_ld(b, JR_R0, JR_S0, (int64_t)offsetof(Function, code));
    jbranch(j, FX_B, 0, 0, false, TO_EXIT, 0);
}

static uint32_t jit_calli(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *pl = in->k.p;
    jm_ncall_check(j, pc);
    jm_movi(b, JR_S0, pc + 1); /* the return address; and the count, for a trap */
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    jit_sync_insns(j, 0);
    jm_mov(b, JR_A0, JR_CTX);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
    jm_ccall(b, (const void *)jit_calli_go);
    jbranch(j, FX_CB, 0, JR_R0, false, TO_UNDO, pc);
    jm_reload(b);
    jm_cmpi(b, JR_R0, 1);
    uint32_t none = jm_fwd(b, JC_EQ);
    jm_ncall_reg(b, JR_R0);
    uint32_t c = (uint32_t)b->n; /* the continuation, as a direct call's */
    jit_cont(j, pl, pl->argc, pc);
    jit_callee_interp(j, none);
    return c;
}

/* A closure tail call (Q_TCALLI_R) from machine code, as the handler makes
 * it; the same results as jit_calli_go. */
static uintptr_t jit_tcalli_go(Ctx *c, const Insn *in) {
    Process *p = c->p;
    Frame *fr = &p->frames[p->nframes - 1];
    const Operand *qo = in->ops;
    Obj *ob;
    memcpy(&ob, &p->regs[fr->base + qo[0].pad], sizeof ob);
    if (!ob || ob->kind != OBJ_CLOSURE) return 0;
    ObjClosure *cl = (ObjClosure *)ob;
    Function *fn = cl->fn;
    const MFunc *mf = fn->mf;
    int argc = (int)qo[1].imm;
    if (argc != mf->nparams || fr->base + fn->rsize > p->rcap) return 0;
    for (int k = 0; k < argc; k++)
        if (!wide_type(mf->params[k]) || REG_BANK(qo[2 + k].reg) != (int)type_bank((MlwsType)mf->params[k])) return 0;
    uint64_t args[MAX_OPERANDS];
    const uint64_t *ow = p->regs + fr->base;
    for (int k = 0; k < argc; k++) memcpy(&args[k], &ow[qo[2 + k].pad], 8);
    const Insn *ri = fr->ret_insn;
    uint8_t qret = tail_qret(ri, fr->qret, mf);
    proc_pop_frame(p);
    proc_push_frame_fitting(p, fn, ri);
    Frame *nf = &p->frames[p->nframes - 1];
    nf->closure = &cl->h;
    nf->qret = qret;
    uint64_t *nw = p->regs + nf->base;
    uint32_t cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        int bk = (int)type_bank((MlwsType)mf->params[k]);
        memcpy(&nw[fn->roff[bk] + cnt[bk]++], &args[k], 8);
    }
    ctx_load(c);
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    return ent && ent[0] ? (uintptr_t)ent[0] : 1;
}

static void jit_tcalli(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    jm_movi(b, JR_S0, pc + 1);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    jit_sync_insns(j, 0);
    jm_mov(b, JR_A0, JR_CTX);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
    jm_ccall(b, (const void *)jit_tcalli_go);
    jbranch(j, FX_CB, 0, JR_R0, false, TO_UNDO, pc);
    jm_reload(b);
    jm_cmpi(b, JR_R0, 1);
    uint32_t none = jm_fwd(b, JC_EQ);
    jm_jmpr(b, JR_R0);
    jit_callee_interp(j, none);
}

static void jit_ret(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *rp = in->k.p;
    jm_ld8(b, JR_S0, JR_FR, FR_OFF(qret));
    jbranch(j, FX_CB, 0, JR_S0, false, TO_UNDO, pc);
    jm_ld(b, JR_S1, JR_FR, FR_OFF(ret_insn));
    jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Insn, target)); /* the continuation, or 0 */
    jbranch(j, FX_CB, 0, JR_S2, false, TO_UNDO, pc);
    for (int k = 0; k < rp->nres; k++) {
        if (rp->slot[k] == QPLAN_K) jm_movi(b, JR_S0, in->ops[k].kw); /* Q_RET_RK's constant */
        else jm_ld(b, JR_S0, JR_WIN, (int64_t)rp->slot[k] * 8);
        jm_st(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
    }
    int P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp));
    jm_st(b, JR_S0, P, P_OFF(sp));
    jm_ld(b, JR_S0, JR_FR, FR_OFF(saved_fp));
    jm_st(b, JR_S0, P, P_OFF(fp));
    jm_ld32(b, JR_S0, JR_FR, FR_OFF(base));
    jm_st32(b, JR_S0, P, P_OFF(rtop));
    jm_ld32(b, JR_S0, P, P_OFF(nframes));
    jm_sub32i(b, JR_S0, JR_S0, 1);
    jm_st32(b, JR_S0, P, P_OFF(nframes));
    jm_alui(b, JA_SUB, JR_FR, JR_FR, sizeof(Frame));
    jm_nret(b, JR_S2, j->jt->mem);
}

/* A direct tail call (Q_TCALL_R): the callee's frame replaces this one at
 * the same base, as in the handler. The arguments go through Ctx.retv, since
 * the callee's window is this one's. */
static uint8_t jit_tail_qret(const Frame *fr, const Function *fn) { return tail_qret(fr->ret_insn, fr->qret, fn->mf); }

static void jit_tcall(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const Operand *qo = in->ops;
    const Function *fn = qo[0].rv.ptr;
    const MFunc *mf = fn->mf;
    int argc = (int)qo[1].imm;
    jm_ld32(b, JR_S1, JR_FR, FR_OFF(base));
    jm_add32i(b, JR_S2, JR_S1, fn->rsize);
    int P = jm_proc(b, JR_S3);
    jm_ld32(b, JR_S4, P, P_OFF(rcap));
    jm_cmp32(b, JR_S2, JR_S4);
    jbranch(j, FX_BCOND, JC_HI, 0, false, TO_UNDO, pc); /* the stacks grow on the generic path */
    for (int k = 0; k < argc; k++) {
        if (qo[2 + k].mode == OM_REG) jm_ld(b, JR_S0, JR_WIN, (int64_t)qo[2 + k].pad * 8);
        else jm_movi(b, JR_S0, qo[2 + k].kw);
        jm_st(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
    }
    jm_mov(b, JR_A0, JR_FR);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)fn);
    jm_ccall(b, (const void *)jit_tail_qret);
    jm_st8(b, JR_R0, JR_FR, FR_OFF(qret));
    jm_ld32(b, JR_S1, JR_FR, FR_OFF(base)); /* (the call kept no scratch register) */
    jm_add32i(b, JR_S2, JR_S1, fn->rsize);
    P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp)); /* popped and pushed: sp and fp are the entry sp */
    jm_st(b, JR_S0, P, P_OFF(sp));
    jm_st(b, JR_S0, P, P_OFF(fp));
    jm_movi(b, JR_S4, (uint64_t)(uintptr_t)fn);
    jm_st(b, JR_S4, JR_FR, FR_OFF(fn));
    jm_movi(b, JR_S0, 0);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    jm_stz(b, JR_FR, FR_OFF(closure));
    jm_st32(b, JR_S2, P, P_OFF(rtop));
    for (unsigned k = 0; k < fn->zlen; k++) jm_stz(b, JR_WIN, (int64_t)(fn->zoff + k) * 8);
    uint32_t cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        int bk = (int)type_bank((MlwsType)mf->params[k]);
        jm_ld(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
        jm_st(b, JR_S0, JR_WIN, (int64_t)(fn->roff[bk] + cnt[bk]++) * 8);
    }
    if (fn == j->f) {
        jbranch(j, FX_B, 0, 0, false, TO_BODY, 0);
        return;
    }
    jm_movi(b, JR_S0, (uint64_t)(uintptr_t)&fn->jit_entry);
    jm_ld(b, JR_S0, JR_S0, 0);
    uint32_t z1 = jm_fwd_cbz(b, JR_S0, false);
    jm_ld(b, JR_S0, JR_S0, 0);
    uint32_t z2 = jm_fwd_cbz(b, JR_S0, false);
    jm_jmpr(b, JR_S0);
    jm_bind(b, z1), jm_bind(b, z2);
    jexit(j, fn->code);
}

/* ---- globals, as Q_LDG_*, Q_STG_* and GLOBAL_REF: S0 the word's segment,
 * S3 the word's index in it; the tag checked (tag: 1 must be r, -1 must not
 * be r, 0 set it to `set`), anything else a side exit. Leaves S0 the
 * segment's data. ---- */
static void jit_global(JComp *j, const Insn *in, uint32_t pc, int tag, unsigned set) {
    JBuf *b = &j->b;
    uint32_t g = (uint32_t)((uint64_t)in->k.i >> 32), off = (uint32_t)in->k.i;
    int P = jm_proc(b, JR_S0);
    jm_ld32(b, JR_S1, P, P_OFF(nglobals));
    jm_movi(b, JR_S2, g);
    jm_cmp32(b, JR_S2, JR_S1);
    SIDE_IF(JC_HS);
    jm_ld(b, JR_S0, P, P_OFF(globals));
    jm_movi(b, JR_S1, (uint64_t)g * sizeof(Segment));
    jm_alu(b, JA_ADD, JR_S0, JR_S0, JR_S1);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(Segment, size));
    jm_movi(b, JR_S2, (uint64_t)off + 8);
    jm_cmp(b, JR_S1, JR_S2);
    SIDE_IF(JC_LO);
    jm_movi(b, JR_S3, off >> 3);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(Segment, tags));
    if (tag) {
        jm_ld8x(b, JR_S2, JR_S1, JR_S3);
        jm_cmpi32(b, JR_S2, T_R);
        SIDE_IF(tag > 0 ? JC_NE : JC_EQ);
    } else {
        jm_movi(b, JR_S2, set);
        jm_st8x(b, JR_S2, JR_S1, JR_S3);
    }
    jm_ld(b, JR_S0, JR_S0, (int64_t)offsetof(Segment, data));
}

/* ---- elements, as ELEM_LOAD and ELEM_STORE: a list element tagged with the
 * instruction's type, or an element of an array of it; S0 the object, ix the
 * index's register; anything else is a side exit. ---- */
static void jit_elem_access(JBuf *b, bool store, int bank, int val, int ix) { /* at S1 + ix*8 */
    if (store) bank == 1 ? jm_fstx(b, val, JR_S1, ix) : jm_stx(b, val, JR_S1, ix);
    else bank == 1 ? jm_fldx(b, val, JR_S1, ix) : jm_ldx(b, val, JR_S1, ix);
}
static void jit_elem(JComp *j, const Insn *in, uint32_t pc, int ix, bool store, int bank, int val) {
    JBuf *b = &j->b;
    unsigned type = in->type;
    SIDE_IF_NULL(JR_S0);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
    jm_cmpi32(b, JR_S1, OBJ_LIST);
    uint32_t to_arr = jm_fwd(b, JC_NE);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
    jm_cmp(b, ix, JR_S1);
    SIDE_IF(JC_HS);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
    if (store) {
        jm_movi(b, JR_S2, type);
        jm_st8x(b, JR_S2, JR_S1, ix);
    } else {
        jm_ld8x(b, JR_S2, JR_S1, ix);
        jm_cmpi32(b, JR_S2, type);
        SIDE_IF(JC_NE);
    }
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
    jit_elem_access(b, store, bank, val, ix);
    uint32_t to_done = jm_fwd(b, JC_AL);
    jm_bind(b, to_arr);
    jm_cmpi32(b, JR_S1, OBJ_ARRAY);
    SIDE_IF(JC_NE);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, len));
    jm_cmp(b, ix, JR_S1);
    SIDE_IF(JC_HS);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, elem));
    jm_cmpi32(b, JR_S1, type);
    SIDE_IF(JC_NE);
    jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, data));
    jit_elem_access(b, store, bank, val, ix);
    jm_bind(b, to_done);
}

/* A struct in S0 (from R register r), checked as IS_STRUCT checks it: of the
 * type in k. */
static void jit_struct(JComp *j, const Insn *in, uint32_t pc, unsigned r) {
    JBuf *b = &j->b;
    jld_r(j, JR_S0, r);
    SIDE_IF_NULL(JR_S0);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
    jm_cmpi32(b, JR_S1, OBJ_STRUCT);
    SIDE_IF(JC_NE);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjStruct, def));
    jm_movi(b, JR_S2, (uint64_t)(uintptr_t)in->k.p);
    jm_cmp(b, JR_S1, JR_S2);
    SIDE_IF(JC_NE);
}

/* ---- the other forms ---- */
static int jit_arith_op(uint16_t x, int *shape) {
    switch (x) {
#define X(N, o)                                                                \
    case Q_##N##_III: *shape = 0; return JA_##N;                               \
    case Q_##N##_IIK: *shape = 1; return JA_##N;                               \
    case Q_##N##_II: *shape = 2; return JA_##N;                                \
    case Q_##N##_IK: *shape = 3; return JA_##N;
        QARITH_OPS(X)
        QSHIFT_OPS(X)
#undef X
    default: return -1;
    }
}

static int jit_farith_op(uint16_t x, int *shape) {
    switch (x) {
#define X(N, o)                                                                \
    case Q_F##N##_FFF: *shape = 0; return JF_##N;                              \
    case Q_F##N##_FFK: *shape = 1; return JF_##N;                              \
    case Q_F##N##_FF: *shape = 2; return JF_##N;                               \
    case Q_F##N##_FK: *shape = 3; return JF_##N;                               \
    case Q_F##N##_FKF: *shape = 4; return JF_##N;
        QFARITH_OPS(X)
#undef X
    default: return -1;
    }
}

/* the condition of a compare-and-branch form, and its kind (0 int, 1 f64; +2: a constant) */
static int jit_branch_cond(uint16_t x, int *kind) {
    switch (x) {
#define X(N, c, cf)                                                            \
    case Q_##N##_II: *kind = 0; return c;                                      \
    case Q_##N##_IK: *kind = 2; return c;                                      \
    case Q_##N##_FFL: *kind = 1; return cf;                                    \
    case Q_##N##_FKL: *kind = 3; return cf;
        X(BEQ, JC_EQ, JC_FEQ) X(BNE, JC_NE, JC_FNE) X(BLT, JC_LT, JC_FLT)
        X(BLE, JC_LE, JC_FLE) X(BGT, JC_GT, JC_FGT) X(BGE, JC_GE, JC_FGE)
#undef X
    case Q_BLT_UU: *kind = 0; return JC_LO;
    case Q_BLE_UU: *kind = 0; return JC_LS;
    case Q_BGT_UU: *kind = 0; return JC_HI;
    case Q_BGE_UU: *kind = 0; return JC_HS;
    case Q_BLT_UK: *kind = 2; return JC_LO;
    case Q_BLE_UK: *kind = 2; return JC_LS;
    case Q_BGT_UK: *kind = 2; return JC_HI;
    case Q_BGE_UK: *kind = 2; return JC_HS;
    default: return -1;
    }
}

/* An element's index register (r[2]), plus Insn.k's offset (R1[I2+d]) in S3. */
static int jit_index(JComp *j, const Insn *in, bool plus) {
    int ix = jc_get(j, 0, in->r[2]);
    if (!plus) return ix;
    jm_alui(&j->b, JA_ADD, JR_S3, ix, in->k.i);
    return JR_S3;
}

/* Q_MGET_R's lookup, for the machine code: ok 0 is the handler's case. */
typedef struct {
    uint64_t val, ok;
} JitWord;
static JitWord jit_mget(const Obj *ob, const Insn *in) {
    if (!ob || ob->kind != OBJ_MAP) return (JitWord){0, 0};
    Word key;
    key.r = (Obj *)in->k.p;
    const MapEntry *e = map_find((ObjMap *)ob, key, T_R);
    if (e) return e->vtag == T_R ? (JitWord){(uint64_t)(uintptr_t)e->val.r, 1} : (JitWord){0, 0};
    return in->nops < 4 ? (JitWord){0, 0} : (JitWord){0, 1};
}

static void jit_insn(JComp *j, const Insn *in, uint16_t x, uint32_t pc, uint32_t *cont) {
    JBuf *b = &j->b;
    int shape, kind, s1, s2, d, op;
    uint32_t t = pc + (uint32_t)(int32_t)in->target; /* a branch's target (relative in this build) */
    j->jc.pin[0] = j->jc.pin[1] = 0;
    switch (x) {
    case Q_MOV_II:
        s1 = jc_get(j, 0, in->r[1]);
        d = jc_def(j, 0, in->r[0]);
        jm_mov(b, d, s1);
        return;
    case Q_MOV_IK: d = jc_def(j, 0, in->r[0]); jm_movi(b, d, (uint64_t)in->k.i); return;
    case Q_MOV_FF:
        s1 = jc_get(j, 1, in->r[1]);
        d = jc_def(j, 1, in->r[0]);
        jm_fmov(b, d, s1);
        return;
    case Q_MOV_FK: d = jc_def(j, 1, in->r[0]); jm_fconst(b, d, in->k.f); return;
    case Q_MOV_RR: jld_r(j, JR_S0, in->r[1]); jst_r(j, JR_S0, in->r[0]); return;
    case Q_MOV_RK: jm_movi(b, JR_S0, (uint64_t)(uintptr_t)in->k.p); jst_r(j, JR_S0, in->r[0]); return;
    case Q_INC_I: case Q_DEC_I:
        s1 = jc_get(j, 0, in->r[0]);
        d = jc_def(j, 0, in->r[0]);
        jm_alui(b, x == Q_INC_I ? JA_ADD : JA_SUB, d, s1, 1);
        return;
    case Q_DIV_IIK: case Q_DIV_IK: case Q_REM_IIK: case Q_REM_IK: {
        bool three = x == Q_DIV_IIK || x == Q_REM_IIK;
        s1 = jc_get(j, 0, in->r[three ? 1 : 0]);
        d = jc_def(j, 0, in->r[0]);
        jm_divk(b, d, s1, in->k.i, x == Q_REM_IIK || x == Q_REM_IK);
        return;
    }
    case Q_JMP: jc_flush(j); jbranch(j, FX_B, 0, 0, false, TO_BODY, t); return;
    case Q_BZ_I: case Q_BNZ_I:
        s1 = jc_get(j, 0, in->r[0]);
        jc_flush(j);
        jbranch(j, FX_CB, 0, s1, x == Q_BNZ_I, TO_BODY, t);
        return;
    case Q_CALL_R: case Q_CALL_RK: jc_flush(j); *cont = jit_call(j, in, pc); return;
    case Q_CALLI_R: case Q_CALLI_RK: jc_flush(j); *cont = jit_calli(j, in, pc); return;
    case Q_RET_R: case Q_RET_RK: jc_flush(j); jit_ret(j, in, pc); return;
    case Q_TCALL_R: jc_flush(j); jit_tcall(j, in, pc); return;
    case Q_TCALLI_R: jc_flush(j); jit_tcalli(j, in, pc); return;
    case Q_BITCAST_FI: s1 = jc_get(j, 0, in->r[1]); d = jc_def(j, 1, in->r[0]); jm_fmov_from_x(b, d, s1); return;
    case Q_BITCAST_IF: s1 = jc_get(j, 1, in->r[1]); d = jc_def(j, 0, in->r[0]); jm_fmov_to_x(b, d, s1); return;
    case Q_CVT_FI: s1 = jc_get(j, 0, in->r[1]); d = jc_def(j, 1, in->r[0]); jm_scvtf(b, d, s1); return;
    case Q_TYPEOF: {
        jld_r(j, JR_S0, in->r[1]);
        d = jc_def(j, 0, in->r[0]);
        jm_movi(b, d, OBJ_NULL);
        uint32_t null = jm_fwd_cbz(b, JR_S0, false);
        jm_ld8(b, d, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_bind(b, null);
        return;
    }
    case Q_BZ_R: case Q_BNZ_R:
        jc_flush(j);
        jld_r(j, JR_S0, in->r[0]);
        jbranch(j, FX_CB, 0, JR_S0, x == Q_BNZ_R, TO_BODY, t);
        return;
    case Q_BEQ_RR: case Q_BNE_RR: case Q_BEQ_RK: case Q_BNE_RK:
        jc_flush(j);
        jld_r(j, JR_S0, in->r[0]);
        if (x == Q_BEQ_RR || x == Q_BNE_RR) jld_r(j, JR_S1, in->r[1]);
        else jm_movi(b, JR_S1, (uint64_t)(uintptr_t)in->k.p);
        jm_cmp(b, JR_S0, JR_S1);
        jbranch(j, FX_BCOND, x == Q_BEQ_RR || x == Q_BEQ_RK ? JC_EQ : JC_NE, 0, false, TO_BODY, t);
        return;
    case Q_BEQ_EKK: case Q_BNE_EKK: { /* R0[i] (a list element tagged r) against a constant */
        uint32_t i = in->r[1] | (uint32_t)in->r[2] << 8;
        jld_r(j, JR_S0, in->r[0]);
        SIDE_IF_NULL(JR_S0);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_LIST);
        SIDE_IF(JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        jm_cmpi32(b, JR_S1, i);
        SIDE_IF(JC_LS); /* len <= i */
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
        jm_ld8(b, JR_S1, JR_S1, i);
        jm_cmpi32(b, JR_S1, T_R);
        SIDE_IF(JC_NE);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
        jm_ld(b, JR_S1, JR_S1, (int64_t)i * 8);
        jm_movi(b, JR_S2, (uint64_t)(uintptr_t)in->k.p);
        jc_flush(j);
        jm_cmp(b, JR_S1, JR_S2);
        jbranch(j, FX_BCOND, x == Q_BEQ_EKK ? JC_EQ : JC_NE, 0, false, TO_BODY, t);
        return;
    }
    case Q_LDF_I: case Q_LDF_F: case Q_LDF_R: case Q_STF_I: case Q_STF_F: case Q_STF_R: {
        bool load = x <= Q_LDF_R;
        int bank = load ? x - Q_LDF_I : x - Q_STF_I;
        int64_t off = (int64_t)offsetof(ObjStruct, fields) + (int64_t)in->target * 8;
        jit_struct(j, in, pc, in->r[load ? 1 : 0]);
        if (bank == 2) {
            if (load) jm_ld(b, JR_S1, JR_S0, off), jst_r(j, JR_S1, in->r[0]);
            else jld_r(j, JR_S1, in->r[1]), jm_st(b, JR_S1, JR_S0, off);
        } else if (load) {
            d = jc_def(j, bank, in->r[0]);
            bank ? jm_fld(b, d, JR_S0, off) : jm_ld(b, d, JR_S0, off);
        } else {
            s1 = jc_get(j, bank, in->r[1]);
            bank ? jm_fst(b, s1, JR_S0, off) : jm_st(b, s1, JR_S0, off);
        }
        return;
    }
    case Q_STFK: { /* a constant to a struct's field: its word in ops[1].kw */
        int64_t off = (int64_t)offsetof(ObjStruct, fields) + (int64_t)in->target * 8;
        jit_struct(j, in, pc, in->r[0]);
        jm_movi(b, JR_S1, in->ops[1].kw);
        jm_st(b, JR_S1, JR_S0, off);
        return;
    }
    case Q_NEW: case Q_LNEW: /* these may trap (a quota), so the frame's pc and the count are up to date */
        jc_flush(j);
        jm_movi(b, JR_S0, pc + 1);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jit_sync_insns(j, j->rest[pc] - 1); /* the rest of the block, not yet run */
        jm_mov(b, JR_A0, jm_proc(b, JR_S3));
        jm_movi(b, JR_A1, x == Q_NEW ? (uint64_t)(uintptr_t)in->k.p : (uint64_t)in->k.i);
        jm_ccall(b, x == Q_NEW ? (const void *)struct_new : (const void *)list_new);
        jst_r(j, JR_R0, in->r[0]);
        return;
    case Q_MGET_R:
        jc_flush(j);
        jld_r(j, JR_A0, in->r[1]);
        jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
        jm_ccall(b, (const void *)jit_mget);
        SIDE_IF_NULL(JR_R1);
        jst_r(j, JR_R0, in->r[0]);
        return;
    case Q_SEQ_RR: case Q_SNE_RR: case Q_SEQ_RK: case Q_SNE_RK:
        jld_r(j, JR_S0, in->r[1]);
        if (x == Q_SEQ_RR || x == Q_SNE_RR) jld_r(j, JR_S1, in->r[2]);
        else jm_movi(b, JR_S1, (uint64_t)(uintptr_t)in->k.p);
        d = jc_def(j, 0, in->r[0]);
        jm_cmp(b, JR_S0, JR_S1);
        jm_cset(b, d, x == Q_SEQ_RR || x == Q_SEQ_RK ? JC_EQ : JC_NE);
        return;
#define X(N, c)                                                                \
    case Q_##N##_FF: case Q_##N##_FK:                                          \
        s1 = jc_get(j, 1, in->r[1]);                                           \
        if (x == Q_##N##_FK) jm_fconst(b, JF_S1, in->k.f), s2 = JF_S1;         \
        else s2 = jc_get(j, 1, in->r[2]);                                      \
        d = jc_def(j, 0, in->r[0]);                                            \
        jm_fcset(b, d, c, s1, s2);                                             \
        return;
        X(SEQ, JC_FEQ) X(SNE, JC_FNE) X(SLT, JC_FLT) X(SLE, JC_FLE) X(SGT, JC_FGT) X(SGE, JC_FGE)
#undef X
    case Q_LDU_I: case Q_LDU_F: case Q_LDU_R: case Q_STU_I: case Q_STU_F: case Q_STU_R: {
        /* a capture of the frame's closure (UP[u]): of the instruction's type, for a load */
        bool load = x <= Q_LDU_R;
        int bank = load ? x - Q_LDU_I : x - Q_STU_I;
        uint32_t u = in->target;
        int64_t off = (int64_t)offsetof(ObjClosure, up) + (int64_t)u * 8;
        int val = JR_S4;
        if (!load) {
            if (bank == 2) jld_r(j, JR_S4, in->r[1]);
            else val = jc_get(j, bank, in->r[1]);
        }
        jm_ld(b, JR_S0, JR_FR, FR_OFF(closure));
        SIDE_IF_NULL(JR_S0);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjClosure, nup));
        jm_cmpi32(b, JR_S1, u);
        SIDE_IF(JC_LS); /* nup <= u */
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjClosure, tags));
        if (load) {
            jm_ld8(b, JR_S2, JR_S1, u);
            jm_cmpi32(b, JR_S2, in->type);
            SIDE_IF(JC_NE);
            if (bank == 2) jm_ld(b, JR_S2, JR_S0, off), jst_r(j, JR_S2, in->r[0]);
            else d = jc_def(j, bank, in->r[0]), bank ? jm_fld(b, d, JR_S0, off) : jm_ld(b, d, JR_S0, off);
        } else {
            jm_movi(b, JR_S2, in->type);
            jm_st8(b, JR_S2, JR_S1, u);
            bank == 1 ? jm_fst(b, val, JR_S0, off) : jm_st(b, val, JR_S0, off);
        }
        return;
    }
    case Q_ISA: {
        jld_r(j, JR_S0, in->r[1]);
        d = jc_def(j, 0, in->r[0]);
        jm_movi(b, d, 0);
        uint32_t null = jm_fwd_cbz(b, JR_S0, false);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_STRUCT);
        uint32_t other = jm_fwd(b, JC_NE);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjStruct, def));
        jm_movi(b, JR_S2, (uint64_t)(uintptr_t)in->k.p);
        jm_cmp(b, JR_S1, JR_S2);
        jm_cset(b, d, JC_EQ);
        jm_bind(b, null), jm_bind(b, other);
        return;
    }
    case Q_SQRT_F: case Q_SQRT_FF: case Q_NEG_F: case Q_NEG_FF: case Q_ABS_F: case Q_ABS_FF: {
        bool two = x == Q_SQRT_FF || x == Q_NEG_FF || x == Q_ABS_FF;
        int fop = x == Q_SQRT_F || x == Q_SQRT_FF ? JF_SQRT : x == Q_NEG_F || x == Q_NEG_FF ? JF_NEG : JF_ABS;
        s1 = jc_get(j, 1, in->r[two ? 1 : 0]);
        d = jc_def(j, 1, in->r[0]);
        jm_fun(b, fop, d, s1);
        return;
    }
    case Q_JTAB: { /* out of range: on to the next instruction */
        uint32_t nt = (uint32_t)in->nops - 1, oob;
        uint32_t *at = xmalloc((nt + 1) * sizeof *at);
        s1 = jc_get(j, 0, in->r[0]);
        jc_flush(j);
        jm_jtab(b, s1, nt, at, &oob);
        jfix(j, oob, TO_BODY, pc + 1);
        for (uint32_t k = 0; k < nt; k++) jfix(j, at[k], TO_BODY, in->ops[1 + k].rv.target);
        free(at);
        return;
    }
    case Q_LDV_E: case Q_LDV_ED: case Q_LDV_EK: { /* the number half of a value, into the bank its tag picks */
        int ix;
        if (x == Q_LDV_EK) {
            jm_movi(b, JR_S3, in->target), ix = JR_S3;
        } else {
            ix = jc_get(j, 0, (int)in->target);
            if (x == Q_LDV_ED) jm_alui(b, JA_ADD, JR_S3, ix, (int8_t)in->r[2]), ix = JR_S3;
        }
        jld_r(j, JR_S0, (unsigned)in->k.i);
        SIDE_IF_NULL(JR_S0);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_LIST);
        uint32_t to_arr = jm_fwd(b, JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        jm_cmp(b, ix, JR_S1);
        SIDE_IF(JC_HS);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
        jm_ld8x(b, JR_S2, JR_S1, ix);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
        jm_ldx(b, JR_S4, JR_S1, ix);
        uint32_t to_tag = jm_fwd(b, JC_AL);
        jm_bind(b, to_arr);
        jm_cmpi32(b, JR_S1, OBJ_ARRAY);
        SIDE_IF(JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, len));
        jm_cmp(b, ix, JR_S1);
        SIDE_IF(JC_HS);
        jm_ld8(b, JR_S2, JR_S0, (int64_t)offsetof(ObjArray, elem));
        jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, data));
        jm_ldx(b, JR_S4, JR_S1, ix);
        jm_bind(b, to_tag);
        /* the tag in S2, the word in S4: i64, f64 or none (zeros), else the handler's */
        jm_cmpi32(b, JR_S2, T_I64);
        uint32_t ok1 = jm_fwd(b, JC_EQ);
        jm_cmpi32(b, JR_S2, T_F64);
        uint32_t ok2 = jm_fwd(b, JC_EQ);
        jm_cmpi32(b, JR_S2, T_NONE);
        SIDE_IF(JC_NE);
        jm_bind(b, ok1), jm_bind(b, ok2);
        int di = jc_def(j, 0, in->r[0]), df = jc_def(j, 1, in->r[1]);
        jm_cmpi32(b, JR_S2, T_I64);
        jm_cselz(b, di, JR_S4, JC_EQ);
        jm_cmpi32(b, JR_S2, T_F64);
        jm_cselz(b, JR_S3, JR_S4, JC_EQ);
        jm_fmov_from_x(b, df, JR_S3);
        return;
    }
    case Q_BISA: case Q_BNISA: { /* a struct of the type in k: branch (BISA) or not */
        jc_flush(j);
        bool is = x == Q_BISA;
        uint32_t null = 0, other = 0;
        jld_r(j, JR_S0, in->r[0]);
        if (is) null = jm_fwd_cbz(b, JR_S0, false);
        else jbranch(j, FX_CB, 0, JR_S0, false, TO_BODY, t);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_STRUCT);
        if (is) other = jm_fwd(b, JC_NE);
        else jbranch(j, FX_BCOND, JC_NE, 0, false, TO_BODY, t);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjStruct, def));
        jm_movi(b, JR_S2, (uint64_t)(uintptr_t)in->k.p);
        jm_cmp(b, JR_S1, JR_S2);
        jbranch(j, FX_BCOND, is ? JC_EQ : JC_NE, 0, false, TO_BODY, t);
        if (is) jm_bind(b, null), jm_bind(b, other);
        return;
    }
    case Q_BEQ_RG: case Q_BNE_RG:
        jit_global(j, in, pc, 1, 0);
        jm_ldx(b, JR_S1, JR_S0, JR_S3);
        jc_flush(j);
        jld_r(j, JR_S0, in->r[0]);
        jm_cmp(b, JR_S0, JR_S1);
        jbranch(j, FX_BCOND, x == Q_BEQ_RG ? JC_EQ : JC_NE, 0, false, TO_BODY, t);
        return;
    case Q_LDG_I: case Q_LDG_F: case Q_LDG_R: {
        int bank = x - Q_LDG_I;
        jit_global(j, in, pc, bank == 2 ? 1 : -1, 0);
        if (bank == 2) jm_ldx(b, JR_S1, JR_S0, JR_S3), jst_r(j, JR_S1, in->r[0]);
        else d = jc_def(j, bank, in->r[0]), bank ? jm_fldx(b, d, JR_S0, JR_S3) : jm_ldx(b, d, JR_S0, JR_S3);
        return;
    }
    case Q_STG_I: case Q_STG_F: case Q_STG_R: {
        int bank = x - Q_STG_I;
        if (bank != 2) s1 = jc_get(j, bank, in->r[1]);
        else s1 = -1;
        jit_global(j, in, pc, 0, in->type);
        if (bank == 2) jld_r(j, JR_S1, in->r[1]), jm_stx(b, JR_S1, JR_S0, JR_S3);
        else bank ? jm_fstx(b, s1, JR_S0, JR_S3) : jm_stx(b, s1, JR_S0, JR_S3);
        return;
    }
    case Q_LPUSH_R: case Q_LPUSH_RK: { /* into the list's spare capacity; growing it is the handler's */
        unsigned nv = (unsigned)in->nops - 1;
        int vals[MAX_OPERANDS];
        for (unsigned k = 0; k < nv; k++) { /* the values first: a load may evict from the cache */
            const Operand *a = &in->ops[1 + k];
            vals[k] = -1;
            if (a->mode == OM_REG && REG_BANK(a->reg) != BANK_R)
                vals[k] = jc_get(j, REG_BANK(a->reg) == BANK_I ? 0 : 1, REG_IDX(a->reg));
        }
        jld_r(j, JR_S0, in->r[0]);
        SIDE_IF_NULL(JR_S0);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_LIST);
        SIDE_IF(JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        jm_ld32(b, JR_S2, JR_S0, (int64_t)offsetof(ObjList, cap));
        jm_sub32(b, JR_S2, JR_S2, JR_S1);
        jm_cmpi32(b, JR_S2, nv);
        SIDE_IF(JC_LO);
        jm_ld(b, JR_S3, JR_S0, (int64_t)offsetof(ObjList, items));
        jm_addsh3(b, JR_S3, JR_S3, JR_S1);
        jm_ld(b, JR_S4, JR_S0, (int64_t)offsetof(ObjList, tags));
        jm_alu(b, JA_ADD, JR_S4, JR_S4, JR_S1);
        for (unsigned k = 0; k < nv; k++) {
            const Operand *a = &in->ops[1 + k];
            unsigned tg;
            if (a->mode != OM_REG) {
                jm_movi(b, JR_S2, a->kw);
                jm_st(b, JR_S2, JR_S3, (int64_t)k * 8);
                tg = a->pad;
            } else if (REG_BANK(a->reg) == BANK_I) {
                jm_st(b, vals[k], JR_S3, (int64_t)k * 8), tg = T_I64;
            } else if (REG_BANK(a->reg) == BANK_F) {
                jm_fst(b, vals[k], JR_S3, (int64_t)k * 8), tg = T_F64;
            } else {
                jld_r(j, JR_S2, (unsigned)REG_IDX(a->reg));
                jm_st(b, JR_S2, JR_S3, (int64_t)k * 8), tg = T_R;
            }
            jm_movi(b, JR_S2, tg);
            jm_st8(b, JR_S2, JR_S4, k);
        }
        jm_add32i(b, JR_S1, JR_S1, nv);
        jm_st32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        return;
    }
    case Q_LEN: {
        static const struct {
            uint8_t kind;
            uint16_t off;
        } kinds[] = {{OBJ_LIST, offsetof(ObjList, len)}, {OBJ_STRING, offsetof(ObjString, len)}, {OBJ_ARRAY, offsetof(ObjArray, len)}};
        uint32_t done[3];
        jld_r(j, JR_S0, in->r[1]);
        SIDE_IF_NULL(JR_S0);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        for (int k = 0; k < 3; k++) {
            jm_cmpi32(b, JR_S1, kinds[k].kind);
            uint32_t skip = jm_fwd(b, JC_NE);
            jm_ld32(b, JR_S2, JR_S0, kinds[k].off);
            done[k] = jm_fwd(b, JC_AL);
            jm_bind(b, skip);
        }
        jside(j, FX_B, 0, 0, false, pc); /* a map or bytes: the handler's */
        for (int k = 0; k < 3; k++) jm_bind(b, done[k]);
        d = jc_def(j, 0, in->r[0]);
        jm_mov(b, d, JR_S2);
        return;
    }
    case Q_LDE_I: case Q_LDE_F: case Q_LDE_R: case Q_LDEK_I: case Q_LDEK_F: case Q_LDEK_R:
    case Q_LDED_I: case Q_LDED_F: case Q_LDED_R: {
        int bank = x >= Q_LDED_I ? x - Q_LDED_I : x >= Q_LDEK_I ? x - Q_LDEK_I : x - Q_LDE_I;
        int ix;
        if (x >= Q_LDEK_I && x <= Q_LDEK_R) jm_movi(b, JR_S3, in->target), ix = JR_S3;
        else ix = jit_index(j, in, x >= Q_LDED_I);
        jld_r(j, JR_S0, in->r[1]);
        jit_elem(j, in, pc, ix, false, bank, bank == 1 ? JF_S0 : JR_S4);
        if (bank == 2) jst_r(j, JR_S4, in->r[0]);
        else if (bank == 1) d = jc_def(j, 1, in->r[0]), jm_fmov(b, d, JF_S0);
        else d = jc_def(j, 0, in->r[0]), jm_mov(b, d, JR_S4);
        return;
    }
    case Q_STE_I: case Q_STE_F: case Q_STE_R: case Q_STEK_I: case Q_STEK_F: case Q_STEK_R:
    case Q_STED_I: case Q_STED_F: case Q_STED_R: {
        int bank = x >= Q_STED_I ? x - Q_STED_I : x >= Q_STEK_I ? x - Q_STEK_I : x - Q_STE_I;
        int ix, val;
        if (x >= Q_STEK_I && x <= Q_STEK_R) jm_movi(b, JR_S3, in->target), ix = JR_S3;
        else ix = jit_index(j, in, x >= Q_STED_I);
        if (bank == 2) jld_r(j, JR_S4, in->r[1]), val = JR_S4;
        else val = jc_get(j, bank, in->r[1]);
        jld_r(j, JR_S0, in->r[0]);
        jit_elem(j, in, pc, ix, true, bank, val);
        return;
    }
    default: break;
    }
    if ((op = jit_arith_op(x, &shape)) >= 0) { /* (shifts: the count modulo 64, as shiftop) */
        s1 = jc_get(j, 0, in->r[shape <= 1 ? 1 : 0]);
        if (shape == 0 || shape == 2) {
            s2 = jc_get(j, 0, in->r[shape == 0 ? 2 : 1]);
            d = jc_def(j, 0, in->r[0]);
            jm_alu(b, op, d, s1, s2);
        } else if (op == JA_ADD || op == JA_SUB) {
            d = jc_def(j, 0, in->r[0]);
            jm_alui(b, op, d, s1, in->k.i);
        } else {
            jm_movi(b, JR_S1, (uint64_t)in->k.i);
            d = jc_def(j, 0, in->r[0]);
            jm_alu(b, op, d, s1, JR_S1);
        }
        return;
    }
    if ((op = jit_farith_op(x, &shape)) >= 0) {
        switch (shape) {
        case 0: s1 = jc_get(j, 1, in->r[1]); s2 = jc_get(j, 1, in->r[2]); break;
        case 1: s1 = jc_get(j, 1, in->r[1]); jm_fconst(b, JF_S1, in->k.f); s2 = JF_S1; break;
        case 2: s1 = jc_get(j, 1, in->r[0]); s2 = jc_get(j, 1, in->r[1]); break;
        case 3: s1 = jc_get(j, 1, in->r[0]); jm_fconst(b, JF_S1, in->k.f); s2 = JF_S1; break;
        default: jm_fconst(b, JF_S0, in->k.f); s1 = JF_S0; s2 = jc_get(j, 1, in->r[2]); break;
        }
        d = jc_def(j, 1, in->r[0]);
        jm_fop(b, op, d, s1, s2);
        return;
    }
    int cond = jit_branch_cond(x, &kind);
    if (cond >= 0) {
        if (kind & 1) {
            s1 = jc_get(j, 1, in->r[0]);
            if (kind & 2) jm_fconst(b, JF_S1, in->k.f), s2 = JF_S1;
            else s2 = jc_get(j, 1, in->r[1]);
            jc_flush(j);
            uint32_t at[2];
            int n = jm_fbranch(b, cond, s1, s2, at);
            for (int k = 0; k < n; k++) jfix(j, at[k], TO_BODY, t);
            return;
        }
        s1 = jc_get(j, 0, in->r[0]);
        if (kind & 2) {
            jc_flush(j);
            jm_cmpi(b, s1, in->k.i);
        } else {
            s2 = jc_get(j, 0, in->r[1]);
            jc_flush(j);
            jm_cmp(b, s1, s2);
        }
        jbranch(j, FX_BCOND, cond, 0, false, TO_BODY, t);
        return;
    }
    jm_brk(b); /* not reached: jit_handles agrees with this switch */
}

/* Whether entering the machine code at leader L pays: the code it can run
 * from there before it must stop for the interpreter has a loop or a call,
 * or is long, and steps few instructions. Each entry and exit, and each step,
 * costs about as much as a few instructions in the interpreter, so a short
 * run between two instructions the interpreter runs would be slower than the
 * interpreter alone. */
#define JIT_RUN_MIN 16
#define JIT_STEP_COST 32
static bool jit_worth_entering(const Function *f, const uint16_t *xop, uint32_t L, uint8_t *mark, uint32_t *stack) {
    uint32_t n = f->ncode, sp = 0, seen = 0, stepped = 0;
    bool loop = false;
    memset(mark, 0, n);
    stack[sp++] = L;
    mark[L] = 1;
    while (sp && !loop) { /* depth first, with 1 on the path and 2 done */
        uint32_t pc = stack[sp - 1];
        uint32_t succ[2], ns = 0;
        uint16_t x = xop[pc];
        if (!jit_handles(x) || !jit_ends_block(x)) succ[ns++] = pc + 1; /* (a stepped one: on, if it does not branch) */
        else if (jit_calls(x) || x == Q_JTAB) loop = true; /* it goes on in the callee's machine code, if it has some */
        else if (jit_branches(x)) {
            succ[ns++] = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (x != Q_JMP) succ[ns++] = pc + 1;
        }
        bool pushed = false;
        for (uint32_t k = 0; k < ns && !pushed; k++) {
            uint32_t t = succ[k];
            if (t >= n || (!jit_handles(xop[t]) && !jit_steps(&f->code[t], xop[t]))) continue;
            if (mark[t] == 1) { /* a loop: what runs to its head is what counts */
                loop = true;
                break;
            }
            if (mark[t]) continue;
            mark[t] = 1;
            stack[sp++] = t;
            if (jit_handles(xop[t])) seen++;
            else stepped++;
            pushed = true;
        }
        if (!pushed) mark[pc] = 2, sp--;
    }
    return (loop || seen + 1 >= JIT_RUN_MIN) && seen + 1 >= JIT_STEP_COST * stepped;
}

/* Compile f (under vm->jit_lock): its machine code, its blocks' entries, and
 * Q_JIT where entering pays; its Q_JITC triggers go. */
static void jit_compile(VM *vm, Function *f) {
    if (f->jit_state) return;
    uint64_t t0 = plat_now_ns();
    uint32_t n = f->ncode;
    uint16_t *xop = f->jit_xop;
    if (!xop) {
        xop = xmalloc((n + 1) * sizeof *xop);
        for (uint32_t pc = 0; pc < n; pc++) xop[pc] = f->code[pc].xop;
    }
    bool any = false;
    for (uint32_t pc = 0; pc < n && !any; pc++) any = jit_handles(xop[pc]);
    Jit *jt = any ? jit_get(vm) : NULL;
    if (!jt) {
        f->jit_state = JIT_NONE;
        if (f->jit_xop) /* the triggers go */
            for (uint32_t pc = 0; pc < n; pc++) __atomic_store_n(&f->code[pc].xop, xop[pc], __ATOMIC_RELAXED);
        else free(xop);
        return;
    }
    /* Blocks, and rest[pc]: the instructions from pc to its block's end. */
    uint8_t *lead = xcalloc(n + 1, 1);
    uint32_t *rest = xcalloc(n + 1, sizeof *rest);
    for (uint32_t pc = 0; pc < n; pc++) {
        if (pc == 0 || !jit_handles(xop[pc - 1]) || jit_ends_block(xop[pc - 1])) lead[pc] = 1;
        if (xop[pc] == Q_JTAB)
            for (int k = 1; k < f->code[pc].nops; k++)
                if (f->code[pc].ops[k].rv.target < n) lead[f->code[pc].ops[k].rv.target] = 1;
        if (jit_handles(xop[pc]) && jit_branches(xop[pc])) {
            uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (t < n) lead[t] = 1;
        }
    }
    for (uint32_t pc = n; pc-- > 0;) {
        if (jit_steps(&f->code[pc], xop[pc])) rest[pc] = 1;
        if (!jit_handles(xop[pc])) continue;
        bool last = pc + 1 >= n || lead[pc + 1] || !jit_handles(xop[pc + 1]) || jit_ends_block(xop[pc]);
        rest[pc] = last ? 1 : rest[pc + 1] + 1;
        if (rest[pc] > 4000) rest[pc] = 1, lead[pc + 1] = 1; /* (a block's length fits in 12 bits) */
    }
    JComp jcomp = {.jt = jt, .f = f, .rest = rest};
    JComp *j = &jcomp;
    JBuf *b = &j->b;
    jc_reset(&j->jc);
    memset(j->jc.next, 0, sizeof j->jc.next);
    uint32_t *body = xcalloc(n + 1, sizeof *body), *undo = xcalloc(n + 1, sizeof *undo);
    uint32_t *cont = xcalloc(n + 1, sizeof *cont);
    uint32_t nnat = 0;
    for (uint32_t pc = 0; pc < n; pc++) {
        body[pc] = (uint32_t)b->n;
        const Insn *in = &f->code[pc];
        if (jit_steps(in, xop[pc])) { /* a block of its own: counted, then run by its handler */
            jm_subs32i(b, JR_LEFT, JR_LEFT, 1);
            jbranch(j, FX_BCOND, JC_LO, 0, false, TO_UNDO, pc);
            jm_mov(b, JR_A0, JR_CTX);
            jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
            jm_mov32(b, JR_A2, JR_LEFT);
            jm_ccall(b, (const void *)jit_step);
            jm_mov32(b, JR_LEFT, JR_R1);
            uint32_t stop = jm_fwd_cbz(b, JR_R0, false);
            jm_reload(b);
            jm_jmpr(b, JR_R0);
            jm_bind(b, stop);
            jm_ld(b, JR_R0, JR_CTX, C_OFF(jnext));
            jbranch(j, FX_B, 0, 0, false, TO_EXIT, 0);
            continue;
        }
        if (!jit_handles(xop[pc])) { /* the interpreter's: stop here */
            jexit(j, in);
            continue;
        }
        nnat++;
        if (lead[pc]) { /* the block's header: count it, or stop before it */
            jm_subs32i(b, JR_LEFT, JR_LEFT, rest[pc]);
            jbranch(j, FX_BCOND, JC_LO, 0, false, TO_UNDO, pc);
        }
        jit_insn(j, in, xop[pc], pc, &cont[pc]);
        if (rest[pc] == 1) jc_flush(j); /* the block ends: back to the window */
    }
    jm_brk(b); /* past the end: not reached (the verifier ends every path) */
    uint32_t *side = xcalloc(j->sides.n + 1, sizeof *side);
    for (size_t k = 0; k < j->sides.n; k++) {
        const JSide *sd = &j->sides.v[k];
        side[k] = (uint32_t)b->n;
        for (int r = 0; r < sd->n; r++) {
            if (sd->bank[r]) jm_fst(b, sd->mreg[r], JR_WIN, jwoff(j, BANK_F, sd->vreg[r]));
            else jm_st(b, sd->mreg[r], JR_WIN, jwoff(j, BANK_I, sd->vreg[r]));
        }
        jm_add32i(b, JR_LEFT, JR_LEFT, rest[sd->pc]);
        jexit(j, &f->code[sd->pc]);
    }
    for (uint32_t pc = 0; pc < n; pc++) { /* give the count back and stop before pc */
        if (!jit_handles(xop[pc]) && !jit_steps(&f->code[pc], xop[pc])) continue;
        undo[pc] = (uint32_t)b->n;
        jm_add32i(b, JR_LEFT, JR_LEFT, rest[pc]);
        jexit(j, &f->code[pc]);
    }
    uint8_t *at = jit_reserve(jt, b->n);
    if (at) {
        for (size_t k = 0; k < j->fx.n; k++) {
            const JFix *x = &j->fx.v[k];
            int64_t dest = x->to == TO_BODY ? body[x->pc] : x->to == TO_SIDE ? side[x->pc] : x->to == TO_UNDO ? undo[x->pc]
                                                                                                         : jt->exit - at;
            jm_patch_at(b->v, x->at, dest);
        }
        if (!jit_install(jt, at, b)) at = NULL;
    }
    if (at) {
        char nm[256];
        snprintf(nm, sizeof nm, "f%llu_%s", (unsigned long long)jt->nfuncs, f->name);
        for (char *q = nm; *q; q++)
            if (!isalnum((unsigned char)*q)) *q = '_';
        jit_dump(nm, b);
        void **ent = xcalloc(n, sizeof *ent);
        uint8_t *mark = xmalloc(n);
        uint32_t *stack = xmalloc(n * sizeof *stack);
        bool *enter = xcalloc(n, sizeof *enter);
        for (uint32_t pc = 0; pc < n; pc++) {
            if (jit_steps(&f->code[pc], xop[pc])) ent[pc] = at + body[pc];
            if (!jit_handles(xop[pc])) continue;
            if (cont[pc]) /* the continuation, before any return can look for it */
                __atomic_store_n(&f->code[pc].target, (uint32_t)(at + cont[pc] - jt->mem), __ATOMIC_RELEASE);
            if (!lead[pc]) continue;
            ent[pc] = at + body[pc]; /* for calls from machine code */
            enter[pc] = jit_worth_entering(f, xop, pc, mark, stack);
        }
        f->jit_xop = xop;
        f->jit_native = nnat;
        __atomic_store_n(&f->jit_entry, ent, __ATOMIC_RELEASE);
        for (uint32_t pc = 0; pc < n; pc++)
            __atomic_store_n(&f->code[pc].xop, enter[pc] ? (uint16_t)Q_JIT : xop[pc], __ATOMIC_RELEASE);
        f->jit_state = JIT_COMPILED;
        free(mark), free(stack), free(enter);
        jt->nfuncs++;
        jt->ninsns += n;
        jt->nnative += nnat;
        jt->bytes += b->n;
    } else { /* no room: it stays the interpreter's */
        jt->no_memory = true;
        f->jit_state = JIT_NONE;
        if (f->jit_xop)
            for (uint32_t pc = 0; pc < n; pc++) __atomic_store_n(&f->code[pc].xop, xop[pc], __ATOMIC_RELAXED);
        else free(xop);
    }
    jt->ns += plat_now_ns() - t0;
    free(lead), free(rest), free(body), free(undo), free(cont), free(side), free(j->sides.v);
    free(b->v), free(j->fx.v);
}

/* At load, after quickening: with --jit all, compile f now; with --jit on,
 * its first instruction and its loops' heads count towards compiling it. A
 * run with a tool's hook (debugger, tracer, profiler) interprets. */
void interp_jit_function(VM *vm, Function *f) {
    if (!vm->jit_mode || vm->hook || !f->ncode) return;
    if (vm->jit_mode == JIT_MODE_ALL) {
        plat_lock(vm->jit_lock);
        jit_compile(vm, f);
        plat_unlock(vm->jit_lock);
        return;
    }
    uint32_t n = f->ncode;
    uint16_t *xop = xmalloc((n + 1) * sizeof *xop);
    bool any = false;
    for (uint32_t pc = 0; pc < n; pc++) {
        xop[pc] = f->code[pc].xop;
        any = any || jit_handles(xop[pc]);
    }
    if (!any) {
        free(xop);
        f->jit_state = JIT_NONE;
        return;
    }
    f->jit_xop = xop;
    f->code[0].xop = Q_JITC;
    for (uint32_t pc = 0; pc < n; pc++) {
        if (jit_handles(xop[pc]) && jit_branches(xop[pc])) {
            uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (t <= pc) f->code[t].xop = Q_JITC; /* a loop's head */
        }
        if (xop[pc] == Q_JTAB)
            for (int k = 1; k < f->code[pc].nops; k++)
                if (f->code[pc].ops[k].rv.target <= pc) f->code[f->code[pc].ops[k].rv.target].xop = Q_JITC;
    }
}

bool interp_jit_available(void) { return true; }

/* ---- the handlers ---- */

/* A count towards compiling the function, then the instruction's own form
 * (or, once compiled, its machine code). */
static QCC int h_Q_JITC(QPARAMS) {
    Function *fn = (Function *)QFRAME->fn;
    uint32_t pc = (uint32_t)(in - QCODE);
    if (!c->hooked && __atomic_add_fetch(&fn->jit_count, 1, __ATOMIC_RELAXED) == c->vm->jit_hot) {
        plat_lock(c->vm->jit_lock);
        jit_compile(c->vm, fn);
        plat_unlock(c->vm->jit_lock);
    }
    uint16_t x = __atomic_load_n(&in->xop, __ATOMIC_ACQUIRE);
    if (x == Q_JITC || (x == Q_JIT && c->hooked)) x = fn->jit_xop[pc];
    MUSTTAIL return qhandlers[x](QARGS);
}

/* The interpreter reached a block's first instruction: run the machine code
 * from there, then go on with the instruction it stopped at, as NEXT would. */
static QCC int h_Q_JIT(QPARAMS) {
    uint32_t pc = (uint32_t)(in - QCODE);
    const Function *fn = QFRAME->fn;
    if (c->hooked) MUSTTAIL return qhandlers[fn->jit_xop[pc]](QARGS); /* a tool sees each instruction */
    Jit *jt = c->vm->jit;
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    JitExit r = jt->enter(c, left + 1, ent[pc]);
    if (jt->stats && r.in) {
        __atomic_fetch_add(&jt->entries, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&jt->ran, left + 1 - (uint32_t)r.left, __ATOMIC_RELAXED);
        const Process *pp = c->p;
        uint16_t sx = jit_form(pp->frames[pp->nframes - 1].fn, r.in);
        __atomic_fetch_add(&jt->stops[sx], 1, __ATOMIC_RELAXED);
        jt->opof[sx] = r.in->op;
    }
    left = (uint32_t)r.left;
    const Insn *nx = r.in;
    if (!nx) return c->jstatus; /* a stepped instruction ended the slice (its handler's state is the process's) */
    ctx_load(c); /* the machine code kept the frame and windows to itself */
    I = c->I, F = c->F, R = c->R;
    QSETFRAME();
    if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx));
    left--;
    /* (its own form: at a block's first instruction, the block did not fit in the slice) */
    uint16_t x = __atomic_load_n(&nx->xop, __ATOMIC_RELAXED);
    if (x == Q_JIT) x = QFRAME->fn->jit_xop[nx - QCODE];
    MUSTTAIL return qhandlers[x](QARGSN(nx));
}

/* ---- the report (--jit-report) ---- */
void interp_jit_report(VM *vm, FILE *out) {
    Jit *jt = vm->jit;
    if (vm->jit_mode == JIT_MODE_ALL) fprintf(out, "mlwsvm jit (%s): every function compiled as it is loaded\n", JIT_ARCH);
    else fprintf(out, "mlwsvm jit (%s): a function compiled after %u calls or loop iterations\n", JIT_ARCH, vm->jit_hot);
    if (!jt) {
        fprintf(out, "  %s\n", vm->jit_failed ? "no code memory: nothing compiled" : "nothing compiled");
        return;
    }
    fprintf(out, "  functions compiled  %llu\n", (unsigned long long)jt->nfuncs);
    fprintf(out, "  instructions        %llu of %llu compiled (%.1f%%)\n", (unsigned long long)jt->nnative,
            (unsigned long long)jt->ninsns, jt->ninsns ? 100.0 * (double)jt->nnative / (double)jt->ninsns : 0.0);
    fprintf(out, "  machine code        %llu bytes, in %.2f ms\n", (unsigned long long)jt->bytes, (double)jt->ns / 1e6);
    if (jt->no_memory) fprintf(out, "  (the code memory filled: later functions were not compiled)\n");
    if (!jt->stats) return;
    fprintf(out, "  entries             %llu, %llu instructions run in machine code (%.1f per entry)\n",
            (unsigned long long)jt->entries, (unsigned long long)jt->ran,
            jt->entries ? (double)jt->ran / (double)jt->entries : 0.0);
    fprintf(out, "  stepped             %llu instructions\n", (unsigned long long)jt->nsteps);
    uint64_t steps[Q__COUNT], stops[Q__COUNT];
    memcpy(steps, jt->steps, sizeof steps);
    memcpy(stops, jt->stops, sizeof stops);
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int x = 0; x < Q__COUNT; x++)
            if (steps[x] && (best < 0 || steps[x] > steps[best])) best = x;
        if (best < 0) break;
        if (!k) fprintf(out, "  most stepped (forms the JIT does not compile):\n");
        fprintf(out, "    %12llu  %s (form %d)\n", (unsigned long long)steps[best], op_info[jt->opof[best]].mnem, best);
        steps[best] = 0;
    }
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int x = 0; x < Q__COUNT; x++)
            if (stops[x] && (best < 0 || stops[x] > stops[best])) best = x;
        if (best < 0) break;
        if (!k) fprintf(out, "  most common stops (where the interpreter took over):\n");
        fprintf(out, "    %12llu  %s (form %d)\n", (unsigned long long)stops[best], op_info[jt->opof[best]].mnem, best);
        stops[best] = 0;
    }
}

uint64_t interp_jit_steps(const VM *vm) { return vm->jit ? vm->jit->nsteps : 0; }
#endif
