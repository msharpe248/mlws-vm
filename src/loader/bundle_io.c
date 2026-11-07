/* bundle_io.c - see bundle.h. */
#include <stdio.h>

#include "bundle.h"

void bundle_write(const char *root, char *const *opts, uint32_t nopts, Module *const *mods, uint32_t nmods, ByteBuf *out) {
    size_t start = out->n;
    bb_bytes(out, MLWSB_MAGIC, 4);
    bb_u16(out, MLWSB_VERSION);
    bb_u16(out, 0);
    bb_str(out, root);
    bb_uleb(out, nopts);
    for (uint32_t i = 0; i < nopts; i++) bb_str(out, opts[i]);
    bb_uleb(out, nmods);
    for (uint32_t i = 0; i < nmods; i++) {
        ByteBuf b = {0};
        module_write(mods[i], &b);
        bb_str(out, mods[i]->name);
        bb_uleb(out, b.n);
        bb_bytes(out, b.b, b.n);
        bb_free(&b);
    }
    bb_u64(out, (uint64_t)(out->n - start));
    bb_bytes(out, MLWSB_TRAILER, 8);
}

/* The payload's length from a file's last bytes, or 0. */
static uint64_t trailer_len(const uint8_t *t) {
    if (memcmp(t + 8, MLWSB_TRAILER, 8)) return 0;
    ByteReader r = {t, 8, 0, false};
    return br_u64(&r);
}

static void bundle_free(Bundle *b) {
    free(b->path);
    free(b->data);
    free(b->root);
    for (uint32_t i = 0; i < b->nmods; i++) free(b->mods[i].name);
    free(b->mods);
    for (uint32_t i = 0; i < b->nopts; i++) free(b->opts[i]);
    free(b->opts);
    free(b);
}

Bundle *bundle_read(const char *exe, MlwsError *err) {
    err->code[0] = 0;
    FILE *f = fopen(exe, "rb");
    if (!f) return NULL;
    uint8_t t[MLWSB_TRAILER_LEN];
    uint64_t n = 0;
    if (!fseek(f, -MLWSB_TRAILER_LEN, SEEK_END) && fread(t, 1, sizeof t, f) == sizeof t) n = trailer_len(t);
    if (!n) { /* a plain mlwsvm, which is nearly always the case: nothing more is read */
        fclose(f);
        return NULL;
    }
    Bundle *b = xcalloc(1, sizeof *b);
    b->path = xstrdup(exe);
    b->len = (size_t)n;
    b->data = xmalloc(b->len + 1);
    const char *why = NULL;
    if (fseek(f, -(long)(MLWSB_TRAILER_LEN + n), SEEK_END) || fread(b->data, 1, b->len, f) != b->len) why = "a truncated payload";
    fclose(f);
    ByteReader r = {b->data, b->len, 0, false};
    char magic[4];
    if (!why && (!br_bytes(&r, magic, 4) || memcmp(magic, MLWSB_MAGIC, 4))) why = "not a bundle";
    else if (!why && br_u16(&r) != MLWSB_VERSION) why = "an unsupported bundle version";
    if (!why) {
        br_u16(&r);
        b->root = br_str(&r);
        uint64_t no = br_uleb(&r);
        if (r.err || !b->root || no > b->len - r.pos) why = "a bad header";
        else {
            b->opts = xcalloc(no + 1, sizeof *b->opts);
            for (; b->nopts < no && !why; b->nopts++)
                if (!(b->opts[b->nopts] = br_str(&r))) why = "a bad option";
        }
    }
    uint64_t nm = why ? 0 : br_uleb(&r);
    if (!why && (r.err || nm > (b->len - r.pos) / 2)) why = "a bad module count";
    if (!why) {
        b->mods = xcalloc(nm + 1, sizeof *b->mods);
        for (; b->nmods < nm && !why; b->nmods++) {
            LibEntry *e = &b->mods[b->nmods];
            e->name = br_str(&r);
            uint64_t ml = br_uleb(&r);
            if (r.err || !e->name || ml > b->len - r.pos) why = "a truncated module";
            e->off = (uint32_t)r.pos;
            e->len = (uint32_t)ml;
            r.pos += (size_t)ml;
        }
    }
    if (!why && bundle_find(b, b->root) < 0) why = "no root module";
    if (why) {
        mlws_err(err, "E_LOAD_FORMAT", "%s: %s", exe, why);
        bundle_free(b);
        return NULL;
    }
    return b;
}

int bundle_find(const Bundle *b, const char *module) {
    for (uint32_t i = 0; i < b->nmods; i++)
        if (!strcmp(b->mods[i].name, module)) return (int)i;
    return -1;
}

Module *bundle_module(const Bundle *b, uint32_t i, MlwsError *err) {
    Module *m = module_read(b->data + b->mods[i].off, b->mods[i].len, err);
    if (m && strcmp(m->name, b->mods[i].name)) {
        mlws_err(err, "E_LOAD_FORMAT", "%s: module %s is stored as %s", b->path, m->name, b->mods[i].name);
        module_free(m);
        return NULL;
    }
    return m;
}
