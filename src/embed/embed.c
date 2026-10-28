/* embed.c - the embedding API (include/mlwsvm/embed.h): MLWSVM as a library
 * inside another program. A thin layer over the VM, its loader and run.c. */
#include "../../include/mlwsvm/embed.h"
#include "../heap/heap.h"
#include "../native/extapi.h"
#include "../platform/platform.h"
#include "../vm/run.h"

struct MlwsVM {
    VM *vm;
    MlwsError err;
    char *backtrace;
    char message[sizeof(((MlwsError *)0)->msg) + 40];
    RunResult last; /* owns the text of the last call's string results */
    bool have_last;
    ExtLib *host;   /* the host's natives (their `serial` lock) */
    /* a background run (mlws_start) */
    PlatThread *bg;
    PlatMutex *bg_lock;
    PlatCond *bg_cv;
    bool bg_started;
    uint64_t bg_pid;
    Function *bg_fn;
    Word bg_args[NATIVE_MAX_ARGS];
    MlwsArg bg_in[NATIVE_MAX_ARGS];
    char *bg_strs[NATIVE_MAX_ARGS];
    int bg_nargs;
    RunResult bg_res;
};

static bool fail(MlwsVM *e, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
static bool fail(MlwsVM *e, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    snprintf(e->err.code, sizeof e->err.code, "%s", code);
    vsnprintf(e->err.msg, sizeof e->err.msg, fmt, ap);
    va_end(ap);
    return false;
}

static void clear(MlwsVM *e) {
    e->err.code[0] = e->err.msg[0] = 0;
    free(e->backtrace);
    e->backtrace = NULL;
}

static bool busy(MlwsVM *e) {
    if (e->bg) return fail(e, "E_BUSY", "a background run (mlws_start) is in progress: mlws_wait for it first");
    return true;
}

MlwsVM *mlws_new(void) {
    MlwsVM *e = xcalloc(1, sizeof *e);
    e->vm = vm_new();
    e->host = xcalloc(1, sizeof *e->host);
    e->host->name = xstrdup("host");
    e->host->path = xstrdup("");
    e->host->serial = plat_lock_new();
    VEC_PUSH(e->vm->extlibs, e->host); /* freed with the VM's other libraries */
    e->bg_lock = plat_mutex_new();
    e->bg_cv = plat_cond_new();
    return e;
}

void mlws_free(MlwsVM *e) {
    if (!e) return;
    if (e->bg) {
        MlwsArg ignore[1];
        mlws_wait(e, ignore, 0, NULL);
    }
    if (e->have_last) run_result_free(&e->last);
    free(e->backtrace);
    vm_free(e->vm);
    plat_mutex_free(e->bg_lock);
    plat_cond_free(e->bg_cv);
    free(e);
}

const char *mlws_error(const MlwsVM *e) {
    MlwsVM *m = (MlwsVM *)e;
    if (!e->err.code[0]) return "";
    snprintf(m->message, sizeof m->message, "%s: %s", e->err.code, e->err.msg);
    return m->message;
}

const char *mlws_error_code(const MlwsVM *e) { return e->err.code; }
const char *mlws_backtrace(const MlwsVM *e) { return e->backtrace ? e->backtrace : ""; }

bool mlws_set_option(MlwsVM *e, const char *name, const char *value) {
    clear(e);
    MlwsError err;
    if (vm_set_option(e->vm, name, value, &err)) return true;
    e->err = err;
    return false;
}

void mlws_add_path(MlwsVM *e, const char *dir) { vm_add_libpath(e->vm, dir); }

void mlws_set_output(MlwsVM *e, void (*write)(void *ctx, const char *s, size_t n), void *ctx) {
    e->vm->out = write;
    e->vm->out_ctx = ctx;
}

void mlws_set_args(MlwsVM *e, int argc, const char *const *argv) {
    for (size_t i = 0; i < e->vm->prog_args.n; i++) free(e->vm->prog_args.v[i]);
    e->vm->prog_args.n = 0;
    for (int i = 0; i < argc; i++) VEC_PUSH(e->vm->prog_args, xstrdup(argv[i]));
}

bool mlws_define(MlwsVM *e, const MlwsNativeDesc *d) {
    clear(e);
    char why[512];
    if (native_define(e->vm, e->host, d, false, why, sizeof why)) return true;
    return fail(e, "E_LINK_NATIVE", "%s", why);
}

const MlwsApi *mlws_api(void) { return native_api(); }

bool mlws_load_file(MlwsVM *e, const char *path) {
    clear(e);
    if (!busy(e)) return false;
    return vm_load_root(e->vm, path, &e->err) != NULL;
}

bool mlws_load_source(MlwsVM *e, const char *module, const char *source) {
    clear(e);
    if (!busy(e)) return false;
    if (!module || !*module || !source) return fail(e, "E_LOAD_NAME", "mlws_load_source needs a module name and source");
    return vm_load_text(e->vm, module, source, &e->err) != NULL;
}

/* ---- running --------------------------------------------------------------- */

static Function *find_function(MlwsVM *e, const char *module, const char *name) {
    LModule *lm = vm_module_named(e->vm, module, NULL);
    if (!lm) {
        fail(e, "E_LOAD_NOT_FOUND", "module %s is not loaded", module);
        return NULL;
    }
    int i = module_find_export_func(lm->m, name);
    if (i < 0) {
        fail(e, "E_LINK_UNDEF", "module %s does not export a function %s", module, name);
        return NULL;
    }
    return lm->funcs[i].target;
}

/* Check the arguments against the parameters; numbers go into args now,
 * strings are made in the process's heap by `prepare`. */
static bool convert_args(MlwsVM *e, Function *fn, const MlwsArg *in, int n, Word *args) {
    const MFunc *mf = fn->mf;
    if (n != mf->nparams) return fail(e, "E_TYPE", "%s takes %d argument(s), got %d", fn->name, mf->nparams, n);
    if (n > NATIVE_MAX_ARGS) return fail(e, "E_TYPE", "%s: at most %d arguments can be passed", fn->name, NATIVE_MAX_ARGS);
    for (int k = 0; k < n; k++) {
        MlwsType pt = (MlwsType)mf->params[k];
        Bank want = type_bank(pt);
        uint8_t t = in[k].type;
        Bank have = t == MLWS_T_R ? BANK_R : (t == MLWS_T_F32 || t == MLWS_T_F64) ? BANK_F : BANK_I;
        if (want != have || pt == T_BIG || pt == T_DEC)
            return fail(e, "E_TYPE", "%s: argument %d is a %s, but the parameter is %s", fn->name, k + 1,
                        type_name((MlwsType)t), type_name(pt));
        memcpy(&args[k], &in[k].v, sizeof args[k]);
        if (want == BANK_I) args[k].i = canon_int(pt, args[k].i);
        if (pt == T_F32) args[k].f = (float)args[k].f;
    }
    return true;
}

typedef struct {
    const MlwsArg *in;
    int n;
} Prep;

static void prepare(void *ctx, Process *p, Word *args) {
    const Prep *pr = ctx;
    for (int k = 0; k < pr->n; k++)
        if (pr->in[k].type == MLWS_T_R) args[k].r = pr->in[k].str ? str_new(p, pr->in[k].str, pr->in[k].len) : NULL;
}

static bool results_out(MlwsVM *e, RunResult *r, MlwsArg *out, int nout, int *got) {
    if (r->status == RUN_TRAP) {
        e->err = r->err;
        e->backtrace = r->backtrace ? xstrdup(r->backtrace) : NULL;
        return false;
    }
    if (got) *got = r->nresults;
    for (int k = 0; k < nout && k < r->nresults; k++) {
        uint8_t t = r->result_types[k];
        out[k].type = (t == T_R || t == T_BIG || t == T_DEC) ? MLWS_T_R : t;
        memcpy(&out[k].v, &r->results[k], sizeof out[k].v);
        out[k].str = r->result_text[k];
        out[k].len = r->result_len[k];
    }
    return true;
}

bool mlws_call(MlwsVM *e, const char *module, const char *function, const MlwsArg *args, int nargs,
               MlwsArg *results, int nresults, int *got) {
    clear(e);
    if (!busy(e)) return false;
    Function *fn = find_function(e, module, function);
    Word w[NATIVE_MAX_ARGS];
    if (!fn || !convert_args(e, fn, args, nargs, w)) return false;
    if (e->have_last) run_result_free(&e->last);
    Prep pr = {args, nargs};
    RunHooks h = {prepare, NULL, &pr};
    memset(&e->last, 0, sizeof e->last);
    vm_run_function(e->vm, fn, w, nargs, &e->last, &h);
    e->have_last = true;
    return results_out(e, &e->last, results, nresults, got);
}

bool mlws_run_main(MlwsVM *e, const char *module, int64_t *exit_code) {
    clear(e);
    if (!busy(e)) return false;
    LModule *lm = vm_module_named(e->vm, module, NULL);
    if (!lm) return fail(e, "E_LOAD_NOT_FOUND", "module %s is not loaded", module);
    Function *fn = vm_find_main(lm, &e->err);
    if (!fn) return false;
    if (e->have_last) run_result_free(&e->last);
    memset(&e->last, 0, sizeof e->last);
    vm_run_function(e->vm, fn, NULL, 0, &e->last, NULL);
    e->have_last = true;
    if (e->last.status == RUN_TRAP) return results_out(e, &e->last, NULL, 0, NULL);
    if (exit_code) *exit_code = e->last.exit_code;
    return true;
}

/* ---- background runs ----------------------------------------------------------- */

typedef struct {
    Prep prep;
    MlwsVM *e;
} BgCtx;

static void bg_prepare(void *ctx, Process *p, Word *args) { prepare(&((BgCtx *)ctx)->prep, p, args); }

static void bg_on_start(void *ctx, uint64_t pid) {
    MlwsVM *e = ((BgCtx *)ctx)->e;
    plat_mutex_lock(e->bg_lock);
    e->bg_pid = pid;
    e->bg_started = true;
    plat_cond_broadcast(e->bg_cv);
    plat_mutex_unlock(e->bg_lock);
}

static void bg_main(void *arg) {
    MlwsVM *e = arg;
    BgCtx c = {{e->bg_in, e->bg_nargs}, e};
    RunHooks h = {bg_prepare, bg_on_start, &c};
    memset(&e->bg_res, 0, sizeof e->bg_res);
    vm_run_function(e->vm, e->bg_fn, e->bg_args, e->bg_nargs, &e->bg_res, &h);
    bg_on_start(&c, e->bg_pid); /* (in case it never started) */
}

bool mlws_start(MlwsVM *e, const char *module, const char *function, const MlwsArg *args, int nargs,
                uint64_t *pid) {
    clear(e);
    if (!busy(e)) return false;
    Function *fn = find_function(e, module, function);
    if (!fn || !convert_args(e, fn, args, nargs, e->bg_args)) return false;
    /* Keep copies of the string arguments: the caller's may go before the thread uses them. */
    for (int k = 0; k < nargs; k++) {
        e->bg_in[k] = args[k];
        e->bg_strs[k] = args[k].type == MLWS_T_R && args[k].str ? xstrndup(args[k].str, args[k].len) : NULL;
        if (e->bg_strs[k]) e->bg_in[k].str = e->bg_strs[k];
    }
    e->bg_fn = fn;
    e->bg_nargs = nargs;
    e->bg_started = false;
    e->vm->external_input = true; /* waiting for the host's messages is not a deadlock */
    e->bg = plat_thread_start(bg_main, e);
    if (!e->bg) return fail(e, "E_RESOURCES", "cannot start a thread for the background run");
    plat_mutex_lock(e->bg_lock);
    while (!e->bg_started) plat_cond_wait(e->bg_cv, e->bg_lock, 0);
    plat_mutex_unlock(e->bg_lock);
    if (pid) *pid = e->bg_pid;
    return true;
}

bool mlws_send(MlwsVM *e, uint64_t pid, const MlwsArg *m) {
    Word w;
    memcpy(&w, &m->v, sizeof w);
    uint8_t t = m->type;
    if (t == MLWS_T_R) { /* a string, or null */
        w.r = NULL;
        return sched_post(e->vm, pid, w, T_R, m->str, m->str ? m->len : 0);
    }
    if (t != MLWS_T_F32 && t != MLWS_T_F64) w.i = canon_int((MlwsType)t, w.i);
    return sched_post(e->vm, pid, w, t, NULL, 0);
}

bool mlws_wait(MlwsVM *e, MlwsArg *results, int nresults, int *got) {
    clear(e);
    if (!e->bg) return fail(e, "E_BUSY", "nothing is running in the background");
    plat_thread_join(e->bg);
    e->bg = NULL;
    e->vm->external_input = false;
    for (int k = 0; k < e->bg_nargs; k++) free(e->bg_strs[k]), e->bg_strs[k] = NULL;
    if (e->have_last) run_result_free(&e->last);
    e->last = e->bg_res; /* its result text lives until the next call */
    e->have_last = true;
    return results_out(e, &e->last, results, nresults, got);
}
