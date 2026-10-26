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
          "  run  [-L dir] [--stats] [--stack-words N] file.(masm|mlwso)  run a program's main\n"
          "  asm  [-L dir] [-o out.mlwso] [-E] file.masm                   assemble (-E: preprocess only)\n"
          "  dis  [-L dir] file.(masm|mlwso)                               disassemble\n"
          "  test [-L dir] [-v] [paths...]                               run assembly tests (default: tests/)\n"
          "  doc  isa|natives [-o file]                                  generate reference docs\n"
          "  version\n"
          "\n"
          "Libraries are searched in: the program's directory, -L dirs, $MLWSVM_PATH, and lib/\n"
          "next to the mlwsvm executable.\n",
          f);
}

static ToolEnv g_env;
static char *g_exe_lib;

void tool_env_libpaths(const ToolEnv *env, VM *vm) {
    for (size_t i = 0; i < env->libpaths.n; i++) vm_add_libpath(vm, env->libpaths.v[i]);
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
        if (lib && plat_is_dir(lib)) g_exe_lib = lib;
        else free(lib);
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
    bool stats = false;
    uint32_t stack_words = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--stack-words") && i + 1 < argc) stack_words = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!file) file = argv[i];
        else {
            fprintf(stderr, "mlwsvm run: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!file) {
        usage(stderr);
        return 2;
    }
    VM *vm = vm_new();
    if (stack_words) vm->stack_words = stack_words;
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
                "raw leaked:       %llu bytes\n",
                (unsigned long long)r.insns, (double)r.wall_ns / 1e6,
                r.wall_ns ? (double)r.insns / ((double)r.wall_ns / 1e9) / 1e6 : 0.0, r.max_depth,
                (unsigned long long)r.stack_peak_bytes, (unsigned long long)r.raw_allocs,
                (unsigned long long)r.raw_frees, (unsigned long long)r.raw_peak, (unsigned long long)r.raw_leaked);
    }
    run_result_free(&r);
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
    else if (!strcmp(cmd, "doc")) rc = tool_doc(n, args);
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
