/* vm.h - runtime structures: loaded modules, functions, processes, memory
 * segments, natives. See docs/ABI.md for the execution model. */
#ifndef MLWSVM_VM_H
#define MLWSVM_VM_H

#include <setjmp.h>

#include "../core/insn.h"
#include "../core/module.h"

#include "../heap/object.h"

ObjString *str_new_immortal(const char *s, size_t len);

/* ---- natives ---------------------------------------------------------------- */

typedef struct Process Process;
typedef void (*NativeFn)(Process *p, Word *args, Word *rets);

#define NATIVE_MAX_ARGS 8
#define NATIVE_MAX_RETS 4

typedef struct NativeDef {
    const char *name;
    uint8_t np;
    uint8_t pt[NATIVE_MAX_ARGS];
    uint8_t nr;
    uint8_t rt[NATIVE_MAX_RETS];
    NativeFn fn;
    const char *doc;
} NativeDef;

/* ---- loaded code ------------------------------------------------------------ */

struct LModule;

typedef struct Function {
    const char *name;
    struct LModule *mod;
    const MFunc *mf;
    Insn *code;
    uint32_t ncode;
    struct Function *target; /* itself, or the resolved import */
} Function;

enum { LM_LOADING = 1, LM_READY = 2 };

#include "../core/builtins.h"

/* Built-in struct types (Error, Down), indexed by BUILTIN_*. */
extern StructDef VM_BUILTIN_DEFS[BUILTIN__COUNT]; /* filled by vm_new */
#define VM_ERROR_DEF (VM_BUILTIN_DEFS[BUILTIN_ERROR])
#define VM_DOWN_DEF (VM_BUILTIN_DEFS[BUILTIN_DOWN])

typedef struct FieldRef {
    const StructDef *def;
    uint32_t index;
} FieldRef;

typedef struct LModule {
    Module *m;
    char *path;
    int index;
    int state;
    Function *funcs;          /* m->nfuncs */
    ObjString **consts;       /* m->nconsts */
    uint64_t *grefs;          /* resolved global pointers, m->ngrefs */
    StructDef *sdefs;         /* struct types defined here, m->nstructs (imports unused) */
    const StructDef **structs; /* resolved struct table (local or imported), m->nstructs */
    struct FieldRef *frefs;   /* resolved field references, m->nfrefs */
    const NativeDef **natives; /* m->nnatives */
} LModule;

typedef void (*OutputFn)(void *ctx, const char *s, size_t n);

/* Per-process resource limits (the `quota` instruction kinds). */
enum { QUOTA_HEAP = 0, QUOTA_RAW = 1, QUOTA_MAILBOX = 2, QUOTA_INSNS = 3, QUOTA__COUNT };

/* A timer heap entry: wake process `pid` at `at` (ns), if still applicable. */
typedef struct {
    uint64_t at, pid;
} Timer;

typedef struct VM {
    VEC(LModule *) mods;
    StrMap modmap;
    VEC(char *) libpaths;
    VEC(char *) loading; /* import chain for cycle reporting */
    StrMap natives;      /* name -> const NativeDef* */
    OutputFn out;
    void *out_ctx;
    uint32_t stack_words;
    uint32_t max_frames;
    uint64_t next_pid;
    bool gc_stress;          /* collect at every instruction boundary after an allocation */
    uint64_t gc_min_threshold;
    /* processes and scheduling (sched.c) */
    VEC(struct Process *) procs; /* indexed by pid - 1; NULL once freed */
    VEC(uint64_t) runq;          /* ring buffer of ready pids */
    size_t runq_head, runq_len;
    VEC(Timer) timers;           /* min-heap by wake time */
    struct Process *current;     /* process executing an instruction, or NULL */
    uint64_t main_pid;           /* finishing it ends the program; freed by its runner */
    void *copy_state;            /* scratch for msg_copy (heap/copy.c) */
    struct Process *pending_spawn; /* child being set up by spawn (freed if that traps) */
    uint64_t insns_retired;      /* instructions of processes already freed */
    uint64_t live, spawned, max_live, slices;
    uint32_t slice_insns;        /* time slice length in instructions */
    uint64_t default_quota[QUOTA__COUNT];
    OutputFn err_out;            /* crash reports of unobserved processes */
    void *err_ctx;
} VM;

/* ---- processes ------------------------------------------------------------- */

/* Checked pointers: segment:20 | generation:12 | offset:32. */
#define PTR_MAKE(seg, gen, off) (((uint64_t)(seg) << 44) | ((uint64_t)((gen) & 0xFFFu) << 32) | (uint32_t)(off))
#define PTR_SEG(p) ((uint32_t)((p) >> 44))
#define PTR_GEN(p) ((uint32_t)(((p) >> 32) & 0xFFFu))
#define PTR_OFF(p) ((uint32_t)(p))
#define SEG_STACK 1u
#define SEG_GLOBAL0 2u    /* module k's globals live in segment 2 + k */
#define SEG_RAW0 4096u    /* raw heap blocks */
#define SEG_MAX (1u << 20)

enum { SEGK_STACK = 1, SEGK_GLOBAL, SEGK_RAW };

typedef struct {
    uint8_t *data;
    uint8_t *tags; /* per-word MlwsType tags; NULL for raw memory */
    uint32_t size;
    uint16_t gen;
    uint8_t kind;
    uint8_t live;
} Segment;

typedef struct {
    Function *fn;
    uint32_t pc;
    uint32_t base[NBANKS];
    uint64_t entry_sp; /* SP at the call; restored on return */
    uint64_t saved_fp;
    const Insn *ret_insn; /* caller's call instruction that receives results */
} Frame;

enum { PROC_READY = 0, PROC_RUNNING, PROC_WAITING, PROC_SLEEPING, PROC_DONE, PROC_FAILED };

/* How a time slice ended. */
enum { SLICE_YIELD = 0, SLICE_BLOCK, SLICE_SLEEP, SLICE_DONE, SLICE_FAILED };

typedef struct {
    Word v;
    uint8_t tag;
} Message;

typedef VEC(uint64_t) PidVec;

struct Process {
    VM *vm;
    uint64_t pid;
    int state;
    int64_t exit_code;
    /* register stacks, one per bank; each frame owns a window */
    int64_t *ireg;
    double *freg;
    Obj **rreg;
    uint64_t *preg;
    uint32_t rcap[NBANKS], rtop[NBANKS];
    Frame *frames;
    uint32_t nframes, fcap;
    Segment stack;
    Segment *globals;
    uint32_t nglobals;
    VEC(Segment) raw;
    VEC(uint32_t) rawfree;
    uint64_t sp, fp;
    MlwsError err;
    StrBuf backtrace;
    jmp_buf *jb;
    /* statistics */
    uint64_t insns;
    uint64_t raw_bytes, raw_peak, raw_allocs, raw_frees;
    uint32_t stack_low_off; /* lowest SP offset seen (stack high-water mark) */
    uint32_t max_depth;
    /* garbage-collected heap (heap/gc.c) */
    Obj *heap;
    uint64_t heap_bytes, heap_peak, next_gc;
    uint64_t gc_count, gc_ns, gc_freed_objects, objects_allocated;
    bool gc_pending;
    VEC(Obj *) handles;
    VEC(uint32_t) handle_free;
    VEC(Obj *) markstack;
    Obj *exception; /* being thrown or just caught (a GC root) */
    uint64_t exceptions_thrown;
    /* processes (sched.c) */
    Function *entry;
    VEC(Message) mbox;           /* GC roots */
    size_t mbox_head;
    uint64_t wake_at;            /* SLEEPING, or WAITING with a recvt deadline */
    bool deadline_set;           /* recvt is waiting with a timeout */
    bool deadlock;               /* recv must raise E_DEADLOCK */
    bool kill_pending;           /* killed while running: stop at the next instruction */
    bool in_runq;
    bool raising;                /* building an Error: exempt from the heap quota */
    PidVec links;
    PidVec monitors;             /* pids watching this process */
    uint64_t quota[QUOTA__COUNT];
    uint64_t cpu_ns, msgs_sent, msgs_received;
};

/* vm.c */
VM *vm_new(void);
void vm_free(VM *vm);
void vm_add_libpath(VM *vm, const char *dir);
void vm_register_native(VM *vm, const NativeDef *nd);
void vm_write(VM *vm, const char *s, size_t n);

/* loader.c */
LModule *vm_load_root(VM *vm, const char *path, MlwsError *err);
LModule *vm_require(VM *vm, const char *name, MlwsError *err);
/* verify.c */
bool vm_verify_module(VM *vm, LModule *lm, MlwsError *err);

/* process.c */
Process *proc_new(VM *vm);
void proc_free(Process *p);
/* Raise a runtime fault as a catchable Error exception. */
MLWS_NORETURN void vm_trap(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
/* Raise any non-null object as an exception. */
MLWS_NORETURN void proc_throw(Process *p, Obj *ex);
/* Run up to `budget` instructions; returns a SLICE_* code. On SLICE_FAILED,
 * p->err describes the failure. */
int proc_run_slice(Process *p, uint32_t budget);
/* Push the entry frame of a new process and bind its arguments. */
void proc_start(Process *p, Function *fn, const Word *args, int nargs);
Process *vm_process(VM *vm, uint64_t pid); /* live process or NULL */
Segment *proc_segment(Process *p, uint64_t ptr);
uint64_t proc_alloc(Process *p, int64_t size);
void proc_free_block(Process *p, uint64_t ptr);
uint64_t proc_realloc(Process *p, uint64_t ptr, int64_t size);

/* interp.c */
int interp_run(Process *p, uint32_t budget); /* SLICE_* */

/* sched.c */
void sched_ready(VM *vm, Process *p);
void sched_add_timer(VM *vm, Process *p);
/* Run the scheduler until `main` finishes; other processes are then killed. */
void sched_run(VM *vm, Process *main);
void sched_finish(VM *vm, Process *p, bool failed); /* notify links/monitors; frees non-main */
void sched_kill(VM *vm, Process *target, const char *code, const char *msg);
void sched_shutdown(VM *vm, Process *keep);
void sched_down_noproc(VM *vm, Process *watcher, uint64_t pid);
/* Copy a value from one process into another's heap (heap/copy.c). */
Word msg_copy(Process *from, Process *to, Word v, uint8_t tag);
void copy_state_free(VM *vm);

/* natives.c */
void natives_register_builtin(VM *vm);
const NativeDef *natives_builtin_table(size_t *n);

#endif
