/* run.c - see run.h */
#include "run.h"

#include "../platform/platform.h"

bool vm_run_file(VM *vm, const char *path, RunResult *res) {
    memset(res, 0, sizeof *res);
    LModule *root = vm_load_root(vm, path, &res->err);
    if (!root) {
        res->status = RUN_LOAD_ERROR;
        return false;
    }
    Function *main_fn = NULL;
    for (uint32_t i = 0; i < root->m->nfuncs; i++) {
        Function *f = &root->funcs[i];
        if (f->target == f && !strcmp(f->name, "main")) main_fn = f;
    }
    if (!main_fn) {
        mlws_err(&res->err, "E_LINK_NO_MAIN", "%s: no function named main", path);
        res->status = RUN_LOAD_ERROR;
        return false;
    }
    if (main_fn->mf->nparams != 0) {
        mlws_err(&res->err, "E_LINK_NO_MAIN", "%s: main must not take parameters", path);
        res->status = RUN_LOAD_ERROR;
        return false;
    }
    Process *p = proc_new(vm);
    uint64_t t0 = plat_now_ns();
    bool ok = proc_run_main(p, main_fn);
    res->wall_ns = plat_now_ns() - t0;
    res->insns = p->insns;
    res->raw_peak = p->raw_peak;
    res->raw_allocs = p->raw_allocs;
    res->raw_frees = p->raw_frees;
    res->raw_leaked = p->raw_bytes;
    res->stack_peak_bytes = p->stack.size - p->stack_low_off;
    res->max_depth = p->max_depth;
    if (ok) {
        res->status = RUN_OK;
        res->exit_code = p->exit_code;
    } else {
        res->status = RUN_TRAP;
        res->err = p->err;
        res->backtrace = xstrdup(p->backtrace.s ? p->backtrace.s : "");
    }
    proc_free(p);
    return ok;
}

void run_result_free(RunResult *res) {
    free(res->backtrace);
    res->backtrace = NULL;
}
