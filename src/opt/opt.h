/* opt.h - the optimizer: rewrites a module's code, before it is linked, to do
 * the same in fewer instructions. It runs on the Module the assembler makes
 * (or a .mlwso holds), so `mlwsvm opt` can write the result to a file and the
 * loader can optimize every module it loads (`--opt`). Each pass can be turned
 * on or off by name. See docs/OPTIMIZER.md. */
#ifndef MLWSVM_OPT_H
#define MLWSVM_OPT_H

#include <stdio.h>

#include "../core/module.h"

/* The passes: name, the -O level that turns it on, and what the report calls
 * one change. Their order here is the order the report lists them in.
 * -O1 changes nothing a program can see but how many instructions it runs.
 * -O2 adds what only the tools see (the debugger shows fewer registers and
 * functions). -O3 adds what removes frames, which a program can see in a
 * backtrace (Error.trace): a language that reads them should stop at -O2. */
#define OPT_PASSES(X)                                                               \
    X(FOLD, "fold", 1, "constant expressions folded")                                \
    X(SIMPLIFY, "simplify", 1, "instructions simplified")                            \
    X(CONSTPROP, "constprop", 1, "register reads replaced by constants")             \
    X(CSE, "cse", 1, "computations and loads reused")                                \
    X(COPYPROP, "copyprop", 1, "register reads replaced by the register copied")     \
    X(DCE, "dce", 1, "dead instructions removed")                                    \
    X(BRANCHES, "branches", 1, "jumps threaded, removed, inverted or decided")       \
    X(UNREACHABLE, "unreachable", 1, "unreachable instructions removed")             \
    X(PEEPHOLE, "peephole", 1, "redundant moves and nops removed")                   \
    X(DEADFUNCS, "deadfuncs", 2, "unused functions removed")                         \
    X(COMPACT, "compact", 2, "register windows made smaller")                        \
    X(INLINE, "inline", 3, "calls inlined")                                          \
    X(TAILCALL, "tailcall", 3, "calls turned into tail calls")

typedef enum {
#define X(e, name, level, doc) PASS_##e,
    OPT_PASSES(X)
#undef X
    PASS__COUNT
} OptPass;

/* Not a pass: decode and re-encode every function, changing nothing (the
 * spec word "encode"; tests the optimizer's encoder). */
#define OPT_ENCODE_ONLY (1u << 31)

typedef struct {
    uint32_t passes; /* a bit per OptPass (and OPT_ENCODE_ONLY); 0: the optimizer is off */
} OptConfig;

const char *opt_pass_name(int pass);
const char *opt_pass_doc(int pass);
int opt_pass_level(int pass);
/* A pass list: comma-separated levels and names, applied in order: "2",
 * "3,-inline", "fold,dce", "none". */
bool opt_parse(const char *spec, OptConfig *cfg, char *err, size_t errlen);

/* What the optimizer did to one function. */
typedef struct {
    char *func;
    uint32_t before, after;             /* instructions */
    uint32_t regs_before, regs_after;   /* registers in its window */
} OptFuncStat;

/* What it did to one module. */
typedef struct {
    char *name, *source;
    uint64_t counts[PASS__COUNT];
    VEC(OptFuncStat) funcs;
    uint32_t funcs_removed;
} OptModStat;

typedef struct OptReport {
    bool log;            /* keep a change log (one line per change) */
    VEC(OptModStat) mods;
    StrVec changes;
    uint64_t ns;         /* time spent optimizing */
} OptReport;

/* Optimize m in place with the passes cfg turns on; rep may be NULL. */
void opt_module(Module *m, const OptConfig *cfg, OptReport *rep);
void opt_report_print(const OptReport *rep, FILE *f);
void opt_report_free(OptReport *rep);

#endif
