/* vm.h - runtime structures: loaded modules, functions, processes, memory
 * segments, natives. See docs/ABI.md for the execution model. */
#ifndef MLWSVM_VM_H
#define MLWSVM_VM_H

#include <setjmp.h>
#include <stdatomic.h>

/* On macOS and the BSDs setjmp saves the signal mask with a system call; the
 * VM never changes signal masks, so use the cheap variant there (one setjmp
 * per time slice). glibc's and Windows' setjmp are already the cheap kind. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define VM_SETJMP(b) _setjmp(b)
#define VM_LONGJMP(b, v) _longjmp(b, v)
#else
#define VM_SETJMP(b) setjmp(b)
#define VM_LONGJMP(b, v) longjmp(b, v)
#endif

#include "../core/insn.h"
#include "../core/module.h"

#include "../heap/object.h"

ObjString *str_new_immortal(const char *s, size_t len);

/* ---- natives ---------------------------------------------------------------- */

typedef struct Process Process;
typedef void (*NativeFn)(Process *p, Word *args, Word *rets);

#define NATIVE_MAX_ARGS 8
#define NATIVE_MAX_RETS 4

/* NativeDef flags. Most built-in natives have none and take the fast path. */
#define NF_FOREIGN 0x1  /* from an extension or the FFI: foreign code, counted by quota 4 */
#define NF_BLOCKING 0x2 /* runs on the I/O thread pool while the caller waits */
#define NF_SERIAL 0x4   /* one call at a time per library (its `serial` lock) */
#define NF_CHECKED 0x8  /* reports errors with native_fail (never traps): see extapi.c */
#define NF_OS 0x10      /* reaches the operating system (std.fs, std.os): counted by quota 5 */

/* What a checked native may ask for instead of finishing (Process.native_req):
 * wait for a descriptor and run again, or run again on the I/O thread pool. */
enum { NREQ_NONE = 0, NREQ_WAIT_FD, NREQ_BLOCKING, NREQ_EXIT, NREQ_WAIT_REPLY };

struct ExtLib;

typedef struct NativeDef {
    const char *name;
    uint8_t np;
    uint8_t pt[NATIVE_MAX_ARGS];
    uint8_t nr;
    uint8_t rt[NATIVE_MAX_RETS];
    NativeFn fn;
    const char *doc;
    uint32_t flags;      /* NF_* */
    void *data;          /* the extension's, or the FFI call description */
    void (*xfn)(void);   /* an extension's MlwsNativeFn (called by a trampoline) */
    struct ExtLib *lib;  /* where foreign code came from */
} NativeDef;

/* A loaded native extension (.mlwsx) or FFI library (extensions.c). */
typedef struct ExtLib {
    char *name;               /* extension name, or the FFI library as declared */
    char *path;               /* the file loaded ("" for the program's own symbols) */
    void *handle;
    bool ffi;
    struct PlatLock *serial;  /* held around NF_SERIAL natives and their finalizers */
} ExtLib;

/* ---- loaded code ------------------------------------------------------------ */

struct LModule;

typedef struct Function {
    const char *name;
    struct LModule *mod;
    const MFunc *mf;
    Insn *code;
    uint32_t ncode;
    struct Function *target; /* itself, or the resolved import */
    uint8_t *bp;             /* debugger breakpoints per instruction (tools), or NULL */
    uint64_t *prof;          /* profiler counts per instruction (tools), or NULL */
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
    Obj **consts;             /* m->nconsts: immortal strings, BigInts and decimals */
    uint64_t *grefs;          /* resolved global pointers, m->ngrefs */
    StructDef *sdefs;         /* struct types defined here, m->nstructs (imports unused) */
    const StructDef **structs; /* resolved struct table (local or imported), m->nstructs */
    struct FieldRef *frefs;   /* resolved field references, m->nfrefs */
    const NativeDef **natives; /* m->nnatives */
} LModule;

typedef void (*OutputFn)(void *ctx, const char *s, size_t n);

struct VM;

/* A process that has finished, kept for the monitor's "recent exits". */
typedef struct {
    uint64_t pid;
    const char *entry;
    bool failed;
    char code[32];
    int64_t exit_code;
    uint64_t insns, cpu_ns, ended_ns;
} ExitRecord;

/* Called before every instruction while a tool (debugger, trace, profiler) is
 * active; returning true stops the scheduler before the instruction runs. */
typedef bool (*InsnHook)(struct Process *p, uint32_t pc);
/* Called by worker thread 0 between time slices (e.g. to serve the monitor);
 * `wait_ns` > 0 means the VM is idle and may block for that long. */
typedef void (*PollHook)(struct VM *vm, uint64_t wait_ns);

/* Per-process resource limits (the `quota` instruction kinds). */
enum {
    QUOTA_HEAP = 0, QUOTA_RAW = 1, QUOTA_MAILBOX = 2, QUOTA_INSNS = 3, QUOTA_FOREIGN = 4, QUOTA_OS = 5,
    QUOTA__COUNT
};

struct PlatLock;
struct Sched;

/* A descriptor in transit between processes (fs.give / fs.take). */
typedef struct {
    uint64_t id; /* random: a ticket cannot be guessed */
    int fd, kind;
} FdTicket;

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
    uint64_t next_pid;           /* under `lock` */
    bool gc_stress;          /* collect at every instruction boundary after an allocation */
    uint64_t gc_min_threshold;
    /* processes and scheduling (sched.c) */
    struct Sched *sched;         /* process table, run queues, timers, worker threads */
    int threads;                 /* worker threads; 0 = one per core (MLWSVM_THREADS) */
    struct PlatLock *lock;       /* pid allocation, counters, exit records */
    struct PlatLock *out_lock;   /* program output and crash reports */
    uint64_t main_pid;           /* finishing it ends the program; freed by its runner */
    _Atomic uint64_t insns_retired; /* instructions of processes already freed */
    _Atomic uint64_t live, spawned, max_live, slices;
    uint32_t slice_insns;        /* time slice length in instructions */
    uint64_t default_quota[QUOTA__COUNT];
    OutputFn err_out;            /* crash reports of unobserved processes */
    void *err_ctx;
    /* tools */
    InsnHook hook;
    PollHook poll;
    void *tools;                 /* tool state, owned by src/tools */
    void (*tools_free)(struct VM *vm);
    struct Process *stopped;     /* stopped by the hook; resumes first */
    VEC(ExitRecord) exits;       /* ring of recent exits (under `lock`) */
    size_t exits_next;
    uint64_t start_ns;
    bool hold;                   /* --hold: keep serving the monitor after main ends */
    /* native code (extensions.c, docs/EXTENDING.md) */
    VEC(char *) ext_paths;       /* trusted extension directories, in search order */
    bool unsafe_native;          /* --unsafe-native: skip the trust checks */
    bool warned_unsafe;
    VEC(ExtLib *) extlibs;
    VEC(NativeDef *) foreign_natives; /* owned NativeDefs of extensions and the FFI */
    StrMap ffi_types;            /* FFI handle tag -> its MlwsForeignType */
    /* the program's view of the OS (std.os) */
    StrVec prog_args;            /* os.args: the arguments after the program file */
    _Atomic bool exit_requested; /* os.exit was called: the program ends with exit_code */
    _Atomic int64_t exit_code;
    VEC(FdTicket) tickets;       /* handles given away with fs.give, not yet taken (under `lock`) */
    StrMap registry;             /* proc.register: name -> pid (0 once released), under `lock` */
    bool external_input;         /* the host may send messages (mlws_start): never a deadlock */
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
    Obj *closure;         /* the closure being run (UP[n] reads it), or NULL */
} Frame;

/* PROC_NATIVE: in a blocking native call on the I/O pool. PROC_IOWAIT: waiting
 * for a descriptor (Process.fdwait), woken by the poller. */
enum { PROC_READY = 0, PROC_RUNNING, PROC_WAITING, PROC_SLEEPING, PROC_DONE, PROC_FAILED, PROC_NATIVE, PROC_IOWAIT };

/* How a time slice ended. SLICE_NATIVE: a blocking native call is to be made
 * on the I/O thread pool (Process.bcall). SLICE_IOWAIT: wait for Process.fdwait. */
enum { SLICE_YIELD = 0, SLICE_BLOCK, SLICE_SLEEP, SLICE_DONE, SLICE_FAILED, SLICE_BREAK, SLICE_NATIVE, SLICE_IOWAIT };

/* A wait for a descriptor that a native asked for (native_wait_fd). The native
 * runs again when the descriptor is ready, or with timed_out set. */
typedef struct {
    int fd, events;     /* PLAT_POLL_READ / PLAT_POLL_WRITE */
    uint64_t deadline;  /* ns, or 0 */
    uint32_t gen;       /* matches the poller's token; stale wake-ups are ignored */
    bool timed_out;
} FdWait;

/* A blocking native call in progress: made by an I/O thread, then completed
 * when the ncall instruction runs again. */
enum { BCALL_NONE = 0, BCALL_PENDING, BCALL_DONE };
typedef struct {
    int state;
    const NativeDef *nd;
    const Insn *insn;
    Word args[NATIVE_MAX_ARGS], rets[NATIVE_MAX_RETS]; /* GC roots while active */
} BlockingCall;

typedef struct {
    Word v;
    uint8_t tag;
} Message;

/* A message delivered to a process but not yet taken into its heap: the
 * sender copies the value into a detached list of objects (`objs`, totalling
 * `bytes`), which the receiver adopts on its own thread. */
typedef struct {
    Word v;
    uint8_t tag;
    Obj *objs;
    uint64_t bytes, nobjs;
} InMsg;

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
    /* processes (sched.c). Fields marked [L] are shared with other threads and
     * guarded by `lock`; the rest belong to the thread running the process. */
    Function *entry;
    struct PlatLock *lock;
    VEC(Message) mbox;           /* taken messages, in this heap (GC roots) */
    size_t mbox_head;
    VEC(InMsg) inbox;            /* [L] delivered, not yet taken */
    _Atomic uint64_t mcount;     /* messages waiting: mbox + inbox */
    uint64_t wake_at;            /* SLEEPING, or WAITING with a recvt deadline */
    bool deadline_set;           /* recvt is waiting with a timeout */
    bool deadlock;               /* [L] recv must raise E_DEADLOCK */
    bool kill_pending;           /* killed by its own thread: stop at the next instruction */
    bool kill_req;               /* [L] killed by another thread: stop after this slice */
    bool in_runq;                /* [L] */
    bool published;              /* in the process table */
    bool raising;                /* building an Error: exempt from the heap quota */
    char kill_code[32];          /* [L] why kill_req was set */
    char kill_msg[256];
    PidVec links;                /* [L] */
    PidVec monitors;             /* [L] pids watching this process */
    struct Process *pending_spawn; /* child being set up by spawn (freed if that traps) */
    uint64_t quota[QUOTA__COUNT]; /* the mailbox quota is [L] */
    /* native calls (extapi.c) */
    const NativeDef *cur_native; /* the foreign native being called */
    bool in_foreign;             /* inside foreign code: allocation never traps */
    bool foreign_err;            /* api->error was called: raise when it returns */
    char foreign_code[32];
    char foreign_msg[512];
    uint64_t foreign_calls;
    BlockingCall bcall;
    int native_req;              /* NREQ_*: what the checked native just asked for */
    bool on_io_thread;           /* the native is running on the I/O pool: it may block */
    bool resumed;                /* the native is being run again after a wait (not counted again) */
    FdWait fdwait;               /* PROC_IOWAIT */
    uint64_t os_calls;
    VEC(char *) names;           /* registered names (under vm->lock) */
    /* proc.call: waiting for a reply, which goes to `reply`, never the mailbox */
    uint64_t call_seq, call_deadline;
    uint64_t call_id;            /* [L] the call being waited for, or 0 */
    bool reply_ready;            /* [L] */
    InMsg reply;                 /* [L] detached until taken */
    uint64_t cpu_ns, msgs_sent, msgs_received;
    /* what the entry function returned (for embedding: mlws_call) */
    Word results[NATIVE_MAX_RETS];
    uint8_t result_types[NATIVE_MAX_RETS];
    uint8_t nresults;
};

/* vm.c */
VM *vm_new(void);
void vm_free(VM *vm);
void vm_add_libpath(VM *vm, const char *dir);
void vm_register_native(VM *vm, const NativeDef *nd);
void vm_write(VM *vm, const char *s, size_t n);
void vm_flush(VM *vm); /* flush buffered program output (when the VM is idle, and at exit) */

/* options.c: settings by command-line name, without the "--": stack-words,
 * threads, max-frames, slice, gc-min, quota. vm_new applies MLWSVM_<NAME>. */
bool vm_option_known(const char *name);
bool vm_set_option(VM *vm, const char *name, const char *value, MlwsError *err);
void vm_options_from_env(VM *vm);

/* loader.c */
LModule *vm_load_root(VM *vm, const char *path, MlwsError *err);
LModule *vm_load_text(VM *vm, const char *name, const char *text, MlwsError *err); /* source in memory */
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
/* Push the entry frame of a new process (running `closure`, or NULL), bind
 * its arguments, and hand it to the scheduler. */
void proc_start(Process *p, Function *fn, Obj *closure, const Word *args, int nargs);
Process *vm_process(VM *vm, uint64_t pid); /* live process or NULL (tools; world stopped) */
Segment *proc_segment(Process *p, uint64_t ptr);
/* Inline fast path of proc_segment for valid pointers; errors go the slow way. */
static inline Segment *proc_segment_fast(Process *p, uint64_t ptr) {
    uint32_t seg = PTR_SEG(ptr);
    Segment *s;
    if (seg == SEG_STACK) s = &p->stack;
    else if (seg >= SEG_RAW0 && seg - SEG_RAW0 < p->raw.n) s = &p->raw.v[seg - SEG_RAW0];
    else if (seg >= SEG_GLOBAL0 && seg < SEG_RAW0 && seg - SEG_GLOBAL0 < p->nglobals) s = &p->globals[seg - SEG_GLOBAL0];
    else return proc_segment(p, ptr);
    if (!s->live || s->gen != PTR_GEN(ptr)) return proc_segment(p, ptr);
    return s;
}
uint64_t proc_alloc(Process *p, int64_t size);
void proc_free_block(Process *p, uint64_t ptr);
uint64_t proc_realloc(Process *p, uint64_t ptr, int64_t size);

/* interp.c */
int interp_run(Process *p, uint32_t budget); /* SLICE_* */
/* Pick a specialised handler for common instruction shapes (after verification). */
void interp_quicken(Insn *in);
void interp_quicken_function(Function *f);

/* sched.c - see docs/ABI.md ("Scheduling"). The `p` arguments are the
 * calling (running) process; errors trap in it. */
void sched_start(VM *vm, Process *p); /* publish a new process and make it ready */
/* Run the scheduler until `main` finishes (returns false) or a tool hook
 * stops it (returns true; call again to resume). */
bool sched_run(VM *vm, Process *main);
void sched_send(Process *p, uint64_t pid, Word v, uint8_t tag);
void sched_take_inbox(Process *p); /* move delivered messages into p's mailbox */
void sched_kill(Process *p, uint64_t pid, const char *code, const char *msg);
void sched_link(Process *p, uint64_t pid);
void sched_add_link(Process *p, uint64_t pid); /* p's side of a link only */
void sched_monitor(Process *p, uint64_t pid);
void sched_set_quota(Process *p, int kind, uint64_t limit);
bool sched_alive(VM *vm, uint64_t pid);
void sched_shutdown(VM *vm, Process *keep); /* free every process except `keep` */
void sched_free(VM *vm);
bool sched_abandoned(VM *vm); /* a blocking native call never returned: the VM cannot be freed */
/* Pause every other worker thread at a time-slice boundary (tools that read
 * other processes' state), and let them continue. No-ops when single-threaded. */
void sched_stop_world(VM *vm);
void sched_start_world(VM *vm);
/* The process table, for tools: only while the world is stopped. */
Process *vm_proc_at(VM *vm, uint64_t pid); /* any process still in the table */
uint64_t vm_pid_end(VM *vm);               /* one past the highest pid assigned */
/* Copy a value from one process into another's heap (heap/copy.c). */
Word msg_copy(Process *from, Process *to, Word v, uint8_t tag);
void copy_state_free(void); /* the calling thread's copy scratch */

/* natives.c */
void natives_register_builtin(VM *vm);
const NativeDef *natives_builtin_table(size_t *n);

/* extapi.c: calling foreign natives. native_call runs one in the caller
 * (traps on failure); native_invoke only runs it (also on an I/O thread);
 * native_complete raises what it recorded. */
void native_call(Process *p, const NativeDef *nd, Word *args, Word *rets);
void native_invoke(Process *p, const NativeDef *nd, Word *args, Word *rets);
void native_complete(Process *p);
void native_count_call(Process *p, const NativeDef *nd); /* the foreign (4) and OS (5) quotas */
/* A checked native's requests (it then returns; it is run again later, with
 * Process.resumed set): wait for a descriptor (timeout_ms < 0: none), or run on
 * the I/O thread pool, where it may block. */
void native_wait_fd(Process *p, int fd, int events, int64_t timeout_ms);
void native_run_blocking(Process *p);
/* osio.c: std.fs and std.os */
void natives_register_os(VM *vm);
const NativeDef *natives_os_table(size_t *n);
/* sched.c: std.proc (the registry, request/reply calls) */
void natives_register_proc(VM *vm);
const NativeDef *natives_proc_table(size_t *n);
/* text.c: std.text and std.json */
void natives_register_text(VM *vm);
const NativeDef *natives_text_table(size_t *n);
/* sched.c: os.exit: end the whole program now with this exit code. */
void sched_request_exit(VM *vm, int64_t code);
/* Send a message from outside the VM (any thread): a number, or a string if
 * str is not NULL. False if the process does not exist or has finished. */
bool sched_post(VM *vm, uint64_t pid, Word v, uint8_t tag, const char *str, size_t len);
/* Inside a foreign call: fail it (raised when the call returns). */
void native_fail(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
void foreign_finalize(Obj *o);       /* run a foreign handle's finalizer (GC) */
ObjForeign *foreign_new_obj(Process *p, const void *type, void *ptr, ExtLib *lib);
bool ext_run_init(VM *vm, ExtLib *lib, void *init, MlwsError *err);


/* extensions.c: loading native code, with the trust checks. */
bool ext_load(VM *vm, const char *name, MlwsError *err);           /* `.extension "name"` */
ExtLib *ext_ffi_library(VM *vm, const char *spec, MlwsError *err); /* `.extern` libraries */
void ext_free_all(VM *vm);
/* ffi.c: an `.extern` declaration as a native (false if built without libffi). */
struct MExtern;
bool ffi_define(VM *vm, const struct MExtern *x, MlwsError *err);
bool ffi_available(void);

#endif
