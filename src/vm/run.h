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
} RunResult;

bool vm_run_file(VM *vm, const char *path, RunResult *res);
void run_result_free(RunResult *res);

#endif
