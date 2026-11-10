/* tools_internal.h - state shared by the runtime tools: trace, profiler,
 * debugger and monitor. A single instruction hook (hooks.c) dispatches to
 * whichever are active. */
#ifndef MLWSVM_TOOLS_INTERNAL_H
#define MLWSVM_TOOLS_INTERNAL_H

#include "../vm/vm.h"

struct Debugger;
struct Monitor;

typedef struct Tools {
    /* --trace */
    FILE *trace;
    uint64_t trace_limit, traced;
    /* --profile */
    bool profile;
    uint64_t op_counts[OP__COUNT];
    uint64_t op_fast[OP__COUNT]; /* of those, run as a quickened form (interp.c) */
    VEC(Function *) prof_funcs; /* functions with a prof[] array */
    StrMap stacks;              /* collapsed stack -> sample count */
    uint64_t sample_every, sample_tick, samples;
    /* debugger, monitor */
    struct Debugger *dbg;
    struct Monitor *mon;
    struct PDebug *pdbg; /* the monitor's debugger (pdebug.c): its breakpoints */
} Tools;

/* The monitor's keys (keys.c): a token kept as its SHA-256, with a scope. */
enum { SCOPE_NONE, SCOPE_READ, SCOPE_ADMIN };
typedef struct {
    char label[64];
    uint8_t hash[32];
    int scope;
    char from;        /* 's' printed at the start, 'f' the tokens file, 'a' made through the API with no file */
    uint64_t used_ns; /* when last used (plat_now_ns); 0: never */
} MonKey;
typedef VEC(MonKey) MonKeyVec;
const char *key_scope_name(int scope);
int key_scope_of(const char *name); /* SCOPE_NONE for neither "read" nor "admin" */
bool key_label_ok(const char *s);
void key_hash(const char *token, size_t n, uint8_t out[32]);
void key_random(char out[33]);
/* A tokens file's keys (from 'f'): false with why. A file not there has none. */
bool keyfile_read(const char *path, MonKeyVec *out, char *err, size_t errlen);
/* Add a key's line, or remove the line of a label: the file is written whole,
 * beside it, and renamed over it. */
bool keyfile_add(const char *path, int scope, const char *label, const uint8_t hash[32], char *err, size_t errlen);
bool keyfile_remove(const char *path, const char *label, char *err, size_t errlen);

Tools *tools_of(VM *vm);       /* creates on first use */
void tools_enable_hook(VM *vm); /* install the per-instruction hook */
void tools_free(VM *vm);
void tools_monitor_close(VM *vm);
bool debugger_hook(struct Debugger *d, Process *p, uint32_t pc);

/* The debugger's engine, under `mlwsvm debug` (debugger.c) and `mlwsvm dap`
 * (dap.c). */
enum { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT };
#define DBG_POLL_EVERY 65536 /* instructions between calls of Debugger.poll */

typedef struct {
    char *spec;
    Function *fn;
    uint32_t pc;
    uint64_t hits;
    bool live;
} Breakpoint;

typedef struct Debugger {
    VM *vm;
    Process *main;
    Function *main_fn;
    LModule *root;
    VEC(Breakpoint) bps;
    int step;
    uint64_t step_pid;
    uint32_t step_depth;
    /* After a stop, the stopped instruction must run once before it can stop again. */
    bool skip;
    uint64_t skip_pid;
    Function *skip_fn;
    uint32_t skip_pc;
    int hit;            /* breakpoint that caused the stop, or -1 */
    uint64_t sel;       /* pid of the process being inspected */
    bool started, ended;
    OutputFn out;
    void *out_ctx;
    /* Asked now and then while the program runs: true stops it (paused). */
    bool (*poll)(void *ctx);
    void *poll_ctx;
    uint32_t poll_tick;
    bool paused;
} Debugger;

void dbg_init(Debugger *d, VM *vm, LModule *root, Function *main_fn, OutputFn out, void *out_ctx);
void dbg_finish(Debugger *d);
bool dbg_go(Debugger *d, int step);
Process *dbg_selected(Debugger *d);
void dbg_add_bp(Debugger *d, const char *spec, Function *f, uint32_t pc);
bool dbg_break_line(Debugger *d, const char *spec, const char *file, uint32_t line);
void dbg_clear_spec(Debugger *d, const char *spec);
/* The named variable (`.local`, a parameter or an alias) live at pc, or NULL.
 * dbg_var_reg reads register k of it in a frame of p; dbg_show_var shows it:
 * by its module's formatter for its registers (`.debug_format`), run in a
 * sandboxed process of its own, else its value, or each of its registers. */
const MVar *dbg_var_at(const Function *f, uint32_t pc, const char *name);
void dbg_var_reg(const Process *p, const Frame *fr, const MVar *v, int k, Word *w, MlwsType *t);
void dbg_show_var(Process *p, const Frame *fr, const MVar *v, int depth, StrBuf *out);
void dbg_var_regs(StrBuf *b, const MVar *v); /* "I3, F3, R3" */

/* The monitor's debugger (pdebug.c): a process debugged alone, /api/debug.
 * Requests are answered with the world stopped, as JSON into out, with the
 * status returned; path is what follows /api/debug. An action that the log
 * should have gives its event's name and value. */
void pdebug_enable(VM *vm);
void pdebug_free(VM *vm);
void pdebug_refresh(VM *vm); /* the breakpoints placed again, if the modules have changed */
int pdebug_get(VM *vm, const char *path, const char *query, StrBuf *out);
int pdebug_post(VM *vm, const char *path, const char *body, size_t n, StrBuf *out, const char **event, StrBuf *audit);
/* (monitor.c) JSON for it: a string; n bytes cut at max; a flat object's
 * named values as text (false: not one, with nothing kept); a URL's escapes. */
void tools_json_str(StrBuf *b, const char *s);
void tools_json_strn(StrBuf *b, const char *s, size_t n, size_t max);
bool tools_json_flat(const char *body, size_t n, const char *const *names, char **vals, size_t nnames);
void tools_url_decode(char *s);

/* Formatting shared by the debugger and the monitor. */
void fmt_location(StrBuf *b, const Function *f, uint32_t pc);
void fmt_value(StrBuf *b, Word v, MlwsType t, int depth);

#endif
