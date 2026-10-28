/* modcache.c - a cache of assembled modules, so that a program's .masm is
 * assembled once rather than at every run (docs/TOOLING.md, "Module cache").
 *
 * An entry is found by a SHA-256 of what decides the assembler's output: the
 * running executable (its path, size and modification time, so that a
 * rebuilt VM never reads an older one's entries), the default module name
 * and the source text. A compiler that writes the same program to a fresh
 * temporary file each time still hits. An entry holds the files the source
 * included, each with the SHA-256 of its contents, and what their
 * resolution depended on (the working directory, the source's directory and
 * the library path); a hit requires all of them to match. The module follows,
 * in the .mlwso format.
 *
 * The directory is $MLWSVM_CACHE, else $XDG_CACHE_HOME/mlwsvm, else
 * ~/.cache/mlwsvm; MLWSVM_NO_CACHE=1 or `run --no-cache` turns the cache
 * off. It is made private (0700), and used only if it passes the same trust
 * check as native code, because a module can declare foreign functions. An
 * entry is written to a temporary file and renamed into place, so readers
 * never see half of one. Past MODCACHE_MAX entries the oldest are removed. */
#include "vm.h"

#include "../asm/asm.h"
#include "../core/bytes.h"
#include "../core/sha256.h"
#include "../platform/platform.h"

#define MODCACHE_MAGIC "MLWC"
#define MODCACHE_VERSION 1
#define MODCACHE_MAX 512  /* entries; the oldest go when there are more */
#define MODCACHE_KEEP 384 /* how many remain after a clean-up */

/* The cache directory, created if need be, or NULL: off, or not trusted. */
static const char *cache_dir(VM *vm) {
    if (vm->cache_state) return vm->cache_state > 0 ? vm->cache_dir : NULL;
    vm->cache_state = -1;
    const char *off = getenv("MLWSVM_NO_CACHE");
    if (vm->no_cache || (off && *off && strcmp(off, "0"))) return NULL;
    const char *dir = getenv("MLWSVM_CACHE");
    char *d;
    if (dir && *dir) {
        d = xstrdup(dir);
    } else {
        const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        char *base;
        if (xdg && *xdg) base = xstrdup(xdg);
        else if (home && *home) base = path_join(home, ".cache");
        else return NULL;
        plat_mkdir(base); /* ~/.cache, as usual for it (0755) */
        d = path_join(base, "mlwsvm");
        free(base);
    }
    char why[512];
    if (!plat_mkdir_private(d) || !plat_path_trusted(d, why, sizeof why)) {
        free(d);
        return NULL;
    }
    vm->cache_dir = d;
    vm->cache_state = 1;
    return d;
}

static void hash_str(Sha256 *h, const char *s) { sha256_update(h, s, strlen(s) + 1); }

static bool file_hash(const char *path, uint8_t out[32]) {
    size_t n;
    char *data = read_file(path, &n);
    if (!data) return false;
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, data, n);
    sha256_final(&h, out);
    free(data);
    return true;
}

/* What the resolution of .include depended on: the working directory, the
 * source's directory and the library path. */
static void resolution_context(VM *vm, const char *path, ByteBuf *b) {
    char *cwd = plat_getcwd(), *dir = path_dirname(path), *rdir = plat_realpath(dir);
    bb_str(b, cwd ? cwd : "");
    bb_str(b, rdir ? rdir : dir);
    bb_uleb(b, vm->libpaths.n);
    for (size_t i = 0; i < vm->libpaths.n; i++) bb_str(b, vm->libpaths.v[i]);
    free(cwd);
    free(dir);
    free(rdir);
}

/* The entry for this source, if every file it included is unchanged and was
 * resolved the same way. */
static Module *cache_read(VM *vm, const char *entry, const char *path) {
    size_t len;
    char *data = read_file(entry, &len);
    if (!data) return NULL;
    ByteReader r = {(const uint8_t *)data, len, 0, false};
    char magic[4];
    br_bytes(&r, magic, 4);
    bool ok = !r.err && !memcmp(magic, MODCACHE_MAGIC, 4) && br_u16(&r) == MODCACHE_VERSION;
    uint64_t ndeps = ok ? br_uleb(&r) : 0;
    if (ok && ndeps) {
        ByteBuf want = {0}; /* the context, as written: compared byte for byte */
        resolution_context(vm, path, &want);
        ok = r.n - r.pos >= want.n && !memcmp(r.b + r.pos, want.b, want.n);
        r.pos += ok ? want.n : 0;
        bb_free(&want);
    }
    for (uint64_t i = 0; ok && i < ndeps; i++) {
        char *dep = br_str(&r);
        uint8_t h[32], now[32];
        ok = dep && br_bytes(&r, h, 32) && file_hash(dep, now) && !memcmp(h, now, 32);
        free(dep);
    }
    Module *m = NULL;
    if (ok && !r.err) {
        MlwsError e;
        m = module_read(r.b + r.pos, r.n - r.pos, &e);
        if (m) { /* the backtraces' file is this one, wherever the entry was made */
            free(m->source);
            m->source = xstrdup(path);
        }
    }
    free(data);
    return m;
}

typedef struct {
    int64_t mtime_ns;
    const char *name;
} Entry;

/* Oldest first; entries made in the same nanosecond, by name. */
static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->mtime_ns != y->mtime_ns) return x->mtime_ns < y->mtime_ns ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* More than MODCACHE_MAX entries: remove the oldest, down to MODCACHE_KEEP. */
static void cache_trim(const char *dir) {
    char **names;
    size_t n;
    if (!plat_list_dir(dir, &names, &n)) return;
    Entry *es = xcalloc(n + 1, sizeof *es);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!str_ends_with(names[i], ".mlwsc")) continue;
        char *p = path_join(dir, names[i]);
        PlatStat st;
        int e;
        if (plat_stat(p, false, &st, &e)) es[k++] = (Entry){st.mtime_ns, names[i]};
        free(p);
    }
    if (k > MODCACHE_MAX) {
        qsort(es, k, sizeof *es, entry_cmp);
        for (size_t i = 0; i < k - MODCACHE_KEEP; i++) {
            char *p = path_join(dir, es[i].name);
            int e;
            plat_remove(p, &e);
            free(p);
        }
    }
    free(es);
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void cache_write(VM *vm, const char *dir, const char *entry, const char *path, Module *m, char **files,
                        size_t nfiles) {
    ByteBuf b = {0};
    bb_bytes(&b, MODCACHE_MAGIC, 4);
    bb_u16(&b, MODCACHE_VERSION);
    bb_uleb(&b, nfiles ? nfiles - 1 : 0); /* files[0] is the source itself */
    if (nfiles > 1) resolution_context(vm, path, &b);
    for (size_t i = 1; i < nfiles; i++) {
        uint8_t h[32];
        if (!file_hash(files[i], h)) {
            bb_free(&b);
            return;
        }
        bb_str(&b, files[i]);
        bb_bytes(&b, h, 32);
    }
    module_write(m, &b);
    char tmp[64];
    uint64_t rnd = 0;
    plat_random_bytes(&rnd, sizeof rnd);
    snprintf(tmp, sizeof tmp, "tmp-%016llx", (unsigned long long)rnd);
    char *tpath = path_join(dir, tmp);
    FILE *f = fopen(tpath, "wb");
    bool ok = f && fwrite(b.b, 1, b.n, f) == b.n;
    if (f) ok = fclose(f) == 0 && ok;
    int e;
    if (ok) ok = plat_rename(tpath, entry, &e);
    if (!ok) plat_remove(tpath, &e);
    free(tpath);
    bb_free(&b);
    if (ok) cache_trim(dir);
}

Module *modcache_assemble(VM *vm, const char *path, const char *default_name, MlwsError *err) {
    const char *const *lp = (const char *const *)vm->libpaths.v;
    const char *dir = cache_dir(vm);
    char *exe = dir ? plat_exe_path(NULL) : NULL;
    PlatStat st;
    int e;
    size_t len;
    char *text = exe && plat_stat(exe, true, &st, &e) ? read_file(path, &len) : NULL;
    if (!text) {
        free(exe);
        return assemble_file(path, default_name, lp, vm->libpaths.n, err);
    }
    Sha256 h;
    sha256_init(&h);
    hash_str(&h, "mlwsvm module cache");
    hash_str(&h, exe);
    sha256_update(&h, &st.size, sizeof st.size);
    sha256_update(&h, &st.mtime_ns, sizeof st.mtime_ns);
    hash_str(&h, default_name ? default_name : "main");
    sha256_update(&h, text, len);
    uint8_t key[32];
    sha256_final(&h, key);
    free(text);
    free(exe);
    char name[80];
    for (int i = 0; i < 32; i++) snprintf(name + 2 * i, 3, "%02x", key[i]);
    strcat(name, ".mlwsc");
    char *entry = path_join(dir, name);
    Module *m = cache_read(vm, entry, path);
    if (!m) {
        char **files = NULL;
        size_t nfiles = 0;
        m = assemble_file_deps(path, default_name, lp, vm->libpaths.n, &files, &nfiles, err);
        if (m) cache_write(vm, dir, entry, path, m, files, nfiles);
        for (size_t i = 0; i < nfiles; i++) free(files[i]);
        free(files);
    }
    free(entry);
    return m;
}
