/* extensions.c - loading native code: extensions (.mlwsx) and FFI libraries,
 * with the trust checks of docs/EXTENDING.md ("Security").
 *
 * Native code is only loaded from a trusted extension directory (the default
 * one next to the mlwsvm executable, or one named with --ext-path or
 * MLWSVM_EXT_PATH). Each directory has a TRUSTED manifest, written by
 * `mlwsvm ext`:
 *
 *     ext NAME sha256:HEX FILE     an installed extension and its exact hash
 *     ffi LIBRARY                  a library `.extern` may load ("-": the program itself)
 *
 * The directory, the manifest and the extension must not be writable by other
 * users, and an extension must match its recorded hash. --unsafe-native skips
 * all of this (for development). */
#include "../../include/mlwsvm/mlwsvm.h"
#include "../core/sha256.h"
#include "../platform/platform.h"
#include "vm.h"

#define XSTR(x) STR(x)
#define STR(x) #x

typedef struct {
    bool ffi;
    char *name, *hash, *file;
} TrustEntry;

typedef VEC(TrustEntry) TrustVec;

static void trust_free(TrustVec *t) {
    for (size_t i = 0; i < t->n; i++) {
        free(t->v[i].name);
        free(t->v[i].hash);
        free(t->v[i].file);
    }
    VEC_FREE(*t);
}

/* Parse dir/TRUSTED; false (with `why`) if it is missing, unsafe or malformed. */
static bool read_manifest(const char *dir, TrustVec *out, char *why, size_t whylen) {
    char *path = path_join(dir, "TRUSTED");
    bool ok = plat_path_trusted(dir, why, whylen) && plat_path_trusted(path, why, whylen);
    char *text = ok ? read_file(path, NULL) : NULL;
    if (ok && !text) {
        snprintf(why, whylen, "cannot read %s", path);
        ok = false;
    }
    for (char *line = text; ok && line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char kind[8], a[256], b[256], c[1024];
        int n = sscanf(line, "%7s %255s %255s %1023s", kind, a, b, c);
        if (n <= 0 || kind[0] == '#') {
        } else if (n == 2 && !strcmp(kind, "ffi")) {
            TrustEntry e = {true, xstrdup(a), NULL, NULL};
            VEC_PUSH(*out, e);
        } else if (n == 4 && !strcmp(kind, "ext") && !strncmp(b, "sha256:", 7)) {
            TrustEntry e = {false, xstrdup(a), xstrdup(b + 7), xstrdup(c)};
            VEC_PUSH(*out, e);
        } else {
            snprintf(why, whylen, "%s: unrecognised line '%s'", path, line);
            ok = false;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    free(path);
    return ok;
}

static bool exists(const char *path) {
    char *r = plat_realpath(path);
    bool ok = r != NULL;
    free(r);
    return ok;
}

static void warn_unsafe(VM *vm) {
    if (vm->warned_unsafe) return;
    vm->warned_unsafe = true;
    fprintf(stderr, "mlwsvm: warning: --unsafe-native: loading native code without trust checks\n");
}

static ExtLib *find_lib(VM *vm, const char *name, bool ffi) {
    for (size_t i = 0; i < vm->extlibs.n; i++)
        if (vm->extlibs.v[i]->ffi == ffi && !strcmp(vm->extlibs.v[i]->name, name)) return vm->extlibs.v[i];
    return NULL;
}

static ExtLib *add_lib(VM *vm, const char *name, const char *path, void *handle, bool ffi) {
    ExtLib *l = xcalloc(1, sizeof *l);
    l->name = xstrdup(name);
    l->path = xstrdup(path);
    l->handle = handle;
    l->ffi = ffi;
    l->serial = plat_lock_new();
    VEC_PUSH(vm->extlibs, l);
    return l;
}

static void searched(StrBuf *b, VM *vm) {
    if (!vm->ext_paths.n) {
        sb_puts(b, "no extension directories are configured");
        return;
    }
    sb_puts(b, "looked in");
    for (size_t i = 0; i < vm->ext_paths.n; i++) sb_printf(b, "%s %s", i ? "," : "", vm->ext_paths.v[i]);
}

bool ext_load(VM *vm, const char *name, MlwsError *err) {
    if (find_lib(vm, name, false)) return true;
    size_t fl = strlen(name) + 8;
    char *file = xmalloc(fl);
    snprintf(file, fl, "%s.mlwsx", name);
    for (size_t d = 0; d < vm->ext_paths.n; d++) {
        const char *dir = vm->ext_paths.v[d];
        char *path = path_join(dir, file);
        if (!exists(path)) { /* not here */
            free(path);
            continue;
        }
        char why[1200];
        if (vm->unsafe_native) {
            warn_unsafe(vm);
        } else {
            TrustVec t = {0};
            bool ok = read_manifest(dir, &t, why, sizeof why) && plat_path_trusted(path, why, sizeof why);
            const TrustEntry *e = NULL;
            for (size_t i = 0; ok && i < t.n; i++)
                if (!t.v[i].ffi && !strcmp(t.v[i].name, name)) e = &t.v[i];
            char hex[65];
            if (ok && !e) snprintf(why, sizeof why, "%s is not in %s/TRUSTED (install it with `mlwsvm ext install`)", file, dir);
            else if (ok && strcmp(e->file, file) != 0) snprintf(why, sizeof why, "TRUSTED names %s for %s", e->file, name);
            else if (ok && !sha256_file_hex(path, hex)) snprintf(why, sizeof why, "cannot read %s", path);
            else if (ok && strcmp(hex, e->hash) != 0)
                snprintf(why, sizeof why, "%s has changed since it was installed (its SHA-256 no longer matches TRUSTED)", path);
            ok = ok && e && !strcmp(e->file, file) && !strcmp(hex, e->hash);
            trust_free(&t);
            if (!ok) {
                mlws_err(err, "E_LINK_UNTRUSTED", "extension %s: %s", name, why);
                free(path);
                free(file);
                return false;
            }
        }
        char dlerr[512];
        void *h = plat_dlopen(path, dlerr, sizeof dlerr);
        void *init = h ? plat_dlsym(h, XSTR(MLWSVM_EXTENSION_INIT)) : NULL;
        bool ok = false;
        if (!h) mlws_err(err, "E_LINK_EXT", "extension %s: cannot load %s: %s", name, path, dlerr);
        else if (!init)
            mlws_err(err, "E_LINK_EXT", "extension %s: %s does not export %s (built for another API version?)", name,
                     path, XSTR(MLWSVM_EXTENSION_INIT));
        else ok = ext_run_init(vm, add_lib(vm, name, path, h, false), init, err);
        free(path);
        free(file);
        return ok;
    }
    StrBuf b = {0};
    searched(&b, vm);
    mlws_err(err, "E_LINK_EXT", "extension %s is not installed (%s)", name, b.s);
    sb_free(&b);
    free(file);
    return false;
}

/* Which of a library spec's alternatives ("libm.so.6|libm") are allowed. */
static bool ffi_allowed(VM *vm, const char *alt, char *why, size_t whylen) {
    if (vm->unsafe_native) {
        warn_unsafe(vm);
        return true;
    }
    const char *want = *alt ? alt : "-";
    bool found = false;
    snprintf(why, whylen, "no trusted extension directory allows it");
    for (size_t d = 0; d < vm->ext_paths.n && !found; d++) {
        TrustVec t = {0};
        char w[1200];
        if (read_manifest(vm->ext_paths.v[d], &t, w, sizeof w)) {
            for (size_t i = 0; i < t.n; i++) found = found || (t.v[i].ffi && !strcmp(t.v[i].name, want));
        } else if (exists(vm->ext_paths.v[d])) {
            snprintf(why, whylen, "%s", w); /* a directory that exists but cannot be trusted: say so */
        }
        trust_free(&t);
    }
    return found;
}

ExtLib *ext_ffi_library(VM *vm, const char *spec, MlwsError *err) {
    ExtLib *l = find_lib(vm, spec, true);
    if (l) return l;
    char why[1200] = "";
    StrBuf tried = {0};
    bool any_allowed = false;
    const char *s = spec;
    do { /* alternatives separated by '|'; "" is the program's own symbols */
        const char *bar = strchr(s, '|');
        size_t n = bar ? (size_t)(bar - s) : strlen(s);
        char *alt = xstrndup(s, n);
        if (ffi_allowed(vm, alt, why, sizeof why)) {
            any_allowed = true;
            if (!*alt) {
                free(alt);
                sb_free(&tried);
                return add_lib(vm, spec, "", NULL, true);
            }
            char dlerr[512];
            void *h = plat_dlopen(alt, dlerr, sizeof dlerr);
            if (!h && !strchr(alt, '/')) { /* "libm" -> "libm.dylib" / "libm.so" */
                char *with = xmalloc(n + 16);
                snprintf(with, n + 16, "%s%s", alt, plat_dl_suffix());
                h = plat_dlopen(with, dlerr, sizeof dlerr);
                free(with);
            }
            if (h) {
                l = add_lib(vm, spec, alt, h, true);
                free(alt);
                sb_free(&tried);
                return l;
            }
            sb_printf(&tried, "%s%s: %s", tried.n ? "; " : "", alt, dlerr);
        }
        free(alt);
        s = bar ? bar + 1 : NULL;
    } while (s);
    if (!any_allowed)
        mlws_err(err, "E_LINK_UNTRUSTED",
                 "FFI library \"%s\" is not allowed: %s (allow it with `mlwsvm ext allow-ffi %s`)", spec, why,
                 *spec ? spec : "-");
    else mlws_err(err, "E_LINK_FFI", "cannot load FFI library \"%s\" (%s)", spec, tried.s ? tried.s : "");
    sb_free(&tried);
    return NULL;
}

void ffi_free_data(void *data);

void ext_free_all(VM *vm) {
    for (size_t i = 0; i < vm->foreign_natives.n; i++) {
        NativeDef *nd = vm->foreign_natives.v[i];
        if (nd->lib && nd->lib->ffi) ffi_free_data(nd->data);
        free((char *)nd->name);
        free((char *)nd->doc);
        free(nd);
    }
    VEC_FREE(vm->foreign_natives);
    /* Libraries stay loaded: unloading code that may have started threads or
     * registered callbacks is not safe in general. */
    for (size_t i = 0; i < vm->extlibs.n; i++) {
        ExtLib *l = vm->extlibs.v[i];
        free(l->name);
        free(l->path);
        plat_lock_free(l->serial);
        free(l);
    }
    VEC_FREE(vm->extlibs);
    for (size_t i = 0; i < vm->ext_paths.n; i++) free(vm->ext_paths.v[i]);
    VEC_FREE(vm->ext_paths);
    for (size_t i = 0; i < vm->ffi_types.cap; i++)
        if (vm->ffi_types.keys && vm->ffi_types.keys[i]) {
            MlwsForeignType *t = vm->ffi_types.vals[i];
            free((char *)t->name);
            free(t);
        }
    smap_free(&vm->ffi_types);
}
