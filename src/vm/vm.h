/* vm.h - runtime structures: loaded modules, functions, processes, memory
 * segments, natives. See docs/ABI.md for the execution model. */
#ifndef MLWSVM_VM_H
#define MLWSVM_VM_H

#include <setjmp.h>

#include "../core/insn.h"
#include "../core/module.h"

/* ---- heap objects (minimal until the GC milestone) ------------------------ */

enum { OBJ_STRING = 1 };
#define OBJF_IMMORTAL 0x01

typedef struct Obj {
    uint8_t kind;
    uint8_t flags;
    uint16_t pad;
    uint32_t size;
    struct Obj *next;
} Obj;

typedef struct {
    Obj h;
    uint32_t len;
    uint32_t hash;
    char data[];
} ObjString;

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

typedef struct LModule {
    Module *m;
    char *path;
    int index;
    int state;
    Function *funcs;          /* m->nfuncs */
    ObjString **consts;       /* m->nconsts */
    uint64_t *grefs;          /* resolved global pointers, m->ngrefs */
    const NativeDef **natives; /* m->nnatives */
} LModule;

typedef void (*OutputFn)(void *ctx, const char *s, size_t n);

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

enum { PROC_READY = 0, PROC_RUNNING, PROC_DONE, PROC_FAILED };

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
MLWS_NORETURN void vm_trap(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
/* Run `fn` (no arguments) to completion. Returns false on a trap (see p->err). */
bool proc_run_main(Process *p, Function *fn);
Segment *proc_segment(Process *p, uint64_t ptr);
uint64_t proc_alloc(Process *p, int64_t size);
void proc_free_block(Process *p, uint64_t ptr);
uint64_t proc_realloc(Process *p, uint64_t ptr, int64_t size);

/* interp.c */
void interp_run(Process *p);

/* natives.c */
void natives_register_builtin(VM *vm);
const NativeDef *natives_builtin_table(size_t *n);

#endif
