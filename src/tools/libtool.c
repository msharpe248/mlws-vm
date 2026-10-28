/* libtool.c - `mlwsvm lib`: build, inspect and list library archives (.mlwsl;
 * docs/TOOLING.md, "Libraries"). */
#include "../asm/asm.h"
#include "../loader/library.h"
#include "../platform/platform.h"
#include "tools.h"

static int lib_usage(void) {
    fputs("usage: mlwsvm lib build DIR [-o FILE] [-L dir]   build an archive from DIR/library.masm and DIR's modules\n"
          "       mlwsvm lib info FILE.mlwsl                  show an archive's manifest and modules\n"
          "       mlwsvm lib list [-L dir]                    the archives on the library path\n",
          stderr);
    return 2;
}

typedef struct {
    char *path, *name;
} SrcModule;
typedef VEC(SrcModule) SrcVec;

static int src_cmp(const void *a, const void *b) {
    return strcmp(((const SrcModule *)a)->name, ((const SrcModule *)b)->name);
}

/* Every module source under dir/rel: a/b.masm is module a.b. Directories
 * whose names start with '_' or '.' hold include files, and are skipped, as
 * is the manifest. */
static void find_sources(const char *dir, const char *rel, SrcVec *out) {
    char *here = *rel ? path_join(dir, rel) : xstrdup(dir);
    char **names;
    size_t n;
    if (!plat_list_dir(here, &names, &n)) {
        free(here);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *nm = names[i];
        char *sub = *rel ? path_join(rel, nm) : xstrdup(nm);
        char *full = path_join(dir, sub);
        if (plat_is_dir(full)) {
            if (nm[0] != '_' && nm[0] != '.') find_sources(dir, sub, out);
        } else if (str_ends_with(nm, ".masm") && strcmp(sub, "library.masm")) {
            SrcModule s = {xstrdup(full), xstrndup(sub, strlen(sub) - 5)};
            for (char *c = s.name; *c; c++)
                if (*c == '/' || *c == '\\') *c = '.';
            VEC_PUSH(*out, s);
        }
        free(sub);
        free(full);
        free(names[i]);
    }
    free(names);
    free(here);
}

static int lib_build(const char *dir, const char *out, const ToolEnv *env) {
    VEC(char *) lp = {0};
    VEC_PUSH(lp, xstrdup(dir));
    for (size_t i = 0; i < env->libpaths.n; i++) VEC_PUSH(lp, xstrdup(env->libpaths.v[i]));
    MlwsError err = {{0}, {0}};
    int rc = 65;
    char *mpath = path_join(dir, "library.masm");
    SrcVec srcs = {0};
    VEC(Module *) mods = {0};
    Module *man = NULL;
    if (!file_exists(mpath)) {
        fprintf(stderr, "mlwsvm lib build: %s has no library.masm (the manifest)\n", dir);
        goto done;
    }
    man = assemble_file(mpath, "library", (const char *const *)lp.v, lp.n, &err);
    if (!man) goto fail;
    if (!man->lib_name) {
        fprintf(stderr, "mlwsvm lib build: %s does not name its library (.library name, \"version\")\n", mpath);
        goto done;
    }
    find_sources(dir, "", &srcs);
    qsort(srcs.v, srcs.n, sizeof *srcs.v, src_cmp);
    for (size_t i = 0; i < srcs.n; i++) {
        if (!lib_owns(man->lib_name, srcs.v[i].name)) {
            fprintf(stderr, "mlwsvm lib build: %s would be module %s, outside library %s\n", srcs.v[i].path,
                    srcs.v[i].name, man->lib_name);
            goto done;
        }
        Module *m = assemble_file(srcs.v[i].path, srcs.v[i].name, (const char *const *)lp.v, lp.n, &err);
        if (!m) goto fail;
        VEC_PUSH(mods, m);
        if (strcmp(m->name, srcs.v[i].name)) {
            fprintf(stderr, "mlwsvm lib build: %s declares module %s, but its path makes it %s\n", srcs.v[i].path,
                    m->name, srcs.v[i].name);
            goto done;
        }
        if (m->lib_name) {
            fprintf(stderr, "mlwsvm lib build: %s is a manifest; only library.masm may be\n", srcs.v[i].path);
            goto done;
        }
    }
    for (uint32_t i = 0; i < man->npublics; i++) {
        bool found = false;
        for (size_t k = 0; k < mods.n && !found; k++) found = !strcmp(mods.v[k]->name, man->publics[i]);
        if (!found) {
            fprintf(stderr, "mlwsvm lib build: public module %s is not in %s\n", man->publics[i], dir);
            goto done;
        }
    }
    {
        StrBuf o = {0};
        if (out) sb_puts(&o, out);
        else sb_printf(&o, "%s-%s.mlwsl", man->lib_name, man->lib_version);
        ByteBuf bb = {0};
        lib_write(man, mods.v, (uint32_t)mods.n, &bb);
        FILE *f = fopen(o.s, "wb");
        bool ok = f && fwrite(bb.b, 1, bb.n, f) == bb.n;
        if (f) ok = fclose(f) == 0 && ok;
        if (ok) {
            printf("%s: library %s %s, %zu module%s (public: ", o.s, man->lib_name, man->lib_version, mods.n,
                   mods.n == 1 ? "" : "s");
            for (uint32_t i = 0; i < man->npublics; i++) printf("%s%s", i ? ", " : "", man->publics[i]);
            printf("%s)\n", man->npublics ? "" : "none");
            rc = 0;
        } else {
            fprintf(stderr, "mlwsvm lib build: cannot write %s\n", o.s);
            rc = 1;
        }
        bb_free(&bb);
        sb_free(&o);
    }
    goto done;
fail:
    fflush(stdout);
    fprintf(stderr, "error[%s]: %s\n", err.code, err.msg);
done:
    module_free(man);
    for (size_t i = 0; i < mods.n; i++) module_free(mods.v[i]);
    VEC_FREE(mods);
    for (size_t i = 0; i < srcs.n; i++) free(srcs.v[i].path), free(srcs.v[i].name);
    VEC_FREE(srcs);
    for (size_t i = 0; i < lp.n; i++) free(lp.v[i]);
    VEC_FREE(lp);
    free(mpath);
    return rc;
}

static int lib_info(const char *file) {
    MlwsError err;
    LibArchive *a = lib_read(file, &err);
    if (!a) {
        fprintf(stderr, "error[%s]: %s\n", err.code, err.msg);
        return 65;
    }
    const Module *m = a->manifest;
    printf("library %s %s\n", m->lib_name, m->lib_version);
    for (uint32_t i = 0; i < m->nrequires; i++) printf("requires %s %s\n", m->requires[i].lib, m->requires[i].constraint);
    for (uint32_t i = 0; i < a->nmods; i++)
        printf("module %s%s (%u bytes)\n", a->mods[i].name, lib_is_public(a, a->mods[i].name) ? " (public)" : "",
               a->mods[i].len);
    lib_free(a);
    return 0;
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int lib_list(const ToolEnv *env, const char *const *extra, size_t nextra) {
    VEC(const char *) dirs = {0};
    for (size_t i = 0; i < nextra; i++) VEC_PUSH(dirs, extra[i]);
    for (size_t i = 0; i < env->libpaths.n; i++) VEC_PUSH(dirs, env->libpaths.v[i]);
    for (size_t d = 0; d < dirs.n; d++) {
        char **names;
        size_t n;
        if (!plat_list_dir(dirs.v[d], &names, &n)) continue;
        qsort(names, n, sizeof *names, name_cmp);
        for (size_t k = 0; k < n; k++) {
            if (str_ends_with(names[k], ".mlwsl")) {
                char *p = path_join(dirs.v[d], names[k]);
                MlwsError err;
                LibArchive *a = lib_read(p, &err);
                if (a) printf("%-24s %-10s %s\n", a->manifest->lib_name, a->manifest->lib_version, p);
                else printf("%-24s %-10s %s (%s)\n", "?", "?", p, err.msg);
                lib_free(a);
                free(p);
            }
            free(names[k]);
        }
        free(names);
    }
    VEC_FREE(dirs);
    return 0;
}

int tool_lib(int argc, char **argv, const ToolEnv *env) {
    if (argc < 1) return lib_usage();
    const char *cmd = argv[0], *out = NULL, *arg = NULL;
    VEC(const char *) extra = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) VEC_PUSH(extra, argv[++i]);
        else if (!arg) arg = argv[i];
        else {
            VEC_FREE(extra);
            return lib_usage();
        }
    }
    int rc;
    if (!strcmp(cmd, "build") && arg) {
        ToolEnv e = *env; /* -L dirs first */
        VEC(char *) lp = {0};
        for (size_t i = 0; i < extra.n; i++) VEC_PUSH(lp, (char *)extra.v[i]);
        for (size_t i = 0; i < env->libpaths.n; i++) VEC_PUSH(lp, env->libpaths.v[i]);
        e.libpaths.v = lp.v;
        e.libpaths.n = lp.n;
        rc = lib_build(arg, out, &e);
        VEC_FREE(lp);
    } else if (!strcmp(cmd, "info") && arg) {
        rc = lib_info(arg);
    } else if (!strcmp(cmd, "list") && !arg) {
        rc = lib_list(env, extra.v, extra.n);
    } else {
        rc = lib_usage();
    }
    VEC_FREE(extra);
    return rc;
}
