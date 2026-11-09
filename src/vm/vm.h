/* vm.h - runtime structures: loaded modules, functions, processes, memory
 * segments, natives. See docs/ABI.md for the execution model. */
#ifndef MLWSVM_VM_H
#define MLWSVM_VM_H

#include <setjmp.h>
#include <stdatomic.h>

/* On macOS and the BSDs setjmp saves the signal mask with a system call; the
 * VM never changes signal masks, so use the cheap variant there (one setjmp
 * per time slice). glibc's setjmp is already the cheap kind. On Windows
 * (MinGW), longjmp unwinds the frames it leaves with the system's unwinder:
 * the JIT's machine code has no unwind tables, and nothing on the way needs
 * unwinding, so on x86-64 setjmp is given no frame, and on arm64 (which has
 * no such setjmp) MinGW's own pair, which never unwinds, is used. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define VM_SETJMP(b) _setjmp(b)
#define VM_LONGJMP(b, v) _longjmp(b, v)
#elif defined(__MINGW64__) && defined(__x86_64__)
#define VM_SETJMP(b) _setjmp((b), NULL)
#define VM_LONGJMP(b, v) longjmp(b, v)
#elif defined(__MINGW64__) && defined(__aarch64__)
#define VM_SETJMP(b) __mingw_setjmp(b)
#define VM_LONGJMP(b, v) __mingw_longjmp(b, v)
#else
#define VM_SETJMP(b) setjmp(b)
#define VM_LONGJMP(b, v) longjmp(b, v)
#endif

#include "../core/insn.h"
#include "../core/module.h"
#include "../opt/opt.h"

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
    /* Its register window, one block in the process's register stack: the I
     * registers, then F, R and P. roff[b] is where bank b starts (roff[BANK_I]
     * is 0); rsize is the whole window, in 8-byte words. */
    uint16_t roff[NBANKS];
    uint16_t rsize;
    /* What a call clears of the window: the zlen words from word zoff (one
     * run, maybe empty; verify.c, plan_window). The rest is written before it
     * is read on every path. 16 bits, as rsize: with a byte, GCC knows the
     * clear is under 2 KiB and expands it inline as `rep stos`, whose start-up
     * made calls on x86 up to a third slower than a call to memset. */
    uint16_t zoff, zlen;
    /* No instruction of it moves or reads SP or FP (no push, pop, drop or
     * enter, no SP or FP operand): its frame neither saves nor restores them
     * (proc_push_frame_fitting, proc_pop_frame), since it leaves them as it
     * found them. */
    bool stackless;
    /* The I, F and P registers it may read before writing them (bit k:
     * register k of that bank; R is always cleared): what makes the run
     * longer, shown by the profiler. */
    uint32_t read_first[NBANKS];
    /* The JIT's (interp.c, jit.inc, docs/JIT.md): each instruction's
     * machine-code entry, or NULL (NULL until compiled); each instruction's
     * quickened form, under Q_JIT and Q_JITC; calls and loop iterations
     * towards compiling it; its instructions with machine code, and loops
     * with their registers in machine registers; JIT_COMPILED, JIT_NONE
     * (nothing to compile, or no room), or 0. */
    void **jit_entry;
    uint16_t *jit_xop;
    uint32_t jit_count;
    uint32_t jit_native;
    uint16_t jit_loops;
    uint16_t jit_spec; /* closure calls compiled for the function they called last */
    uint8_t jit_state;
} Function;

enum { LM_LOADING = 1, LM_READY = 2, LM_FAILED = 3, LM_PURGED = 4 }; /* FAILED: a load at run time that did not link;
                                                                     PURGED: a replaced version freed (vm_purge) */

#include "../core/builtins.h"

/* Built-in struct types (Error, Down), indexed by BUILTIN_*. */
extern StructDef VM_BUILTIN_DEFS[BUILTIN__COUNT]; /* filled by vm_new */
#define VM_ERROR_DEF (VM_BUILTIN_DEFS[BUILTIN_ERROR])
#define VM_DOWN_DEF (VM_BUILTIN_DEFS[BUILTIN_DOWN])

typedef struct FieldRef {
    const StructDef *def;
    uint32_t index;
} FieldRef;

/* One of the programs a run started (`mlwsvm run a.masm --also b.masm`): its
 * main process, and the arguments os.args gives its processes. Every process
 * belongs to the program that spawned it; when a program's main ends, its
 * other processes are stopped, and the run ends with the last program. */
typedef struct Program {
    uint32_t id;          /* its place in VM.progs, from 0 */
    char *path;
    StrVec args;
    char *cap, *quota;    /* mlwsvm serve: what it was started with (NULL: the VM's own) */
    struct Process *main; /* kept, not freed, when it ends: its runner reads its results */
} Program;

typedef struct LibWant {
    char *lib, *constraint, *who; /* who: the module or library that asked */
    const struct LModule *mod;      /* who, if a module (NULL: a library, or a program at the start) */
    const struct LibArchive *by;    /* who, if a library */
} LibWant;

/* A program's own directory (that of its root module's file). Its modules
 * belong to the programs started from it: they are found there first, and
 * are told apart from same-named modules of other directories by `real`. */
typedef struct Home {
    char *dir;  /* as the program's path gave it */
    char *real; /* resolved: the same directory spelled two ways is one */
} Home;

typedef struct LModule {
    Module *m;
    char *path;
    /* Where it was found: in a program's directory (home), or on the library
     * path (NULL). key is what modmap knows it by: a library-path module's
     * name, or its home and name. shared_name: another loaded module has the
     * same name (one of them is program-local), so tools add its file. */
    const Home *home;
    char *key;
    atomic_bool shared_name;
    bool replaced;            /* a newer version took its name (vm_reload) */
    bool handle;              /* a module.handle's own module (vm_handle): h, and gone in its place */
    /* Its version's identity across nodes (vm_module_hash): SHA-256 of its
     * .mlwso image but for its source files' names, first 16 bytes. */
    bool hashed;
    uint8_t hash[16];
    /* A closure's stub (vm_remote_stub): the module, version and function
     * of the closure it stands for, which this VM does not have. NULL else. */
    char *remote_mod, *remote_fn;
    uint8_t remote_hash[16];
    bool remote_handle; /* it stands for a module.handle (no version) */
    struct LModule **deps;    /* m->nimports: the module each import resolved to */
    int index;
    int state;
    Function *funcs;          /* m->nfuncs */
    Obj **consts;             /* m->nconsts: immortal strings, BigInts and decimals */
    uint64_t *grefs;          /* resolved global pointers, m->ngrefs */
    StructDef *sdefs;         /* struct types defined here, m->nstructs (imports unused) */
    const StructDef **structs; /* resolved struct table (local or imported), m->nstructs */
    struct FieldRef *frefs;   /* resolved field references, m->nfrefs */
    const NativeDef **natives; /* m->nnatives */
    struct LibArchive *lib;   /* the library archive it came from, or NULL (a plain file) */
    /* A version that replaced another (vm_reload): the globals it carries from
     * that one, same name and size in both, as byte ranges. A process copies
     * them from its segment of the old version as it makes its segment of
     * this one (proc_grow_globals). */
    int carry_from;           /* the old version's index, or -1 */
    uint32_t ncarry;
    struct GlobalCarry {
        uint32_t from, to, size;
    } *carry;
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
    int32_t prog;            /* its program's id, or -1 */
    bool main;               /* its program's main */
} ExitRecord;

/* An entry of the monitor's log channel (monitor.log, monitor.event). */
typedef struct MLogEntry {
    uint64_t seq, t_ns, pid; /* t_ns: since the VM started */
    int32_t prog;            /* the writer's program's id, or -1 (the monitor) */
    int level;               /* 0 debug, 1 info, 2 warn, 3 error; -1 for an event */
    char *name;              /* an event's name, or NULL */
    char *text;              /* a log line, or an event's value as JSON */
} MLogEntry;

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
    struct Tls *tls; /* its TLS session, which goes with it (native/tls.c), or NULL */
    bool shook;
} FdTicket;

typedef struct VM {
    VEC(LModule *) mods;
    StrMap modmap;       /* LModule.key -> LModule (NULL: a run-time load of it failed) */
    VEC(char *) libpaths;
    VEC(Home *) homes;   /* the directories of the programs loaded */
    size_t homes_scanned; /* how many have been searched for library archives */
    VEC(LModule *) loading; /* import chain for cycle reporting */
    /* libraries (loader.c): the archives on the library path, found on first
     * need; the one chosen for each library name; and the version constraints
     * stated so far, which a library chosen later must meet */
    VEC(struct LibArchive *) archives;
    bool archives_scanned;
    StrMap libsel;                      /* a NULL value: none chosen (a failed load took it back) */
    VEC(struct LibArchive *) chosen;    /* the archives in libsel, in the order they were chosen */
    /* every module's constants (strings, BigInts, decimals), one object per
     * kind and text, shared by all modules: owned here, freed with the VM */
    StrMap const_pool;
    StrMap handles;      /* module.handle: "module key\x1f function" -> its stub Function (vm_handle) */
    StrMap remotes;      /* closures from other nodes of code not here: identity -> stub Function (vm_remote_stub) */
    VEC(Obj *) const_objs;
    VEC(struct LibWant) wants;
    StrMap natives;      /* name -> const NativeDef* */
    OutputFn out;
    void *out_ctx;
    uint32_t stack_words;
    uint32_t max_frames;
    uint64_t next_pid;           /* under `lock` */
    atomic_bool reloading;   /* vm_reload is at work: one at a time */
    const struct LModule *reload_old; /* the version being replaced, as its successor links (under mods_lock) */
    VEC(void *) retired;     /* what reloads replaced and another thread may still read: freed with the VM */
    struct Caps *default_caps; /* --cap: every process's, as default_quota */
    uint32_t node_self;      /* this VM's number among nodes, or 0 (dist.c) */
    uint64_t *node_route;    /* per node: the process that carries its messages, or 0 */
    struct NodeStats **node_stats; /* per node: its connection's traffic, since it last came (made by node.attach) */
    char *node_name;         /* [lock] this VM's name among nodes, or NULL (node.set_name) */
    char **node_names;       /* [lock] per node: the name it gave when it last connected, or NULL */
    StrMap node_numbers;     /* [lock] and back: name -> the numbers that have it (NodeNums) */
    VEC(uint64_t) node_watchers; /* [lock] processes told of each connection that comes or goes (node.watch) */
    VEC(struct ModWatch { char *name; uint64_t pid; }) mod_watchers; /* [lock] processes told of each reload of a module (module.watch) */
    struct Bundle *bundle;   /* the program this executable carries, or NULL: modules are found there first */
    bool gc_stress;          /* collect at every instruction boundary after an allocation */
    bool gc_verify;          /* after each minor collection's marking, check it against a walk of the whole heap */
    bool slabs;              /* processes use the small-object allocator (gc.c); MLWSVM_NO_SLABS=1: malloc */
    uint64_t gc_min_threshold;
    bool whole_windows;      /* every call clears its whole register window (the debugger) */
    /* processes and scheduling (sched.c) */
    struct Sched *sched;         /* process table, run queues, timers, worker threads */
    int threads;                 /* worker threads; 0 = one per core (MLWSVM_THREADS) */
    struct PlatLock *lock;       /* pid allocation, counters, exit records */
    struct PlatLock *mods_lock;  /* mods and the loader's state, once processes run (module.load) */
    struct PlatLock *out_lock;   /* program output and crash reports */
    uint64_t main_pid;           /* the first program's main process (tools show it) */
    VEC(Program *) progs;        /* the programs of the current run (run.c) */
    _Atomic int progs_live;      /* those whose main has not ended */
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
    /* The monitor's log channel: the last mlog_cap entries, kept while a monitor
     * is attached (mlog_on; otherwise monitor.log and monitor.event do nothing).
     * Entry s (from 1) is at mlog[(s - 1) % mlog_cap]. */
    _Atomic bool mlog_on;
    struct PlatLock *mlog_lock;
    MLogEntry *mlog;
    uint32_t mlog_cap;
    uint64_t mlog_seq;
    size_t exits_next;
    uint64_t start_ns;
    bool hold;                   /* --hold: keep serving the monitor after main ends */
    /* native code (extensions.c, docs/EXTENDING.md) */
    VEC(char *) ext_paths;       /* trusted extension directories, in search order */
    bool unsafe_native;          /* --unsafe-native: skip the trust checks */
    bool no_cache;               /* --no-cache: assemble every .masm afresh (modcache.c) */
    /* the optimizer (src/opt, docs/OPTIMIZER.md): run on each module as it is loaded */
    OptConfig opt;               /* --opt: the passes; none by default */
    OptReport *opt_report;       /* what they did, if --opt-report (or a test) asks */
    char *opt_report_path;       /* --opt-report FILE, or "-" for stderr: written by vm_free */
    /* the JIT (interp.c, jit.inc, docs/JIT.md) */
    uint8_t jit_mode;            /* --jit: JIT_MODE_OFF, _ON (hot functions) or _ALL (every function, at load) */
    uint32_t jit_hot;            /* --jit-hot: calls or loop iterations before a function is compiled */
    char *jit_report_path;       /* --jit-report FILE, or "-": written by vm_free */
    bool jit_stats;              /* count entries, steps and stops (a test's expect-jit) */
    bool jit_failed;             /* no code memory */
    struct Jit *jit;             /* its code memory, once something is compiled */
    struct PlatLock *jit_lock;   /* compiling */
    int cache_state;             /* 0 not looked at yet, 1 cache_dir in use, -1 off */
    char *cache_dir;
    bool warned_unsafe;
    VEC(ExtLib *) extlibs;
    VEC(NativeDef *) foreign_natives; /* owned NativeDefs of extensions and the FFI */
    StrMap ffi_types;            /* FFI handle tag -> its MlwsForeignType */
    StrMap ffi_structs;          /* .cstruct name -> its C layout (ffi.c) */
    StrMap ffi_callbacks;        /* .callback name -> its C function type (ffi.c) */
    _Atomic int64_t async_sources; /* live async callbacks and extension holds: messages may come from C */
    /* the program's view of the OS (std.os) */
    StrVec prog_args;            /* os.args: the arguments after the program file */
    _Atomic bool exit_requested; /* os.exit was called: the program ends with exit_code */
    _Atomic int64_t exit_code;
    VEC(FdTicket) tickets;       /* handles given away with fs.give, not yet taken (under `lock`) */
    StrMap registry;             /* proc.register: name -> pid (0 once released), under `lock` */
    VEC(char *) pid_names;       /* [lock] the names node.name_pid has made pids for, numbered from 1 */
    StrMap pid_name_ids;         /* [lock] and back */
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
/* The most bytes in one block of memory (a raw block or a stack): 4 GB, or
 * 1 GB on a 32-bit host, where a block's size and overheads must fit a size_t. */
#define MAX_BLOCK (SIZE_MAX > UINT32_MAX ? (int64_t)UINT32_MAX : (int64_t)1 << 30)

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
    /* Pushed by a quickened call (interp.c) whose result registers were checked
     * against this function's declared result types: its quickened ret copies
     * the results straight across. A tail call replaces the frame with one
     * that is not marked. */
    uint8_t qret;
    uint8_t stackless; /* fn->stackless: entry_sp and saved_fp are not kept */
    uint32_t base; /* its register window: regs[base .. base + fn->rsize) */
    uint64_t entry_sp; /* SP at the call; restored on return */
    uint64_t saved_fp;
    const Insn *ret_insn; /* caller's call instruction that receives results */
    Obj *closure;         /* the closure being run (UP[n] reads it), or NULL */
} Frame;

/* Register i of bank b in frame fr's window: a word of the register stack,
 * read and written with the bank's type (int64_t, double, Obj * or uint64_t). */
static inline uint64_t *frame_reg(uint64_t *regs, const Frame *fr, int b, int i) {
    return regs + fr->base + fr->fn->roff[b] + i;
}

/* What a coroutine switch exchanges: the register stack, the frames and the
 * data stack, the same fields as in Process (coro.c swaps them one by one, so
 * the rest of the VM always finds the running context in Process). */
typedef struct {
    uint64_t *regs;
    uint32_t rcap, rtop;
    Frame *frames;
    uint32_t nframes, fcap;
    uint32_t flimit; /* frames that fit without growing or the depth limit: min(fcap, max_frames) */
    Segment stack;
    uint64_t sp, fp;
    uint32_t stack_low_off, stack_ref_lo;
} ExecCtx;

/* Coroutine states, as the costatus instruction reports them. */
enum { CO_SUSPENDED = 0, CO_RUNNING = 1, CO_NORMAL = 2, CO_DEAD = 3 };

/* A coroutine (coro.c): a second execution context in the same process and
 * heap. While it is suspended, `ctx` holds its own context; while it runs (or
 * has resumed another coroutine), `ctx` holds the context of the code that
 * resumed it, since a switch swaps the two. */
typedef struct ObjCoro {
    Obj h;
    uint8_t status;     /* CO_* */
    uint32_t words;     /* its data stack's size, charged to the heap; 0 once dead */
    ObjClosure *fn;     /* the body */
    struct ObjCoro *prev; /* running or normal: the coroutine that resumed it (NULL: the process's own code) */
    /* Suspended: its coyield, which receives the next coresume's values (NULL
     * before the first resume). Running: the coresume that receives its results. */
    const Insn *wait;
    ExecCtx ctx;
} ObjCoro;

/* PROC_NATIVE: in a blocking native call on the I/O pool. PROC_IOWAIT: waiting
 * for a descriptor (Process.fdwait), woken by the poller. */
enum { PROC_READY = 0, PROC_RUNNING, PROC_WAITING, PROC_SLEEPING, PROC_DONE, PROC_FAILED, PROC_NATIVE, PROC_IOWAIT };

/* How a time slice ended. SLICE_NATIVE: a blocking native call is to be made
 * on the I/O thread pool (Process.bcall). SLICE_IOWAIT: wait for Process.fdwait.
 * SLICE_BUSY: a send to another node found its connection busy (send_far). */
enum { SLICE_YIELD = 0, SLICE_BLOCK, SLICE_SLEEP, SLICE_DONE, SLICE_FAILED, SLICE_BREAK, SLICE_NATIVE, SLICE_IOWAIT, SLICE_BUSY };

/* A wait for a descriptor that a native asked for (native_wait_fd). The native
 * runs again when the descriptor is ready, or with timed_out set. */
typedef struct {
    int fd, events;     /* PLAT_POLL_READ / PLAT_POLL_WRITE */
    uint64_t deadline;  /* ns, or 0 */
    uint32_t gen;       /* matches the poller's token; stale wake-ups are ignored */
    bool timed_out;
} FdWait;

/* A serial library's lock held by a foreign call in progress, and the hold of
 * the call that made the callback this call came from (on the C stack). */
typedef struct SerialHold {
    struct PlatLock *lock;
    struct SerialHold *outer;
} SerialHold;

/* A finalizer of a serial library whose call is in progress on this process:
 * run under the lock when that call returns. */
typedef struct {
    void (*finalize)(void *ptr);
    void *ptr;
    struct PlatLock *lock;
} DeferredFinalizer;

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

/* A connection to another node, counted for the monitor: messages sent there
 * (send_far) and received from there (node.take), the frames' bytes written
 * and read, and how many times a sender waited for room (back-pressure). */
/* The node numbers that have had a name (VM.node_numbers), few. */
struct NodeNums {
    uint32_t *v;
    size_t n, cap;
};

typedef struct NodeStats {
    _Atomic uint64_t sent, received, bytes_out, bytes_in, waits;
    uint64_t up_ns; /* when it came */
} NodeStats;

/* Why a process stopped: a runtime trap's or an uncaught exception's code
 * and message (longer messages are cut). Smaller than an MlwsError, whose
 * message carries the assembler's source excerpts: every process has one. */
typedef struct {
    char code[32];
    char msg[512];
} ProcError;

struct Process {
    VM *vm;
    uint64_t pid;
    int state;
    int64_t exit_code;
    /* the register stack: each frame owns a window of 8-byte words, its I, F,
     * R and P registers one after another (Function.roff), each word read and
     * written with its bank's type */
    uint64_t *regs;
    uint32_t rcap, rtop;
    Frame *frames;
    uint32_t nframes, fcap;
    uint32_t flimit; /* frames that fit without growing or the depth limit: min(fcap, max_frames) */
    Segment stack;
    Segment *globals;
    uint32_t nglobals;
    VEC(Segment *) old_globals; /* arrays replaced by proc_grow_globals, kept until the process ends */
    VEC(Segment) raw;
    VEC(uint32_t) rawfree;
    uint64_t sp, fp;
    ProcError err;
    StrBuf backtrace;
    StrBuf outline;              /* program output after its last newline, not yet written (proc_write) */
    struct Program *prog;        /* the program it belongs to (inherited at spawn), or NULL */
    jmp_buf *jb;
    /* statistics */
    uint64_t insns;
    uint64_t raw_bytes, raw_peak, raw_allocs, raw_frees;
    uint32_t stack_low_off; /* lowest SP offset seen (stack high-water mark) */
    uint32_t stack_ref_lo;  /* lowest stack word ever tagged as a reference: the GC scans from here */
    uint32_t max_depth;
    /* garbage-collected heap (heap/gc.c) */
    Obj *heap;                   /* the objects not in the chunks below: from malloc */
    uint64_t heap_bytes, heap_peak, next_gc;
    uint64_t gc_count, gc_ns, gc_freed_objects, objects_allocated;
    bool gc_pending;
    VEC(Obj *) handles;
    VEC(uint32_t) handle_free;
    VEC(Obj *) markstack;
    /* generations (gc.c): an object that survives a collection is old */
    Obj *heap_old;      /* the first object of `heap` that was there at the last collection */
    VEC(Obj *) remset;  /* old objects that may hold young ones (written since the last collection) */
    uint64_t old_bytes, next_major, gc_majors;
    uint64_t nursery;   /* what is allocated between minor collections: from --gc-min, doubled while too much survives */
    bool gc_major_req, gc_verifying;
    Obj *gc_parent;     /* gc_verify: the object being scanned */
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
    bool sandboxed;              /* a debugger's formatter: may not reach other processes (proc_reach) */
    bool killed;                 /* [L] a kill with E_KILLED was asked for: an end with it is not reported */
    char kill_code[32];          /* [L] why kill_req was set */
    char kill_msg[256];
    PidVec links;                /* [L] */
    PidVec monitors;             /* [L] pids watching this process */
    /* Across nodes (sched.c, "Across nodes"): pids on other nodes, as they name
     * themselves there (with their node's number). */
    PidVec rlinks;               /* [L] linked with this process */
    PidVec rmonitors;            /* [L] watching this process */
    PidVec rwatch;               /* [L] watched by this process: told if their node goes */
    /* Back-pressure between nodes (sched.c, send_far): for a process carrying a
     * node's connection, the bytes of frames sent to it and not yet written,
     * and who waits for them to go down; for a sender, which carrier it waits
     * for after a send (0: none). */
    uint64_t qbytes;             /* [L] */
    uint32_t carries;            /* [L] the node whose connection it carries (node.attach), or 0 */
    PidVec busy_senders;         /* [L] */
    _Atomic uint64_t busy_on;
    bool busy_woken;             /* [L] told to go on before it slept */
    bool busy_sleep;             /* [L] sleeping for it (SLICE_BUSY) */
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
    uint32_t call_node;          /* [L] the node called, or 0 for this one */
    const char *reply_err;       /* [L] with reply_ready: no answer comes, for this reason (E_NOPROC, E_NODE_DOWN) */
    bool reply_ready;            /* [L] */
    InMsg reply;                 /* [L] detached until taken */
    uint64_t cpu_ns, msgs_sent, msgs_received;
    /* callbacks from C into this process during a foreign call (proc_callback) */
    uint32_t cb_base;            /* frames below this belong to the code that made the call; 0: none */
    bool reentered;              /* a callback ran during the last native call: cached frame pointers are stale */
    SerialHold *serial_held;     /* serial library locks held by the calls in progress, innermost first */
    VEC(DeferredFinalizer) fin_deferred; /* finalizers of those libraries, waiting for their calls */
    Obj *cb_exception;           /* what a failed callback threw: rethrown when its caller returns */
    VEC(Obj *) pins;             /* objects C may be using during the call: GC roots */
    /* the small-object allocator (gc.c) */
    bool slabs;                  /* small objects come from this process's chunks */
    uint8_t mark_bit;            /* what OBJF_MARK reads as in a live old object: flipped by each major collection */
    unsigned char *bump, *bump_lim, *hole_end; /* the hole being allocated into: next free byte, end of the zeroed part, end */
    unsigned char *bump_base;    /* the start of its chunk's data */
    uint64_t alloc_limit;        /* the fast path allocates while heap_bytes stays below this (heap.h) */
    struct Chunk *bump_chunk;    /* the chunk holding it */
    uint32_t bump_line;          /* the line of bump_chunk to look for the next hole from */
    VEC(struct Chunk *) recycle; /* the chunks with free lines, in the order to allocate into them */
    size_t recycle_i;            /* the next one: those before it were allocated into since the last collection */
    struct Chunk *chunks;
    uint32_t chunk_next;         /* the size of the next chunk */
    uint64_t chunk_bytes;        /* the chunks' total size */
    VEC(Obj *) young_owners, old_owners; /* chunk objects that own memory besides themselves (OBJF_OWNS) */
    uint64_t young_plain_bytes;  /* the bytes of the chunk objects allocated since the last collection, but owners */
    uint64_t gc_allocated0;      /* objects_allocated at the last collection */
    uint64_t old_count;          /* the objects that survived a collection */
    uint64_t gc_live, gc_live_bytes, gc_kept_bytes; /* counted by marking: objects, their bytes, young plain bytes */
    void *buf_free[16];          /* the same for objects' own buffers (a list's storage) */
    struct Chunk *bchunks;
    uint32_t bchunk_next;
    uint64_t rng[4];             /* std.random's generator (util.c) */
    bool rng_seeded;
    /* what the entry function returned (for embedding: mlws_call), a callback or a coroutine */
    Word results[MAX_OPERANDS];
    uint8_t result_types[MAX_OPERANDS];
    uint8_t nresults;
    /* coroutines (coro.c) */
    struct ObjCoro *co;          /* the running coroutine (a GC root), or NULL */
    uint16_t stack_gen;          /* the generation given to the last coroutine stack */
    /* weak references and finalizers (gc.c) */
    VEC(Obj *) weak_refs, weak_maps; /* those a collection has reached, until it ends */
    VEC(Obj *) fin_young, fin_old;   /* gc.finalize records: since the last collection, and before */
    VEC(Obj *) fin_ready;            /* records whose targets were found dead: waiting to run (GC roots) */
    bool fin_running;
    bool fin_held;                   /* gc.hold_finalizers: they wait for gc.run_finalizers */
    /* large strings this heap holds (gc.c): a set, by address */
    struct SharedHold *shared;
    uint32_t shared_cap, shared_n;
    uint64_t shared_bytes;
    uint64_t shared_seen;  /* the major collection marking now, or 0 */
    bool is_frag;          /* a fragment heap, whose objects go to a process with a message (sched.c) */
    struct Caps *caps;     /* what it may reach (caps.c), or NULL: anything */
};

/* vm.c */
VM *vm_new(void);
void vm_free(VM *vm);
void vm_add_libpath(VM *vm, const char *dir);
void vm_register_native(VM *vm, const NativeDef *nd);
void vm_write(VM *vm, const char *s, size_t n);
void vm_flush(VM *vm); /* flush buffered program output (when the VM is idle, and at exit) */
/* A process's program output (io.print*, fs.write to stdout). Whole lines are
 * written at once, so lines of processes running at the same time never mix:
 * the start of a line waits in p->outline until its newline is written, or
 * until proc_write_line_out: when p stops running other than by using up its
 * time slice (it waits, sleeps, makes a blocking call, stops at a breakpoint
 * or ends), calls io.flush, or writes to stderr. A line longer than
 * OUTLINE_MAX goes out in pieces. */
#define OUTLINE_MAX 65536
void proc_write(Process *p, const char *s, size_t n);
void proc_write_line_out(Process *p); /* write out p's unfinished line, if any */

/* options.c: settings by command-line name, without the "--": stack-words,
 * threads, max-frames, slice, gc-min, quota. vm_new applies MLWSVM_<NAME>. */
bool vm_option_known(const char *name);
bool vm_set_option(VM *vm, const char *name, const char *value, MlwsError *err);
/* --cap's and --quota's syntax: capabilities within the VM's (NULL, with err,
 * if malformed), and quotas starting from the VM's. */
struct Caps *vm_parse_caps(VM *vm, const char *value, MlwsError *err);
bool vm_parse_quota(VM *vm, const char *value, uint64_t q[QUOTA__COUNT], MlwsError *err);
void vm_options_from_env(VM *vm);

/* loader.c */
LModule *vm_load_root(VM *vm, const char *path, MlwsError *err);
/* Load several programs' root modules (paths[i] -> out[i]; errs[i] says why
 * one failed). Every program's .requires is stated before any library
 * version is chosen, so the order of the programs does not matter. */
bool vm_load_roots(VM *vm, const char *const *paths, int n, LModule **out, MlwsError *errs);
LModule *vm_load_text(VM *vm, const char *name, const char *text, MlwsError *err); /* source in memory */
Obj *vm_intern(VM *vm, const char *s, size_t n); /* text.intern: the literals' one string of this text; NULL for a NUL in it */
Obj *vm_intern_num(VM *vm, MlwsType t, const char *s, size_t n); /* a BigInt or decimal literal of this value; NULL if malformed */
Function *vm_handle(VM *vm, LModule *lm, const char *fname, MlwsError *err); /* module.handle's function */
void vm_module_hash(VM *vm, LModule *lm, uint8_t out[16]); /* its version's identity across nodes */
LModule *vm_module_version(VM *vm, const char *name, const uint8_t hash[16]); /* that version, if loaded */
Function *vm_module_func(LModule *lm, const char *fname); /* by name, exported or not */
Function *vm_remote_stub(VM *vm, const char *mod, const uint8_t *hash, const char *fname, const uint8_t *params,
                         int np, const uint8_t *rets, int nr, MlwsError *err); /* raises E_LINK_CODE; hash NULL: a handle (E_LINK_UNDEF) */
/* Load a module while the program runs (module.load), as an import of `from`
 * (which directory it is looked for in first, and privacy; NULL: the
 * library path only), under mods_lock. A load that fails leaves the modules
 * it had not linked unusable and their names free. */
LModule *vm_load_dynamic(VM *vm, const char *name, const LModule *from, MlwsError *err);
LModule *vm_module_for_node(VM *vm, const char *name, MlwsError *err); /* loaded, or from a program's directory or the library path */
/* A new version of a loaded module, from its file (text NULL) or from text:
 * calls to it from other modules go to the new version from now on. Code
 * already running, and the old version's calls to itself, stay on the old
 * one. Fails, changing nothing, if a module using it would not link to the
 * new one (docs/ABI.md, "Reloading a module"). */
LModule *vm_reload(VM *vm, const char *name, const char *text, const LModule *from, MlwsError *err);
/* Free the versions of a module that reloads replaced and that nothing can run
 * now: *freed of them; *kept are still in use, or could not be checked. */
bool vm_purge(VM *vm, const char *name, const LModule *from, uint32_t *freed, uint32_t *kept, MlwsError *err);
/* The loaded, linked module that `name` means to `from` (as its import
 * would), or NULL. With from NULL: the library-path module of that name, or
 * else the only program-local one. */
LModule *vm_module_named(VM *vm, const char *name, const LModule *from);
/* modcache.c: a .masm file assembled, or its module from the cache; home:
 * the program directory it was found in (searched first for .include), or NULL */
Module *modcache_assemble(VM *vm, const char *path, const char *default_name, const Home *home, MlwsError *err);
/* m optimized as --opt says: from the cache if it holds this module optimized
 * with these passes, else optimized now (and kept). Returns the module to use;
 * m is freed if it is not that. */
Module *modcache_optimize(VM *vm, Module *m);
/* verify.c */
bool vm_verify_module(VM *vm, LModule *lm, MlwsError *err);
/* Clear every call's whole window from now on (the debugger shows them). */
void vm_whole_windows(VM *vm);

/* process.c */
Process *proc_new(VM *vm);
/* run.c: make main the main process of a new program of the current run */
Program *vm_program_add(VM *vm, Process *main, const char *path, const StrVec *args);
void vm_programs_free(VM *vm); /* (run.c) when the run is over */
/* Give p the global segments of modules loaded since it started (module.load). */
void proc_grow_globals(Process *p);
/* Where instruction pc of f is: "fib.lua:4; fib.masm:120" when a `.loc`
 * says what program line it came from, else "fib.masm:120" (base: file
 * names without their directories). */
void fn_where(StrBuf *b, const Function *f, uint32_t pc, bool base);
/* The .masm file f's lines are lines of: its module's source, or the included
 * file it was defined in. */
const char *fn_file(const Function *f);
/* "util.who", or "util.who [b/util.masm]" when two loaded modules are named util. */
void fn_qualname(StrBuf *b, const Function *f);
void proc_free(Process *p);
/* Raise a runtime fault as a catchable Error exception. */
MLWS_NORETURN void vm_trap(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
/* A debugger's formatter (Process.sandboxed) may not reach other processes:
 * no send, spawn, kill, link or monitor, and no ending the program. */
static inline void proc_reach(Process *p, const char *what) {
    if (__builtin_expect(p->sandboxed, 0)) vm_trap(p, "E_PERMISSION", "%s is not allowed in a debugger's formatter", what);
}
/* Raise any non-null object as an exception. */
MLWS_NORETURN void proc_throw(Process *p, Obj *ex);
/* Run up to `budget` instructions; returns a SLICE_* code. On SLICE_FAILED,
 * p->err describes the failure. */
int proc_run_slice(Process *p, uint32_t budget);
/* Run a closure to completion on top of the current frames: a callback from C
 * during a foreign call. It cannot wait (recv, sleep and waiting natives trap
 * E_CALLBACK inside it), and an exception it does not catch stops at its
 * frame. Returns false and fills p->err if it failed (and p->cb_exception with
 * what it threw, if anything); otherwise its results are in p->results. */
bool proc_callback(Process *p, ObjClosure *cl, const Word *args);
/* Call the finalizers whose objects a collection found dead (gc.finalize), at
 * a safepoint where a callback may run, and say how many ran. */
uint32_t proc_run_finalizers(Process *p);
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

/* coro.c - coroutines: see docs/ABI.md ("Coroutines"). */
ObjCoro *coro_new(Process *p, ObjClosure *fn, int64_t words);
/* Exchange the running context with the one co holds. */
void coro_swap(Process *p, ObjCoro *co);
/* Switch from the running coroutine back to its resumer, as `status` (CO_SUSPENDED
 * or CO_DEAD; a dead coroutine's context is freed). Returns the coroutine. */
ObjCoro *coro_leave(Process *p, int status);
void coro_free_ctx(Process *p, ObjCoro *co); /* free a suspended or dead coroutine's context */
void coro_release(ObjCoro *co);              /* the collector frees co */

/* interp.c */
int interp_run(Process *p, uint32_t budget); /* SLICE_* */
/* Pick a specialised handler for common instruction shapes (after verification). */
void interp_quicken(Insn *in);
void interp_quicken_function(Function *f);
/* The JIT (jit.inc, docs/JIT.md). */
enum { JIT_MODE_OFF, JIT_MODE_ON, JIT_MODE_ALL };
#define JIT_HOT_DEFAULT 1000
void interp_jit_function(VM *vm, Function *f); /* after quickening: compile it, or count towards it */
void interp_jit_forget(VM *vm, Function *f);
void interp_jit_again(VM *vm, Function *f);
void interp_retarget(VM *vm, Insn *in, int k, Function *to); /* operand k, a function, is to now */
void interp_forget_callees(VM *vm, const struct LModule *lm); /* (vm_purge) */
void interp_jit_free(VM *vm);
void interp_jit_report(VM *vm, FILE *out);
bool interp_jit_available(void); /* in this build, on this machine */
const char *interp_jit_arch(void);  /* its machine code's: "arm64", "x86-64", or NULL without one */
uint64_t interp_jit_steps(const VM *vm);
/* Push a frame running closure cl with its arguments bound (proc_callback). */
void interp_enter(Process *p, ObjClosure *cl, const Word *args);

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
/* Across nodes (std.node, dist.c): a control frame from another node, a message
 * for a process here, and a node's connection gone. */
void sched_node_control(Process *p, const ObjList *frame);
Process *sched_frag(VM *vm); /* this thread's scratch heap, emptied, to make a message from a node in */
void sched_node_deliver(Process *p, uint64_t pid, Word v, uint8_t tag); /* v made in sched_frag's heap */
void sched_node_written(Process *p, uint64_t n); /* a connection's carrier wrote n bytes of frames */
void sched_set_carries(VM *vm, uint64_t carrier, uint32_t node); /* (node.attach) */
uint64_t sched_whereis(VM *vm, const char *name); /* the registry: the pid with a name, or 0 */
ObjBytes *node_named_frame(Process *p, Process *f, const char *name, Word v, uint8_t tag); /* dist.c: [12, name, value] */
NodeStats *node_stats_of(VM *vm, uint32_t node); /* dist.c: NULL before its first connection */
void sched_node_down(VM *vm, uint32_t node);
void sched_node_event(VM *vm, uint32_t node, bool up); /* a NodeEvent to each watcher */
void sched_module_event(VM *vm, const char *name);     /* a Reloaded to each watcher of the module */
void sched_set_quota(Process *p, int kind, uint64_t limit);
bool sched_alive(VM *vm, uint64_t pid);
/* The monitor's log channel (vm.c): keep the last cap entries from now on; add
 * one (text of n bytes, cut to MLOG_TEXT_MAX at a character's start). */
#define MLOG_TEXT_MAX 4096
void vm_mlog_enable(VM *vm, uint32_t cap);
void vm_mlog_put(VM *vm, const struct Process *p, int level, const char *name, const char *text, size_t n); /* p NULL: the monitor's */
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
bool msg_copy_soft(Process *from, Process *to, Word *v, uint8_t tag, char *why, size_t n); /* for tools: no trap */
void copy_state_free(void); /* the calling thread's copy scratch */
void node_wire_free(void); /* the calling thread's scratch for values to and from other nodes (dist.c) */
/* Frames for the wire (dist.c), made in f by the sending thread: a message
 * [0, pid, value] and a control frame [kind, w...]; what cannot be sent is p's error. */
ObjBytes *node_message_frame(Process *p, Process *f, uint64_t pid, Word v, uint8_t tag);
ObjBytes *node_control_frame(Process *p, Process *f, int kind, int n, const Word *w, const uint8_t *t);

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
/* caps.c: capabilities. A set is shared by the processes that inherit it, and
 * never changes; NULL: not restricted. */
enum { CAP_READ, CAP_WRITE, CAP_CONNECT, CAP_LISTEN, CAP_RUN, CAP_ENV, CAP__COUNT };
typedef struct Caps {
    uint32_t refs;
    StrVec allow[CAP__COUNT];
} Caps;
int cap_kind(const char *name, size_t n);
Caps *caps_new(void);
void caps_grant(Caps *c, const Caps *within, int kind, const char *entry);
void caps_retain(Caps *c);
void caps_release(Caps *c);
bool caps_allow_path(const Caps *c, int kind, const char *path);
bool caps_allow_net(const Caps *c, int kind, const char *host, int64_t port); /* port -1: any */
bool caps_allow_name(const Caps *c, int kind, const char *name);
void natives_register_caps(VM *vm);
/* dist.c: a pid's top 16 bits name its node (0: this one, by a plain pid). */
#define NODE_SHIFT 48
#define PID_LOCAL ((((uint64_t)1) << NODE_SHIFT) - 1) /* a pid's part that names its process on its node */
/* A pid that stands for whichever process has a registered name on its node
 * when a message arrives there (node.name_pid): its low bits number the name
 * in VM.pid_names. No process has this bit. */
#define PID_NAMED (((uint64_t)1) << 47)
#define PID_NAME_MAX 255
void natives_register_node(VM *vm);
const NativeDef *natives_node_table(size_t *n);
const NativeDef *natives_caps_table(size_t *n);
void natives_register_modules(VM *vm); /* modload.c */
const NativeDef *natives_modules_table(size_t *n);
/* text.c: std.text and std.json */
void natives_register_text(VM *vm);
void text_json_append(Process *p, Word v, uint8_t tag, StrBuf *out); /* E_JSON for a value with no JSON form */
bool text_utf8_valid(const char *s, size_t n);
bool text_json_parse(Process *heap, const char *s, size_t n, Word *v, uint8_t *tag, char *err, size_t errlen);
void natives_register_util(VM *vm);
const NativeDef *natives_util_table(size_t *n);
const NativeDef *natives_text_table(size_t *n);
/* sched.c: os.exit: end the whole program now with this exit code. */
void sched_request_exit(VM *vm, int64_t code);
/* Send a message from outside the VM (any thread): a number, or a string if
 * str is not NULL. False if the process does not exist or has finished. */
bool sched_post(VM *vm, uint64_t pid, Word v, uint8_t tag, const char *str, size_t len);
/* Post a message built by `build` on a scratch heap, from any thread. */
bool sched_post_build(VM *vm, uint64_t pid, Word (*build)(Process *heap, void *ctx), void *ctx);
bool sched_post_json(VM *vm, uint64_t pid, const char *json, size_t n, char *err, size_t errlen);
bool sched_kill_from_outside(VM *vm, uint64_t pid, const char *code, const char *msg);
bool vm_purge_kill(VM *vm, const char *name, const struct LModule *from, uint64_t self, uint32_t *freed, uint32_t *kept,
                   uint32_t *killed, MlwsError *err);
/* Inside a foreign call: fail it (raised when the call returns). */
void native_fail(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
/* Run a foreign handle's finalizer (p: the process whose handle it is, or NULL
 * when it is being freed outside a run). */
void foreign_finalize(Process *p, Obj *o);
void foreign_run_deferred(Process *p, struct PlatLock *lock); /* lock NULL: all */
ObjForeign *foreign_new_obj(Process *p, const void *type, void *ptr, ExtLib *lib);
bool ext_run_init(VM *vm, ExtLib *lib, void *init, MlwsError *err);


/* extensions.c: loading native code, with the trust checks. */
bool ext_load(VM *vm, const char *name, MlwsError *err);           /* `.extension "name"` */
ExtLib *ext_ffi_library(VM *vm, const char *spec, MlwsError *err); /* `.extern` libraries */
void ext_free_all(VM *vm);
/* ffi.c: an `.extern` declaration as a native (false if built without libffi). */
struct MExtern;
struct MCStruct;
struct MFfiFree;
struct LModule;
bool ffi_define(VM *vm, const struct MExtern *x, MlwsError *err);
bool ffi_define_struct(VM *vm, struct LModule *lm, const struct MCStruct *c, MlwsError *err); /* `.cstruct` */
bool ffi_define_free(VM *vm, const struct MFfiFree *f, MlwsError *err);                        /* `.ffi_free` */
struct MCallback;
bool ffi_define_callback(VM *vm, const struct MCallback *cb, MlwsError *err);                  /* `.callback` */
void ffi_free_callback(void *v);
void n_ffi_async_callback(Process *p, Word *args, Word *rets); /* the ffi.async_callback native */
void n_ffi_sizeof(Process *p, Word *args, Word *rets);         /* ffi.sizeof */
void ffi_free_data(void *data);   /* an `.extern` native's call data */
void ffi_free_struct(void *v);    /* a `.cstruct` layout */
bool ffi_available(void);

#endif
