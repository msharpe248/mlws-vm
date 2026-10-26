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

/* Formatting shared by the debugger and the monitor. */
void fmt_location(StrBuf *b, const Function *f, uint32_t pc);
void fmt_value(StrBuf *b, Word v, MlwsType t, int depth);

#endif
