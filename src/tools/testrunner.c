/* testrunner.c - `mlwsvm test`: runs every .masm test file and checks the
 * expectations declared in its comments:
 *
 *   ; expect-stdout: <one line of expected output>   (repeatable, in order)
 *   ; expect-exit: <n>                               (default 0)
 *   ; expect-error: <E_CODE>                         (assembler, loader or trap)
 *   ; skip: <reason>
 *   ; threads: <n>                                   (run on n worker threads)
 *   ; option: --<name> <value>                       (repeatable: a VM option, as on the command line)
 *   ; requires: ffi                                  (skipped in a build without libffi)
 *   ; requires: posix | windows                      (skipped on other systems)
 *   ; debug-script: <debugger command>                (repeatable: run under
 *                                                       `mlwsvm debug`; stdout is the transcript)
 *   ; expect-opt: <pass> <n>                         (repeatable: the optimizer made exactly n
 *                                                       changes of that pass to this file's module)
 *   ; expect-insns: <function> <n>                   (repeatable: its instructions once optimized)
 *   ; no-opt: <reason>                               (not optimized by MLWSVM_OPT or --opt on the
 *                                                       command line; its own `; option: --opt` still applies)
 *   ; expect-jit: compiled <function> <n>            (repeatable: the JIT gave n of its instructions
 *                                                       machine code; checked where there is a JIT)
 *   ; expect-jit: loops <function> <n>               (n of its loops keep their registers in
 *                                                       machine registers)
 *   ; expect-jit: steps <n>                          (the machine code stepped n instructions)
 *   ; expect-jit: speculated <function> <n>          (n of its closure calls were compiled for the
 *                                                       function they called last)
 *                                                    (each of these may end with an architecture,
 *                                                       arm64 or x86-64: checked only for its JIT)
 *
 * Directories whose name starts with '_' hold helper modules and are skipped.
 *
 * A test still running after MLWSVM_TEST_TIMEOUT seconds (default 300) is
 * reported by name, and the run ends: a VM that hangs cannot be stopped from
 * outside, and a silent hang is the worst way for a CI job to fail. */
#include "tools.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "../platform/platform.h"
#include "../vm/run.h"

typedef struct {
    StrBuf stdout_want;
    bool has_stdout;
    int64_t exit_want;
    char error_want[32];
    char *skip;
    int threads; /* `; threads: N` - run on N worker threads */
    StrVec options; /* `; option: --name value`, as name, value pairs */
    char *option_error;
    StrVec script; /* debugger commands */
    StrVec opt_want; /* "pass n" */
    StrVec insns_want; /* "function n" */
    bool no_opt;
    StrVec jit_want; /* "compiled function n", "steps n" */
} Expect;

static void capture(void *ctx, const char *s, size_t n) { sb_putn(ctx, s, n); }

typedef struct {
    StrVec *lines;
    size_t next;
} Script;

static bool script_line(void *ctx, char *buf, size_t n) {
    Script *s = ctx;
    if (s->next >= s->lines->n) return false;
    snprintf(buf, n, "%s\n", s->lines->v[s->next++]);
    return true;
}

/* Run the test under the debugger with its scripted commands. */
static void run_debug_script(VM *vm, const char *path, Expect *e, StrBuf *out, RunResult *r) {
    memset(r, 0, sizeof *r);
    LModule *root = vm_load_root(vm, path, &r->err);
    Function *main_fn = root ? vm_find_main(root, &r->err) : NULL;
    if (!main_fn) {
        r->status = RUN_LOAD_ERROR;
        return;
    }
    Script sc = {&e->script, 0};
    DebugIO io = {capture, out, script_line, &sc, true, true};
    debugger_run(vm, root, main_fn, &io);
    r->status = RUN_OK;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void collect(const char *path, StrVec *out) {
    if (!plat_is_dir(path)) {
        if (str_ends_with(path, ".masm")) VEC_PUSH(*out, xstrdup(path));
        return;
    }
    char **names;
    size_t n;
    if (!plat_list_dir(path, &names, &n)) return;
    qsort(names, n, sizeof *names, cmp_str);
    for (size_t i = 0; i < n; i++) {
        char *full = path_join(path, names[i]);
        if (plat_is_dir(full)) {
            if (names[i][0] != '_' && names[i][0] != '.') collect(full, out);
        } else if (str_ends_with(names[i], ".masm")) {
            VEC_PUSH(*out, xstrdup(full));
        }
        free(full);
        free(names[i]);
    }
    free(names);
}

static const char *after_key(const char *line, const char *key) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (*p != ';') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t k = strlen(key);
    if (strncmp(p, key, k) != 0) return NULL;
    p += k;
    if (*p == ' ') p++;
    return p;
}

static void parse_expect(const char *text, Expect *e) {
    memset(e, 0, sizeof *e);
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, len);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        const char *v;
        if ((v = after_key(line, "expect-stdout:"))) {
            sb_puts(&e->stdout_want, v);
            sb_putc(&e->stdout_want, '\n');
            e->has_stdout = true;
        } else if ((v = after_key(line, "expect-exit:"))) {
            e->exit_want = strtoll(v, NULL, 10);
        } else if ((v = after_key(line, "expect-error:"))) {
            snprintf(e->error_want, sizeof e->error_want, "%s", v);
            char *s = e->error_want + strlen(e->error_want);
            while (s > e->error_want && isspace((unsigned char)s[-1])) *--s = 0;
        } else if ((v = after_key(line, "expect-opt:"))) {
            VEC_PUSH(e->opt_want, xstrdup(v));
        } else if ((v = after_key(line, "expect-insns:"))) {
            VEC_PUSH(e->insns_want, xstrdup(v));
        } else if ((v = after_key(line, "expect-jit:"))) {
            VEC_PUSH(e->jit_want, xstrdup(v));
        } else if ((v = after_key(line, "no-opt:"))) {
            e->no_opt = true;
        } else if ((v = after_key(line, "debug-script:"))) {
            VEC_PUSH(e->script, xstrdup(v));
        } else if ((v = after_key(line, "threads:"))) {
            e->threads = atoi(v);
        } else if ((v = after_key(line, "option:"))) {
            char name[64], value[256];
            if (sscanf(v, "--%63s %255s", name, value) == 2 && vm_option_known(name)) {
                VEC_PUSH(e->options, xstrdup(name));
                VEC_PUSH(e->options, xstrdup(value));
            } else if (!e->option_error) {
                e->option_error = xstrdup(v);
            }
        } else if ((v = after_key(line, "requires:"))) { /* a build feature (ffi) or a system */
#ifdef _WIN32
            bool windows = true;
#else
            bool windows = false;
#endif
            if (!strncmp(v, "ffi", 3) && !ffi_available() && !e->skip) e->skip = xstrdup("built without libffi");
            if (!strncmp(v, "posix", 5) && windows && !e->skip) e->skip = xstrdup("POSIX only");
            if (!strncmp(v, "windows", 7) && !windows && !e->skip) e->skip = xstrdup("Windows only");
        } else if ((v = after_key(line, "skip:"))) {
            free(e->skip);
            e->skip = xstrdup(v);
        }
        free(line);
        p = nl ? nl + 1 : p + len;
    }
}

/* The optimizer's report on the test's own module. */
static const OptModStat *opt_stat_for(const OptReport *rep, const char *path) {
    for (size_t i = 0; i < rep->mods.n; i++)
        if (!strcmp(rep->mods.v[i].source, path)) return &rep->mods.v[i];
    return NULL;
}

static void check_opt(const Expect *e, const OptReport *rep, const char *path, StrBuf *why) {
    const OptModStat *ms = rep ? opt_stat_for(rep, path) : NULL;
    if (!ms) {
        sb_printf(why, "expect-opt / expect-insns: the module was not optimized (add `; option: --opt ...`)");
        return;
    }
    for (size_t k = 0; k < e->opt_want.n && !why->n; k++) {
        char name[64];
        long long want;
        if (sscanf(e->opt_want.v[k], "%63s %lld", name, &want) != 2) {
            sb_printf(why, "bad '; expect-opt:' line: %s (expected a pass and a count)", e->opt_want.v[k]);
            return;
        }
        int p = -1;
        for (int q = 0; q < PASS__COUNT; q++)
            if (!strcmp(opt_pass_name(q), name)) p = q;
        if (p < 0) sb_printf(why, "expect-opt: unknown pass '%s'", name);
        else if ((long long)ms->counts[p] != want)
            sb_printf(why, "expect-opt: %s made %llu changes, expected %lld", name, (unsigned long long)ms->counts[p], want);
    }
    for (size_t k = 0; k < e->insns_want.n && !why->n; k++) {
        char name[128];
        long long want;
        if (sscanf(e->insns_want.v[k], "%127s %lld", name, &want) != 2) {
            sb_printf(why, "bad '; expect-insns:' line: %s (expected a function and a count)", e->insns_want.v[k]);
            return;
        }
        const OptFuncStat *fs = NULL;
        for (size_t q = 0; q < ms->funcs.n; q++)
            if (!strcmp(ms->funcs.v[q].func, name)) fs = &ms->funcs.v[q];
        if (!fs) sb_printf(why, "expect-insns: no function '%s' after optimizing", name);
        else if ((long long)fs->after != want)
            sb_printf(why, "expect-insns: %s has %u instructions, expected %lld", name, fs->after, want);
    }
}

/* What the JIT compiled and stepped (where this build has one). */
static void check_jit(const Expect *e, VM *vm, StrBuf *why) {
    for (size_t k = 0; k < e->jit_want.n && !why->n; k++) {
        char name[128];
        long long want;
        const char *line = e->jit_want.v[k];
        char arch[16] = "";
        int nf = sscanf(line, "%*s %*s %*s %15s", arch);
        if (!strncmp(line, "steps", 5)) nf = sscanf(line, "%*s %*s %15s", arch);
        if (nf == 1 && strcmp(arch, interp_jit_arch())) continue; /* another architecture's */
        bool loops = sscanf(line, "loops %127s %lld", name, &want) == 2;
        bool spec = !loops && sscanf(line, "speculated %127s %lld", name, &want) == 2;
        if (loops || spec || sscanf(line, "compiled %127s %lld", name, &want) == 2) {
            const Function *f = NULL;
            for (size_t i = 0; i < vm->mods.n && !f; i++) {
                const LModule *lm = vm->mods.v[i];
                for (uint32_t q = 0; q < lm->m->nfuncs && !f; q++)
                    if (!strcmp(lm->funcs[q].name, name)) f = &lm->funcs[q];
            }
            if (!f) sb_printf(why, "expect-jit: no function '%s'", name);
            else if (loops && (long long)f->jit_loops != want)
                sb_printf(why, "expect-jit: %s has %u loops with their registers in machine registers, expected %lld", name,
                          f->jit_loops, want);
            else if (spec && (long long)f->jit_spec != want)
                sb_printf(why, "expect-jit: %s has %u closure calls compiled for a callee, expected %lld", name,
                          f->jit_spec, want);
            else if (!loops && !spec && (long long)f->jit_native != want)
                sb_printf(why, "expect-jit: %s has %u instructions with machine code, expected %lld", name, f->jit_native, want);
        } else if (sscanf(line, "steps %lld", &want) == 1) {
            if ((long long)interp_jit_steps(vm) != want)
                sb_printf(why, "expect-jit: %llu instructions stepped, expected %lld", (unsigned long long)interp_jit_steps(vm), want);
        } else {
            sb_printf(why, "bad '; expect-jit:' line: %s (expected compiled FUNCTION N, loops FUNCTION N, speculated FUNCTION N, or steps N)", line);
        }
    }
}

static void free_expect(Expect *e) {
    for (size_t k = 0; k < e->jit_want.n; k++) free(e->jit_want.v[k]);
    VEC_FREE(e->jit_want);
    for (size_t k = 0; k < e->opt_want.n; k++) free(e->opt_want.v[k]);
    VEC_FREE(e->opt_want);
    for (size_t k = 0; k < e->insns_want.n; k++) free(e->insns_want.v[k]);
    VEC_FREE(e->insns_want);
}

/* The watchdog: which test is running, and since when. */
static const char *_Atomic wd_path;
static _Atomic uint64_t wd_start;
static _Atomic bool wd_stop;
static uint64_t wd_limit_ns;

static void watchdog(void *arg) {
    (void)arg;
    while (!atomic_load(&wd_stop)) {
        plat_sleep_ns(100 * 1000000ull);
        const char *path = atomic_load(&wd_path);
        uint64_t t0 = atomic_load(&wd_start);
        if (path && t0 && plat_now_ns() - t0 > wd_limit_ns) {
            fflush(stdout);
            fprintf(stderr, "TIMEOUT %s: still running after %llu s (MLWSVM_TEST_TIMEOUT)\n", path,
                    (unsigned long long)(wd_limit_ns / 1000000000ull));
            fflush(stderr);
            _Exit(3);
        }
    }
}

int tool_test(int argc, char **argv, const ToolEnv *env) {
    bool verbose = false;
    StrVec opts = {0}; /* VM options from the command line: name, value pairs */
    StrVec files = {0};
    VEC(const char *) roots = {0};
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strncmp(argv[i], "--", 2) && vm_option_known(argv[i] + 2) && i + 1 < argc) {
            VEC_PUSH(opts, argv[i] + 2);
            VEC_PUSH(opts, argv[++i]);
        }
        else VEC_PUSH(roots, argv[i]);
    }
    if (roots.n == 0) VEC_PUSH(roots, "tests");
    for (size_t i = 0; i < roots.n; i++) collect(roots.v[i], &files);
    VEC_FREE(roots);
    if (files.n == 0) {
        fprintf(stderr, "mlwsvm test: no .masm files found\n");
        return 2;
    }
    int passed = 0, failed = 0, skipped = 0;
    const char *limit = getenv("MLWSVM_TEST_TIMEOUT");
    wd_limit_ns = (limit && atoi(limit) > 0 ? (uint64_t)atoi(limit) : 300) * 1000000000ull;
    atomic_store(&wd_stop, false);
    PlatThread *wd = plat_thread_start(watchdog, NULL);
    for (size_t i = 0; i < files.n; i++) {
        const char *path = files.v[i];
        char *text = read_file(path, NULL);
        Expect e;
        parse_expect(text ? text : "", &e);
        free(text);
        if (e.skip) {
            skipped++;
            if (verbose) printf("SKIP %s (%s)\n", path, e.skip);
            free(e.skip);
            sb_free(&e.stdout_want);
            for (size_t k = 0; k < e.options.n; k++) free(e.options.v[k]);
            VEC_FREE(e.options);
            free(e.option_error);
            free_expect(&e);
            continue;
        }
        VM *vm = vm_new();
        StrBuf why = {0};
        MlwsError oerr;
        for (size_t k = 0; k < opts.n && !why.n; k += 2)
            if (!vm_set_option(vm, opts.v[k], opts.v[k + 1], &oerr)) sb_printf(&why, "%s", oerr.msg);
        if (e.no_opt) vm->opt.passes = 0; /* (MLWSVM_OPT, --opt) */
        bool want_opt = e.opt_want.n || e.insns_want.n;
        if (want_opt && !vm->opt_report) vm->opt_report = xcalloc(1, sizeof *vm->opt_report);
        for (size_t k = 0; k < e.options.n && !why.n; k += 2) /* the test's own settings win */
            if (!vm_set_option(vm, e.options.v[k], e.options.v[k + 1], &oerr)) sb_printf(&why, "%s", oerr.msg);
        if (e.option_error && !why.n) sb_printf(&why, "bad '; option:' line: %s (expected --name value)", e.option_error);
        if (e.threads > 0) vm->threads = e.threads; /* the test needs this many */
        bool want_jit = e.jit_want.n && interp_jit_available() && vm->jit_mode;
        if (want_jit) vm->jit_stats = true;
        StrBuf out = {0};
        vm->out = capture;
        vm->out_ctx = &out;
        StrBuf errs = {0}; /* crash reports of unobserved child processes */
        vm->err_out = capture;
        vm->err_ctx = &errs;
        tool_env_libpaths(env, vm);
        RunResult r;
        memset(&r, 0, sizeof r);
        atomic_store(&wd_start, plat_now_ns());
        atomic_store(&wd_path, path);
        if (why.n) { /* a bad option: not run */
        } else if (e.script.n) run_debug_script(vm, path, &e, &out, &r);
        else vm_run_file(vm, path, &r);
        atomic_store(&wd_path, NULL);
        const char *got_code = r.status == RUN_OK ? "" : r.err.code;
        if (why.n) {
        } else if (e.error_want[0]) {
            if (strcmp(got_code, e.error_want) != 0) {
                if (r.status == RUN_OK) sb_printf(&why, "expected error %s, but the program succeeded", e.error_want);
                else sb_printf(&why, "expected error %s, got %s: %s", e.error_want, got_code, r.err.msg);
            }
        } else if (r.status != RUN_OK) {
            sb_printf(&why, "%s: %s", got_code, r.err.msg);
            if (r.backtrace && *r.backtrace) sb_printf(&why, "\n%s", r.backtrace);
        } else if (r.exit_code != e.exit_want) {
            sb_printf(&why, "exit code %lld, expected %lld", (long long)r.exit_code, (long long)e.exit_want);
        }
        if (!why.n && want_opt) check_opt(&e, vm->opt_report, path, &why);
        if (!why.n && want_jit) check_jit(&e, vm, &why);
        if (!why.n && e.has_stdout && strcmp(out.s ? out.s : "", e.stdout_want.s) != 0) {
            sb_printf(&why, "stdout mismatch\n--- expected ---\n%s--- got ---\n%s", e.stdout_want.s, out.s ? out.s : "");
        }
        if (why.n) {
            failed++;
            printf("FAIL %s\n     %s\n", path, why.s);
        } else {
            passed++;
            if (verbose) printf("PASS %s\n", path);
        }
        sb_free(&why);
        sb_free(&out);
        sb_free(&errs);
        for (size_t k = 0; k < e.script.n; k++) free(e.script.v[k]);
        VEC_FREE(e.script);
        for (size_t k = 0; k < e.options.n; k++) free(e.options.v[k]);
        VEC_FREE(e.options);
        free(e.option_error);
        free_expect(&e);
        sb_free(&e.stdout_want);
        run_result_free(&r);
        vm_free(vm);
    }
    atomic_store(&wd_stop, true);
    if (wd) plat_thread_join(wd);
    printf("%d passed, %d failed, %d skipped (%zu files)\n", passed, failed, skipped, files.n);
    for (size_t i = 0; i < files.n; i++) free(files.v[i]);
    VEC_FREE(files);
    VEC_FREE(opts);
    return failed ? 1 : 0;
}
