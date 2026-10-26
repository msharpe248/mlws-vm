/* run.c - see run.h */
#include "run.h"

#include "../platform/platform.h"

#include "../heap/heap.h"

bool vm_run_file(VM *vm, const char *path, RunResult *res) {
    memset(res, 0, sizeof *res);
    LModule *root = vm_load_root(vm, path, &res->err);
    if (!root) {
        res->status = RUN_LOAD_ERROR;
        return false;
    }
    Function *main_fn = vm_find_main(root, &res->err);
    if (!main_fn) {
        res->status = RUN_LOAD_ERROR;
        return false;
    }
    return vm_run_function(vm, main_fn, NULL, 0, res, NULL);
}

/* Copy a result out of the process before it is freed. */
static void keep_result(Process *p, RunResult *res, int k) {
    Word w = p->results[k];
    uint8_t t = p->result_types[k];
    res->results[k] = w;
    res->result_types[k] = t;
    if (t != T_R && t != T_BIG && t != T_DEC) return;
    Obj *o = w.r;
    res->results[k].r = NULL;
    if (!o) return;
    if (o->kind == OBJ_STRING || o->kind == OBJ_BYTES) {
        const char *s = o->kind == OBJ_STRING ? ((ObjString *)o)->data : (const char *)((ObjBytes *)o)->data;
        size_t n = o->kind == OBJ_STRING ? ((ObjString *)o)->len : ((ObjBytes *)o)->len;
        res->result_text[k] = xstrndup(s, n);
        res->result_len[k] = n;
        return;
    }
    ObjString *s = value_to_string(p, w, T_R);
    res->result_text[k] = xstrndup(s->data, s->len);
    res->result_len[k] = s->len;
}

bool vm_run_function(VM *vm, Function *fn, Word *args, int nargs, RunResult *res, const RunHooks *hooks) {
    MlwsError keep = res->err; /* (a load error already reported stays) */
    memset(res, 0, sizeof *res);
    res->err = keep;
    vm->exit_requested = false;
    Process *p = proc_new(vm);
    vm->main_pid = p->pid;
    if (hooks && hooks->prepare) hooks->prepare(hooks->ctx, p, args);
    proc_start(p, fn, NULL, args, nargs);
    if (hooks && hooks->started) hooks->started(hooks->ctx, p->pid); /* in the table: messages reach it */
    uint64_t t0 = plat_now_ns();
    while (sched_run(vm, p)) {} /* a `break` with tools active stops; just resume */
    bool ok = p->state == PROC_DONE;
    res->wall_ns = plat_now_ns() - t0;
    res->insns = p->insns;
    res->spawned = vm->spawned;
    res->max_live = vm->max_live;
    res->slices = vm->slices;
    res->raw_peak = p->raw_peak;
    res->raw_allocs = p->raw_allocs;
    res->raw_frees = p->raw_frees;
    res->raw_leaked = p->raw_bytes;
    res->stack_peak_bytes = p->stack.size - p->stack_low_off;
    res->max_depth = p->max_depth;
    res->heap_peak = p->heap_peak;
    res->heap_live = p->heap_bytes;
    res->gc_count = p->gc_count;
    res->gc_ns = p->gc_ns;
    res->objects_allocated = p->objects_allocated;
    res->objects_freed = p->gc_freed_objects;
    if (vm->exit_requested) { /* os.exit, from any process */
        ok = true;
        res->status = RUN_OK;
        res->exit_code = vm->exit_code;
    } else if (ok) {
        res->status = RUN_OK;
        res->exit_code = p->exit_code;
        res->nresults = p->nresults;
        for (int k = 0; k < p->nresults; k++) keep_result(p, res, k);
    } else {
        res->status = RUN_TRAP;
        res->err = p->err;
        res->backtrace = xstrdup(p->backtrace.s ? p->backtrace.s : "");
    }
    if (vm->hold && vm->poll) { /* keep serving the monitor's final picture until interrupted */
        fprintf(stderr, "mlwsvm: main has finished; still serving the monitor (Ctrl-C to quit)\n");
        for (;;) vm->poll(vm, 200000000);
    }
    sched_shutdown(vm, p); /* the program ends with main; stop everything else */
    res->total_insns = vm->insns_retired + p->insns;
    proc_free(p);
    return ok;
}

Function *vm_find_main(LModule *root, MlwsError *err) {
    Function *main_fn = NULL;
    for (uint32_t i = 0; i < root->m->nfuncs; i++) {
        Function *f = &root->funcs[i];
        if (f->target == f && !strcmp(f->name, "main")) main_fn = f;
    }
    if (!main_fn) {
        mlws_err(err, "E_LINK_NO_MAIN", "%s: no function named main", root->path);
        return NULL;
    }
    if (main_fn->mf->nparams != 0) {
        mlws_err(err, "E_LINK_NO_MAIN", "%s: main must not take parameters", root->path);
        return NULL;
    }
    return main_fn;
}

void run_result_free(RunResult *res) {
    free(res->backtrace);
    res->backtrace = NULL;
    for (int k = 0; k < NATIVE_MAX_RETS; k++) {
        free(res->result_text[k]);
        res->result_text[k] = NULL;
    }
}
