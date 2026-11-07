/* bundle.c - `mlwsvm bundle`: a program and everything it imports in one
 * executable: this mlwsvm, copied, with the modules after it (loader/bundle.h). */
#include <stdio.h>

#include "../loader/bundle.h"
#include "../platform/platform.h"
#include "../vm/run.h"
#include "tools.h"

static int bundle_usage(void) {
    fprintf(stderr, "usage: mlwsvm bundle [-L dir] [-o out] [--with MODULE]... [VM options] file.(masm|mlwso)\n");
    return 2;
}

/* Modules of the same name from two directories cannot share a bundle:
 * it finds modules by name alone. */
static const LModule *name_clash(const VM *vm) {
    for (size_t i = 0; i < vm->mods.n; i++)
        for (size_t j = 0; j < i; j++)
            if (!strcmp(vm->mods.v[i]->m->name, vm->mods.v[j]->m->name)) return vm->mods.v[i];
    return NULL;
}

static char *default_out(const char *file) {
    const char *base = path_basename(file), *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    StrBuf b = {0};
    sb_putn(&b, base, n);
#ifdef _WIN32
    sb_puts(&b, ".exe");
#endif
    return b.s;
}

int tool_bundle(int argc, char **argv, const ToolEnv *env) {
    const char *file = NULL, *out = NULL;
    VEC(const char *) with = {0};
    VEC(char *) opts = {0}; /* name, value: all go to this VM; the bundle keeps those for run time */
    int rc = 1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--with") && i + 1 < argc) VEC_PUSH(with, argv[++i]);
        else if (!strncmp(argv[i], "--", 2) && vm_option_known(argv[i] + 2) && i + 1 < argc) {
            VEC_PUSH(opts, argv[i] + 2);
            VEC_PUSH(opts, argv[++i]);
        } else if (!file && argv[i][0] != '-') file = argv[i];
        else return bundle_usage();
    }
    if (!file) return bundle_usage();
    if (!env->exe) {
        fprintf(stderr, "mlwsvm bundle: cannot find this executable\n");
        return 1;
    }
    VM *vm = vm_new();
    MlwsError err;
    VEC(char *) kept = {0};
    for (size_t i = 0; i < opts.n; i += 2) {
        if (!vm_set_option(vm, opts.v[i], opts.v[i + 1], &err)) {
            fprintf(stderr, "mlwsvm bundle: %s\n", err.msg);
            goto done;
        }
        /* The optimizer runs now, on what goes into the file. */
        if (strcmp(opts.v[i], "opt") && strcmp(opts.v[i], "opt-report")) {
            VEC_PUSH(kept, opts.v[i]);
            VEC_PUSH(kept, opts.v[i + 1]);
        }
    }
    tool_env_libpaths(env, vm);
    LModule *root = vm_load_root(vm, file, &err);
    if (root && !vm_find_main(root, &err)) root = NULL;
    for (size_t i = 0; root && i < with.n; i++)
        if (!vm_load_dynamic(vm, with.v[i], root, &err)) root = NULL;
    if (!root) {
        fflush(stdout);
        fprintf(stderr, "error[%s]: %s\n", err.code, err.msg);
        rc = 65;
        goto done;
    }
    const LModule *clash = name_clash(vm);
    if (clash) {
        fprintf(stderr, "mlwsvm bundle: two modules named '%s' (a bundle finds modules by name)\n", clash->m->name);
        goto done;
    }
    size_t exe_len;
    char *exe = read_file(env->exe, &exe_len);
    if (!exe) {
        fprintf(stderr, "mlwsvm bundle: cannot read %s\n", env->exe);
        goto done;
    }
    ByteBuf b = {0};
    bb_bytes(&b, exe, exe_len);
    free(exe);
    Module **mods = xcalloc(vm->mods.n + 1, sizeof *mods);
    for (size_t i = 0; i < vm->mods.n; i++) mods[i] = vm->mods.v[i]->m;
    bundle_write(root->m->name, kept.v, (uint32_t)kept.n, mods, (uint32_t)vm->mods.n, &b);
    free(mods);
    char *path = out ? xstrdup(out) : default_out(file);
    if (write_file(path, b.b, b.n) && plat_make_executable(path)) {
        printf("%s: %s, %zu module%s, %zu bytes\n", path, root->m->name, vm->mods.n, vm->mods.n == 1 ? "" : "s", b.n);
        size_t pn = strlen(path);
        const char *ext = pn >= 4 ? path + pn - 4 : "";
        if (ext[0] == '.' && (ext[1] | 32) == 'a' && (ext[2] | 32) == 'p' && (ext[3] | 32) == 'p') /* macOS: an app bundle */
            fprintf(stderr,
                    "mlwsvm bundle: warning: macOS kills a program whose name ends in .app as it starts (an .app is a "
                    "directory, an application bundle there): name it otherwise\n");
        rc = 0;
    } else {
        fprintf(stderr, "mlwsvm bundle: cannot write %s\n", path);
    }
    free(path);
    bb_free(&b);
done:
    VEC_FREE(kept);
    VEC_FREE(opts);
    VEC_FREE(with);
    vm_free(vm);
    return rc;
}
