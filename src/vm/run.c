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

/* A finished program's result, from its main process. */
static void program_result(VM *vm, Process *p, RunResult *res, uint64_t wall_ns) {
    res->wall_ns = wall_ns;
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
    res->heap_peak = gc_heap_peak(p);
    res->heap_live = p->heap_bytes;
    res->gc_count = p->gc_count;
    res->gc_majors = p->gc_majors;
    res->gc_ns = p->gc_ns;
    res->objects_allocated = p->objects_allocated;
    res->objects_freed = p->gc_freed_objects;
    gc_chunk_usage(p, &res->chunk_bytes, &res->chunk_lines, &res->chunk_lines_used);
    if (vm->exit_requested) { /* os.exit, from any process: the whole run ends with its code */
        res->status = RUN_OK;
        res->exit_code = vm->exit_code;
    } else if (p->state == PROC_DONE) {
        res->status = RUN_OK;
        res->exit_code = p->exit_code;
        res->nresults = p->nresults;
        for (int k = 0; k < p->nresults; k++) keep_result(p, res, k);
    } else {
        res->status = RUN_TRAP;
        snprintf(res->err.code, sizeof res->err.code, "%s", p->err.code);
        snprintf(res->err.msg, sizeof res->err.msg, "%s", p->err.msg);
        res->backtrace = xstrdup(p->backtrace.s ? p->backtrace.s : "");
    }
}

Program *vm_program_add(VM *vm, Process *main, const char *path, const StrVec *args) {
    Program *pr = xcalloc(1, sizeof *pr);
    pr->path = xstrdup(path ? path : "");
    for (size_t i = 0; args && i < args->n; i++) VEC_PUSH(pr->args, xstrdup(args->v[i]));
    pr->main = main;
    main->prog = pr;
    VEC_PUSH(vm->progs, pr);
    atomic_fetch_add(&vm->progs_live, 1);
    return pr;
}

void vm_programs_free(VM *vm) {
    for (size_t i = 0; i < vm->progs.n; i++) {
        Program *pr = vm->progs.v[i];
        for (size_t k = 0; k < pr->args.n; k++) free(pr->args.v[k]);
        VEC_FREE(pr->args);
        free(pr->path);
        free(pr);
    }
    vm->progs.n = 0;
    atomic_store(&vm->progs_live, 0);
}

/* Run the programs whose main processes are mains[0..n) (each made and
 * started by the caller), to the end of the last; fill res[0..n). */
static bool run_programs(VM *vm, Process **mains, int n, RunResult *res) {
    uint64_t t0 = plat_now_ns();
    while (sched_run(vm, mains[0])) {} /* a `break` with tools active stops; just resume */
    uint64_t wall = plat_now_ns() - t0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        program_result(vm, mains[i], &res[i], wall);
        ok = ok && res[i].status == RUN_OK;
    }
    if (vm->hold && vm->poll) { /* keep serving the monitor's final picture until interrupted */
        fprintf(stderr, "mlwsvm: main has finished; still serving the monitor (Ctrl-C to quit)\n");
        for (;;) vm->poll(vm, 200000000);
    }
    sched_shutdown(vm, NULL); /* the run ends with the last program; stop everything else */
    uint64_t total = vm->insns_retired;
    for (int i = 0; i < n; i++) total += mains[i]->insns;
    for (int i = 0; i < n; i++) {
        res[i].total_insns = total;
        proc_free(mains[i]);
    }
    vm_programs_free(vm);
    return ok;
}

Program *vm_start_program(VM *vm, const char *path, const StrVec *args, const char *cap, const char *quota,
                          MlwsError *err) {
    Caps *caps = NULL;
    uint64_t q[QUOTA__COUNT];
    if (cap && *cap && !(caps = vm_parse_caps(vm, cap, err))) return NULL;
    if (!vm_parse_quota(vm, quota ? quota : "", q, err)) {
        if (caps) caps_release(caps);
        return NULL;
    }
    /* The same file again is another main of its module, as with --also: the
     * module's current version, if that file's is loaded. */
    LModule *root = NULL;
    plat_lock(vm->mods_lock);
    for (size_t i = 0; i < vm->mods.n && !root; i++) {
        LModule *lm = vm->mods.v[i];
        if (lm->state == LM_READY && !lm->replaced && lm->path && !strcmp(lm->path, path)) root = lm;
    }
    plat_unlock(vm->mods_lock);
    if (!root) root = vm_load_root(vm, path, err);
    Function *fn = root ? vm_find_main(root, err) : NULL;
    if (!fn) {
        if (caps) caps_release(caps);
        return NULL;
    }
    Process *p = proc_new(vm);
    if (caps) { /* (as --cap: no foreign code for a program kept within bounds) */
        caps_release(p->caps);
        p->caps = caps;
        q[QUOTA_FOREIGN] = 0;
    }
    memcpy(p->quota, q, sizeof q);
    plat_lock(vm->lock); /* (vm->progs: the monitor reads it) */
    Program *pr = vm_program_add(vm, p, path, args);
    plat_unlock(vm->lock);
    proc_start(p, fn, NULL, NULL, 0); /* (and in the table, ready) */
    return pr;
}

/* The program mlwsvm serve runs so that its run never ends of itself: a main
 * that sleeps. */
static const char SERVE_KEEPER[] = ".func main\n1:  sleep 3600000\n    jmp 1b\n.endf\n";

void vm_serve(VM *vm) {
    MlwsError err = {{0}, {0}};
    LModule *root = vm_load_text(vm, "(serve)", SERVE_KEEPER, &err);
    Function *fn = root ? vm_find_main(root, &err) : NULL;
    if (!fn) { /* (never: the text is ours) */
        fprintf(stderr, "mlwsvm serve: %s\n", err.msg);
        return;
    }
    vm->exit_requested = false;
    Process *keeper = proc_new(vm);
    vm->main_pid = keeper->pid;
    vm_program_add(vm, keeper, "(serve)", NULL);
    proc_start(keeper, fn, NULL, NULL, 0);
    while (sched_run(vm, keeper)) {}
    sched_shutdown(vm, NULL);
    proc_free(keeper);
    for (size_t i = 1; i < vm->progs.n; i++) /* (each program's main was kept for its results) */
        if (vm->progs.v[i]->main) proc_free(vm->progs.v[i]->main);
    vm_programs_free(vm);
}

void vm_serve_stop(VM *vm) {
    plat_lock(vm->lock);
    size_t n = vm->progs.n;
    uint64_t *pids = xcalloc(n + 1, sizeof *pids);
    for (size_t i = 0; i < n; i++) pids[i] = vm->progs.v[i]->main->pid;
    plat_unlock(vm->lock);
    for (size_t i = n; i-- > 0;) sched_kill_from_outside(vm, pids[i], "E_KILLED", "mlwsvm serve was stopped");
    free(pids);
}

bool vm_run_function(VM *vm, Function *fn, Word *args, int nargs, RunResult *res, const RunHooks *hooks) {
    MlwsError keep = res->err; /* (a load error already reported stays) */
    memset(res, 0, sizeof *res);
    res->err = keep;
    vm->exit_requested = false;
    Process *p = proc_new(vm);
    vm->main_pid = p->pid;
    vm_program_add(vm, p, fn->mod->path, &vm->prog_args);
    if (hooks && hooks->prepare) hooks->prepare(hooks->ctx, p, args);
    proc_start(p, fn, NULL, args, nargs);
    if (hooks && hooks->started) hooks->started(hooks->ctx, p->pid); /* in the table: messages reach it */
    return run_programs(vm, &p, 1, res);
}

bool vm_run_files(VM *vm, const ProgSpec *specs, int n, RunResult *res) {
    memset(res, 0, (size_t)n * sizeof *res);
    vm->exit_requested = false;
    /* Load every program first, together (so that library versions are
     * chosen for all of them): a load error in any is reported before
     * anything runs. The same file again is another main of the same module. */
    const char **paths = xcalloc((size_t)n + 1, sizeof *paths);
    int *first = xcalloc((size_t)n + 1, sizeof *first); /* the spec that first named each file */
    int *which = xcalloc((size_t)n + 1, sizeof *which); /* each spec's file */
    int nfiles = 0;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < nfiles && strcmp(paths[j], specs[i].path)) j++;
        if (j == nfiles) paths[nfiles] = specs[i].path, first[nfiles++] = i;
        which[i] = j;
    }
    LModule **roots = xcalloc((size_t)nfiles + 1, sizeof *roots);
    MlwsError *errs = xcalloc((size_t)nfiles + 1, sizeof *errs);
    Function **mains_of = xcalloc((size_t)nfiles + 1, sizeof *mains_of);
    bool loaded = vm_load_roots(vm, paths, nfiles, roots, errs);
    for (int j = 0; j < nfiles && loaded; j++) loaded = (mains_of[j] = vm_find_main(roots[j], &errs[j])) != NULL;
    Function **fns = xcalloc((size_t)n + 1, sizeof *fns);
    for (int i = 0; i < n; i++) fns[i] = mains_of[which[i]];
    for (int j = 0; j < nfiles && !loaded; j++) {
        if (!errs[j].code[0]) continue;
        res[first[j]].status = RUN_LOAD_ERROR;
        res[first[j]].err = errs[j];
    }
    free(paths);
    free(first);
    free(which);
    free(roots);
    free(errs);
    free(mains_of);
    if (!loaded) {
        free(fns);
        return false;
    }
    Process **mains = xcalloc((size_t)n + 1, sizeof *mains);
    for (int i = 0; i < n; i++) {
        StrVec args = {0};
        for (int k = 0; k < specs[i].argc; k++) VEC_PUSH(args, specs[i].argv[k]);
        mains[i] = proc_new(vm);
        vm_program_add(vm, mains[i], specs[i].path, &args);
        VEC_FREE(args);
    }
    vm->main_pid = mains[0]->pid;
    for (int i = 0; i < n; i++) proc_start(mains[i], fns[i], NULL, NULL, 0);
    bool ok = run_programs(vm, mains, n, res);
    free(mains);
    free(fns);
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
