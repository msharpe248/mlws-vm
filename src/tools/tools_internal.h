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
} Tools;

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

/* Formatting shared by the debugger and the monitor. */
void fmt_location(StrBuf *b, const Function *f, uint32_t pc);
void fmt_value(StrBuf *b, Word v, MlwsType t, int depth);

#endif
