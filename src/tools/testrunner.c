/* testrunner.c - `mlwsvm test`: runs every .masm test file and checks the
 * expectations declared in its comments:
 *
 *   ; expect-stdout: <one line of expected output>   (repeatable, in order)
 *   ; expect-exit: <n>                               (default 0)
 *   ; expect-error: <E_CODE>                         (assembler, loader or trap)
 *   ; skip: <reason>
 *   ; debug-script: <debugger command>                (repeatable: run under
 *                                                       `mlwsvm debug`; stdout is the transcript)
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
        } else if ((v = after_key(line, "requires:"))) { /* a build feature: "ffi" */
            if (!strncmp(v, "ffi", 3) && !ffi_available() && !e->skip) e->skip = xstrdup("built without libffi");
        } else if ((v = after_key(line, "skip:"))) {
            free(e->skip);
            e->skip = xstrdup(v);
        }
        free(line);
        p = nl ? nl + 1 : p + len;
    }
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
            continue;
        }
        VM *vm = vm_new();
        StrBuf why = {0};
        MlwsError oerr;
        for (size_t k = 0; k < opts.n && !why.n; k += 2)
            if (!vm_set_option(vm, opts.v[k], opts.v[k + 1], &oerr)) sb_printf(&why, "%s", oerr.msg);
        for (size_t k = 0; k < e.options.n && !why.n; k += 2) /* the test's own settings win */
            if (!vm_set_option(vm, e.options.v[k], e.options.v[k + 1], &oerr)) sb_printf(&why, "%s", oerr.msg);
        if (e.option_error && !why.n) sb_printf(&why, "bad '; option:' line: %s (expected --name value)", e.option_error);
        if (e.threads > 0) vm->threads = e.threads; /* the test needs this many */
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
