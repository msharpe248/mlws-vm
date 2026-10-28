/* run.h - load a program and run its `main` in a fresh process. */
#ifndef MLWSVM_RUN_H
#define MLWSVM_RUN_H

#include "vm.h"

enum { RUN_OK = 0, RUN_LOAD_ERROR = 1, RUN_TRAP = 2 };

typedef struct {
    int status;
    int64_t exit_code;
    MlwsError err;
    char *backtrace; /* malloc'd, RUN_TRAP only */
    /* statistics */
    uint64_t insns, wall_ns;
    uint64_t raw_peak, raw_allocs, raw_frees, raw_leaked;
    uint64_t stack_peak_bytes;
    uint32_t max_depth;
    uint64_t heap_peak, heap_live, gc_count, gc_ns, objects_allocated, objects_freed;
    uint64_t spawned, max_live, slices, total_insns; /* all processes */
    /* the function's results; a reference comes back as text (malloc'd): a
     * string or byte buffer as itself, anything else as `tostr` shows it */
    int nresults;
    uint8_t result_types[NATIVE_MAX_RETS];
    Word results[NATIVE_MAX_RETS];
    char *result_text[NATIVE_MAX_RETS];
    size_t result_len[NATIVE_MAX_RETS];
} RunResult;

bool vm_run_file(VM *vm, const char *path, RunResult *res);
/* Several programs in one VM (`mlwsvm run a.masm --also b.masm`): each one's
 * main runs as its own process, with its own os.args; a program ends with its
 * main (its other processes are stopped), and the run with the last one.
 * res[i] is program i's result. False if any did not finish normally. */
typedef struct {
    const char *path;
    int argc;
    char *const *argv;
} ProgSpec;
bool vm_run_files(VM *vm, const ProgSpec *specs, int n, RunResult *res);
/* Optional steps around vm_run_function: `prepare` runs once the process
 * exists, before it starts (to make reference arguments in its heap);
 * `started` is told its pid once it is in the process table. */
typedef struct {
    void (*prepare)(void *ctx, Process *p, Word *args);
    void (*started)(void *ctx, uint64_t pid);
    void *ctx;
} RunHooks;

/* Run a function as the main process (the program ends when it returns). */
bool vm_run_function(VM *vm, Function *fn, Word *args, int nargs, RunResult *res, const RunHooks *hooks);
Function *vm_find_main(LModule *root, MlwsError *err);
void run_result_free(RunResult *res);

#endif
