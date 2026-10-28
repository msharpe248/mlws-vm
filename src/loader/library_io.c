/* library_io.c - see library.h. */
#include "library.h"

bool lib_owns(const char *lib, const char *module) {
    size_t n = strlen(lib);
    return !strncmp(lib, module, n) && (module[n] == 0 || module[n] == '.');
}

void lib_write(const Module *manifest, Module *const *mods, uint32_t nmods, ByteBuf *out) {
    bb_bytes(out, MLWSL_MAGIC, 4);
    bb_u16(out, MLWSL_VERSION);
    bb_u16(out, 0);
    ByteBuf mb = {0};
    module_write(manifest, &mb);
    bb_uleb(out, mb.n);
    bb_bytes(out, mb.b, mb.n);
    bb_free(&mb);
    bb_uleb(out, nmods);
    for (uint32_t i = 0; i < nmods; i++) {
        ByteBuf b = {0};
        module_write(mods[i], &b);
        bb_str(out, mods[i]->name);
        bb_uleb(out, b.n);
        bb_bytes(out, b.b, b.n);
        bb_free(&b);
    }
}

void lib_free(LibArchive *a) {
    if (!a) return;
    free(a->path);
    module_free(a->manifest);
    free(a->data);
    for (uint32_t i = 0; i < a->nmods; i++) free(a->mods[i].name);
    free(a->mods);
    free(a);
}

LibArchive *lib_read(const char *path, MlwsError *err) {
    size_t len;
    char *data = read_file(path, &len);
    if (!data) {
        mlws_err(err, "E_LOAD_NOT_FOUND", "cannot read library archive '%s'", path);
        return NULL;
    }
    LibArchive *a = xcalloc(1, sizeof *a);
    a->path = xstrdup(path);
    a->data = (uint8_t *)data;
    a->len = len;
    ByteReader r = {a->data, len, 0, false};
    char magic[4];
    const char *why = NULL;
    MlwsError me;
    if (!br_bytes(&r, magic, 4) || memcmp(magic, MLWSL_MAGIC, 4)) why = "not a library archive";
    else if (br_u16(&r) != MLWSL_VERSION) why = "an unsupported archive version";
    br_u16(&r);
    uint64_t mlen = why ? 0 : br_uleb(&r);
    if (!why && (r.err || mlen > len - r.pos)) why = "a truncated manifest";
    if (!why) {
        a->manifest = module_read(a->data + r.pos, (size_t)mlen, &me);
        r.pos += (size_t)mlen;
        if (!a->manifest) why = me.msg;
        else if (!a->manifest->lib_name || !a->manifest->lib_version) why = "a manifest without .library";
        else semver_parse(a->manifest->lib_version, &a->version);
    }
    uint64_t n = why ? 0 : br_uleb(&r);
    if (!why && (r.err || n > (len - r.pos) / 2)) why = "a bad module count";
    if (!why) {
        a->nmods = (uint32_t)n;
        a->mods = xcalloc(n + 1, sizeof(LibEntry));
        for (uint32_t i = 0; i < a->nmods && !why; i++) {
            LibEntry *e = &a->mods[i];
            e->name = br_str(&r);
            uint64_t ml = br_uleb(&r);
            if (r.err || !e->name || ml > len - r.pos) why = "a truncated module";
            else if (!lib_owns(a->manifest->lib_name, e->name)) why = "a module outside its library's name";
            e->off = (uint32_t)r.pos;
            e->len = (uint32_t)ml;
            r.pos += (size_t)ml;
        }
        if (!why && r.pos != len) why = "trailing bytes";
    }
    if (why) {
        mlws_err(err, "E_LOAD_FORMAT", "%s: %s", path, why);
        lib_free(a);
        return NULL;
    }
    return a;
}

Module *lib_module(const LibArchive *a, uint32_t i, MlwsError *err) {
    Module *m = module_read(a->data + a->mods[i].off, a->mods[i].len, err);
    if (m && strcmp(m->name, a->mods[i].name)) {
        mlws_err(err, "E_LOAD_FORMAT", "%s: module %s is stored as %s", a->path, m->name, a->mods[i].name);
        module_free(m);
        return NULL;
    }
    return m;
}

int lib_find(const LibArchive *a, const char *module) {
    for (uint32_t i = 0; i < a->nmods; i++)
        if (!strcmp(a->mods[i].name, module)) return (int)i;
    return -1;
}

bool lib_is_public(const LibArchive *a, const char *module) {
    for (uint32_t i = 0; i < a->manifest->npublics; i++)
        if (!strcmp(a->manifest->publics[i], module)) return true;
    return false;
}
