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
 * all, every function is compiled as it is loaded, and the machine code is
 * entered at every block, not only where it pays: a test of all of it.
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
#if MLWS_TAILCALL && (defined(__aarch64__) || defined(__x86_64__))
#define MLWS_JIT 1
#include "../platform/platform.h"

/* The C functions the machine code calls, and the entry it is called at,
 * use System V's convention on Windows x86-64 too (Clang's sysv_abi): one
 * encoder serves both, and the C code around a call saves what Windows'
 * convention keeps and System V's does not (rsi, rdi, xmm6-15). Windows on
 * arm64 has the standard arm64 convention, and reserves x18, which the
 * arm64 encoder never uses (macOS reserves it too). */
#if defined(_WIN32) && defined(__x86_64__)
#define JIT_CC __attribute__((sysv_abi))
#else
#define JIT_CC
#endif

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

#if defined(__x86_64__)
/* Hacker's Delight (2nd ed.), Fig. 10-1 adapted to 64-bit signed integers.
 * Valid for all d with |d| >= 3 and not a power of 2. */
typedef struct {
    int64_t m;
    int s;
    int add; /* +1: add dividend; -1: sub dividend; 0: neither */
} JMagic;
static JMagic jit_magic64(int64_t d) {
    uint64_t ad = d < 0 ? (uint64_t)(0 - (uint64_t)d) : (uint64_t)d;
    uint64_t t = ((uint64_t)1 << 63) + ((uint64_t)d >> 63);
    uint64_t anc = t - 1 - t % ad;
    int p = 63;
    uint64_t q1 = ((uint64_t)1 << 63) / anc;
    uint64_t r1 = ((uint64_t)1 << 63) - q1 * anc;
    uint64_t q2 = ((uint64_t)1 << 63) / ad;
    uint64_t r2 = ((uint64_t)1 << 63) - q2 * ad;
    uint64_t delta;
    do {
        p++;
        q1 = 2 * q1;
        r1 = 2 * r1;
        if (r1 >= anc) { q1++; r1 -= anc; }
        q2 = 2 * q2;
        r2 = 2 * r2;
        if (r2 >= ad) { q2++; r2 -= ad; }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    int64_t m = (int64_t)(q2 + 1);
    if (d < 0) m = (int64_t)(0 - (uint64_t)m);
    int add = d > 0 && m < 0 ? 1 : d < 0 && m > 0 ? -1 : 0;
    return (JMagic){m, p - 64, add};
}
#endif

#if defined(__aarch64__)
#include "jit_a64.inc"
#else
#include "jit_x64.inc"
#endif

#define JC_MAX 24
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
    JIT_CC JitExit (*enter)(Ctx *c, uint32_t left, void *target);
    uint8_t *exit;
    bool no_memory;
    bool stats; /* count entries, steps and stops (--jit-report) */
    uint64_t nfuncs, ninsns, nnative, nloops, ncalli, nspec, bytes, ns;
    uint64_t entries, ran, nsteps;
    uint64_t stops[Q__COUNT], steps[Q__COUNT];
    uint16_t opof[Q__COUNT];
    VEC(uintptr_t *) words; /* blocks of the words the machine code keeps data in (jit_word) */
    size_t nwords;          /* used of the last block */
};
enum { JIT_COMPILED = 1, JIT_NONE = 2 }; /* Function.jit_state */
#define JIT_WORDS 512

/* A word of data for the machine code, zero, kept for the VM's life (the
 * code itself is never written again). Under the JIT lock. */
static uintptr_t *jit_word(Jit *jt) {
    if (!jt->words.n || jt->nwords == JIT_WORDS) {
        VEC_PUSH(jt->words, xcalloc(JIT_WORDS, sizeof(uintptr_t)));
        jt->nwords = 0;
    }
    return &jt->words.v[jt->words.n - 1][jt->nwords++];
}

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

/* MLWSVM_JIT_MAP=FILE: where each compiled instruction's code starts, for a
 * profiler's addresses: a line "ADDRESS FUNCTION PC FORM" per instruction,
 * then "ADDRESS FUNCTION end" where its stubs start, and the function's end. */
static void jit_map(const Function *f, const uint8_t *at, const uint32_t *body, uint32_t n, size_t stubs,
                    size_t len) {
    static FILE *mf;
    static int tried;
    if (!__atomic_exchange_n(&tried, 1, __ATOMIC_RELAXED)) {
        const char *path = getenv("MLWSVM_JIT_MAP");
        if (path) mf = fopen(path, "w");
    }
    if (!mf) return;
    for (uint32_t pc = 0; pc < n; pc++) {
        if (pc && body[pc] == 0) continue; /* (not compiled) */
        fprintf(mf, "%llx %s %u %s\n", (unsigned long long)(uintptr_t)(at + body[pc]), f->name, pc,
                op_info[f->code[pc].op].mnem);
    }
    fprintf(mf, "%llx %s stubs -\n%llx %s end -\n", (unsigned long long)(uintptr_t)(at + stubs), f->name,
            (unsigned long long)(uintptr_t)(at + len), f->name);
    fflush(mf);
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
    jt->enter = (JIT_CC JitExit(*)(Ctx *, uint32_t, void *))(void *)at;
    jt->exit = at + exit_at;
    free(b.v);
    vm->jit = jt;
    return jt;
}

void interp_jit_free(VM *vm) {
    Jit *jt = vm->jit;
    if (!jt) return;
    plat_code_free(jt->mem, JIT_MEM_SIZE);
    for (size_t i = 0; i < jt->words.n; i++) free(jt->words.v[i]);
    VEC_FREE(jt->words);
    free(jt);
    vm->jit = NULL;
}

/* ---- a function being compiled ---- */
typedef struct {
    uint32_t pc;
    uint8_t n;
    uint8_t bank[2 * JC_MAX], mreg[2 * JC_MAX], vreg[2 * JC_MAX];
    /* A list element's load: an element never written (its tag, in S2,
     * T_NONE) is zero (null), put in val (of bank) by the stub, which goes
     * back to `back`, after the load. */
    bool unset;
    uint8_t ubank, uval;
    uint32_t back;
    /* lpush into a full list: the stub writes back, grows the list (glist,
     * for gn more), loads every cached register again (all of them: na, the
     * R ones as bank 2), the list in S0 and its length in S1, and goes back
     * to `back`. */
    bool grow;
    bool alloc;
    bool alloc_struct;
    uint8_t glist, gn, na;
    uint8_t abank[3 * JC_MAX], amreg[3 * JC_MAX], avreg[3 * JC_MAX];
} JSide;

/* The register cache. Bank 0 is I (general registers), bank 1 F (float
 * registers). of[b][r]: the slot holding register r, or -1; vm[b][s]: the
 * register in slot s, or -1. With `record`, nothing is emitted for it: the
 * registers an instruction uses are noted in used[b][r] (1 read, 2 written).
 *
 * R registers may also be in bank 0's slots (vm[0][s] is JC_RBASE + r, and
 * rof[r] the slot), within a block: written through to the window as they
 * are set (where the collector sees them), so never dirty; only in a slot
 * that was free, and the first to go when an I register needs one. rk[r]:
 * what the block has checked of R r's object since r was last set (kind 0:
 * nothing; OBJ_STRUCT with its def; OBJ_LIST), so that the checks are not
 * repeated. Both are forgotten at every block's start. */
#define JC_RBASE 256
#define JC_PBASE 512
#define JC_ACAP 4
#define JLOOP_MAX_R 8
#define JLOOP_MAX_P 2
typedef struct {
    uint8_t kind;
    uint8_t elem; /* OBJ_ARRAY: element MlwsType */
    bool young;   /* made in this block (new, lnew): no collection runs before it is left */
    const void *def;
} JRKnown;
typedef struct {
    int8_t of[2][256];
    int16_t vm[2][JC_MAX];
    bool dirty[2][JC_MAX];
    uint32_t pin[2];      /* slots this instruction uses */
    uint32_t loop_pin[2]; /* slots pinned for the enclosing JLoop */
    uint8_t next[2];
    bool record;
    int32_t gok; /* a module whose globals this process is known to have (checked in this block), or -1 */
    uint8_t used[2][256];
    uint16_t used_cnt[2][256];
    uint8_t cur_pin[2], max_pin[2];
    int8_t rof[256];
    JRKnown rk[256];
    uint8_t rseen[256]; /* R registers read in the block (jld_r) */
    uint8_t aok_r[JC_ACAP], aok_ix[JC_ACAP], aok_n; /* (R arr, I ix) pairs already bounds-checked in this block */
} JCache;

/* A loop whose registers stay in machine registers from one iteration to the
 * next (jit_loops): the instructions L to E, the last a branch back to L,
 * entered only at L (from outside) and calling nothing. Its I and F registers
 * each have a slot of the cache for the whole loop, loaded on the way in
 * (body[L], and a stub for the interpreter at each block inside) and written
 * back on the way out: at a branch out (a stub), a side exit, or after E. */
typedef struct {
    uint32_t L, E;
    uint16_t n;
    bool partial[2]; /* true if bank uses more regs than slots: top regs pinned, rest dynamic */
    uint8_t bank[2 * JC_MAX], vreg[2 * JC_MAX], slot[2 * JC_MAX];
    bool written[2 * JC_MAX];
    /* Loop-invariant R registers cached in free bank-0 slots and/or hoisted type guards. */
    uint8_t nr;
    uint8_t rvreg[JLOOP_MAX_R];
    int8_t rslot[JLOOP_MAX_R]; /* bank-0 slot, or -1 if guard-only */
    uint8_t rkind[JLOOP_MAX_R]; /* 0: cache-only, OBJ_STRUCT, OBJ_LIST, OBJ_ARRAY */
    uint8_t relem[JLOOP_MAX_R]; /* OBJ_ARRAY element type */
    const void *rdef[JLOOP_MAX_R]; /* OBJ_STRUCT def */
    /* Loop-invariant raw P registers hoisted to (data+off, size-off) in free bank-0 slots. */
    uint8_t np;
    uint8_t pvreg[JLOOP_MAX_P];
    uint8_t pbase_slot[JLOOP_MAX_P];
    uint8_t psize_slot[JLOOP_MAX_P];
} JLoop;

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
    const JLoop *loop;    /* the loop being compiled, or NULL */
    const int32_t *loop_of; /* each instruction's loop (jit_loops), or NULL */
    const JLoop *loops;
    struct {
        JSide *v; /* (pc: the target outside the loop) */
        size_t n, cap;
    } leaves;
    bool broken; /* a loop's register was not in its slot (a bug): compile nothing */
    /* The branch being compiled falls through into a block that keeps the
     * cache (fallin): its flush writes back what changed but forgets
     * nothing. (Writing back only on the way to the target, in a stub, costs
     * two more jumps where the branch is usually taken: Lua's loop on the
     * Ryzen, 7%.) */
    bool keep;
    uint32_t ncalli, nspec; /* closure calls, and those compiled for a callee */
} JComp;

enum { TO_BODY, TO_UNDO, TO_EXIT, TO_SIDE, TO_LOOP, TO_LEAVE, TO_INNER, TO_BODYX /* body, as is */ };

static void jside_record(const JComp *j, JSide *sd, uint32_t pc);
static void jfix(JComp *j, uint32_t at, int to, uint32_t pc) {
    if (to == TO_BODY && j->loop) { /* inside a loop: back to its head, or out through a stub (which goes on as
                                     * a branch from outside it) */
        if (pc == j->loop->L) {
            to = TO_LOOP;
        } else if (!j->loop_of || j->loop_of[pc] < 0 || &j->loops[j->loop_of[pc]] != j->loop) {
            if (j->leaves.n == j->leaves.cap) {
                j->leaves.cap = j->leaves.cap ? 2 * j->leaves.cap : 8;
                j->leaves.v = xrealloc(j->leaves.v, j->leaves.cap * sizeof *j->leaves.v);
            }
            jside_record(j, &j->leaves.v[j->leaves.n], pc);
            to = TO_LEAVE, pc = (uint32_t)j->leaves.n++;
        }
    } else if (to == TO_BODY && j->loop_of && j->loop_of[pc] >= 0 && j->loops[j->loop_of[pc]].L != pc) {
        to = TO_INNER; /* into a loop past L, from outside it: its registers are loaded first */
    }
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
static int jc_mreg(int bank, int s) { return bank ? jc_freg[s] : jc_ireg[s]; }
static int jc_nslots(int bank) { return bank ? JC_NF : JC_NI; }

static void jc_reset(JCache *c) {
    memset(c->of, -1, sizeof c->of);
    memset(c->vm, -1, sizeof c->vm);
    memset(c->dirty, 0, sizeof c->dirty);
    memset(c->rof, -1, sizeof c->rof);
    memset(c->rk, 0, sizeof c->rk);
    memset(c->rseen, 0, sizeof c->rseen);
    c->pin[0] = c->pin[1] = 0;
    c->loop_pin[0] = c->loop_pin[1] = 0;
    c->gok = -1;
    c->aok_n = 0;
}

static bool jc_aok_has(const JCache *c, unsigned r, int ix_vreg) {
    if (ix_vreg < 0 || c->record) return false;
    for (unsigned k = 0; k < c->aok_n; k++)
        if (c->aok_r[k] == r && c->aok_ix[k] == (uint8_t)ix_vreg) return true;
    return false;
}
static void jc_aok_add(JCache *c, unsigned r, int ix_vreg) {
    if (ix_vreg < 0 || c->record || jc_aok_has(c, r, ix_vreg)) return;
    if (c->aok_n < JC_ACAP) c->aok_r[c->aok_n] = (uint8_t)r, c->aok_ix[c->aok_n] = (uint8_t)ix_vreg, c->aok_n++;
}
static void jc_aok_kill_r(JCache *c, unsigned r) {
    for (unsigned k = 0; k < c->aok_n;) {
        if (c->aok_r[k] == r) c->aok_r[k] = c->aok_r[--c->aok_n], c->aok_ix[k] = c->aok_ix[c->aok_n];
        else k++;
    }
}
static void jc_aok_kill_i(JCache *c, unsigned ix) {
    for (unsigned k = 0; k < c->aok_n;) {
        if (c->aok_ix[k] == ix) c->aok_r[k] = c->aok_r[--c->aok_n], c->aok_ix[k] = c->aok_ix[c->aok_n];
        else k++;
    }
}

/* Forget block-local R registers and type facts (a block's start: it may be
 * reached from anywhere), while keeping loop-invariant R/P slots and guards. */
static void jc_forget_r(JComp *j) {
    JCache *c = &j->jc;
    for (int s = 0; s < JC_NI; s++) {
        if (c->loop_pin[0] & (1u << s)) continue;
        if (c->vm[0][s] >= JC_RBASE && c->vm[0][s] < JC_PBASE)
            c->rof[c->vm[0][s] - JC_RBASE] = -1, c->vm[0][s] = -1;
    }
    memset(c->rk, 0, sizeof c->rk);
    memset(c->rseen, 0, sizeof c->rseen);
    c->gok = -1;
    c->aok_n = 0;
    if (j->loop) {
        const JLoop *lp = j->loop;
        for (int k = 0; k < lp->nr; k++)
            if (lp->rkind[k]) c->rk[lp->rvreg[k]] = (JRKnown){lp->rkind[k], lp->relem[k], false, lp->rdef[k]};
    }
}

/* A free slot of bank 0 for an intra-block R register, or -1 (none is evicted
 * for one). On x86-64, JM_RCACHE is 0: with 5 slots, dynamically caching R
 * registers within short blocks made the Intel machine up to 7% slower, where
 * register moves are not free. Loop-invariant R registers in JLoop (jit_loops)
 * still use otherwise-unused bank-0 slots on x86-64: loaded once at loop entry
 * with no per-block moves, they eliminate repeated window loads across the loop. */
static int jc_rslot(JCache *c) {
    if (c->record || !JM_RCACHE) return -1;
    for (int s = 0; s < JC_NI; s++)
        if (c->vm[0][s] < 0) return s;
    return -1;
}

/* R register r into machine register d: from its slot, or from the window
 * (and, read before in the block, into a free slot: a slot taken for each
 * read once costs more moves than it saves loads where slots are few). */
static void jld_r(JComp *j, int d, unsigned r) {
    JCache *c = &j->jc;
    int s = c->record ? -1 : c->rof[r];
    if (s >= 0) {
        if (jc_ireg[s] != d) jm_mov(&j->b, d, jc_ireg[s]);
        return;
    }
    jm_ld(&j->b, d, JR_WIN, jwoff(j, BANK_R, r));
    if (!c->rseen[r]) { /* the first read: a slot only for one read again */
        c->rseen[r] = 1;
        return;
    }
    if ((s = jc_rslot(c)) >= 0) {
        jm_mov(&j->b, jc_ireg[s], d);
        c->rof[r] = (int8_t)s;
        c->vm[0][s] = (int16_t)(JC_RBASE + r);
    }
}

/* Machine register holding R register r: its cached slot (pinned for this
 * instruction) if present, else loaded into tmp. */
static int jreg_r(JComp *j, unsigned r, int tmp) {
    JCache *c = &j->jc;
    int s = c->record ? -1 : c->rof[r];
    if (s >= 0) {
        c->pin[0] |= 1u << s;
        return jc_ireg[s];
    }
    jld_r(j, tmp, r);
    return tmp;
}

/* R register r set from machine register v: written through, and what was
 * known of it forgotten. */
static void jst_r(JComp *j, int v, unsigned r) {
    JCache *c = &j->jc;
    jm_st(&j->b, v, JR_WIN, jwoff(j, BANK_R, r));
    c->rk[r].kind = 0;
    jc_aok_kill_r(c, r);
    if (c->record) return;
    int s = c->rof[r];
    if (s < 0 && (s = jc_rslot(c)) >= 0) {
        c->rof[r] = (int8_t)s;
        c->vm[0][s] = (int16_t)(JC_RBASE + r);
    }
    if (s >= 0) jm_mov(&j->b, jc_ireg[s], v);
}

static void jc_store(JComp *j, int bank, int s, int r) {
    if (bank) jm_fst(&j->b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
    else jm_st(&j->b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
}

/* Write the changed registers back, and forget them all (a block's end; in
 * a loop, keep loop-pinned slots and flush any dynamic slots of a partial
 * loop; before a branch into a fallin block, keep them). */
static void jc_flush(JComp *j) {
    JCache *c = &j->jc;
    if (c->record) return;
    if (j->loop) {
        if (!j->loop->partial[0] && !j->loop->partial[1]) return;
        for (int bank = 0; bank < 2; bank++) {
            if (!j->loop->partial[bank]) continue;
            for (int s = 0; s < jc_nslots(bank); s++) {
                if (c->loop_pin[bank] & (1u << s)) continue;
                int v = c->vm[bank][s];
                if (v >= 0 && v < JC_RBASE) {
                    if (c->dirty[bank][s]) jc_store(j, bank, s, v), c->dirty[bank][s] = false;
                    c->of[bank][v] = -1;
                    c->vm[bank][s] = -1;
                }
            }
        }
        return;
    }
    for (int bank = 0; bank < 2; bank++)
        for (int s = 0; s < jc_nslots(bank); s++)
            if (c->vm[bank][s] >= 0 && c->vm[bank][s] < JC_RBASE && c->dirty[bank][s])
                jc_store(j, bank, s, c->vm[bank][s]), c->dirty[bank][s] = false;
    if (!j->keep) jc_reset(c);
}

static int jc_alloc(JComp *j, int bank) {
    JCache *c = &j->jc;
    int ns = jc_nslots(bank);
    for (int s = 0; s < ns; s++)
        if (c->vm[bank][s] < 0) return s;
    if (!bank)
        for (int s = 0; s < ns; s++) /* an unpinned R register's slot first: its value is in the window */
            if (!((c->pin[0] | c->loop_pin[0]) & (1u << s)) && c->vm[0][s] >= JC_RBASE && c->vm[0][s] < JC_PBASE) {
                c->rof[c->vm[0][s] - JC_RBASE] = -1;
                c->vm[0][s] = -1;
                return s;
            }
    for (int tries = 0; tries < ns; tries++) { /* evict, in turn, a slot this instruction and loop do not pin */
        int s = c->next[bank];
        c->next[bank] = (uint8_t)((s + 1) % ns);
        if ((c->pin[bank] | c->loop_pin[bank]) & (1u << s)) continue;
        if (c->dirty[bank][s]) jc_store(j, bank, s, c->vm[bank][s]);
        c->of[bank][c->vm[bank][s]] = -1;
        c->vm[bank][s] = -1;
        c->dirty[bank][s] = false;
        return s;
    }
    j->broken = true; /* every slot pinned (bug): refuse to install code instead of hanging */
    return 0;
}

/* The machine register holding register r, loaded if it is not yet. */
static int jc_get(JComp *j, int bank, int r) {
    JCache *c = &j->jc;
    if (c->record) {
        c->used[bank][r] |= 1;
        if (c->used_cnt[bank][r] < UINT16_MAX) c->used_cnt[bank][r]++;
        if (++c->cur_pin[bank] > c->max_pin[bank]) c->max_pin[bank] = c->cur_pin[bank];
        return jc_mreg(bank, 0);
    }
    int s = c->of[bank][r];
    if (s < 0 && j->loop && !j->loop->partial[bank]) j->broken = true;
    if (s < 0) {
        s = jc_alloc(j, bank);
        if (bank) jm_fld(&j->b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
        else jm_ld(&j->b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int16_t)r;
    }
    c->pin[bank] |= 1u << s;
    return jc_mreg(bank, s);
}

/* The machine register to write register r to (its value to be written back). */
static int jc_def(JComp *j, int bank, int r) {
    JCache *c = &j->jc;
    if (c->record) {
        c->used[bank][r] |= 2;
        if (c->used_cnt[bank][r] < UINT16_MAX) c->used_cnt[bank][r]++;
        if (++c->cur_pin[bank] > c->max_pin[bank]) c->max_pin[bank] = c->cur_pin[bank];
        return jc_mreg(bank, 0);
    }
    if (bank == 0) jc_aok_kill_i(c, (unsigned)r);
    int s = c->of[bank][r];
    if (s < 0 && j->loop && !j->loop->partial[bank]) j->broken = true;
    if (s < 0) {
        s = jc_alloc(j, bank);
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int16_t)r;
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
/* The registers changed and not yet written back, for a stub to write. */
static void jside_record(const JComp *j, JSide *sd, uint32_t pc) {
    const JCache *c = &j->jc;
    sd->pc = pc;
    sd->n = 0;
    sd->unset = sd->grow = sd->alloc = false;
    for (int bank = 0; bank < 2; bank++)
        for (int k = 0; k < jc_nslots(bank); k++)
            if (c->vm[bank][k] >= 0 && c->dirty[bank][k]) {
                sd->bank[sd->n] = (uint8_t)bank, sd->mreg[sd->n] = (uint8_t)jc_mreg(bank, k);
                sd->vreg[sd->n++] = (uint8_t)c->vm[bank][k];
            }
}
static void jside_at(JComp *j, uint32_t at, uint32_t pc) {
    if (j->sides.n == j->sides.cap) {
        j->sides.cap = j->sides.cap ? 2 * j->sides.cap : 16;
        j->sides.v = xrealloc(j->sides.v, j->sides.cap * sizeof *j->sides.v);
    }
    jside_record(j, &j->sides.v[j->sides.n], pc);
    jfix(j, at, TO_SIDE, (uint32_t)j->sides.n++);
}
/* A stub's stores: the registers it records, back to the window. */
static void jside_stores(JComp *j, const JSide *sd) {
    for (int r = 0; r < sd->n; r++) {
        if (sd->bank[r]) jm_fst(&j->b, sd->mreg[r], JR_WIN, jwoff(j, BANK_F, sd->vreg[r]));
        else jm_st(&j->b, sd->mreg[r], JR_WIN, jwoff(j, BANK_I, sd->vreg[r]));
    }
}
static void jside(JComp *j, int kind, int cond, int reg, bool nz, uint32_t pc) {
    jside_at(j, jm_emit_branch(&j->b, kind, cond, reg, nz), pc);
}
#define SIDE_IF(cond) jside(j, FX_BCOND, (cond), 0, false, pc)
#define SIDE_IF_NULL(reg) jside(j, FX_CB, 0, (reg), false, pc)

/* ---- the forms it compiles ---- */
enum { JX_ARITH = Q__COUNT, JX_SET, JX_FARITH }; /* generic instructions it compiles (jit_class) */
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
    case Q_BZ_MB_U8: case Q_BNZ_MB_U8:
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
    case Q_STEC: case Q_STECK: case Q_STGK:
    case Q_SEQ_RR: case Q_SNE_RR: case Q_SEQ_RK: case Q_SNE_RK: case Q_MGET_R:
    case Q_MGETV_I: case Q_MGETV_F: case Q_MGETV_R:
    case Q_MNEW: case Q_MSET: case Q_MHAS:
    case Q_LDMB_U8: case Q_STMB_U8: case Q_STMBK_U8:
    case Q_STRCAT_RK: case Q_BUFCAT_RK: case Q_TOSTR_I: case Q_TOSTR_F: case Q_SUBSTR_RII: case Q_BUFNEW_K: case Q_BUFPUSH_I:
#define X(N, o) case Q_##N##_FF: case Q_##N##_FK:
        QSET_OPS(X)
#undef X
#define X(N, e) case Q_##N##_III: case Q_##N##_IIK: case Q_##N##_II: case Q_##N##_IK:
        QSHIFT_OPS(X)
#undef X
    case Q_LDED_I: case Q_LDED_F: case Q_LDED_R: case Q_STED_I: case Q_STED_F: case Q_STED_R:
    case Q_LDE_I: case Q_LDE_F: case Q_LDE_R: case Q_STE_I: case Q_STE_F: case Q_STE_R:
    case Q_LDEK_I: case Q_LDEK_F: case Q_LDEK_R: case Q_STEK_I: case Q_STEK_F: case Q_STEK_R:
    case JX_ARITH: case JX_SET: case JX_FARITH:
        return true;
    default: return false;
    }
}

/* Generic instructions the JIT compiles from their operands (they have no
 * quickened form): i64 and u64 add, sub, mul, and, or and xor, and compares
 * into a register, of I registers, constants and captures (UP[k]), into an
 * I register. A capture that does not hold an i64 is a side exit. */
static bool jit_gen_operand(const Operand *o) {
    return (o->mode == OM_REG && REG_BANK(o->reg) == BANK_I) || o->mode == OM_IMM ||
           (o->mode == OM_UPVAL && o->index < 65536);
}
static bool jit_fgen_operand(const Operand *o, bool dst) {
    if (o->mode == OM_REG && REG_BANK(o->reg) == BANK_F) return true;
    if (!dst && (o->mode == OM_FIMM || o->mode == OM_IMM)) return true;
    if (o->mode == OM_ELEM && REG_BANK(o->reg) == BANK_R && REG_BANK(o->ireg) == BANK_I) return true;
    if (o->mode == OM_ELEMI && REG_BANK(o->reg) == BANK_R && o->index <= UINT32_MAX) return true;
    return false;
}
static uint16_t jit_class(const Insn *in, uint16_t x) {
    if (x != in->op) return x; /* quickened */
    bool arith = in->op == OP_ADD || in->op == OP_SUB || in->op == OP_MUL || in->op == OP_AND || in->op == OP_OR ||
                 in->op == OP_XOR;
    bool set = in->op >= OP_SEQ && in->op <= OP_SGE;
    if ((arith || set) && (in->type == T_I64 || in->type == T_U64)) {
        if (in->nops < 2 || (!arith && in->nops != 3)) return x;
        if (in->ops[0].mode != OM_REG || REG_BANK(in->ops[0].reg) != BANK_I) return x;
        for (int k = 1; k < in->nops; k++)
            if (!jit_gen_operand(&in->ops[k])) return x;
        return arith ? JX_ARITH : JX_SET;
    }
    bool farith = in->op == OP_ADD || in->op == OP_SUB || in->op == OP_MUL || in->op == OP_DIV;
    if (farith && in->type == T_F64 && in->nops >= 2 && jit_fgen_operand(&in->ops[0], true)) {
        for (int k = 1; k < in->nops; k++)
            if (!jit_fgen_operand(&in->ops[k], false)) return x;
        return JX_FARITH;
    }
    return x;
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

static JIT_CC JitStep jit_step(Ctx *c, const Insn *in, uint32_t left) {
    Process *p = c->p;
    ctx_load(c);
    c->code = c->fr->fn->code;
    uint32_t depth = p->nframes;
    uint64_t ib = c->ibase;
    c->ibase = ib - left; /* the handler's count, with no slice left, as the interpreter's */
    c->stepping = true;
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
    c->stepping = false;
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
 * arguments copied by the call's plan; then the callee's machine code. A
 * direct call to a function without machine code is the interpreter's. The
 * call's continuation follows it; its offset in the code memory is the
 * call's Insn.target, which calls do not otherwise use (a call has no label,
 * so it is 0 until then). A return to a compiled call puts its values in
 * machine registers (JRES_N of them, the rest in Ctx.retv), pops the frame
 * and returns to the continuation, which takes the caller's window and copies
 * the results. Any other return is the interpreter's. */
#define P_OFF(f) ((int64_t)offsetof(Process, f))
#define FR_OFF(f) ((int64_t)offsetof(Frame, f))
#define C_OFF(f) ((int64_t)offsetof(Ctx, f))

static void jit_loop_load_as(JComp *j, const JLoop *lp, bool seed, bool emit, uint32_t exit_pc);
/* A return to a continuation leaves its first JRES_N values in the cache's
 * first integer registers (free at a return, whose block has written its
 * registers back), the rest in Ctx.retv. Only a compiled return (jit_ret)
 * goes to a continuation. */
#define JRES_N (JC_NI < 4 ? JC_NI : 4)

/* The continuation of a call with plan pl: the caller's window, the results.
 * fall: the code for pc + 1 follows it (no jump there). */
static void jit_cont(JComp *j, const QPlan *pl, int argc, uint32_t pc, bool fall) {
    JBuf *b = &j->b;
    jm_cont(b);
    jm_ld32(b, JR_S0, JR_FR, FR_OFF(base));
    int P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S1, P, P_OFF(regs));
    jm_addsh3(b, JR_WIN, JR_S1, JR_S0);
    for (int k = 0; k < pl->nres; k++) {
        int v = JR_S0;
        if (k < JRES_N) v = jc_ireg[k];
        else jm_ld(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
        jm_st(b, v, JR_WIN, (int64_t)pl->slot[2 * argc + k] * 8);
    }
    int32_t lo = j->loop_of ? j->loop_of[pc + 1] : -1;
    if (lo >= 0 && j->loops[lo].L != pc + 1) { /* a call in a loop: the loop's registers again, here */
        jit_loop_load_as(j, &j->loops[lo], false, true, pc + 1);
        jbranch(j, FX_B, 0, 0, false, TO_BODYX, pc + 1);
        return;
    }
    if (fall && lo < 0 && !j->loop && pc + 1 < j->f->ncode) return;
    jbranch(j, FX_B, 0, 0, false, TO_BODY, pc + 1);
}

/* Push fn's frame for the call `in`, as Q_CALL_R's handler does, and copy its
 * arguments: src[k] is each one's slot in the caller (QPLAN_K: the constant
 * word kw[k]), dst[k] its slot in fn's window. clo, if not negative, is a
 * register holding the closure called. Stops before the call (TO_UNDO) when
 * the frame or register stack is full, for the interpreter to grow it. Leaves
 * JR_FR and JR_WIN on the new frame. Uses only JR_S0..S4 so cached caller
 * registers (except skip_ireg, if >= 0) can be stored directly to the callee's
 * window without store-to-load stalls. */
static void jit_push_frame(JComp *j, const Insn *in, uint32_t pc, const Function *fn, int argc, const uint8_t *src,
                           const uint8_t *dst, const uint64_t *kw, int clo, int skip_ireg) {
    JBuf *b = &j->b;
    int P = jm_proc(b, JR_S3);
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
    jm_st32(b, JR_S1, JR_S4, FR_OFF(base));
    jm_st32(b, JR_S2, P, P_OFF(rtop));
    jm_add32i(b, JR_S0, JR_S0, 1);
    jm_st32(b, JR_S0, P, P_OFF(nframes));
    jm_ld32(b, JR_S1, P, P_OFF(max_depth));
    jm_cmp32(b, JR_S0, JR_S1);
    uint32_t deep = jm_fwd(b, JC_LS);
    jm_st32(b, JR_S0, P, P_OFF(max_depth));
    jm_bind(b, deep);
    jm_movi(b, JR_S1, (uint64_t)(uintptr_t)fn);
    jm_st(b, JR_S1, JR_S4, FR_OFF(fn));
    _Static_assert(offsetof(Frame, pc) == 8 && offsetof(Frame, qret) == 12 && offsetof(Frame, stackless) == 13 &&
                       offsetof(Frame, base) == 16,
                   "pc, qret and stackless are one word");
    jm_movi(b, JR_S1, (uint64_t)1 << 32 | (uint64_t)fn->stackless << 40); /* pc 0, qret, stackless */
    jm_st(b, JR_S1, JR_S4, FR_OFF(pc));
    jm_movi(b, JR_S1, (uint64_t)(uintptr_t)in);
    jm_st(b, JR_S1, JR_S4, FR_OFF(ret_insn));
    if (clo >= 0) jm_st(b, clo, JR_S4, FR_OFF(closure));
    else jm_stz(b, JR_S4, FR_OFF(closure));
    if (!fn->stackless) {
        jm_ld(b, JR_S1, P, P_OFF(sp));
        jm_st(b, JR_S1, JR_S4, FR_OFF(entry_sp));
        jm_ld(b, JR_S2, P, P_OFF(fp));
        jm_st(b, JR_S2, JR_S4, FR_OFF(saved_fp));
        jm_st(b, JR_S1, P, P_OFF(fp));
    }
    jm_alui(b, JA_ADD, JR_S1, JR_WIN, (int64_t)j->f->rsize * 8); /* the callee's window */
    jm_clear(b, JR_S1, (int64_t)fn->zoff * 8, fn->zlen);
    const JCache *c = &j->jc;
    const uint16_t *roff = j->f->roff;
    for (int k = 0; k < argc; k++) {
        uint8_t s = src[k];
        if (s == QPLAN_K) {
            jm_movi(b, JR_S0, kw[k]);
            jm_st(b, JR_S0, JR_S1, (int64_t)dst[k] * 8);
        } else if (s < roff[BANK_F] && c->of[0][s] >= 0 && jc_ireg[c->of[0][s]] != skip_ireg) {
            jm_st(b, jc_ireg[c->of[0][s]], JR_S1, (int64_t)dst[k] * 8);
        } else if (s >= roff[BANK_F] && s < roff[BANK_R] && c->of[1][s - roff[BANK_F]] >= 0) {
            jm_fst(b, jc_freg[c->of[1][s - roff[BANK_F]]], JR_S1, (int64_t)dst[k] * 8);
        } else if (s >= roff[BANK_R] && s < roff[BANK_P] && c->rof[s - roff[BANK_R]] >= 0 &&
                   jc_ireg[c->rof[s - roff[BANK_R]]] != skip_ireg) {
            jm_st(b, jc_ireg[c->rof[s - roff[BANK_R]]], JR_S1, (int64_t)dst[k] * 8);
        } else {
            jm_ld(b, JR_S0, JR_WIN, (int64_t)s * 8);
            jm_st(b, JR_S0, JR_S1, (int64_t)dst[k] * 8);
        }
    }
    jm_mov(b, JR_FR, JR_S4);
    jm_mov(b, JR_WIN, JR_S1);
}

/* Returns the continuation's offset in the code. */
static uint32_t jit_call(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *pl = in->k.p;
    const Function *fn = pl->fn;
    int argc = pl->argc;
    uint64_t kw[MAX_OPERANDS];
    for (int k = 0; k < argc; k++) kw[k] = in->ops[2 + k].kw; /* Q_CALL_RK's constants */
    jm_ncall_check(j, pc);
    jm_movi(b, JR_S0, pc + 1);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    int ent = jc_ireg[2]; /* (jit_push_frame leaves it) */
    if (fn != j->f) { /* the callee's machine code for its first instruction; if it has none, the interpreter's call */
        jm_movi(b, JR_S0, (uint64_t)(uintptr_t)&fn->jit_entry);
        jm_ld(b, JR_S0, JR_S0, 0);
        jbranch(j, FX_CB, 0, JR_S0, false, TO_UNDO, pc);
        jm_ld(b, ent, JR_S0, 0);
        jbranch(j, FX_CB, 0, ent, false, TO_UNDO, pc);
    }
    jit_push_frame(j, in, pc, fn, argc, pl->slot, pl->slot + argc, kw, -1, fn == j->f ? -1 : ent);
    if (fn == j->f) jfix(j, jm_ncall_self(b), TO_BODY, 0); /* (its code is being made: its first instruction's) */
    else jm_ncall_reg(b, ent);
    uint32_t c = (uint32_t)b->n;
    jit_cont(j, pl, argc, pc, true);
    if (!j->loop && !j->jc.record) jc_reset(&j->jc);
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
static JIT_CC uintptr_t jit_calli_go(Ctx *c, const Insn *in) {
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

/* The callee a closure call (Q_CALLI_R, Q_CALLI_RK) is compiled for: the last
 * one it called (QPlan.seen), if the call suits it as call_rk checks, with its
 * arguments' slots in its window and the constants' words; NULL if none. */
static Function *jit_spec_callee(const Insn *in, uint8_t *dst, uint64_t *kw) {
    const QPlan *pl = in->k.p;
    Function *fn = __atomic_load_n(&pl->seen, __ATOMIC_RELAXED);
    if (!fn) return NULL;
    const MFunc *mf = fn->mf;
    const Operand *qo = in->ops;
    int argc = pl->argc, nres = pl->nres;
    if (argc != mf->nparams || nres > mf->nrets || (nres && !(mf->flags & MF_RETS_DECLARED))) return NULL;
    int cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        int pt = mf->params[k], bk = (int)type_bank((MlwsType)pt);
        Word w = {0};
        if (!wide_type(pt)) return NULL;
        if (qo[2 + k].mode == OM_REG ? REG_BANK(qo[2 + k].reg) != bk : !const_word(&qo[2 + k], pt, &w)) return NULL;
        memcpy(&kw[k], &w, 8);
        dst[k] = (uint8_t)(fn->roff[bk] + cnt[bk]++);
    }
    for (int k = 0; k < nres; k++)
        if (!wide_type(mf->rets[k]) || REG_BANK(qo[2 + argc + k].reg) != (int)type_bank((MlwsType)mf->rets[k]))
            return NULL;
    return fn;
}

/* A closure call. When it is compiled for a callee (jit_spec_callee), a
 * closure of that function is called as a direct call is, with the closure
 * in its frame; any other value takes the C helper, as every closure call
 * does otherwise. Both go on at the one call instruction, so the return's
 * check of its continuation holds either way. */
static uint32_t jit_calli(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *pl = in->k.p;
    uint8_t dst[MAX_OPERANDS];
    uint64_t kw[MAX_OPERANDS];
    Function *fn = jit_spec_callee(in, dst, kw);
    j->ncalli++, j->nspec += fn != NULL;
    jm_ncall_check(j, pc);
    jm_movi(b, JR_S0, pc + 1); /* the return address; and the count, for a trap */
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    uint32_t other[3] = {0}, go = 0, z1 = 0, z2 = 0;
    if (fn) {
        int clo = jc_ireg[2];
        jld_r(j, clo, (unsigned)REG_IDX(in->ops[0].reg));
        other[0] = jm_fwd_cbz(b, clo, false);
        jm_ld8(b, JR_S0, clo, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S0, OBJ_CLOSURE);
        other[1] = jm_fwd(b, JC_NE);
        jm_ld(b, JR_S0, clo, (int64_t)offsetof(ObjClosure, fn));
        jm_movi(b, JR_S1, (uint64_t)(uintptr_t)fn);
        jm_cmp(b, JR_S0, JR_S1);
        other[2] = jm_fwd(b, JC_NE);
        jit_push_frame(j, in, pc, fn, pl->argc, pl->slot, dst, kw, clo, clo);
        jm_movi(b, JR_R0, (uint64_t)(uintptr_t)&fn->jit_entry);
        jm_ld(b, JR_R0, JR_R0, 0);
        z1 = jm_fwd_cbz(b, JR_R0, false);
        jm_ld(b, JR_R0, JR_R0, 0);
        z2 = jm_fwd_cbz(b, JR_R0, false);
        go = jm_fwd(b, JC_AL);
        for (int k = 0; k < 3; k++) jm_bind(b, other[k]);
    }
    jit_sync_insns(j, 0);
    jm_mov(b, JR_A0, JR_CTX);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
    jm_ccall(b, (const void *)jit_calli_go);
    jbranch(j, FX_CB, 0, JR_R0, false, TO_UNDO, pc);
    jm_reload(b);
    jm_cmpi(b, JR_R0, 1);
    uint32_t none = jm_fwd(b, JC_EQ);
    if (fn) jm_bind(b, go);
    jm_ncall_reg(b, JR_R0);
    uint32_t c = (uint32_t)b->n; /* the continuation, as a direct call's */
    jit_cont(j, pl, pl->argc, pc, false);
    jit_callee_interp(j, none);
    if (fn) { /* pushed, but the callee runs in the interpreter, from its first instruction */
        jm_bind(b, z1), jm_bind(b, z2);
        jexit(j, fn->code);
    }
    if (!j->loop && !j->jc.record) jc_reset(&j->jc);
    return c;
}

/* A closure tail call (Q_TCALLI_R) from machine code, as the handler makes
 * it; the same results as jit_calli_go. */
static JIT_CC uintptr_t jit_tcalli_go(Ctx *c, const Insn *in) {
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
    plan_saw(in, fn);
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

static void jit_tail_frame(JComp *j, const Insn *in, uint32_t pc, const Function *fn, int clo);

/* A closure tail call; for the callee it is compiled for (jit_spec_callee), as
 * a direct tail call is made, with the closure in the frame. */
static void jit_tcalli(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    uint8_t dst[MAX_OPERANDS];
    uint64_t kw[MAX_OPERANDS];
    Function *fn = jit_spec_callee(in, dst, kw);
    j->ncalli++, j->nspec += fn != NULL;
    jm_movi(b, JR_S0, pc + 1);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    if (fn) {
        int clo = jc_ireg[2];
        jm_ld(b, clo, JR_WIN, (int64_t)in->ops[0].pad * 8);
        uint32_t other[3];
        other[0] = jm_fwd_cbz(b, clo, false);
        jm_ld8(b, JR_S0, clo, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S0, OBJ_CLOSURE);
        other[1] = jm_fwd(b, JC_NE);
        jm_ld(b, JR_S0, clo, (int64_t)offsetof(ObjClosure, fn));
        jm_movi(b, JR_S1, (uint64_t)(uintptr_t)fn);
        jm_cmp(b, JR_S0, JR_S1);
        other[2] = jm_fwd(b, JC_NE);
        jit_tail_frame(j, in, pc, fn, clo);
        for (int k = 0; k < 3; k++) jm_bind(b, other[k]);
    }
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

/* Place n values (constants where slot[k] == QPLAN_K, else window slot[k],
 * reading directly from the register cache when present) into jc_ireg[0..JRES_N-1]
 * and Ctx.retv[JRES_N..n-1]. Resolves any register-move cycles among jc_ireg
 * using JR_S0. */
static void jit_emit_vals(JComp *j, int n, const uint8_t *slot, const uint64_t *kw) {
    JBuf *b = &j->b;
    const JCache *c = &j->jc;
    const uint16_t *roff = j->f->roff;
    /* First, values past JRES_N go straight to Ctx.retv (never clobbering jc_ireg). */
    for (int k = JRES_N; k < n; k++) {
        uint8_t s = slot[k];
        if (s == QPLAN_K) {
            jm_movi(b, JR_S0, kw[k]);
        } else if (s < roff[BANK_F] && c->of[0][s] >= 0) {
            jm_mov(b, JR_S0, jc_ireg[c->of[0][s]]);
        } else if (s >= roff[BANK_F] && s < roff[BANK_R] && c->of[1][s - roff[BANK_F]] >= 0) {
            jm_fmov_to_x(b, JR_S0, jc_freg[c->of[1][s - roff[BANK_F]]]);
        } else if (s >= roff[BANK_R] && s < roff[BANK_P] && c->rof[s - roff[BANK_R]] >= 0) {
            jm_mov(b, JR_S0, jc_ireg[c->rof[s - roff[BANK_R]]]);
        } else {
            jm_ld(b, JR_S0, JR_WIN, (int64_t)s * 8);
        }
        jm_st(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
    }
    int nr = n < JRES_N ? n : JRES_N;
    if (nr <= 0) return;
    /* Classify 0..nr-1: 0=const, 1=mem, 2=freg, 3=ireg. */
    uint8_t kind[4];
    int src[4];
    bool done[4] = {false, false, false, false};
    for (int k = 0; k < nr; k++) {
        uint8_t s = slot[k];
        if (s == QPLAN_K) {
            kind[k] = 0, src[k] = 0;
        } else if (s < roff[BANK_F] && c->of[0][s] >= 0) {
            kind[k] = 3, src[k] = jc_ireg[c->of[0][s]];
        } else if (s >= roff[BANK_F] && s < roff[BANK_R] && c->of[1][s - roff[BANK_F]] >= 0) {
            kind[k] = 2, src[k] = jc_freg[c->of[1][s - roff[BANK_F]]];
        } else if (s >= roff[BANK_R] && s < roff[BANK_P] && c->rof[s - roff[BANK_R]] >= 0) {
            kind[k] = 3, src[k] = jc_ireg[c->rof[s - roff[BANK_R]]];
        } else {
            kind[k] = 1, src[k] = s;
        }
    }
    /* Move all ireg sources first without clobbering any pending source. */
    for (;;) {
        bool progress = false;
        int pending = -1;
        for (int k = 0; k < nr; k++) {
            if (done[k] || kind[k] != 3) continue;
            pending = k;
            int dst = jc_ireg[k];
            bool used = false;
            for (int m = 0; m < nr; m++)
                if (m != k && !done[m] && kind[m] == 3 && src[m] == dst) { used = true; break; }
            if (!used) {
                jm_mov(b, dst, src[k]);
                done[k] = true;
                progress = true;
            }
        }
        if (pending < 0) break;
        if (!progress) { /* cycle among pending ireg moves: break via JR_S0 */
            int dst = jc_ireg[pending];
            jm_mov(b, JR_S0, dst);
            for (int m = 0; m < nr; m++)
                if (!done[m] && kind[m] == 3 && src[m] == dst) src[m] = JR_S0;
        }
    }
    for (int k = 0; k < nr; k++) {
        if (done[k]) continue;
        int dst = jc_ireg[k];
        if (kind[k] == 0) jm_movi(b, dst, kw[k]);
        else if (kind[k] == 1) jm_ld(b, dst, JR_WIN, (int64_t)src[k] * 8);
        else jm_fmov_to_x(b, dst, src[k]);
    }
}

static void jit_ret(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
    const QPlan *rp = in->k.p;
    jm_ld8(b, JR_S0, JR_FR, FR_OFF(qret));
    jside(j, FX_CB, 0, JR_S0, false, pc);
    jm_ld(b, JR_S1, JR_FR, FR_OFF(ret_insn));
    jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Insn, target)); /* the continuation, or 0 */
    jside(j, FX_CB, 0, JR_S2, false, pc);
    uint64_t kw[MAX_OPERANDS];
    for (int k = 0; k < rp->nres; k++) kw[k] = in->ops[k].kw;
    jit_emit_vals(j, rp->nres, rp->slot, kw);
    int P = jm_proc(b, JR_S3);
    if (!j->f->stackless) {
        jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp));
        jm_st(b, JR_S0, P, P_OFF(sp));
        jm_ld(b, JR_S0, JR_FR, FR_OFF(saved_fp));
        jm_st(b, JR_S0, P, P_OFF(fp));
    }
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
/* For a tail call to fn from a frame with qret: tail_qret, which depends only
 * on the frame's ret_insn, kept in the call site's word as ret_insn | qret
 * (one atomic store: threads share the word). */
static JIT_CC uint8_t jit_tail_qret(const Frame *fr, const Function *fn, uintptr_t *word) {
    uint8_t q = tail_qret(fr->ret_insn, 1, fn->mf);
    __atomic_store_n(word, (uintptr_t)fr->ret_insn | q, __ATOMIC_RELAXED);
    return q;
}

static void jit_tcall(JComp *j, const Insn *in, uint32_t pc) { jit_tail_frame(j, in, pc, in->ops[0].rv.ptr, -1); }

/* A tail call to fn, as Q_TCALL_R's handler makes it: fn's frame replaces
 * this one, at the same base. clo, if not negative, is a register holding
 * the closure called. Goes to the callee (or stops for the interpreter). */
static void jit_tail_frame(JComp *j, const Insn *in, uint32_t pc, const Function *fn, int clo) {
    JBuf *b = &j->b;
    const Operand *qo = in->ops;
    const MFunc *mf = fn->mf;
    int argc = (int)qo[1].imm;
    if (fn == j->f) {
        /* Self-tail-call: same window size (already fits rcap), same fn/stackless/qret.
         * Pass up to JRES_N arguments in jc_ireg directly from the register cache. */
        if (clo >= 0) jm_st(b, clo, JR_FR, FR_OFF(closure));
        else jm_stz(b, JR_FR, FR_OFF(closure));
        uint8_t slot[MAX_OPERANDS];
        uint64_t kw[MAX_OPERANDS];
        for (int k = 0; k < argc; k++) {
            slot[k] = qo[2 + k].mode == OM_REG ? qo[2 + k].pad : QPLAN_K;
            kw[k] = qo[2 + k].kw;
        }
        jit_emit_vals(j, argc, slot, kw);
        if (!j->f->stackless) {
            int P = jm_proc(b, JR_S3);
            jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp));
            jm_st(b, JR_S0, P, P_OFF(sp));
            jm_st(b, JR_S0, P, P_OFF(fp));
        }
        jm_movi(b, JR_S0, 0);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jm_clear(b, JR_WIN, (int64_t)fn->zoff * 8, fn->zlen);
        uint32_t cnt[NBANKS] = {0};
        for (int k = 0; k < argc; k++) {
            int bk = (int)type_bank((MlwsType)mf->params[k]);
            int v = k < JRES_N ? jc_ireg[k] : JR_S0;
            if (k >= JRES_N) jm_ld(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
            jm_st(b, v, JR_WIN, (int64_t)(fn->roff[bk] + cnt[bk]++) * 8);
        }
        jbranch(j, FX_B, 0, 0, false, TO_BODY, 0);
        return;
    }
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
    if (clo >= 0) jm_st(b, clo, JR_FR, FR_OFF(closure)); /* (before the call below, which may clobber clo) */
    /* Whether the callee's ret may copy its results straight to the caller
     * (tail_qret): no, if this frame's may not; otherwise what the call site's
     * word says for this frame's ret_insn, or what the helper works out. */
    uintptr_t *word = jit_word(j->jt);
    jm_ld8(b, JR_S0, JR_FR, FR_OFF(qret));
    uint32_t no = jm_fwd_cbz(b, JR_S0, false);
    jm_ld(b, JR_S1, JR_FR, FR_OFF(ret_insn));
    jm_movi(b, JR_S2, (uint64_t)(uintptr_t)word);
    jm_ld(b, JR_S2, JR_S2, 0);
    jm_alu(b, JA_XOR, JR_S2, JR_S2, JR_S1); /* the word's qret, if it is for this ret_insn */
    jm_cmpi(b, JR_S2, 1);
    uint32_t miss = jm_fwd(b, JC_HI);
    jm_st8(b, JR_S2, JR_FR, FR_OFF(qret));
    uint32_t hit = jm_fwd(b, JC_AL);
    jm_bind(b, miss);
    jm_mov(b, JR_A0, JR_FR);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)fn);
    jm_movi(b, JR_A2, (uint64_t)(uintptr_t)word);
    jm_ccall(b, (const void *)jit_tail_qret);
    jm_st8(b, JR_R0, JR_FR, FR_OFF(qret));
    jm_bind(b, no), jm_bind(b, hit);
    jm_ld32(b, JR_S1, JR_FR, FR_OFF(base)); /* (a call keeps no scratch register) */
    jm_add32i(b, JR_S2, JR_S1, fn->rsize);
    P = jm_proc(b, JR_S3);
    /* This frame popped, then fn's pushed (proc_pop_frame, then
     * proc_push_frame_fitting), each saving and restoring sp and fp only for
     * a function that is not stackless. */
    if (!j->f->stackless && !fn->stackless) { /* both: sp and fp are the entry sp */
        jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp));
        jm_st(b, JR_S0, P, P_OFF(sp));
        jm_st(b, JR_S0, P, P_OFF(fp));
    } else if (!j->f->stackless) { /* this one's back, and fn leaves them so */
        jm_ld(b, JR_S0, JR_FR, FR_OFF(entry_sp));
        jm_st(b, JR_S0, P, P_OFF(sp));
        jm_ld(b, JR_S0, JR_FR, FR_OFF(saved_fp));
        jm_st(b, JR_S0, P, P_OFF(fp));
    } else if (!fn->stackless) { /* fn's saved, as its call would */
        jm_ld(b, JR_S0, P, P_OFF(sp));
        jm_st(b, JR_S0, JR_FR, FR_OFF(entry_sp));
        jm_ld(b, JR_S4, P, P_OFF(fp)); /* (S2 holds the new rtop) */
        jm_st(b, JR_S4, JR_FR, FR_OFF(saved_fp));
        jm_st(b, JR_S0, P, P_OFF(fp));
    }
    jm_movi(b, JR_S4, (uint64_t)(uintptr_t)fn);
    jm_st(b, JR_S4, JR_FR, FR_OFF(fn));
    if (fn->stackless != j->f->stackless) {
        jm_movi(b, JR_S4, fn->stackless);
        jm_st8(b, JR_S4, JR_FR, FR_OFF(stackless));
    }
    jm_movi(b, JR_S0, 0);
    jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
    if (clo < 0) jm_stz(b, JR_FR, FR_OFF(closure));
    jm_st32(b, JR_S2, P, P_OFF(rtop));
    jm_clear(b, JR_WIN, (int64_t)fn->zoff * 8, fn->zlen);
    uint32_t cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        int bk = (int)type_bank((MlwsType)mf->params[k]);
        jm_ld(b, JR_S0, JR_CTX, C_OFF(retv) + 8 * k);
        jm_st(b, JR_S0, JR_WIN, (int64_t)(fn->roff[bk] + cnt[bk]++) * 8);
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
static uint32_t jit_global(JComp *j, const Insn *in, uint32_t pc, int tag, unsigned set) {
    JBuf *b = &j->b;
    uint32_t g = (uint32_t)((uint64_t)in->k.i >> 32), off = (uint32_t)in->k.i;
    /* A process's segment of a module is the module's globals, of its size:
     * a word inside the function's own module's needs no check of the size
     * (another module's is checked). Once the process has module g's globals
     * it always will (they only grow): one check a block. */
    const LModule *own = j->f->mod;
    bool inside = own && g == (uint32_t)own->index && (uint64_t)off + 8 <= own->m->gsize;
    int64_t seg = (int64_t)g * (int64_t)sizeof(Segment); /* (the segment's fields at immediate offsets) */
    int P = jm_proc(b, JR_S0);
    if (j->jc.gok != (int32_t)g) {
        jm_ld32(b, JR_S1, P, P_OFF(nglobals));
        jm_cmpi32(b, JR_S1, g);
        SIDE_IF(JC_LS); /* nglobals <= g */
        j->jc.gok = (int32_t)g;
    }
    jm_ld(b, JR_S0, P, P_OFF(globals));
    if (!inside) {
        jm_ld32(b, JR_S1, JR_S0, seg + (int64_t)offsetof(Segment, size));
        jm_movi(b, JR_S2, (uint64_t)off + 8);
        jm_cmp(b, JR_S1, JR_S2);
        SIDE_IF(JC_LO);
    }
    jm_ld(b, JR_S1, JR_S0, seg + (int64_t)offsetof(Segment, tags));
    if (tag) {
        jm_ld8(b, JR_S2, JR_S1, off >> 3);
        jm_cmpi32(b, JR_S2, T_R);
        SIDE_IF(tag > 0 ? JC_NE : JC_EQ);
    } else {
        jm_movi(b, JR_S2, set);
        jm_st8(b, JR_S2, JR_S1, off >> 3);
    }
    jm_ld(b, JR_S0, JR_S0, seg + (int64_t)offsetof(Segment, data));
    if (off <= INT32_MAX) return off; /* the word: at S0 + off */
    jm_movi(b, JR_S1, off); /* (beyond a 32-bit displacement) */
    jm_alu(b, JA_ADD, JR_S0, JR_S0, JR_S1);
    return 0;
}

static bool jit_alloc_inline(const Insn *in, uint16_t x) {
    if (x == Q_NEW) {
        const StructDef *def = in->k.p;
        return sizeof(ObjStruct) + def->nfields * sizeof(Word) <= GC_SMALL_MAX;
    }
    if (x == Q_LNEW) {
        int64_t cap = in->k.i;
        return cap > 0 && cap <= LIST_INLINE_MAX &&
               sizeof(ObjList) + (size_t)cap * (sizeof(Word) + 1) <= GC_SMALL_MAX;
    }
    return false;
}

static void jit_record_all(const JComp *j, JSide *sd) {
    const JCache *c = &j->jc;
    sd->na = 0;
    for (int bank = 0; bank < 2; bank++)
        for (int k = 0; k < jc_nslots(bank); k++) {
            int v = c->vm[bank][k];
            if (v < 0 || v >= JC_PBASE) continue;
            sd->abank[sd->na] = (uint8_t)(bank == 0 && v >= JC_RBASE ? 2 : bank);
            sd->amreg[sd->na] = (uint8_t)jc_mreg(bank, k);
            sd->avreg[sd->na++] = (uint8_t)(v >= JC_RBASE ? v - JC_RBASE : v);
        }
}

/* new (a struct) or lnew (a list with its storage inside it): the allocator's
 * fast path (heap.h, gc_alloc) in machine code, leaving the object in JR_S0
 * without flushing cached registers. When the hole is full, branches to an
 * out-of-line stub (sd->alloc) that flushes, calls C, reloads cached registers
 * and jumps back with the object in JR_S0. Returns the side index. */
static size_t jit_alloc_fast(JComp *j, const Insn *in, uint32_t pc, bool st) {
    JBuf *b = &j->b;
    int64_t cap = st ? 0 : in->k.i;
    const StructDef *def = in->k.p;
    size_t size = st ? sizeof(ObjStruct) + def->nfields * sizeof(Word) : sizeof(ObjList) + (size_t)cap * (sizeof(Word) + 1);
    size_t n = (size + 15) & ~(size_t)15;
    int P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S0, P, P_OFF(bump));
    jm_ld(b, JR_S1, P, P_OFF(bump_lim));
    jm_alu(b, JA_SUB, JR_S1, JR_S1, JR_S0);
    jm_cmpi(b, JR_S1, (int64_t)n);
    SIDE_IF(JC_LO);
    size_t side = j->sides.n - 1;
    JSide *sd = &j->sides.v[side];
    sd->alloc = true;
    sd->alloc_struct = st;
    jit_record_all(j, sd);
    jm_ld(b, JR_S1, P, P_OFF(heap_bytes));
    jm_alui(b, JA_ADD, JR_S1, JR_S1, (int64_t)size);
    jm_st(b, JR_S1, P, P_OFF(heap_bytes));
    jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)n);
    jm_st(b, JR_S1, P, P_OFF(bump));
    jm_ld(b, JR_S1, P, P_OFF(bump_base));
    jm_alu(b, JA_SUB, JR_S1, JR_S0, JR_S1);
    jm_st32(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, off));
    jm_movi(b, JR_S1, (st ? OBJ_STRUCT : OBJ_LIST) | (uint64_t)(n / 16) << 16); /* kind, no flags, slab */
    jm_st32(b, JR_S1, JR_S0, 0);
    jm_ld(b, JR_S1, P, P_OFF(young_plain_bytes));
    jm_alui(b, JA_ADD, JR_S1, JR_S1, (int64_t)size);
    jm_st(b, JR_S1, P, P_OFF(young_plain_bytes));
    jm_ld(b, JR_S1, P, P_OFF(objects_allocated));
    jm_alui(b, JA_ADD, JR_S1, JR_S1, 1);
    jm_st(b, JR_S1, P, P_OFF(objects_allocated));
    if (st) {
        jm_movi(b, JR_S1, (uint64_t)(uintptr_t)def);
        jm_st(b, JR_S1, JR_S0, (int64_t)offsetof(ObjStruct, def));
    } else { /* (the rest of the hole is zeroed: no length, no elements) */
        jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)sizeof(ObjList));
        jm_st(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
        jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)(sizeof(ObjList) + (size_t)cap * sizeof(Word)));
        jm_st(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
        jm_movi(b, JR_S1, (uint64_t)cap);
        jm_st32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, cap));
    }
    return side;
}

/* The write barrier (heap.h, gc_write), before a store of the object in v
 * into the object in o: a young one into an old object not yet remembered is
 * a side exit, where the interpreter stores it and remembers the object. t:
 * a scratch register. */
static void jit_barrier(JComp *j, uint32_t pc, unsigned r, int o, int v, int t) {
    JBuf *b = &j->b;
    if (j->jc.rk[r].young) return; /* made in this block: young until it is left */
    jm_ld8(b, t, o, (int64_t)offsetof(Obj, flags)); /* (most stores are into young objects) */
    jm_alui(b, JA_AND, t, t, OBJF_OLD | OBJF_REMEMBERED);
    jm_cmpi32(b, t, OBJF_OLD);
    uint32_t not_old = jm_fwd(b, JC_NE);
    uint32_t none = jm_fwd_cbz(b, v, false);
    jm_ld8(b, t, v, (int64_t)offsetof(Obj, flags));
    jm_alui(b, JA_AND, t, t, OBJF_OLD | OBJF_IMMORTAL);
    jm_cmpi32(b, t, 0);
    SIDE_IF(JC_EQ);
    jm_bind(b, not_old);
    jm_bind(b, none);
}

static void jit_barrier_up(JComp *j, uint32_t pc, int o, int v, int t) {
    JBuf *b = &j->b;
    jm_ld8(b, t, o, (int64_t)offsetof(Obj, flags));
    jm_alui(b, JA_AND, t, t, OBJF_OLD | OBJF_REMEMBERED);
    jm_cmpi32(b, t, OBJF_OLD);
    uint32_t not_old = jm_fwd(b, JC_NE);
    uint32_t none = jm_fwd_cbz(b, v, false);
    jm_ld8(b, t, v, (int64_t)offsetof(Obj, flags));
    jm_alui(b, JA_AND, t, t, OBJF_OLD | OBJF_IMMORTAL);
    jm_cmpi32(b, t, 0);
    SIDE_IF(JC_EQ);
    jm_bind(b, not_old);
    jm_bind(b, none);
}

/* ---- elements, as ELEM_LOAD and ELEM_STORE: a list element tagged with the
 * instruction's type, or an element of an array of it, and for a load into
 * an I register, a byte of a string or byte buffer; S0 the object, ix the
 * index's register; anything else is a side exit. ---- */
static void jit_elem_access(JBuf *b, bool store, int bank, int val, int ix) { /* at S1 + ix*8 */
    if (store) bank == 1 ? jm_fstx(b, val, JR_S1, ix) : jm_stx(b, val, JR_S1, ix);
    else bank == 1 ? jm_fldx(b, val, JR_S1, ix) : jm_ldx(b, val, JR_S1, ix);
}
static void jit_elem_access_d(JBuf *b, bool store, int bank, int val, int base, int ix, int64_t off) { /* at base + ix*8 + off */
    if (store) bank == 1 ? jm_fstxd(b, val, base, ix, off) : jm_stxd(b, val, base, ix, off);
    else bank == 1 ? jm_fldxd(b, val, base, ix, off) : jm_ldxd(b, val, base, ix, off);
}
/* A list element's tag, in S2, for a load of type: another is a side exit,
 * whose stub gives zero (null) for an element never written and comes back
 * (jit_list_back, after the load). Returns the side's number. */
static size_t jit_list_tag(JComp *j, uint32_t pc, unsigned type, int bank, int val) {
    JBuf *b = &j->b;
    jm_cmpi32(b, JR_S2, type);
    SIDE_IF(JC_NE);
    JSide *sd = &j->sides.v[j->sides.n - 1];
    sd->unset = true, sd->ubank = (uint8_t)bank, sd->uval = (uint8_t)val;
    return j->sides.n - 1;
}
static void jit_list_back(JComp *j, size_t side) { j->sides.v[side].back = (uint32_t)j->b.n; }
/* The side just made for a list without room for n more (lpush): growing
 * it, in its stub, instead of the exit. (Growing collects nothing: an
 * allocation only asks for a collection, at the next safepoint, so what the
 * block knows young stays young.) */
static void jit_grow_side(JComp *j, unsigned list, unsigned n) {
    JSide *sd = &j->sides.v[j->sides.n - 1];
    sd->grow = true, sd->glist = (uint8_t)list, sd->gn = (uint8_t)n;
    jit_record_all(j, sd);
    sd->back = (uint32_t)j->b.n;
}
static JIT_CC void jit_list_grow(Process *p, ObjList *l, uint64_t n) { list_grow(p, l, (uint64_t)l->len + n); }

static void jit_elem(JComp *j, const Insn *in, uint32_t pc, unsigned r, int ix, int ix_vreg, bool store, int bank, int val) {
    JBuf *b = &j->b;
    unsigned type = in->type;
    if (j->jc.rk[r].kind == OBJ_ARRAY && j->jc.rk[r].elem == type) { /* known typed array: its path alone */
        int obj = jreg_r(j, r, JR_S0);
        if (!jc_aok_has(&j->jc, r, ix_vreg)) {
            jm_ld32(b, JR_S1, obj, (int64_t)offsetof(ObjArray, len));
            jm_cmp(b, ix, JR_S1);
            SIDE_IF(JC_HS);
            jc_aok_add(&j->jc, r, ix_vreg);
        }
        if (store && bank == 2) jit_barrier(j, pc, r, obj, val, JR_S2);
        jit_elem_access_d(b, store, bank, val, obj, ix, (int64_t)offsetof(ObjArray, data));
        return;
    }
    jld_r(j, JR_S0, r);
    if (j->jc.rk[r].kind == OBJ_LIST) { /* known to be a list: its path alone */
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        jm_cmp(b, ix, JR_S1);
        SIDE_IF(JC_HS);
        if (store && bank == 2) jit_barrier(j, pc, r, JR_S0, val, JR_S2);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
        size_t unset = SIZE_MAX;
        if (store) {
            jm_movi(b, JR_S2, type);
            jm_st8x(b, JR_S2, JR_S1, ix);
        } else {
            jm_ld8x(b, JR_S2, JR_S1, ix);
            unset = jit_list_tag(j, pc, type, bank, val);
        }
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
        jit_elem_access(b, store, bank, val, ix);
        if (unset != SIZE_MAX) jit_list_back(j, unset);
        return;
    }
    SIDE_IF_NULL(JR_S0);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
    jm_cmpi32(b, JR_S1, OBJ_LIST);
    uint32_t to_arr = jm_fwd(b, JC_NE);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
    jm_cmp(b, ix, JR_S1);
    SIDE_IF(JC_HS);
    if (store && bank == 2) jit_barrier(j, pc, r, JR_S0, val, JR_S2);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
    size_t unset = SIZE_MAX;
    if (store) {
        jm_movi(b, JR_S2, type);
        jm_st8x(b, JR_S2, JR_S1, ix);
    } else {
        jm_ld8x(b, JR_S2, JR_S1, ix);
        unset = jit_list_tag(j, pc, type, bank, val);
    }
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
    jit_elem_access(b, store, bank, val, ix);
    if (unset != SIZE_MAX) jit_list_back(j, unset);
    uint32_t to_done = jm_fwd(b, JC_AL), to_done2 = 0, to_done3 = 0;
    jm_bind(b, to_arr);
    jm_cmpi32(b, JR_S1, OBJ_ARRAY);
    if (!store && bank == 0 && (type == T_I64 || type == T_U64)) {
        uint32_t is_arr = jm_fwd(b, JC_EQ);
        jm_cmpi32(b, JR_S1, OBJ_STRING);
        uint32_t not_str = jm_fwd(b, JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjString, len));
        jm_cmp(b, ix, JR_S1);
        SIDE_IF(JC_HS);
        jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)offsetof(ObjString, data));
        jm_ld8x(b, val, JR_S1, ix);
        to_done2 = jm_fwd(b, JC_AL);
        jm_bind(b, not_str);
        jm_cmpi32(b, JR_S1, OBJ_BYTES);
        SIDE_IF(JC_NE);
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjBytes, len));
        jm_cmp(b, ix, JR_S1);
        SIDE_IF(JC_HS);
        jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjBytes, data));
        jm_ld8x(b, val, JR_S1, ix);
        to_done3 = jm_fwd(b, JC_AL);
        jm_bind(b, is_arr);
    } else {
        SIDE_IF(JC_NE);
    }
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, len));
    jm_cmp(b, ix, JR_S1);
    SIDE_IF(JC_HS);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, elem));
    jm_cmpi32(b, JR_S1, type);
    SIDE_IF(JC_NE);
    if (store && bank == 2) jit_barrier(j, pc, r, JR_S0, val, JR_S2);
    jm_alui(b, JA_ADD, JR_S1, JR_S0, (int64_t)offsetof(ObjArray, data));
    jit_elem_access(b, store, bank, val, ix);
    jm_bind(b, to_done);
    if (to_done2) jm_bind(b, to_done2), jm_bind(b, to_done3);
}

/* A struct from R register r, checked as IS_STRUCT checks it: of the type in
 * k. Returns the machine register holding the struct pointer (its cached slot
 * if already checked, else JR_S0). */
static int jit_struct(JComp *j, const Insn *in, uint32_t pc, unsigned r) {
    JBuf *b = &j->b;
    JRKnown *k = &j->jc.rk[r];
    if (k->kind == OBJ_STRUCT && k->def == in->k.p) return jreg_r(j, r, JR_S0); /* checked already */
    jld_r(j, JR_S0, r);
    k->kind = OBJ_STRUCT, k->def = in->k.p; /* from here on (the checks side-exit) */
    SIDE_IF_NULL(JR_S0);
    jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
    jm_cmpi32(b, JR_S1, OBJ_STRUCT);
    SIDE_IF(JC_NE);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjStruct, def));
    jm_movi(b, JR_S2, (uint64_t)(uintptr_t)in->k.p);
    jm_cmp(b, JR_S1, JR_S2);
    SIDE_IF(JC_NE);
    return JR_S0;
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
static JIT_CC JitWord jit_mget(const Obj *ob, const Insn *in) {
    if (!ob || ob->kind != OBJ_MAP) return (JitWord){0, 0};
    Word key;
    key.r = (Obj *)in->k.p;
    const MapEntry *e = map_find((ObjMap *)ob, key, T_R);
    if (e) return e->vtag == T_R ? (JitWord){(uint64_t)(uintptr_t)e->val.r, 1} : (JitWord){0, 0};
    return in->nops < 4 ? (JitWord){0, 0} : (JitWord){0, 1};
}

/* Q_MGETV_*'s, by a key in a register (its bits in key, its type in
 * Insn.target), of a value of type tag, or the default in Insn.k. */
#if defined(_WIN32) && defined(__x86_64__)
static JIT_CC ObjStruct *jit_struct_new(Process *p, const StructDef *def) { return struct_new(p, def); }
static JIT_CC ObjList *jit_list_new(Process *p, int64_t cap) { return list_new(p, cap); }
#else
#define jit_struct_new struct_new
#define jit_list_new list_new
#endif

/* The string builders' quickened forms, called from the machine code with
 * the windows written back (q_strcat and the others, interp.c): NULL or 0
 * leaves the instruction to the interpreter, nothing done. */
static JIT_CC Obj *jit_strcat(Process *p, const Insn *in, Word *R) { return q_strcat(p, in, R); }
static JIT_CC uintptr_t jit_bufcat(Process *p, const Insn *in, Word *R) { return q_bufcat(p, in, R); }
static JIT_CC Obj *jit_substr(Process *p, const Insn *in, Word *R, const int64_t *I) { return q_substr(p, in, R, I); }
static JIT_CC uintptr_t jit_bufpush(Process *p, Obj *b, int64_t v) { return q_bufpush(p, b, v); }
static JIT_CC Obj *jit_bufnew(Process *p, int64_t n) { return &bytes_new(p, n)->h; }
static JIT_CC Obj *jit_tostr_i(Process *p, int64_t v) {
    Word w;
    w.i = v;
    return &value_to_string(p, w, T_I64)->h;
}
static JIT_CC Obj *jit_tostr_f(Process *p, int64_t bits) {
    Word w;
    w.i = bits;
    return &value_to_string(p, w, T_F64)->h;
}

static JIT_CC Obj *jit_map_new(Process *p) { return &map_new(p)->h; }
static JIT_CC uintptr_t jit_mset(Process *p, const Insn *in, const uint64_t *win, const Function *fn) {
    return q_mset(p, in, (const int64_t *)win, (const double *)(win + fn->roff[BANK_F]),
                  (const Word *)(win + fn->roff[BANK_R]));
}
static JIT_CC JitWord jit_mhas(const Obj *ob, const Insn *in, uint64_t key) {
    if (!ob || ob->kind != OBJ_MAP) return (JitWord){0, 0};
    Word k;
    if (in->target & 0x10000u) k.r = (Obj *)in->k.p;
    else k.u = key;
    return (JitWord){map_find((ObjMap *)ob, k, (uint8_t)(in->target & 0xFF)) != NULL, 1};
}

/* Raw indexed byte memory check (P{I}, .u8): verifies raw segment, live,
 * generation and bounds (or uses the enclosing loop's hoisted base & size);
 * returns the data base register and byte offset register in out_base and out_off.
 * Note: for a hoisted pointer with non-zero offset, comparing ix unsigned
 * against (size - off) keeps the fast path to one compare; a negative ix with
 * off + ix >= 0 side-exits cleanly to the interpreter. */
static void jit_raw_u8(JComp *j, uint32_t pc, unsigned preg, int ix, int *out_base, int *out_off) {
    JBuf *b = &j->b;
    if (j->loop) {
        for (int k = 0; k < j->loop->np; k++) {
            if (j->loop->pvreg[k] == preg) {
                int base = jc_ireg[j->loop->pbase_slot[k]];
                int sz = jc_ireg[j->loop->psize_slot[k]];
                jm_cmp(b, ix, sz);
                SIDE_IF(JC_HS);
                *out_base = base;
                *out_off = ix;
                return;
            }
        }
    }
    _Static_assert(sizeof(Segment) == 24, "Segment size assumed 24 bytes");
    _Static_assert(offsetof(Segment, gen) == 20 && offsetof(Segment, kind) == 22 && offsetof(Segment, live) == 23,
                   "Segment gen/kind/live packed in one 32-bit word");
    jm_ld(b, JR_S0, JR_WIN, jwoff(j, BANK_P, preg));
    jm_alui(b, JA_SHR, JR_S1, JR_S0, 44);
    jm_alui(b, JA_SUB, JR_S1, JR_S1, SEG_RAW0);
    int P = jm_proc(b, JR_S3);
    jm_ld(b, JR_S2, P, P_OFF(raw.n));
    jm_cmp(b, JR_S1, JR_S2);
    SIDE_IF(JC_HS);
    jm_alui(b, JA_SHL, JR_S2, JR_S1, 1);
    jm_alu(b, JA_ADD, JR_S1, JR_S1, JR_S2); /* raw_idx * 3 */
    jm_ld(b, JR_S2, P, P_OFF(raw.v));
    jm_addsh3(b, JR_S1, JR_S2, JR_S1); /* &p->raw.v[raw_idx] */
    jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Segment, gen));
    jm_alui(b, JA_SHR, JR_S3, JR_S0, 32);
    jm_alui(b, JA_AND, JR_S3, JR_S3, 0xFFFu);
    jm_alui(b, JA_OR, JR_S3, JR_S3, (int64_t)((uint64_t)SEGK_RAW << 16 | (uint64_t)1 << 24));
    jm_cmp32(b, JR_S2, JR_S3);
    SIDE_IF(JC_NE);
    jm_mov32(b, JR_S0, JR_S0); /* zero-extend PTR_OFF(ptr) */
    jm_alu(b, JA_ADD, JR_S0, JR_S0, ix);
    jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Segment, size));
    jm_cmp(b, JR_S0, JR_S2);
    SIDE_IF(JC_HS);
    jm_ld(b, JR_S1, JR_S1, (int64_t)offsetof(Segment, data));
    *out_base = JR_S1;
    *out_off = JR_S0;
}

static JitWord jit_mgetv(const Obj *ob, const Insn *in, uint64_t key, uint8_t tag) {
    if (!ob || ob->kind != OBJ_MAP) return (JitWord){0, 0};
    Word k;
    k.u = key;
    const MapEntry *e = map_find((ObjMap *)ob, k, (uint8_t)in->target);
    if (e) return e->vtag == tag ? (JitWord){e->val.u, 1} : (JitWord){0, 0};
    if (in->nops < 4) return (JitWord){0, 0}; /* E_KEY: the handler's */
    uint64_t d;
    memcpy(&d, &in->k, 8);
    return (JitWord){d, 1};
}
static JIT_CC JitWord jit_mgetv_i(const Obj *ob, const Insn *in, uint64_t key) { return jit_mgetv(ob, in, key, T_I64); }
static JIT_CC JitWord jit_mgetv_f(const Obj *ob, const Insn *in, uint64_t key) { return jit_mgetv(ob, in, key, T_F64); }
static JIT_CC JitWord jit_mgetv_r(const Obj *ob, const Insn *in, uint64_t key) { return jit_mgetv(ob, in, key, T_R); }

/* A generic instruction's operand (jit_gen_operand) in a machine register:
 * an I register's, or the constant or capture in `into`. */
static int jit_gen_src(JComp *j, const Operand *o, uint32_t pc, int into) {
    JBuf *b = &j->b;
    if (o->mode == OM_REG) return jc_get(j, 0, REG_IDX(o->reg));
    if (o->mode == OM_IMM) {
        jm_movi(b, into, (uint64_t)o->imm);
        return into;
    }
    uint32_t u = (uint32_t)o->index; /* a capture, read as the generic instruction does if it is an i64 */
    jm_ld(b, JR_S0, JR_FR, FR_OFF(closure));
    SIDE_IF_NULL(JR_S0);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjClosure, nup));
    jm_cmpi32(b, JR_S1, u);
    SIDE_IF(JC_LS);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjClosure, tags));
    jm_ld8(b, JR_S1, JR_S1, u);
    jm_cmpi32(b, JR_S1, T_I64);
    SIDE_IF(JC_NE);
    jm_ld(b, into, JR_S0, (int64_t)offsetof(ObjClosure, up) + (int64_t)u * 8);
    return into;
}

/* ldv's value: the tag in S2, the word in S4, into r[0] (i64) and r[1]
 * (f64), the other zero; none gives zeros, any other tag the handler. */
static void jit_ldv_value(JComp *j, const Insn *in, uint32_t pc) {
    JBuf *b = &j->b;
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
}

/* A value's two words, as the compilers read them: an element and the one
 * after it, of another bank (Lox: `mov.f64 F, X[i]`, `mov.r R, X[i+1]`), or
 * a reference and then ldv's number (Lua: `mov R, X[i]`, `ldv I, F,
 * X[i+1]`); jit_is_pair. One check that X is a list, then both loads.
 * Anything but a list stops before the first (an array cannot give both: its
 * elements have one type); the second's checks stop before it, the first
 * done. From here on the block knows X is a list. */
static void jit_pair_load(JComp *j, const Insn *in, uint32_t pc, int bank, int ix) {
    JBuf *b = &j->b;
    int val = bank == 1 ? JF_S0 : JR_S4, d;
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
    jm_ld8x(b, JR_S2, JR_S1, ix);
    size_t unset = jit_list_tag(j, pc, in->type, bank, val);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
    jit_elem_access(b, false, bank, val, ix);
    jit_list_back(j, unset);
    if (bank == 2) jst_r(j, JR_S4, in->r[0]);
    else if (bank == 1) d = jc_def(j, 1, in->r[0]), jm_fmov(b, d, JF_S0);
    else d = jc_def(j, 0, in->r[0]), jm_mov(b, d, JR_S4);
}
static void jit_pair(JComp *j, const Insn *in, uint32_t pc, uint16_t x1, uint16_t x2) {
    JBuf *b = &j->b;
    j->jc.pin[0] = j->jc.pin[1] = 0;
    j->jc.cur_pin[0] = j->jc.cur_pin[1] = 0;
    int bank0 = x1 - Q_LDE_I;
    const Insn *v = in + 1;
    unsigned r = in->r[1];
    int ix = jc_get(j, 0, in->r[2]);
    jld_r(j, JR_S0, r);
    if (j->jc.rk[r].kind != OBJ_LIST) {
        SIDE_IF_NULL(JR_S0);
        jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
        jm_cmpi32(b, JR_S1, OBJ_LIST);
        SIDE_IF(JC_NE);
        j->jc.rk[r].kind = OBJ_LIST, j->jc.rk[r].def = NULL;
    }
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
    jm_cmp(b, ix, JR_S1);
    SIDE_IF(JC_HS);
    jit_pair_load(j, in, pc, bank0, ix);
    pc++; /* the second */
    jm_alui(b, JA_ADD, JR_S3, ix, 1);
    jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
    jm_cmp(b, JR_S3, JR_S1);
    SIDE_IF(JC_HS);
    if (x2 != Q_LDV_ED) {
        jit_pair_load(j, v, pc, x2 - Q_LDED_I, JR_S3);
        return;
    }
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
    jm_ld8x(b, JR_S2, JR_S1, JR_S3);
    jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
    jm_ldx(b, JR_S4, JR_S1, JR_S3);
    jit_ldv_value(j, v, pc);
}

/* Whether pc and pc + 1 are such a pair (the second's form, or 0). */
static uint16_t jit_is_pair(const Function *f, const uint16_t *jx, const uint8_t *lead, uint32_t pc) {
    if (pc + 1 >= f->ncode || lead[pc + 1] || jx[pc] < Q_LDE_I || jx[pc] > Q_LDE_R ||
        jit_steps(&f->code[pc], jx[pc]) || jit_steps(&f->code[pc + 1], jx[pc + 1]))
        return 0;
    const Insn *in = &f->code[pc], *v = in + 1;
    int bank = jx[pc] - Q_LDE_I;
    if (in->r[0] == (bank == 2 ? in->r[1] : bank == 0 ? in->r[2] : 256)) return 0; /* (it changes X or i) */
    if (jx[pc + 1] == Q_LDV_ED)
        return bank == 2 && in->r[1] == (unsigned)v->k.i && in->r[2] == v->target && (int8_t)v->r[2] == 1 ? Q_LDV_ED : 0;
    if (jx[pc + 1] < Q_LDED_I || jx[pc + 1] > Q_LDED_R || jx[pc + 1] - Q_LDED_I == bank) return 0;
    return in->r[1] == v->r[1] && in->r[2] == v->r[2] && v->k.i == 1 ? jx[pc + 1] : 0;
}

/* A generic f64 operand (jit_fgen_operand) loaded into a float register (`into`
 * if constant or element, or its cache register if an F register). */
static int jit_fgen_src(JComp *j, const Insn *in, const Operand *o, uint32_t pc, int into) {
    JBuf *b = &j->b;
    if (o->mode == OM_REG) return jc_get(j, 1, REG_IDX(o->reg));
    if (o->mode == OM_FIMM || o->mode == OM_IMM) {
        jm_fconst(b, into, o->f);
        return into;
    }
    int ix, ix_vreg = -1;
    if (o->mode == OM_ELEMI) {
        jm_movi(b, JR_S3, (uint32_t)o->index);
        ix = JR_S3;
    } else {
        ix = jc_get(j, 0, REG_IDX(o->ireg));
        if (o->imm) jm_alui(b, JA_ADD, JR_S3, ix, o->imm), ix = JR_S3;
        else ix_vreg = REG_IDX(o->ireg);
    }
    unsigned r = (unsigned)REG_IDX(o->reg);
    jit_elem(j, in, pc, r, ix, ix_vreg, false, 1, into);
    return into;
}

static void jit_insn(JComp *j, const Insn *in, uint16_t x, uint32_t pc, uint32_t *cont) {
    JBuf *b = &j->b;
    int shape, kind, s1, s2, d, op;
    uint32_t goff; /* a global's word, at S0 + goff (jit_global) */
    uint32_t t = pc + (uint32_t)(int32_t)in->target; /* a branch's target (relative in this build) */
    j->jc.pin[0] = j->jc.pin[1] = 0;
    j->jc.cur_pin[0] = j->jc.cur_pin[1] = 0;
    switch (x) {
    case JX_ARITH: { /* (two operands: the destination is the first; 4+: accumulate in S3) */
        static const int ops[] = {[OP_ADD] = JA_ADD, [OP_SUB] = JA_SUB, [OP_MUL] = JA_MUL,
                                  [OP_AND] = JA_AND, [OP_OR] = JA_OR, [OP_XOR] = JA_XOR};
        int aop = ops[in->op];
        if (in->nops <= 3) {
            const Operand *a = &in->ops[in->nops == 3 ? 1 : 0], *c = &in->ops[in->nops - 1];
            s1 = jit_gen_src(j, a, pc, JR_S3);
            s2 = jit_gen_src(j, c, pc, JR_S4);
            d = jc_def(j, 0, REG_IDX(in->ops[0].reg));
            jm_alu(b, aop, d, s1, s2);
            return;
        }
        s1 = jit_gen_src(j, &in->ops[1], pc, JR_S3);
        if (s1 != JR_S3) jm_mov(b, JR_S3, s1);
        j->jc.pin[0] = 0; /* each source is consumed into S3: free its slot */
        j->jc.cur_pin[0] = 0;
        for (int k = 2; k < in->nops; k++) {
            if (in->ops[k].mode == OM_IMM) {
                jm_alui(b, aop, JR_S3, JR_S3, in->ops[k].imm);
            } else {
                s2 = jit_gen_src(j, &in->ops[k], pc, JR_S4);
                jm_alu(b, aop, JR_S3, JR_S3, s2);
                j->jc.pin[0] = 0;
                j->jc.cur_pin[0] = 0;
            }
        }
        d = jc_def(j, 0, REG_IDX(in->ops[0].reg));
        jm_mov(b, d, JR_S3);
        return;
    }
    case JX_FARITH: {
        static const int fops[] = {[OP_ADD] = JF_ADD, [OP_SUB] = JF_SUB, [OP_MUL] = JF_MUL, [OP_DIV] = JF_DIV};
        int fop = fops[in->op];
        const Operand *dst = &in->ops[0];
        /* Fast path for 2-op read-modify-write on a known typed array element:
         * single bounds check and single indexed load/op/store without re-checking. */
        if (in->nops == 2 && (dst->mode == OM_ELEM || dst->mode == OM_ELEMI)) {
            unsigned r = (unsigned)REG_IDX(dst->reg);
            if (j->jc.rk[r].kind == OBJ_ARRAY && j->jc.rk[r].elem == in->type) {
                int ix, ix_vreg = -1;
                if (dst->mode == OM_ELEMI) {
                    jm_movi(b, JR_S3, (uint32_t)dst->index);
                    ix = JR_S3;
                } else {
                    ix = jc_get(j, 0, REG_IDX(dst->ireg));
                    if (dst->imm) jm_alui(b, JA_ADD, JR_S3, ix, dst->imm), ix = JR_S3;
                    else ix_vreg = REG_IDX(dst->ireg);
                }
                int obj = jreg_r(j, r, JR_S0);
                if (!jc_aok_has(&j->jc, r, ix_vreg)) {
                    jm_ld32(b, JR_S1, obj, (int64_t)offsetof(ObjArray, len));
                    jm_cmp(b, ix, JR_S1);
                    SIDE_IF(JC_HS);
                    jc_aok_add(&j->jc, r, ix_vreg);
                }
                jm_fldxd(b, JF_S0, obj, ix, (int64_t)offsetof(ObjArray, data));
                if (in->ops[1].mode == OM_REG) {
                    s2 = jc_get(j, 1, REG_IDX(in->ops[1].reg));
                } else if (in->ops[1].mode == OM_FIMM || in->ops[1].mode == OM_IMM) {
                    jm_fconst(b, JF_S1, in->ops[1].f);
                    s2 = JF_S1;
                } else {
                    j->jc.pin[0] = 0;
                    j->jc.cur_pin[0] = 0;
                    s2 = jit_fgen_src(j, in, &in->ops[1], pc, JF_S1);
                    j->jc.pin[0] = 0;
                    j->jc.cur_pin[0] = 0;
                    if (dst->mode == OM_ELEMI) {
                        jm_movi(b, JR_S3, (uint32_t)dst->index);
                        ix = JR_S3;
                    } else {
                        ix = jc_get(j, 0, REG_IDX(dst->ireg));
                        if (dst->imm) jm_alui(b, JA_ADD, JR_S3, ix, dst->imm), ix = JR_S3;
                    }
                    obj = jreg_r(j, r, JR_S0);
                }
                jm_fop(b, fop, JF_S0, JF_S0, s2);
                jm_fstxd(b, JF_S0, obj, ix, (int64_t)offsetof(ObjArray, data));
                return;
            }
        }
        int acc = jit_fgen_src(j, in, in->nops == 2 ? dst : &in->ops[1], pc, JF_S0);
        if (acc != JF_S0) jm_fmov(b, JF_S0, acc);
        j->jc.pin[0] = j->jc.pin[1] = 0; /* consumed into JF_S0: free its slot(s) */
        j->jc.cur_pin[0] = j->jc.cur_pin[1] = 0;
        for (int k = in->nops == 2 ? 1 : 2; k < in->nops; k++) {
            s2 = jit_fgen_src(j, in, &in->ops[k], pc, JF_S1);
            jm_fop(b, fop, JF_S0, JF_S0, s2);
            j->jc.pin[0] = j->jc.pin[1] = 0;
            j->jc.cur_pin[0] = j->jc.cur_pin[1] = 0;
        }
        if (dst->mode == OM_REG) {
            d = jc_def(j, 1, REG_IDX(dst->reg));
            jm_fmov(b, d, JF_S0);
        } else {
            int ix, ix_vreg = -1;
            if (dst->mode == OM_ELEMI) {
                jm_movi(b, JR_S3, (uint32_t)dst->index);
                ix = JR_S3;
            } else {
                ix = jc_get(j, 0, REG_IDX(dst->ireg));
                if (dst->imm) jm_alui(b, JA_ADD, JR_S3, ix, dst->imm), ix = JR_S3;
                else ix_vreg = REG_IDX(dst->ireg);
            }
            unsigned r = (unsigned)REG_IDX(dst->reg);
            jit_elem(j, in, pc, r, ix, ix_vreg, true, 1, JF_S0);
        }
        return;
    }
    case JX_SET: {
        static const int sc[] = {[OP_SEQ] = JC_EQ, [OP_SNE] = JC_NE, [OP_SLT] = JC_LT,
                                 [OP_SLE] = JC_LE, [OP_SGT] = JC_GT, [OP_SGE] = JC_GE};
        static const int uc[] = {[OP_SEQ] = JC_EQ, [OP_SNE] = JC_NE, [OP_SLT] = JC_LO,
                                 [OP_SLE] = JC_LS, [OP_SGT] = JC_HI, [OP_SGE] = JC_HS};
        s1 = jit_gen_src(j, &in->ops[1], pc, JR_S3);
        s2 = jit_gen_src(j, &in->ops[2], pc, JR_S4);
        d = jc_def(j, 0, REG_IDX(in->ops[0].reg));
        jm_cmp(b, s1, s2);
        jm_cset(b, d, in->type == T_U64 ? uc[in->op] : sc[in->op]);
        return;
    }
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
    case Q_CALL_R: case Q_CALL_RK: {
        bool saved_keep = j->keep;
        j->keep = true;
        jc_flush(j);
        j->keep = saved_keep;
        *cont = jit_call(j, in, pc);
        return;
    }
    case Q_CALLI_R: case Q_CALLI_RK: {
        bool saved_keep = j->keep;
        j->keep = true;
        jc_flush(j);
        j->keep = saved_keep;
        *cont = jit_calli(j, in, pc);
        return;
    }
    case Q_RET_R: case Q_RET_RK:
        jit_ret(j, in, pc);
        if (!j->loop && !j->jc.record) jc_reset(&j->jc);
        return;
    case Q_TCALL_R:
        if (in->ops[0].rv.ptr != j->f) jc_flush(j);
        jit_tcall(j, in, pc);
        if (!j->loop && !j->jc.record) jc_reset(&j->jc);
        return;
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
        if (j->jc.rk[in->r[0]].kind != OBJ_LIST) {
            SIDE_IF_NULL(JR_S0);
            jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
            jm_cmpi32(b, JR_S1, OBJ_LIST);
            SIDE_IF(JC_NE);
            j->jc.rk[in->r[0]].kind = OBJ_LIST, j->jc.rk[in->r[0]].def = NULL;
        }
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
        int obj = jit_struct(j, in, pc, in->r[load ? 1 : 0]);
        if (bank == 2) {
            if (load) jm_ld(b, JR_S1, obj, off), jst_r(j, JR_S1, in->r[0]);
            else jld_r(j, JR_S1, in->r[1]), jit_barrier(j, pc, in->r[0], obj, JR_S1, JR_S2), jm_st(b, JR_S1, obj, off);
        } else if (load) {
            d = jc_def(j, bank, in->r[0]);
            bank ? jm_fld(b, d, obj, off) : jm_ld(b, d, obj, off);
        } else {
            s1 = jc_get(j, bank, in->r[1]);
            bank ? jm_fst(b, s1, obj, off) : jm_st(b, s1, obj, off);
        }
        return;
    }
    case Q_STFK: { /* a constant to a struct's field: its word in ops[1].kw */
        int64_t off = (int64_t)offsetof(ObjStruct, fields) + (int64_t)in->target * 8;
        int obj = jit_struct(j, in, pc, in->r[0]);
        jm_movi(b, JR_S1, in->ops[1].kw);
        jm_st(b, JR_S1, obj, off);
        return;
    }
    case Q_STEC: case Q_STECK: { /* a constant to an element: its word in ops[1].kw */
        int ix;
        if (x == Q_STECK) jm_movi(b, JR_S3, in->target), ix = JR_S3;
        else ix = jit_index(j, in, true);
        jm_movi(b, JR_S4, in->ops[1].kw);
        jit_elem(j, in, pc, in->r[0], ix, -1, true, 0, JR_S4);
        return;
    }
    case Q_STGK: /* a constant to a global */
        goff = jit_global(j, in, pc, 0, in->type);
        jm_movi(b, JR_S1, in->ops[1].kw);
        jm_st(b, JR_S1, JR_S0, goff);
        return;
    case Q_NEW: case Q_LNEW: { /* the C functions may trap (a quota), so the frame's pc and the count are up to date */
        if (jit_alloc_inline(in, x)) {
            size_t side = jit_alloc_fast(j, in, pc, x == Q_NEW);
            j->sides.v[side].back = (uint32_t)b->n;
            jst_r(j, JR_S0, in->r[0]);
            j->jc.rk[in->r[0]] = (JRKnown){x == Q_NEW ? OBJ_STRUCT : OBJ_LIST, 0, true, x == Q_NEW ? in->k.p : NULL};
            return;
        }
        jc_flush(j);
        jm_movi(b, JR_S0, pc + 1);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jit_sync_insns(j, j->rest[pc] - 1); /* the rest of the block, not yet run */
        jm_mov(b, JR_A0, jm_proc(b, JR_S3));
        jm_movi(b, JR_A1, x == Q_NEW ? (uint64_t)(uintptr_t)in->k.p : (uint64_t)in->k.i);
        jm_ccall(b, x == Q_NEW ? (const void *)jit_struct_new : (const void *)jit_list_new);
        jst_r(j, JR_R0, in->r[0]);
        j->jc.rk[in->r[0]] = (JRKnown){x == Q_NEW ? OBJ_STRUCT : OBJ_LIST, 0, true, x == Q_NEW ? in->k.p : NULL};
        return;
    }
    case Q_STRCAT_RK: case Q_BUFCAT_RK: case Q_TOSTR_I: case Q_TOSTR_F: case Q_SUBSTR_RII: case Q_BUFNEW_K:
    case Q_BUFPUSH_I: { /* by their C functions, from the windows; they may trap (a quota), as new may */
        jc_flush(j);
        jm_movi(b, JR_S0, pc + 1);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jit_sync_insns(j, j->rest[pc] - 1);
        jm_mov(b, JR_A0, jm_proc(b, JR_S3));
        const void *fn;
        bool obj = true; /* (the result an object for r[0], else a flag) */
        switch (x) {
        case Q_STRCAT_RK: case Q_BUFCAT_RK: case Q_SUBSTR_RII:
            jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
            jm_alui(b, JA_ADD, JR_A2, JR_WIN, jwoff(j, BANK_R, 0));
            if (x == Q_SUBSTR_RII) jm_mov(b, JR_A3, JR_WIN);
            fn = x == Q_STRCAT_RK ? (const void *)jit_strcat : x == Q_BUFCAT_RK ? (const void *)jit_bufcat
                                                                               : (const void *)jit_substr;
            obj = x != Q_BUFCAT_RK;
            break;
        case Q_TOSTR_I: case Q_TOSTR_F: /* (an f64's bits, as an integer) */
            jm_ld(b, JR_A1, JR_WIN, jwoff(j, x == Q_TOSTR_I ? BANK_I : BANK_F, in->r[1]));
            fn = x == Q_TOSTR_I ? (const void *)jit_tostr_i : (const void *)jit_tostr_f;
            break;
        case Q_BUFNEW_K:
            jm_movi(b, JR_A1, (uint64_t)in->k.i);
            fn = (const void *)jit_bufnew;
            break;
        default: /* bufpush */
            jm_ld(b, JR_A1, JR_WIN, jwoff(j, BANK_R, in->r[0]));
            jm_ld(b, JR_A2, JR_WIN, jwoff(j, BANK_I, in->r[1]));
            fn = (const void *)jit_bufpush;
            obj = false;
            break;
        }
        jm_ccall(b, fn);
        SIDE_IF_NULL(JR_R0);
        if (obj) jst_r(j, JR_R0, in->r[0]);
        return;
    }
    case Q_MNEW:
        jc_flush(j);
        jm_movi(b, JR_S0, pc + 1);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jit_sync_insns(j, j->rest[pc] - 1);
        jm_mov(b, JR_A0, jm_proc(b, JR_S3));
        jm_ccall(b, (const void *)jit_map_new);
        jst_r(j, JR_R0, in->r[0]);
        return;
    case Q_MSET:
        jc_flush(j);
        jm_movi(b, JR_S0, pc + 1);
        jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
        jit_sync_insns(j, j->rest[pc] - 1);
        jm_mov(b, JR_A0, jm_proc(b, JR_S3));
        jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
        jm_mov(b, JR_A2, JR_WIN);
        jm_movi(b, JR_A3, (uint64_t)(uintptr_t)j->f);
        jm_ccall(b, (const void *)jit_mset);
        SIDE_IF_NULL(JR_R0);
        return;
    case Q_MHAS: {
        uint8_t ktag = (uint8_t)(in->target & 0xFF);
        int kb = ktag == T_I64 ? BANK_I : ktag == T_F64 ? BANK_F : BANK_R;
        jc_flush(j);
        if (in->target & 0x10000u) jm_movi(b, JR_A2, 0);
        else jm_ld(b, JR_A2, JR_WIN, jwoff(j, kb, in->r[2]));
        jld_r(j, JR_A0, in->r[1]);
        jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
        jm_ccall(b, (const void *)jit_mhas);
        SIDE_IF_NULL(JR_R1);
        d = jc_def(j, 0, in->r[0]);
        jm_mov(b, d, JR_R0);
        return;
    }
    case Q_LDMB_U8: {
        int rbase, roff;
        s1 = jc_get(j, 0, in->r[2]);
        jit_raw_u8(j, pc, in->r[1], s1, &rbase, &roff);
        d = jc_def(j, 0, in->r[0]);
        jm_ld8x(b, d, rbase, roff);
        return;
    }
    case Q_STMB_U8: {
        int rbase, roff;
        s1 = jc_get(j, 0, in->r[2]);
        s2 = jc_get(j, 0, in->r[1]);
        jit_raw_u8(j, pc, in->r[0], s1, &rbase, &roff);
        jm_st8x(b, s2, rbase, roff);
        return;
    }
    case Q_STMBK_U8: {
        int rbase, roff;
        s1 = jc_get(j, 0, in->r[2]);
        jit_raw_u8(j, pc, in->r[0], s1, &rbase, &roff);
        jm_movi(b, JR_S2, (uint8_t)in->k.i);
        jm_st8x(b, JR_S2, rbase, roff);
        return;
    }
    case Q_BZ_MB_U8: case Q_BNZ_MB_U8: {
        int rbase, roff;
        s1 = jc_get(j, 0, in->r[1]);
        jit_raw_u8(j, pc, in->r[0], s1, &rbase, &roff);
        jm_ld8x(b, JR_S2, rbase, roff);
        jc_flush(j);
        jbranch(j, FX_CB, 0, JR_S2, x == Q_BNZ_MB_U8, TO_BODY, t);
        return;
    }
    case Q_MGET_R:
        jc_flush(j);
        jld_r(j, JR_A0, in->r[1]);
        jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
        jm_ccall(b, (const void *)jit_mget);
        SIDE_IF_NULL(JR_R1);
        jst_r(j, JR_R0, in->r[0]);
        return;
    case Q_MGETV_I: case Q_MGETV_F: case Q_MGETV_R: { /* a key in a register: from the window, once flushed */
        static const void *const get[] = {(const void *)jit_mgetv_i, (const void *)jit_mgetv_f, (const void *)jit_mgetv_r};
        int bank = x - Q_MGETV_I;
        int kb = in->target == T_I64 ? BANK_I : in->target == T_F64 ? BANK_F : BANK_R;
        jc_flush(j);
        jm_ld(b, JR_A2, JR_WIN, jwoff(j, kb, in->r[2]));
        jld_r(j, JR_A0, in->r[1]);
        jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
        jm_ccall(b, get[bank]);
        SIDE_IF_NULL(JR_R1);
        if (bank == 2) jst_r(j, JR_R0, in->r[0]);
        else if (bank == 1) d = jc_def(j, 1, in->r[0]), jm_fmov_from_x(b, d, JR_R0);
        else d = jc_def(j, 0, in->r[0]), jm_mov(b, d, JR_R0);
        return;
    }
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
        if (!load && bank == 2) jit_barrier_up(j, pc, JR_S0, JR_S4, JR_S2);
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
        unsigned obreg = (unsigned)in->k.i;
        jld_r(j, JR_S0, obreg);
        if (j->jc.rk[obreg].kind == OBJ_LIST) {
            jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
            jm_cmp(b, ix, JR_S1);
            SIDE_IF(JC_HS);
            jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, tags));
            jm_ld8x(b, JR_S2, JR_S1, ix);
            jm_ld(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, items));
            jm_ldx(b, JR_S4, JR_S1, ix);
            jit_ldv_value(j, in, pc);
            return;
        }
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
        jit_ldv_value(j, in, pc);
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
        goff = jit_global(j, in, pc, 1, 0);
        jm_ld(b, JR_S1, JR_S0, goff);
        jc_flush(j);
        jld_r(j, JR_S0, in->r[0]);
        jm_cmp(b, JR_S0, JR_S1);
        jbranch(j, FX_BCOND, x == Q_BEQ_RG ? JC_EQ : JC_NE, 0, false, TO_BODY, t);
        return;
    case Q_LDG_I: case Q_LDG_F: case Q_LDG_R: {
        int bank = x - Q_LDG_I;
        goff = jit_global(j, in, pc, bank == 2 ? 1 : -1, 0);
        if (bank == 2) jm_ld(b, JR_S1, JR_S0, goff), jst_r(j, JR_S1, in->r[0]);
        else d = jc_def(j, bank, in->r[0]), bank ? jm_fld(b, d, JR_S0, goff) : jm_ld(b, d, JR_S0, goff);
        return;
    }
    case Q_STG_I: case Q_STG_F: case Q_STG_R: {
        int bank = x - Q_STG_I;
        if (bank != 2) s1 = jc_get(j, bank, in->r[1]);
        else s1 = -1;
        goff = jit_global(j, in, pc, 0, in->type);
        if (bank == 2) jld_r(j, JR_S1, in->r[1]), jm_st(b, JR_S1, JR_S0, goff);
        else bank ? jm_fst(b, s1, JR_S0, goff) : jm_st(b, s1, JR_S0, goff);
        return;
    }
    case Q_LPUSH_R: case Q_LPUSH_RK: { /* into the list's spare capacity; growing it is the handler's */
        unsigned nv = (unsigned)in->nops - 1;
        jld_r(j, JR_S0, in->r[0]);
        if (j->jc.rk[in->r[0]].kind != OBJ_LIST) {
            SIDE_IF_NULL(JR_S0);
            jm_ld8(b, JR_S1, JR_S0, (int64_t)offsetof(Obj, kind));
            jm_cmpi32(b, JR_S1, OBJ_LIST);
            SIDE_IF(JC_NE);
            j->jc.rk[in->r[0]].kind = OBJ_LIST;
        }
        jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
        jm_ld32(b, JR_S2, JR_S0, (int64_t)offsetof(ObjList, cap));
        jm_sub32(b, JR_S2, JR_S2, JR_S1);
        jm_cmpi32(b, JR_S2, nv);
        SIDE_IF(JC_LO);
        jit_grow_side(j, in->r[0], nv);
        for (unsigned k = 0; k < nv; k++) /* (a constant is never young) */
            if (in->ops[1 + k].mode == OM_REG && REG_BANK(in->ops[1 + k].reg) == BANK_R)
                jld_r(j, JR_S2, (unsigned)REG_IDX(in->ops[1 + k].reg)), jit_barrier(j, pc, in->r[0], JR_S0, JR_S2, JR_S3);
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
                int s = j->jc.record ? -1 : j->jc.of[0][REG_IDX(a->reg)];
                if (s >= 0) jm_st(b, jc_ireg[s], JR_S3, (int64_t)k * 8);
                else jm_ld(b, JR_S2, JR_WIN, jwoff(j, BANK_I, (unsigned)REG_IDX(a->reg))), jm_st(b, JR_S2, JR_S3, (int64_t)k * 8);
                tg = T_I64;
            } else if (REG_BANK(a->reg) == BANK_F) {
                int s = j->jc.record ? -1 : j->jc.of[1][REG_IDX(a->reg)];
                if (s >= 0) jm_fst(b, jc_freg[s], JR_S3, (int64_t)k * 8);
                else jm_ld(b, JR_S2, JR_WIN, jwoff(j, BANK_F, (unsigned)REG_IDX(a->reg))), jm_st(b, JR_S2, JR_S3, (int64_t)k * 8);
                tg = T_F64;
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
        if (j->jc.rk[in->r[1]].kind == OBJ_LIST || j->jc.rk[in->r[1]].kind == OBJ_ARRAY) {
            d = jc_def(j, 0, in->r[0]);
            jm_ld32(b, d, JR_S0,
                    (int64_t)(j->jc.rk[in->r[1]].kind == OBJ_LIST ? offsetof(ObjList, len) : offsetof(ObjArray, len)));
            return;
        }
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
        jit_elem(j, in, pc, in->r[1], ix, x >= Q_LDE_I && x <= Q_LDE_R ? (int)in->r[2] : -1, false, bank,
                 bank == 1 ? JF_S0 : JR_S4);
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
        jit_elem(j, in, pc, in->r[0], ix, x >= Q_STE_I && x <= Q_STE_R ? (int)in->r[2] : -1, true, bank, val);
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
        } else {
            d = jc_def(j, 0, in->r[0]);
            jm_alui(b, op, d, s1, in->k.i);
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
        else if (x == Q_RET_R || x == Q_RET_RK) { /* a stop: the caller has machine code only if it was compiled */
        } else if (jit_calls(x) || x == Q_JTAB) loop = true; /* it goes on in the callee's machine code, if it has some */
        else if (jit_branches(x)) {
            succ[ns++] = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (x != Q_JMP) succ[ns++] = pc + 1;
        }
        bool pushed = false;
        for (uint32_t k = 0; k < ns && !pushed; k++) {
            uint32_t t = succ[k];
            if (t >= n || (!jit_handles(xop[t]) && !jit_steps(&f->code[t], xop[t]))) continue;
            if (mark[t] == 1) { /* a loop: only instructions in the cycle count against it */
                uint32_t cseen = 0, cstep = 0;
                for (uint32_t k2 = sp; k2-- > 0;) {
                    if (jit_handles(xop[stack[k2]])) cseen++;
                    else cstep++;
                    if (stack[k2] == t) break;
                }
                if (cseen >= JIT_STEP_COST * cstep) return true;
                continue;
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

/* Whether the form calls C or leaves the machine code by itself (a loop's
 * registers would not survive it). */
static bool jit_calls_out(const Insn *in, uint16_t x) {
    if (jit_alloc_inline(in, x)) return false;
    return jit_calls(x) || x == Q_JTAB || x == Q_NEW || x == Q_LNEW || x == Q_MNEW || x == Q_MSET || x == Q_MHAS ||
           x == Q_MGET_R || x == Q_MGETV_I || x == Q_MGETV_F || x == Q_MGETV_R ||
           (x >= Q_STRCAT_RK && x <= Q_BUFPUSH_I);
}

/* Where an instruction can go next, into out (at most 2 + its operands):
 * the next one unless it never falls through, and its branch targets. */
static int jit_succ(const Function *f, const uint16_t *jx, uint32_t pc, uint32_t *out) {
    const Insn *in = &f->code[pc];
    uint16_t x = jx[pc];
    int k = 0;
    if (!(x == Q_JMP || x == Q_RET_R || x == Q_RET_RK || x == Q_TCALL_R || x == Q_TCALLI_R)) out[k++] = pc + 1;
    if (jit_handles(x) && jit_branches(x)) out[k++] = pc + (uint32_t)(int32_t)in->target;
    for (int a = 0; a < in->nops; a++)
        if (in->ops[a].mode == OM_LABEL) out[k++] = in->ops[a].rv.target;
    return k;
}

/* The loops whose registers stay in machine registers (JLoop): for each
 * branch back from E to L, innermost first, the loop (the instructions in
 * [L, E] from which E is reached without passing L: a cold path in the range
 * that leaves it, to a return or an error's call, is not in it) if it
 * overlaps no loop already taken, and the I and F registers of its compiled
 * instructions that call nothing (its members) fit the cache (found by
 * compiling them once with a recording cache). Its other instructions (a
 * call, one the interpreter runs) are holes: the registers are written back
 * before one, and loaded again on the way back in, as they are for code
 * outside the loop that goes back into it; a loop with more than
 * JM_LOOP_HOLES keeps its registers in the window. loop_of[pc]: the loop pc
 * is a member of, or -1. */
/* The registers an instruction may read or write (conservatively including
 * recvt's destination and pointer auto-increment/decrement operands). */
static void jit_insn_rw(const Insn *in, RegSet *rd, RegSet *wr) {
    insn_effect(in, rd, wr);
    if (in->op == OP_RECVT && in->nops > 0 && in->ops[0].mode == OM_REG) rs_add(wr, in->ops[0].reg);
    for (int k = 0; k < in->nops; k++) {
        uint8_t m = in->ops[k].mode;
        if (m == OM_POSTINC || m == OM_PREDEC || m == OM_POSTINCB || m == OM_PREDECB) rs_add(wr, in->ops[k].reg);
    }
}

/* Merge a required R-register object shape into rkind/relem/rdef (255 = conflict). */
static void jit_rkind_req(uint8_t *rkind, uint8_t *relem, const void **rdef, unsigned r, uint8_t kind, uint8_t elem,
                          const void *def) {
    if (r >= NREGS || rkind[r] == 255) return;
    if (rkind[r] == 0) {
        rkind[r] = kind, relem[r] = elem, rdef[r] = def;
    } else if (rkind[r] != kind || (kind == OBJ_STRUCT && rdef[r] != def) || (kind == OBJ_ARRAY && relem[r] != elem)) {
        rkind[r] = 255;
    }
}

/* True iff every path in in_loop from L to any back-edge (u -> v with v <= u)
 * passes through skip_pc. Used to ensure a hoisted guard only runs when the
 * guarded access executes on the very first pass before any loop iteration can
 * repeat, preventing repeated preheader exit/re-entry thrashing when a guard
 * fails on an untaken conditional branch. */
static bool jit_dom_back(const Function *f, const uint16_t *jx, const uint8_t *in_loop, uint32_t L, uint32_t E,
                         uint32_t skip_pc, uint8_t *seen, uint32_t *work, uint32_t *succ) {
    if (skip_pc == L) return true;
    memset(seen + L, 0, E - L + 1);
    size_t nw = 0;
    work[nw++] = L, seen[L] = 1;
    while (nw) {
        uint32_t u = work[--nw];
        int ns = jit_succ(f, jx, u, succ);
        for (int k = 0; k < ns; k++) {
            uint32_t v = succ[k];
            if (v >= L && v <= E && in_loop[v]) {
                if (v <= u) return false; /* reached a back-edge without passing skip_pc */
                if (v != skip_pc && !seen[v]) seen[v] = 1, work[nw++] = v;
            }
        }
    }
    return true;
}

static size_t jit_loops(JComp *j, const uint16_t *jx, const uint8_t *lead, JLoop **out, int32_t *loop_of) {
    const Function *f = j->f;
    uint32_t n = f->ncode;
    JLoop *v = NULL;
    size_t nl = 0;
    for (uint32_t pc = 0; pc < n; pc++) loop_of[pc] = -1;
    /* Sole-definition hints for R registers across the function (to distinguish
     * typed arrays from lists when hoisting element-access guards). */
    uint8_t sole_cnt[NREGS] = {0}, sole_op[NREGS] = {0}, sole_type[NREGS] = {0};
    if (f->mf) {
        int nrp = 0;
        for (int k = 0; k < f->mf->nparams; k++)
            if (type_bank((MlwsType)f->mf->params[k]) == BANK_R && nrp < NREGS) sole_cnt[nrp++] = 1;
    }
    for (uint32_t pc = 0; pc < n; pc++) {
        RegSet rd, wr;
        jit_insn_rw(&f->code[pc], &rd, &wr);
        for (uint32_t m = wr.b[BANK_R]; m; m &= m - 1) {
            unsigned r = (unsigned)__builtin_ctz(m);
            if (sole_cnt[r] < 2) sole_cnt[r]++;
            if (sole_cnt[r] == 1) sole_op[r] = (uint8_t)f->code[pc].op, sole_type[r] = f->code[pc].type;
        }
    }
    uint8_t *in_loop = xcalloc(n + 1, 1), *work_seen = xcalloc(n + 1, 1);
    uint32_t *work = xmalloc((n + 1) * sizeof *work);
    uint32_t *succ = xmalloc((2 + MAX_OPERANDS) * sizeof *succ);
    /* each instruction's predecessors: pred[pfrom[pc] .. pfrom[pc + 1]) */
    uint32_t *pfrom = xcalloc(n + 2, sizeof *pfrom), npred = 0;
    for (uint32_t pc = 0; pc < n; pc++) {
        int ns = jit_succ(f, jx, pc, succ);
        for (int k = 0; k < ns; k++)
            if (succ[k] < n) pfrom[succ[k] + 1]++, npred++;
    }
    for (uint32_t pc = 0; pc < n; pc++) pfrom[pc + 1] += pfrom[pc];
    uint32_t *pred = xmalloc((npred + 1) * sizeof *pred), *pfill = xmalloc((n + 1) * sizeof *pfill);
    memcpy(pfill, pfrom, n * sizeof *pfill);
    for (uint32_t pc = 0; pc < n; pc++) {
        int ns = jit_succ(f, jx, pc, succ);
        for (int k = 0; k < ns; k++)
            if (succ[k] < n) pred[pfill[succ[k]]++] = pc;
    }
    free(pfill);
    struct {
        uint32_t L, E;
    } *cand = xmalloc((n + 1) * sizeof *cand);
    size_t nc = 0;
    for (uint32_t pc = 0; pc < n; pc++) {
        if (!jit_handles(jx[pc]) || !jit_branches(jx[pc])) continue;
        uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
        if (t <= pc) cand[nc].L = t, cand[nc++].E = pc;
    }
    for (size_t a = 1; a < nc; a++) /* innermost (shortest) first */
        for (size_t k = a; k > 0 && cand[k].E - cand[k].L < cand[k - 1].E - cand[k - 1].L; k--) {
            uint32_t L = cand[k].L, E = cand[k].E;
            cand[k] = cand[k - 1];
            cand[k - 1].L = L, cand[k - 1].E = E;
        }
    /* Two-pass loop selection:
     *   pass 0 (outermost first): promote an enclosing loop when it has 0 holes,
     *     fits all I and F registers without partial spilling, and does not
     *     prevent an inner loop from hoisting its invariant R/P registers.
     *   pass 1 (innermost first): remaining non-overlapping loops. */
    for (int pass = 0; pass < 2; pass++)
    for (size_t ci = 0; ci < nc; ci++) {
        size_t c = pass == 0 ? nc - 1 - ci : ci;
        uint32_t L = cand[c].L, E = cand[c].E;
        if (pass == 0) {
            bool encloses = false;
            for (size_t m = 0; m < nc; m++)
                if (m != c && cand[m].L >= L && cand[m].E <= E && (cand[m].L > L || cand[m].E < E)) {
                    encloses = true;
                    break;
                }
            if (!encloses) continue;
        }
        bool ok = lead[L];
        /* the loop's instructions: backwards from E, within [L, E], not past L */
        memset(in_loop + L, 0, E - L + 1);
        size_t nw = 0;
        in_loop[E] = in_loop[L] = 1;
        work[nw++] = E;
        while (nw) {
            uint32_t m = work[--nw];
            if (m == L) continue;
            for (uint32_t q = pfrom[m]; q < pfrom[m + 1]; q++) { /* m's predecessors in [L, E] */
                uint32_t pc = pred[q];
                if (pc >= L && pc <= E && !in_loop[pc]) in_loop[pc] = 1, work[nw++] = pc;
            }
        }
        /* L must reach E through them (a branch back to a jump out is no loop) */
        uint8_t *seen = work_seen;
        memset(seen + L, 0, E - L + 1);
        nw = 0;
        work[nw++] = L, seen[L] = 1;
        while (nw && !seen[E]) {
            int ns = jit_succ(f, jx, work[--nw], succ);
            for (int k = 0; k < ns; k++)
                if (succ[k] >= L && succ[k] <= E && in_loop[succ[k]] && !seen[succ[k]]) seen[succ[k]] = 1, work[nw++] = succ[k];
        }
        ok = ok && seen[E];
        unsigned holes = 0;
        for (uint32_t pc = L; pc <= E && ok; pc++)
            if (in_loop[pc]) {
                ok = loop_of[pc] < 0;
                if (!jit_handles(jx[pc]) || jit_calls_out(&f->code[pc], jx[pc])) in_loop[pc] = 0, holes++; /* a hole */
            }
        ok = ok && in_loop[L] && in_loop[E] && holes <= (pass == 0 ? 0u : (unsigned)JM_LOOP_HOLES);
        if (!ok) continue;
        JComp d = {.jt = j->jt, .f = f, .rest = j->rest};
        jc_reset(&d.jc);
        d.jc.record = true;
        memset(d.jc.used, 0, sizeof d.jc.used);
        memset(d.jc.used_cnt, 0, sizeof d.jc.used_cnt);
        d.jc.max_pin[0] = d.jc.max_pin[1] = 0;
        uint32_t ignored;
        for (uint32_t pc = L; pc <= E; pc++)
            if (in_loop[pc]) jit_insn(&d, &f->code[pc], jx[pc], pc, &ignored);
        free(d.b.v), free(d.fx.v), free(d.sides.v), free(d.leaves.v);
        JLoop lp = {.L = L, .E = E};
        for (int bank = 0; bank < 2 && ok; bank++) {
            int nused = 0;
            for (int r = 0; r < 256; r++)
                if (d.jc.used[bank][r]) nused++;
            int ns = jc_nslots(bank);
            if (nused <= ns) {
                int slot = 0;
                for (int r = 0; r < 256; r++) {
                    if (!d.jc.used[bank][r]) continue;
                    lp.bank[lp.n] = (uint8_t)bank, lp.vreg[lp.n] = (uint8_t)r, lp.slot[lp.n] = (uint8_t)slot++;
                    lp.written[lp.n++] = (d.jc.used[bank][r] & 2) != 0;
                }
            } else {
                if (pass == 0) {
                    ok = false;
                    break;
                }
                int reserve = d.jc.max_pin[bank] > 3 ? d.jc.max_pin[bank] : 3;
                if (ns <= reserve) {
                    ok = false;
                    break;
                }
                int cap = ns - reserve;
                lp.partial[bank] = true;
                bool picked[256] = {false};
                for (int slot = 0; slot < cap; slot++) {
                    int best = -1;
                    uint16_t bestc = 0;
                    for (int r = 0; r < 256; r++)
                        if (d.jc.used[bank][r] && !picked[r] && d.jc.used_cnt[bank][r] > bestc)
                            best = r, bestc = d.jc.used_cnt[bank][r];
                    if (best < 0) break;
                    picked[best] = true;
                    lp.bank[lp.n] = (uint8_t)bank, lp.vreg[lp.n] = (uint8_t)best, lp.slot[lp.n] = (uint8_t)slot;
                    lp.written[lp.n++] = (d.jc.used[bank][best] & 2) != 0;
                }
            }
        }
        if (!ok) continue;
        /* Analyse loop-invariant R and raw-P registers across [L, E]. */
        RegSet lwr = {{0}};
        uint16_t rreads[NREGS] = {0};
        uint16_t praw_reads[NREGS] = {0};
        uint8_t rkind[NREGS] = {0}, relem[NREGS] = {0};
        const void *rdef[NREGS] = {0};
        bool has_stub_call = false;
        for (uint32_t pc = L; pc <= E; pc++) {
            const Insn *in = &f->code[pc];
            RegSet rd, wr;
            jit_insn_rw(in, &rd, &wr);
            for (int b = 0; b < NBANKS; b++) lwr.b[b] |= wr.b[b];
            for (uint32_t m = rd.b[BANK_R]; m; m &= m - 1) {
                unsigned r = (unsigned)__builtin_ctz(m);
                if (rreads[r] < UINT16_MAX) rreads[r]++;
            }
            uint16_t x = jx[pc];
            if (x == Q_NEW || x == Q_LNEW || x == Q_LPUSH_R || x == Q_LPUSH_RK) has_stub_call = true;
            /* Never hoist a guard for an R register that the loop tests for null or type. */
            if (in->op == OP_BZ || in->op == OP_BNZ || in->op == OP_BISA || in->op == OP_BNISA) {
                if (in->nops > 0 && in->ops[0].mode == OM_REG && REG_BANK(in->ops[0].reg) == BANK_R)
                    rkind[REG_IDX(in->ops[0].reg)] = 255;
            } else if (in->op == OP_TYPEOF || in->op == OP_ISA) {
                if (in->nops > 1 && in->ops[1].mode == OM_REG && REG_BANK(in->ops[1].reg) == BANK_R)
                    rkind[REG_IDX(in->ops[1].reg)] = 255;
            } else if (in->op == OP_BEQ || in->op == OP_BNE || in->op == OP_SEQ || in->op == OP_SNE) {
                for (int k = 0; k < in->nops; k++)
                    if (in->ops[k].mode == OM_REG && REG_BANK(in->ops[k].reg) == BANK_R) rkind[REG_IDX(in->ops[k].reg)] = 255;
            }
            if (!in_loop[pc]) continue;
            if (x == Q_LDMB_U8) {
                if (jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ) && praw_reads[in->r[1]] < UINT16_MAX)
                    praw_reads[in->r[1]]++;
            } else if (x == Q_STMB_U8 || x == Q_STMBK_U8 || x == Q_BZ_MB_U8 || x == Q_BNZ_MB_U8) {
                if (jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ) && praw_reads[in->r[0]] < UINT16_MAX)
                    praw_reads[in->r[0]]++;
            } else if (x >= Q_LDF_I && x <= Q_LDF_R) {
                if (jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ))
                    jit_rkind_req(rkind, relem, rdef, in->r[1], OBJ_STRUCT, 0, in->k.p);
            } else if ((x >= Q_STF_I && x <= Q_STF_R) || x == Q_STFK) {
                if (jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ))
                    jit_rkind_req(rkind, relem, rdef, in->r[0], OBJ_STRUCT, 0, in->k.p);
            } else if (x == Q_LPUSH_R || x == Q_LPUSH_RK || x == Q_BEQ_EKK || x == Q_BNE_EKK) {
                if (jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ))
                    jit_rkind_req(rkind, relem, rdef, in->r[0], OBJ_LIST, 0, NULL);
            } else if ((x >= Q_LDE_I && x <= Q_STE_R) || (x >= Q_LDEK_I && x <= Q_STEK_R) ||
                       (x >= Q_LDED_I && x <= Q_STED_R) || x == Q_STEC || x == Q_STECK || x == Q_LDV_E ||
                       x == Q_LDV_ED || x == Q_LDV_EK) {
                unsigned ob = (x == Q_LDV_E || x == Q_LDV_ED || x == Q_LDV_EK) ? (unsigned)in->k.i
                              : ((x >= Q_LDE_I && x <= Q_LDE_R) || (x >= Q_LDEK_I && x <= Q_LDEK_R) ||
                                 (x >= Q_LDED_I && x <= Q_LDED_R))
                                    ? in->r[1]
                                    : in->r[0];
                if (ob < NREGS && sole_cnt[ob] == 1 && jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ)) {
                    if (sole_op[ob] == OP_ANEW) jit_rkind_req(rkind, relem, rdef, ob, OBJ_ARRAY, sole_type[ob], NULL);
                    else if (sole_op[ob] == OP_LNEW) jit_rkind_req(rkind, relem, rdef, ob, OBJ_LIST, 0, NULL);
                }
            } else if (x == JX_FARITH) {
                bool dom = false, checked = false;
                for (int k = 0; k < in->nops; k++) {
                    const Operand *o = &in->ops[k];
                    if ((o->mode == OM_ELEM || o->mode == OM_ELEMI) && REG_BANK(o->reg) == BANK_R) {
                        unsigned ob = (unsigned)REG_IDX(o->reg);
                        if (sole_cnt[ob] == 1) {
                            if (!checked) dom = jit_dom_back(f, jx, in_loop, L, E, pc, work_seen, work, succ), checked = true;
                            if (!dom) break;
                            if (sole_op[ob] == OP_ANEW) jit_rkind_req(rkind, relem, rdef, ob, OBJ_ARRAY, sole_type[ob], NULL);
                            else if (sole_op[ob] == OP_LNEW) jit_rkind_req(rkind, relem, rdef, ob, OBJ_LIST, 0, NULL);
                        }
                    }
                }
            }
        }
        int used_i = 0;
        for (int k = 0; k < lp.n; k++)
            if (lp.bank[k] == 0) used_i++;
        int free_i = lp.partial[0] ? 0 : JC_NI - used_i;
        /* In pass 0, make sure promoting the outer loop does not prevent an
         * enclosed inner loop from hoisting its own invariant R/P registers. */
        if (pass == 0) {
            bool conflict = false;
            for (size_t m = 0; m < nc && !conflict; m++) {
                if (m == c || cand[m].L < L || cand[m].E > E) continue;
                RegSet ird = {{0}}, iwr = {{0}};
                uint32_t inner_praw = 0;
                for (uint32_t pc = cand[m].L; pc <= cand[m].E; pc++) {
                    RegSet rd, wr;
                    jit_insn_rw(&f->code[pc], &rd, &wr);
                    for (int b = 0; b < NBANKS; b++) ird.b[b] |= rd.b[b], iwr.b[b] |= wr.b[b];
                    uint16_t x = jx[pc];
                    if (x == Q_LDMB_U8) inner_praw |= 1u << f->code[pc].r[1];
                    else if (x == Q_STMB_U8 || x == Q_STMBK_U8 || x == Q_BZ_MB_U8 || x == Q_BNZ_MB_U8)
                        inner_praw |= 1u << f->code[pc].r[0];
                }
                uint32_t inv_r = ird.b[BANK_R] & ~iwr.b[BANK_R];
                uint32_t inv_p = ird.b[BANK_P] & ~iwr.b[BANK_P];
                if ((inv_r & lwr.b[BANK_R]) || (inv_p & lwr.b[BANK_P])) conflict = true;
                uint32_t ip = inner_praw & inv_p;
                if (ip && (has_stub_call || free_i < 2)) conflict = true;
                for (uint32_t mm = ip; mm && !conflict; mm &= mm - 1)
                    if (!praw_reads[__builtin_ctz(mm)]) conflict = true;
            }
            if (conflict) continue;
        }
        /* Hoist loop-invariant raw P registers into 2 free bank-0 slots (base, size) when no C calls occur. */
        if (holes == 0 && !has_stub_call && free_i >= 2) {
            bool ppicked[NREGS] = {false};
            while (free_i >= 2 && lp.np < JLOOP_MAX_P) {
                int best = -1;
                uint16_t bestc = 0;
                for (int r = 0; r < NREGS; r++)
                    if (!ppicked[r] && !(lwr.b[BANK_P] & (1u << r)) && praw_reads[r] > bestc)
                        best = r, bestc = praw_reads[r];
                if (best < 0) break;
                ppicked[best] = true;
                lp.pvreg[lp.np] = (uint8_t)best;
                lp.pbase_slot[lp.np] = (uint8_t)used_i++;
                lp.psize_slot[lp.np] = (uint8_t)used_i++;
                lp.np++;
                free_i -= 2;
            }
        }
        /* Cache and/or guard loop-invariant R registers. */
        bool rpicked[NREGS] = {false};
        while (lp.nr < JLOOP_MAX_R) {
            int best = -1;
            uint16_t bestc = 0;
            for (int r = 0; r < NREGS; r++) {
                if (rpicked[r] || (lwr.b[BANK_R] & (1u << r)) || !rreads[r]) continue;
                bool has_guard = rkind[r] != 0 && rkind[r] != 255;
                if (!has_guard && free_i <= 0) continue;
                if (rreads[r] > bestc) best = r, bestc = rreads[r];
            }
            if (best < 0) break;
            rpicked[best] = true;
            uint8_t rk = rkind[best] == 255 ? 0 : rkind[best];
            int8_t slot = free_i > 0 ? (int8_t)(used_i++, free_i--, used_i - 1) : -1;
            if (slot < 0 && !rk) continue;
            lp.rvreg[lp.nr] = (uint8_t)best;
            lp.rslot[lp.nr] = slot;
            lp.rkind[lp.nr] = rk;
            lp.relem[lp.nr] = relem[best];
            lp.rdef[lp.nr] = rdef[best];
            lp.nr++;
        }
        v = xrealloc(v, (nl + 1) * sizeof *v);
        v[nl] = lp;
        for (uint32_t pc = L; pc <= E; pc++)
            if (in_loop[pc]) loop_of[pc] = (int32_t)nl;
        nl++;
    }
    free(cand), free(in_loop), free(work_seen), free(work), free(succ), free(pfrom), free(pred);
    *out = v;
    return nl;
}

/* A loop's registers, from the window into their slots (with emit), and
 * (with seed) the cache holding them, the written ones dirty (from here on
 * they are written back on every way out). Also emits/seeds hoisted guards
 * and slots for loop-invariant R and raw-P registers; any failed guard exits
 * before exit_pc with JR_LEFT unchanged. */
static void jit_loop_load_as(JComp *j, const JLoop *lp, bool seed, bool emit, uint32_t exit_pc) {
    JBuf *b = &j->b;
    if (seed) jc_reset(&j->jc);
    if (emit) {
        uint32_t fails[3 * JLOOP_MAX_R + 3 * JLOOP_MAX_P];
        int nfail = 0;
        for (int k = 0; k < lp->nr; k++) {
            int reg = lp->rslot[k] >= 0 ? jc_ireg[lp->rslot[k]] : JR_S0;
            jm_ld(b, reg, JR_WIN, jwoff(j, BANK_R, lp->rvreg[k]));
            if (!lp->rkind[k]) continue;
            fails[nfail++] = jm_fwd_cbz(b, reg, false);
            jm_ld8(b, JR_S1, reg, (int64_t)offsetof(Obj, kind));
            jm_cmpi32(b, JR_S1, lp->rkind[k]);
            fails[nfail++] = jm_fwd(b, JC_NE);
            if (lp->rkind[k] == OBJ_STRUCT) {
                jm_ld(b, JR_S1, reg, (int64_t)offsetof(ObjStruct, def));
                jm_movi(b, JR_S2, (uint64_t)(uintptr_t)lp->rdef[k]);
                jm_cmp(b, JR_S1, JR_S2);
                fails[nfail++] = jm_fwd(b, JC_NE);
            } else if (lp->rkind[k] == OBJ_ARRAY) {
                jm_ld8(b, JR_S1, reg, (int64_t)offsetof(ObjArray, elem));
                jm_cmpi32(b, JR_S1, lp->relem[k]);
                fails[nfail++] = jm_fwd(b, JC_NE);
            }
        }
        for (int k = 0; k < lp->np; k++) {
            int base_reg = jc_ireg[lp->pbase_slot[k]];
            int sz_reg = jc_ireg[lp->psize_slot[k]];
            jm_ld(b, JR_S0, JR_WIN, jwoff(j, BANK_P, lp->pvreg[k]));
            jm_alui(b, JA_SHR, JR_S1, JR_S0, 44);
            jm_alui(b, JA_SUB, JR_S1, JR_S1, SEG_RAW0);
            int P = jm_proc(b, JR_S3);
            jm_ld(b, JR_S2, P, P_OFF(raw.n));
            jm_cmp(b, JR_S1, JR_S2);
            fails[nfail++] = jm_fwd(b, JC_HS);
            jm_alui(b, JA_SHL, JR_S2, JR_S1, 1);
            jm_alu(b, JA_ADD, JR_S1, JR_S1, JR_S2);
            jm_ld(b, JR_S2, P, P_OFF(raw.v));
            jm_addsh3(b, JR_S1, JR_S2, JR_S1);
            jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Segment, gen));
            jm_alui(b, JA_SHR, JR_S3, JR_S0, 32);
            jm_alui(b, JA_AND, JR_S3, JR_S3, 0xFFFu);
            jm_alui(b, JA_OR, JR_S3, JR_S3, (int64_t)((uint64_t)SEGK_RAW << 16 | (uint64_t)1 << 24));
            jm_cmp32(b, JR_S2, JR_S3);
            fails[nfail++] = jm_fwd(b, JC_NE);
            jm_mov32(b, JR_S0, JR_S0); /* zero-extend PTR_OFF */
            jm_ld32(b, JR_S2, JR_S1, (int64_t)offsetof(Segment, size));
            jm_cmp(b, JR_S0, JR_S2);
            fails[nfail++] = jm_fwd(b, JC_HI); /* ptr_off > seg->size */
            jm_alu(b, JA_SUB, sz_reg, JR_S2, JR_S0);
            jm_ld(b, JR_S1, JR_S1, (int64_t)offsetof(Segment, data));
            jm_alu(b, JA_ADD, base_reg, JR_S1, JR_S0);
        }
        if (nfail) {
            uint32_t ok_br = jm_fwd(b, JC_AL);
            for (int f = 0; f < nfail; f++) jm_bind(b, fails[f]);
            jexit(j, &j->f->code[exit_pc]);
            jm_bind(b, ok_br);
        }
    }
    for (int k = 0; k < lp->n; k++) {
        int bank = lp->bank[k], s = lp->slot[k], r = lp->vreg[k];
        if (emit && bank) jm_fld(b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
        else if (emit) jm_ld(b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
        if (!seed) continue;
        j->jc.of[bank][r] = (int8_t)s;
        j->jc.vm[bank][s] = (int16_t)r;
        j->jc.dirty[bank][s] = lp->written[k];
        j->jc.loop_pin[bank] |= 1u << s;
    }
    if (!seed) return;
    for (int k = 0; k < lp->nr; k++) {
        if (lp->rslot[k] >= 0) {
            j->jc.rof[lp->rvreg[k]] = lp->rslot[k];
            j->jc.vm[0][lp->rslot[k]] = (int16_t)(JC_RBASE + lp->rvreg[k]);
            j->jc.loop_pin[0] |= 1u << lp->rslot[k];
        }
        if (lp->rkind[k])
            j->jc.rk[lp->rvreg[k]] = (JRKnown){lp->rkind[k], lp->relem[k], false, lp->rdef[k]};
    }
    for (int k = 0; k < lp->np; k++) {
        j->jc.vm[0][lp->pbase_slot[k]] = JC_PBASE;
        j->jc.vm[0][lp->psize_slot[k]] = JC_PBASE;
        j->jc.loop_pin[0] |= (1u << lp->pbase_slot[k]) | (1u << lp->psize_slot[k]);
    }
}
static void jit_loop_load(JComp *j, const JLoop *lp, bool seed, uint32_t exit_pc) {
    jit_loop_load_as(j, lp, seed, true, exit_pc);
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
    uint16_t *jx = xmalloc((n + 1) * sizeof *jx); /* the forms as the JIT sees them (jit_class) */
    bool any = false;
    for (uint32_t pc = 0; pc < n; pc++) {
        jx[pc] = jit_class(&f->code[pc], xop[pc]);
        any = any || jit_handles(jx[pc]);
    }
    Jit *jt = any ? jit_get(vm) : NULL;
    if (!jt) {
        f->jit_state = JIT_NONE;
        free(jx);
        if (f->jit_xop) /* the triggers go */
            for (uint32_t pc = 0; pc < n; pc++) __atomic_store_n(&f->code[pc].xop, xop[pc], __ATOMIC_RELAXED);
        else free(xop);
        return;
    }
    /* Blocks, and rest[pc]: the instructions from pc to its block's end. */
    uint8_t *lead = xcalloc(n + 1, 1);
    uint32_t *rest = xcalloc(n + 1, sizeof *rest);
    for (uint32_t pc = 0; pc < n; pc++) {
        if (pc == 0 || !jit_handles(jx[pc - 1]) || jit_ends_block(jx[pc - 1])) lead[pc] = 1;
        if (jx[pc] == Q_JTAB)
            for (int k = 1; k < f->code[pc].nops; k++)
                if (f->code[pc].ops[k].rv.target < n) lead[f->code[pc].ops[k].rv.target] = 1;
        if (jit_handles(jx[pc]) && jit_branches(jx[pc])) {
            uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (t < n) lead[t] = 1;
        }
    }
    for (uint32_t pc = n; pc-- > 0;) {
        if (jit_steps(&f->code[pc], jx[pc])) rest[pc] = 1;
        if (!jit_handles(jx[pc])) continue;
        bool last = pc + 1 >= n || lead[pc + 1] || !jit_handles(jx[pc + 1]) || jit_ends_block(jx[pc]);
        rest[pc] = last ? 1 : rest[pc + 1] + 1;
        if (rest[pc] > 4000) rest[pc] = 1, lead[pc + 1] = 1; /* (a block's length fits in 12 bits) */
    }
    JComp jcomp = {.jt = jt, .f = f, .rest = rest};
    JComp *j = &jcomp;
    JBuf *b = &j->b;
    jc_reset(&j->jc);
    memset(j->jc.next, 0, sizeof j->jc.next);
    uint32_t *body = xcalloc(n + 1, sizeof *body), *undo = xcalloc(n + 1, sizeof *undo);
    uint32_t *cont = xcalloc(n + 1, sizeof *cont), *head = xcalloc(n + 1, sizeof *head);
    uint32_t nnat = 0;
    JLoop *loops;
    int32_t *loop_of = xmalloc((n + 1) * sizeof *loop_of);
    size_t nloops = jit_loops(j, jx, lead, &loops, loop_of);
    j->loop_of = loop_of, j->loops = loops;
    /* Blocks only reached by falling through from the block before, which
     * ends in a conditional branch (no branch or jump table goes to them; not
     * in a loop, whose blocks keep their registers another way): they keep
     * that block's cache (written back at the branch, so nothing is loaded
     * again) and what it knew of R registers, and are never
     * entered from the interpreter (their code assumes the cache). */
    uint8_t *target = xcalloc(n + 1, 1), *fallin = xcalloc(n + 1, 1);
    for (uint32_t pc = 0; pc < n; pc++) {
        if (jx[pc] == Q_JTAB)
            for (int k = 1; k < f->code[pc].nops; k++)
                if (f->code[pc].ops[k].rv.target < n) target[f->code[pc].ops[k].rv.target] = 1;
        if (jit_branches(jx[pc])) { /* (a jmp's target too; and a branch the interpreter runs: it may go there) */
            uint32_t t = pc + (uint32_t)(int32_t)f->code[pc].target;
            if (t < n) target[t] = 1;
        }
    }
    for (uint32_t pc = 1; pc < n; pc++) {
        uint16_t px = jx[pc - 1];
        fallin[pc] = lead[pc] && !target[pc] && jit_handles(px) && !jit_steps(&f->code[pc - 1], px) &&
                     jit_branches(px) && px != Q_JMP && jit_handles(jx[pc]) && !jit_steps(&f->code[pc], jx[pc]) &&
                     loop_of[pc] < 0 && loop_of[pc - 1] < 0;
    }
    free(target);
    for (uint32_t pc = 0; pc < n; pc++) {
        if (j->loop && (loop_of[pc] < 0 || &loops[loop_of[pc]] != j->loop)) /* out of the loop's code */
            j->loop = NULL, jc_flush(j);
        if (!j->loop && loop_of[pc] >= 0 && loops[loop_of[pc]].L != pc && pc > 0 && jit_handles(jx[pc - 1])) {
            uint32_t sc[2 + MAX_OPERANDS]; /* into the loop past L from the code before: the registers, for that way only */
            int ns = jit_succ(f, jx, pc - 1, sc);
            if (ns && sc[0] == pc) { /* (what the code before changed, back first: a hole may set a loop register) */
                jc_flush(j);
                jit_loop_load_as(j, &loops[loop_of[pc]], false, true, pc);
            }
        }
        body[pc] = (uint32_t)b->n;
        const Insn *in = &f->code[pc];
        if (lead[pc] && !fallin[pc]) jc_forget_r(j);
        if (loop_of[pc] >= 0 && loops[loop_of[pc]].L == pc) { /* a loop's way in, then its head */
            jit_loop_load(j, &loops[loop_of[pc]], true, pc);
            head[pc] = (uint32_t)b->n;
            j->loop = &loops[loop_of[pc]];
        } else if (loop_of[pc] >= 0 && !j->loop) { /* back in the loop's code, after a hole or code outside it */
            j->loop = &loops[loop_of[pc]];
            if (lead[pc]) {
                jit_loop_load_as(j, j->loop, true, false, pc);
            } else { /* only from the hole before it: what it knew of R registers holds, outside the loop's slots */
                JCache *c = &j->jc;
                int16_t vm0[JC_MAX];
                JRKnown *rk = xmalloc(sizeof c->rk);
                memcpy(vm0, c->vm[0], sizeof vm0);
                memcpy(rk, c->rk, sizeof c->rk);
                jit_loop_load_as(j, j->loop, true, false, pc);
                for (int s = 0; s < JC_NI; s++)
                    if (vm0[s] >= JC_RBASE && vm0[s] < JC_PBASE && c->vm[0][s] < 0)
                        c->vm[0][s] = vm0[s], c->rof[vm0[s] - JC_RBASE] = (int8_t)s;
                for (int r = 0; r < 256; r++)
                    if (rk[r].kind && !c->rk[r].kind) c->rk[r] = rk[r];
                free(rk);
            }
        }
        if (jit_steps(in, jx[pc])) { /* a block of its own: counted, then run by its handler */
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
        if (!jit_handles(jx[pc])) { /* the interpreter's: stop here */
            jexit(j, in);
            continue;
        }
        nnat++;
        if (lead[pc]) { /* the block's header: count it, or stop before it */
            jm_subs32i(b, JR_LEFT, JR_LEFT, rest[pc]);
            if (j->loop || fallin[pc]) jside(j, FX_BCOND, JC_LO, 0, false, pc); /* (writing the cached registers back) */
            else jbranch(j, FX_BCOND, JC_LO, 0, false, TO_UNDO, pc);
        }
        j->keep = pc + 1 < n && fallin[pc + 1]; /* (a conditional branch, then) */
        uint16_t x2 = jit_is_pair(f, jx, lead, pc);
        if (x2) jit_pair(j, in, pc, jx[pc], x2);
        else if (!(pc > 0 && jit_is_pair(f, jx, lead, pc - 1))) jit_insn(j, in, jx[pc], pc, &cont[pc]); /* (a pair's second: done) */
        j->keep = false;
        if (rest[pc] == 1 && !(pc + 1 < n && fallin[pc + 1])) jc_flush(j); /* the block ends: back to the window (in a
                                                                              loop, not until it is left) */
    }
    j->loop = NULL; /* (a loop may end the function) */
    size_t stubs_at = b->n;
    jm_brk(b); /* past the end: not reached (the verifier ends every path) */
    uint32_t *side = xcalloc(j->sides.n + 1, sizeof *side);
    for (size_t k = 0; k < j->sides.n; k++) {
        const JSide *sd = &j->sides.v[k];
        side[k] = (uint32_t)b->n;
        if (sd->unset) { /* never written: zero, and back; another type: the exit */
            jm_cmpi32(b, JR_S2, T_NONE);
            uint32_t other = jm_fwd(b, JC_NE);
            if (sd->ubank == 1) jm_fconst(b, sd->uval, 0.0);
            else jm_movi(b, sd->uval, 0);
            uint32_t at = jm_emit_branch(b, FX_B, 0, 0, false); /* (first: the buffer may move as it grows) */
            jm_patch_at(b->v, at, sd->back);
            jm_bind(b, other);
        }
        jside_stores(j, sd);
        if (sd->alloc) { /* bump hole full: allocate in C (which may trap), reload cache, back with obj in S0 */
            jm_movi(b, JR_S0, sd->pc + 1);
            jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
            jit_sync_insns(j, rest[sd->pc] - 1);
            jm_mov(b, JR_A0, jm_proc(b, JR_S3));
            const Insn *ain = &f->code[sd->pc];
            jm_movi(b, JR_A1, sd->alloc_struct ? (uint64_t)(uintptr_t)ain->k.p : (uint64_t)ain->k.i);
            jm_ccall(b, sd->alloc_struct ? (const void *)jit_struct_new : (const void *)jit_list_new);
            jm_mov(b, JR_S0, JR_R0);
            for (int r = 0; r < sd->na; r++) {
                if (sd->abank[r] == 1) jm_fld(b, sd->amreg[r], JR_WIN, jwoff(j, BANK_F, sd->avreg[r]));
                else jm_ld(b, sd->amreg[r], JR_WIN, jwoff(j, sd->abank[r] ? BANK_R : BANK_I, sd->avreg[r]));
            }
            uint32_t at = jm_emit_branch(b, FX_B, 0, 0, false);
            jm_patch_at(b->v, at, sd->back);
            continue;
        }
        if (sd->grow) { /* grown in C (which may trap, as new may), and back */
            jm_movi(b, JR_S0, sd->pc + 1);
            jm_st32(b, JR_S0, JR_FR, FR_OFF(pc));
            jit_sync_insns(j, rest[sd->pc] - 1);
            jm_mov(b, JR_A0, jm_proc(b, JR_S3));
            jm_ld(b, JR_A1, JR_WIN, jwoff(j, BANK_R, sd->glist));
            jm_movi(b, JR_A2, sd->gn);
            jm_ccall(b, (const void *)jit_list_grow);
            for (int r = 0; r < sd->na; r++) {
                if (sd->abank[r] == 1) jm_fld(b, sd->amreg[r], JR_WIN, jwoff(j, BANK_F, sd->avreg[r]));
                else jm_ld(b, sd->amreg[r], JR_WIN, jwoff(j, sd->abank[r] ? BANK_R : BANK_I, sd->avreg[r]));
            }
            jm_ld(b, JR_S0, JR_WIN, jwoff(j, BANK_R, sd->glist));
            jm_ld32(b, JR_S1, JR_S0, (int64_t)offsetof(ObjList, len));
            uint32_t at = jm_emit_branch(b, FX_B, 0, 0, false);
            jm_patch_at(b->v, at, sd->back);
            continue;
        }
        jm_add32i(b, JR_LEFT, JR_LEFT, rest[sd->pc]);
        jexit(j, &f->code[sd->pc]);
    }
    uint32_t *leave = xcalloc(j->leaves.n + 1, sizeof *leave);
    for (size_t k = 0; k < j->leaves.n; k++) { /* a branch out of a loop: its registers back, then on */
        leave[k] = (uint32_t)b->n;
        jside_stores(j, &j->leaves.v[k]);
        jbranch(j, FX_B, 0, 0, false, TO_BODY, j->leaves.v[k].pc);
    }
    uint32_t *inner = xcalloc(n + 1, sizeof *inner); /* the interpreter's way into a block inside a loop */
    for (uint32_t pc = 0; pc < n; pc++) {
        if (loop_of[pc] < 0 || !lead[pc] || loops[loop_of[pc]].L == pc) continue;
        inner[pc] = (uint32_t)b->n;
        jit_loop_load(j, &loops[loop_of[pc]], false, pc);
        j->loop = &loops[loop_of[pc]]; /* (so that the branch goes to the body, not back here) */
        jbranch(j, FX_B, 0, 0, false, TO_BODY, pc);
        j->loop = NULL;
    }
    for (uint32_t pc = 0; pc < n; pc++) { /* give the count back and stop before pc */
        if (!jit_handles(jx[pc]) && !jit_steps(&f->code[pc], jx[pc])) continue;
        undo[pc] = (uint32_t)b->n;
        jm_add32i(b, JR_LEFT, JR_LEFT, rest[pc]);
        jexit(j, &f->code[pc]);
    }
    if (b->n >= JM_CODE_MAX) j->broken = true; /* some branch inside it may not reach: interpret it */
    uint8_t *at = j->broken ? NULL : jit_reserve(jt, b->n);
    if (at) {
        for (size_t k = 0; k < j->fx.n; k++) {
            const JFix *x = &j->fx.v[k];
            int64_t dest;
            switch (x->to) {
            case TO_BODY: case TO_BODYX: dest = body[x->pc]; break;
            case TO_SIDE: dest = side[x->pc]; break;
            case TO_UNDO: dest = undo[x->pc]; break;
            case TO_LOOP: dest = head[x->pc]; break;
            case TO_LEAVE: dest = leave[x->pc]; break;
            case TO_INNER: dest = inner[x->pc]; break;
            default: dest = jt->exit - at; break;
            }
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
        jit_map(f, at, body, n, stubs_at, b->n);
        void **ent = xcalloc(n, sizeof *ent);
        uint8_t *mark = xmalloc(n);
        uint32_t *stack = xmalloc(n * sizeof *stack);
        bool *enter = xcalloc(n, sizeof *enter);
        for (uint32_t pc = 0; pc < n; pc++) {
            if (jit_steps(&f->code[pc], jx[pc])) ent[pc] = at + body[pc];
            if (!jit_handles(jx[pc])) continue;
            if (cont[pc]) /* the continuation, before any return can look for it */
                __atomic_store_n(&f->code[pc].target, (uint32_t)(at + cont[pc] - jt->mem), __ATOMIC_RELEASE);
            if (!lead[pc] || fallin[pc]) continue; /* (fallin: only from the block before) */
            ent[pc] = at + (inner[pc] ? inner[pc] : body[pc]); /* for calls from machine code, and h_Q_JIT */
            enter[pc] = vm->jit_mode == JIT_MODE_ALL || jit_worth_entering(f, jx, pc, mark, stack); /* (all: a test of every block) */
        }
        f->jit_xop = xop;
        f->jit_native = nnat;
        f->jit_loops = (uint16_t)(nloops > UINT16_MAX ? UINT16_MAX : nloops);
        f->jit_spec = (uint16_t)(j->nspec > UINT16_MAX ? UINT16_MAX : j->nspec);
        uint32_t nhoisted = 0;
        for (size_t k = 0; k < nloops; k++) {
            nhoisted += loops[k].np;
            for (int r = 0; r < loops[k].nr; r++)
                if (loops[k].rkind[r]) nhoisted++;
        }
        f->jit_hoisted = (uint16_t)(nhoisted > UINT16_MAX ? UINT16_MAX : nhoisted);
        __atomic_store_n(&f->jit_entry, ent, __ATOMIC_RELEASE);
        for (uint32_t pc = 0; pc < n; pc++)
            __atomic_store_n(&f->code[pc].xop, enter[pc] ? (uint16_t)Q_JIT : xop[pc], __ATOMIC_RELEASE);
        f->jit_state = JIT_COMPILED;
        free(mark), free(stack), free(enter);
        jt->nfuncs++;
        jt->ninsns += n;
        jt->nnative += nnat;
        jt->nloops += nloops;
        jt->ncalli += j->ncalli;
        jt->nspec += j->nspec;
        jt->bytes += b->n;
    } else { /* no room, too large, or a bug: it stays the interpreter's */
        jt->no_memory = !j->broken;
        f->jit_state = JIT_NONE;
        if (f->jit_xop)
            for (uint32_t pc = 0; pc < n; pc++) __atomic_store_n(&f->code[pc].xop, xop[pc], __ATOMIC_RELAXED);
        else free(xop);
    }
    jt->ns += plat_now_ns() - t0;
    free(jx), free(lead), free(fallin), free(rest), free(body), free(undo), free(cont), free(side), free(j->sides.v);
    free(head), free(leave), free(inner), free(loops), free(loop_of), free(j->leaves.v);
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
        any = any || jit_handles(jit_class(&f->code[pc], xop[pc]));
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

/* With the world stopped (vm_reload): f goes back to the interpreter, with no
 * machine code and none to come. Calls compiled for it find no entry and stop
 * for the interpreter's call, whose operand then decides. Its machine code
 * stays in place: the process that asked may be running it. */
void interp_jit_forget(VM *vm, Function *f) {
    if (!f->jit_xop) return;
    for (uint32_t pc = 0; pc < f->ncode; pc++) {
        uint16_t x = f->jit_xop[pc];
        __atomic_store_n(&f->code[pc].xop, x, __ATOMIC_RELEASE);
        if (x == Q_CALL_R || x == Q_CALL_RK || x == Q_CALLI_R || x == Q_CALLI_RK) /* no continuation: a ret goes back through the interpreter */
            __atomic_store_n(&f->code[pc].target, 0, __ATOMIC_RELEASE);
    }
    VEC_PUSH(vm->retired, f->jit_xop); /* (a callback on an I/O thread may still look) */
    VEC_PUSH(vm->retired, f->jit_entry);
    __atomic_store_n(&f->jit_entry, NULL, __ATOMIC_RELEASE);
    f->jit_xop = NULL;
    f->jit_count = 0;
    f->jit_state = JIT_NONE;
}

/* After interp_jit_forget: f may be compiled again, for what it calls now. */
void interp_jit_again(VM *vm, Function *f) {
    f->jit_state = 0;
    interp_jit_function(vm, f);
}

bool interp_jit_available(void) { return true; }
const char *interp_jit_arch(void) { return JIT_ARCH; }

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
 * from there, then go on with the instruction it stopped at, as NEXT would.
 * (Not checked by clang's -fsanitize=function, which looks for a signature
 * before the function it calls: machine code has none.) */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static QCC int h_Q_JIT(QPARAMS) {
    uint32_t pc = (uint32_t)(in - QCODE);
    const Function *fn = QFRAME->fn;
    if (c->hooked) MUSTTAIL return qhandlers[fn->jit_xop[pc]](QARGS); /* a tool sees each instruction */
    Jit *jt = c->vm->jit;
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    if (!ent) MUSTTAIL return qhandlers[in->xop](QARGS); /* forgotten since (interp_jit_forget): its own form is back */
#if QWIDE
    c->fr = qfr;
    c->I = I;
#endif
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
    fprintf(out, "  loops               %llu with their registers in machine registers\n", (unsigned long long)jt->nloops);
    fprintf(out, "  closure calls       %llu of %llu compiled for the function they called last\n",
            (unsigned long long)jt->nspec, (unsigned long long)jt->ncalli);
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
        if (!k) fprintf(out, "  most common stops (where the interpreter took over, a time slice's end included):\n");
        fprintf(out, "    %12llu  %s (form %d)\n", (unsigned long long)stops[best], op_info[jt->opof[best]].mnem, best);
        stops[best] = 0;
    }
}

uint64_t interp_jit_steps(const VM *vm) { return vm->jit ? vm->jit->nsteps : 0; }
#endif
