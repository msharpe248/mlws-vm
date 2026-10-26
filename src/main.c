/* main.c - the `mlwsvm` command line: run, asm, dis, test, doc. */
#include "asm/asm.h"
#include "core/disasm.h"
#include "platform/platform.h"
#include "tools/tools.h"
#include "vm/run.h"

#define MLWSVM_VERSION "0.3.0"

static void usage(FILE *f) {
    fputs("usage: mlwsvm <command> [options]\n"
          "\n"
          "commands:\n"
          "  run   [options] file.(masm|mlwso) [args...]  run a program's main (args: os.args)\n"
          "        --stats                         resource summary on exit\n"
          "        --profile [--flame FILE]        hot functions, lines and opcodes; collapsed stacks\n"
          "        --trace FILE|- [--trace-limit N] log every instruction\n"
          "        --monitor [HOST:]PORT [--hold]  serve the web dashboard and /api while running\n"
          "        --gc-stress                     collect garbage at every allocation (VM testing)\n"
          "        --unsafe-native                 load native code without the trust checks (development)\n"
          "      VM options (also for test; or MLWSVM_<NAME>, e.g. MLWSVM_MAX_FRAMES=500000):\n"
          "        --threads N                     worker threads (default: one per core)\n"
          "        --stack-words N                 data stack per process, in 8-byte words (131072)\n"
          "        --max-frames N                  call depth limit (100000)\n"
          "        --slice N                       time slice, in instructions (2000)\n"
          "        --gc-min BYTES                  smallest heap that triggers a collection (1M)\n"
          "        --quota heap=B,raw=B,mailbox=N,insns=N,foreign=N   starting limits for every process\n"
          "        --ext-path DIR[:DIR]            trusted extension directories, before lib/ext\n"
          "  debug [-L dir] file.(masm|mlwso)      interactive debugger (type 'help')\n"
          "  top   [HOST:PORT] [--once]            live process table of a VM run with --monitor\n"
          "  asm  [-L dir] [-o out.mlwso] [-E] file.masm                   assemble (-E: preprocess only)\n"
          "  dis  [-L dir] file.(masm|mlwso)                               disassemble\n"
          "  test [-L dir] [-v] [VM options] [paths...]                  run assembly tests (default: tests/)\n"
          "  doc  isa|natives [-o file]                                  generate reference docs\n"
          "  ext  list|install|remove|allow-ffi|deny-ffi [--dir D] ...  manage trusted native code\n"
          "  version\n"
          "\n"
          "Libraries are searched in: the program's directory, -L dirs, $MLWSVM_PATH, and lib/\n"
          "next to the mlwsvm executable. MLWSVM_GC_STRESS=1 collects garbage at every\n"
          "instruction boundary after an allocation (for testing the VM).\n",
          f);
}

static ToolEnv g_env;
static char *g_exe_lib;

void tool_env_libpaths(const ToolEnv *env, VM *vm) {
    for (size_t i = 0; i < env->libpaths.n; i++) vm_add_libpath(vm, env->libpaths.v[i]);
    if (env->ext_dir) VEC_PUSH(vm->ext_paths, xstrdup(env->ext_dir)); /* searched last */
}

static void env_init(const char *argv0) {
    const char *mp = getenv("MLWSVM_PATH");
    if (mp && *mp) {
        char sep = plat_path_sep();
        const char *s = mp;
        while (*s) {
            const char *e = strchr(s, sep);
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n) VEC_PUSH(g_env.libpaths, xstrndup(s, n));
            s += n + (e ? 1 : 0);
        }
    }
    char *exe = plat_exe_path(argv0);
    if (exe) {
        char *dir = path_dirname(exe);
        char *lib = path_join(dir, "lib");
        if (!plat_is_dir(lib)) {
            free(lib);
            char *up = path_join(dir, "../lib");
            lib = plat_realpath(up);
            free(up);
        }
        if (lib && plat_is_dir(lib)) {
            g_exe_lib = lib;
            g_env.ext_dir = path_join(lib, "ext");
        } else {
            free(lib);
        }
        free(dir);
        free(exe);
    }
}

/* Remove -L options from argv (they may appear anywhere) into g_env. */
static int take_lib_opts(int argc, char **argv) {
    int j = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-L") && i + 1 < argc) VEC_PUSH(g_env.libpaths, xstrdup(argv[++i]));
        else if (!strncmp(argv[i], "-L", 2) && argv[i][2]) VEC_PUSH(g_env.libpaths, xstrdup(argv[i] + 2));
        else argv[j++] = argv[i];
    }
    if (g_exe_lib) VEC_PUSH(g_env.libpaths, xstrdup(g_exe_lib));
    return j;
}

static void print_error(const MlwsError *e) {
    fflush(stdout);
    fprintf(stderr, "error[%s]: %s\n", e->code, e->msg);
}

static int cmd_run(int argc, char **argv) {
    const char *file = NULL;
    bool stats = false, gc_stress = false, profile = false, hold = false, unsafe_native = false;
    const char *trace = NULL, *flame = NULL, *monitor = NULL;
    uint64_t trace_limit = 0;
    VEC(const char *) opts = {0}; /* VM options: name, value pairs */
    char **prog_argv = NULL;
    int prog_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--profile")) profile = true;
        else if (!strcmp(argv[i], "--hold")) hold = true;
        else if (!strcmp(argv[i], "--flame") && i + 1 < argc) flame = argv[++i], profile = true;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--trace-limit") && i + 1 < argc) trace_limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) monitor = argv[++i];
        else if (!strcmp(argv[i], "--gc-stress")) gc_stress = true;
        else if (!strcmp(argv[i], "--unsafe-native")) unsafe_native = true;
        else if (!strncmp(argv[i], "--", 2) && vm_option_known(argv[i] + 2)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mlwsvm run: %s needs a value\n", argv[i]);
                VEC_FREE(opts);
                return 2;
            }
            VEC_PUSH(opts, argv[i] + 2);
            VEC_PUSH(opts, argv[++i]);
        } else if (!file) {
            file = argv[i];
            prog_argv = argv + i + 1; /* everything after the file belongs to the program (os.args) */
            prog_argc = argc - i - 1;
            break;
        } else {
            fprintf(stderr, "mlwsvm run: unexpected argument '%s'\n", argv[i]);
            VEC_FREE(opts);
            return 2;
        }
    }
    if (!file) {
        usage(stderr);
        VEC_FREE(opts);
        return 2;
    }
    VM *vm = vm_new();
    for (size_t i = 0; i < opts.n; i += 2) {
        MlwsError err;
        if (!vm_set_option(vm, opts.v[i], opts.v[i + 1], &err)) {
            fprintf(stderr, "mlwsvm run: %s\n", err.msg);
            VEC_FREE(opts);
            vm_free(vm);
            return 2;
        }
    }
    VEC_FREE(opts);
    for (int i = 0; i < prog_argc; i++) VEC_PUSH(vm->prog_args, xstrdup(prog_argv[i]));
    if (gc_stress) vm->gc_stress = true;
    if (unsafe_native) vm->unsafe_native = true;
    if (trace && !tools_trace(vm, trace, trace_limit)) {
        fprintf(stderr, "mlwsvm run: cannot write trace file %s\n", trace);
        vm_free(vm);
        return 1;
    }
    if (profile) tools_profile(vm);
    if (monitor) {
        char err[256];
        if (!tools_monitor(vm, monitor, err, sizeof err)) {
            fprintf(stderr, "mlwsvm run: %s\n", err);
            vm_free(vm);
            return 1;
        }
        fprintf(stderr, "mlwsvm: monitor at http://%s/  (or: mlwsvm top %s)\n", tools_monitor_addr(vm),
                tools_monitor_addr(vm));
        vm->hold = hold;
    }
    char *dir = path_dirname(file);
    vm_add_libpath(vm, dir);
    free(dir);
    tool_env_libpaths(&g_env, vm);
    RunResult r;
    vm_run_file(vm, file, &r);
    fflush(stdout);
    int rc;
    if (r.status == RUN_LOAD_ERROR) {
        print_error(&r.err);
        rc = 65;
    } else if (r.status == RUN_TRAP) {
        fprintf(stderr, "runtime error[%s]: %s\n%s", r.err.code, r.err.msg, r.backtrace ? r.backtrace : "");
        rc = 70;
    } else {
        rc = (int)(r.exit_code & 0xFF);
    }
    if (stats && r.status != RUN_LOAD_ERROR) {
        fprintf(stderr,
                "--- mlwsvm stats ---\n"
                "instructions:     %llu\n"
                "wall time:        %.3f ms\n"
                "speed:            %.1f M insn/s\n"
                "max call depth:   %u\n"
                "stack peak:       %llu bytes\n"
                "raw allocs/frees: %llu / %llu\n"
                "raw peak:         %llu bytes\n"
                "raw leaked:       %llu bytes\n"
                "heap peak:        %llu bytes\n"
                "heap at exit:     %llu bytes\n"
                "objects:          %llu allocated, %llu collected\n"
                "gc:               %llu collections, %.3f ms\n"
                "processes:        %llu spawned, %llu at most at once, %llu time slices\n"
                "all instructions: %llu (every process)\n",
                (unsigned long long)r.insns, (double)r.wall_ns / 1e6,
                r.wall_ns ? (double)r.insns / ((double)r.wall_ns / 1e9) / 1e6 : 0.0, r.max_depth,
                (unsigned long long)r.stack_peak_bytes, (unsigned long long)r.raw_allocs,
                (unsigned long long)r.raw_frees, (unsigned long long)r.raw_peak, (unsigned long long)r.raw_leaked,
                (unsigned long long)r.heap_peak, (unsigned long long)r.heap_live,
                (unsigned long long)r.objects_allocated, (unsigned long long)r.objects_freed,
                (unsigned long long)r.gc_count, (double)r.gc_ns / 1e6, (unsigned long long)r.spawned,
                (unsigned long long)r.max_live, (unsigned long long)r.slices, (unsigned long long)r.total_insns);
    }
    if (profile && r.status != RUN_LOAD_ERROR) tools_profile_report(vm, stderr, flame);
    run_result_free(&r);
    vm_free(vm);
    return rc;
}

static bool stdin_line(void *ctx, char *buf, size_t n) {
    (void)ctx;
    return fgets(buf, (int)n, stdin) != NULL;
}

static void stdout_out(void *ctx, const char *s, size_t n) {
    (void)ctx;
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

static int cmd_debug(int argc, char **argv) {
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
    VM *vm = vm_new();
    char *dir = path_dirname(argv[0]);
    vm_add_libpath(vm, dir);
    free(dir);
    tool_env_libpaths(&g_env, vm);
    MlwsError err = {{0}, {0}};
    LModule *root = vm_load_root(vm, argv[0], &err);
    Function *main_fn = root ? vm_find_main(root, &err) : NULL;
    if (!main_fn) {
        print_error(&err);
        vm_free(vm);
        return 65;
    }
    vm->out = stdout_out;
    bool tty = plat_is_tty();
    DebugIO io = {stdout_out, NULL, stdin_line, NULL, true, !tty};
    int rc = debugger_run(vm, root, main_fn, &io);
    vm_free(vm);
    return rc;
}

static char *default_module_name(const char *path) {
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
}

static int cmd_asm(int argc, char **argv) {
    const char *file = NULL, *out = NULL;
    bool pre = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-E")) pre = true;
        else file = argv[i];
    }
    if (!file) {
        usage(stderr);
        return 2;
    }
    VEC(char *) lp = {0};
    VEC_PUSH(lp, path_dirname(file));
    for (size_t i = 0; i < g_env.libpaths.n; i++) VEC_PUSH(lp, xstrdup(g_env.libpaths.v[i]));
    MlwsError err = {{0}, {0}};
    int rc = 0;
    if (pre) {
        StrBuf b = {0};
        if (preprocess_file(file, (const char *const *)lp.v, lp.n, &b, &err)) fwrite(b.s, 1, b.n, stdout);
        else print_error(&err), rc = 65;
        sb_free(&b);
    } else {
        char *name = default_module_name(file);
        Module *m = assemble_file(file, name, (const char *const *)lp.v, lp.n, &err);
        free(name);
        if (!m) {
            print_error(&err);
            rc = 65;
        } else {
            StrBuf o = {0};
            if (!out) {
                sb_putn(&o, file, strlen(file) - (str_ends_with(file, ".masm") ? 5 : 0));
                sb_puts(&o, ".mlwso");
            } else {
                sb_puts(&o, out);
            }
            ByteBuf bb = {0};
            module_write(m, &bb);
            if (!write_file(o.s, bb.b, bb.n)) {
                fprintf(stderr, "mlwsvm asm: cannot write %s\n", o.s);
                rc = 1;
            }
            bb_free(&bb);
            sb_free(&o);
            module_free(m);
        }
    }
    for (size_t i = 0; i < lp.n; i++) free(lp.v[i]);
    VEC_FREE(lp);
    return rc;
}

static int cmd_dis(int argc, char **argv) {
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
    const char *file = argv[0];
    MlwsError err = {{0}, {0}};
    Module *m = NULL;
    if (str_ends_with(file, ".mlwso")) {
        size_t len;
        char *data = read_file(file, &len);
        if (!data) {
            fprintf(stderr, "mlwsvm dis: cannot read %s\n", file);
            return 1;
        }
        m = module_read((const uint8_t *)data, len, &err);
        free(data);
    } else {
        VEC(char *) lp = {0};
        VEC_PUSH(lp, path_dirname(file));
        for (size_t i = 0; i < g_env.libpaths.n; i++) VEC_PUSH(lp, xstrdup(g_env.libpaths.v[i]));
        char *name = default_module_name(file);
        m = assemble_file(file, name, (const char *const *)lp.v, lp.n, &err);
        free(name);
        for (size_t i = 0; i < lp.n; i++) free(lp.v[i]);
        VEC_FREE(lp);
    }
    if (!m) {
        print_error(&err);
        return 65;
    }
    StrBuf b = {0};
    int rc = 0;
    if (disasm_module(&b, m, &err)) fwrite(b.s, 1, b.n, stdout);
    else print_error(&err), rc = 65;
    sb_free(&b);
    module_free(m);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    env_init(argv[0]);
    const char *cmd = argv[1];
    int n = take_lib_opts(argc - 2, argv + 2);
    char **args = argv + 2;
    int rc;
    if (!strcmp(cmd, "run")) rc = cmd_run(n, args);
    else if (!strcmp(cmd, "asm")) rc = cmd_asm(n, args);
    else if (!strcmp(cmd, "dis")) rc = cmd_dis(n, args);
    else if (!strcmp(cmd, "test")) rc = tool_test(n, args, &g_env);
    else if (!strcmp(cmd, "debug")) rc = cmd_debug(n, args);
    else if (!strcmp(cmd, "top")) rc = tool_top(n, args);
    else if (!strcmp(cmd, "doc")) rc = tool_doc(n, args);
    else if (!strcmp(cmd, "ext")) rc = tool_ext(n, args, &g_env);
    else if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("mlwsvm %s\n", MLWSVM_VERSION);
        rc = 0;
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        rc = 0;
    } else {
        fprintf(stderr, "mlwsvm: unknown command '%s'\n", cmd);
        usage(stderr);
        rc = 2;
    }
    for (size_t i = 0; i < g_env.libpaths.n; i++) free(g_env.libpaths.v[i]);
    VEC_FREE(g_env.libpaths);
    free(g_exe_lib);
    return rc;
}
