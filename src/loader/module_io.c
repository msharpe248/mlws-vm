/* module_io.c - .mlwso serialisation and Module helpers. Layout documented in
 * docs/BINARY_FORMAT.md; keep the two in sync. */
#include "../core/module.h"

void module_free(Module *m) {
    if (!m) return;
    free(m->name);
    free(m->source);
    for (uint32_t i = 0; i < m->nimports; i++) free(m->imports[i]);
    free(m->imports);
    for (uint32_t i = 0; i < m->nconsts; i++) free(m->consts[i].data);
    free(m->consts);
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        free(m->funcs[i].name);
        free(m->funcs[i].lines);
        free(m->funcs[i].handlers);
    }
    free(m->funcs);
    for (uint32_t i = 0; i < m->nsyms; i++) free(m->syms[i].name);
    free(m->syms);
    for (uint32_t i = 0; i < m->ngrefs; i++) free(m->grefs[i].sym);
    free(m->grefs);
    for (uint32_t i = 0; i < m->nnatives; i++) free(m->natives[i]);
    free(m->natives);
    for (uint32_t i = 0; i < m->nstructs; i++) {
        MStruct *st = &m->structs[i];
        free(st->name);
        for (uint32_t k = 0; k < st->nfields; k++) free(st->fields[k]);
        free(st->fields);
        free(st->types);
    }
    free(m->structs);
    for (uint32_t i = 0; i < m->nfrefs; i++) free(m->frefs[i].field);
    free(m->frefs);
    free(m->gdata);
    free(m->gtags);
    free(m->code);
    free(m);
}

int module_find_export_func(const Module *m, const char *name) {
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        const MFunc *f = &m->funcs[i];
        if ((f->flags & MF_EXPORT) && !(f->flags & MF_IMPORT) && strcmp(f->name, name) == 0)
            return (int)i;
    }
    return -1;
}

int module_find_export_sym(const Module *m, const char *name) {
    for (uint32_t i = 0; i < m->nsyms; i++) {
        if ((m->syms[i].flags & MS_EXPORT) && strcmp(m->syms[i].name, name) == 0) return (int)i;
    }
    return -1;
}

int module_find_export_struct(const Module *m, const char *name) {
    for (uint32_t i = 0; i < m->nstructs; i++) {
        const MStruct *st = &m->structs[i];
        if ((st->flags & MSF_EXPORT) && !(st->flags & MSF_IMPORT) && strcmp(st->name, name) == 0) return (int)i;
    }
    return -1;
}

void module_write(const Module *m, ByteBuf *b) {
    bb_bytes(b, MLWSO_MAGIC, 4);
    bb_u16(b, MLWSO_VERSION);
    bb_u16(b, 0);
    bb_str(b, m->name);
    bb_str(b, m->source);
    bb_uleb(b, m->nimports);
    for (uint32_t i = 0; i < m->nimports; i++) bb_str(b, m->imports[i]);
    bb_uleb(b, m->nconsts);
    for (uint32_t i = 0; i < m->nconsts; i++) {
        bb_u8(b, m->consts[i].kind);
        bb_uleb(b, m->consts[i].len);
        bb_bytes(b, m->consts[i].data, m->consts[i].len);
    }
    bb_uleb(b, m->nfuncs);
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        const MFunc *f = &m->funcs[i];
        bb_str(b, f->name);
        bb_uleb(b, f->flags);
        if (f->flags & MF_IMPORT) {
            bb_uleb(b, f->import_idx);
            continue;
        }
        bb_u8(b, f->nparams);
        bb_bytes(b, f->params, f->nparams);
        bb_u8(b, f->nrets);
        bb_bytes(b, f->rets, f->nrets);
        for (int k = 0; k < NBANKS; k++) bb_uleb(b, f->nregs[k]);
        bb_uleb(b, f->code_off);
        bb_uleb(b, f->code_len);
        bb_uleb(b, f->ninsns);
        for (uint32_t k = 0; k < f->ninsns; k++) bb_uleb(b, f->lines ? f->lines[k] : 0);
        bb_uleb(b, f->nhandlers);
        for (uint32_t k = 0; k < f->nhandlers; k++) {
            bb_uleb(b, f->handlers[k].start);
            bb_uleb(b, f->handlers[k].end);
            bb_uleb(b, f->handlers[k].target);
        }
    }
    bb_uleb(b, m->nsyms);
    for (uint32_t i = 0; i < m->nsyms; i++) {
        bb_str(b, m->syms[i].name);
        bb_uleb(b, m->syms[i].offset);
        bb_uleb(b, m->syms[i].size);
        bb_uleb(b, m->syms[i].flags);
    }
    bb_uleb(b, m->ngrefs);
    for (uint32_t i = 0; i < m->ngrefs; i++) {
        const MGref *g = &m->grefs[i];
        bb_u8(b, g->kind);
        if (g->kind == GREF_LOCAL) {
            bb_uleb(b, g->offset);
        } else {
            bb_uleb(b, g->import_idx);
            bb_str(b, g->sym);
        }
    }
    bb_uleb(b, m->nnatives);
    for (uint32_t i = 0; i < m->nnatives; i++) bb_str(b, m->natives[i]);
    bb_uleb(b, m->nstructs);
    for (uint32_t i = 0; i < m->nstructs; i++) {
        const MStruct *st = &m->structs[i];
        bb_str(b, st->name);
        bb_uleb(b, st->flags);
        if (st->flags & MSF_BUILTIN) continue;
        if (st->flags & MSF_IMPORT) {
            bb_uleb(b, st->import_idx);
            continue;
        }
        bb_uleb(b, st->nfields);
        for (uint32_t k = 0; k < st->nfields; k++) {
            bb_str(b, st->fields[k]);
            bb_u8(b, st->types[k]);
        }
    }
    bb_uleb(b, m->nfrefs);
    for (uint32_t i = 0; i < m->nfrefs; i++) {
        bb_uleb(b, m->frefs[i].struct_idx);
        bb_str(b, m->frefs[i].field);
    }
    bb_uleb(b, m->gsize);
    bb_bytes(b, m->gdata, m->gsize);
    bb_bytes(b, m->gtags, m->gsize / 8);
    bb_uleb(b, m->codelen);
    bb_bytes(b, m->code, m->codelen);
}

#define FAIL(...)                                                              \
    do {                                                                       \
        mlws_err(err, "E_LOAD_FORMAT", __VA_ARGS__);                             \
        module_free(m);                                                        \
        return NULL;                                                           \
    } while (0)
#define CHECK()                                                                \
    do {                                                                       \
        if (r.err) FAIL("truncated or malformed module");                      \
    } while (0)
/* Guard counts before allocating so a corrupt file can't request huge arrays. */
#define CHECK_COUNT(cnt, minbytes)                                             \
    do {                                                                       \
        CHECK();                                                               \
        if ((cnt) > (r.n - r.pos) / (minbytes)) FAIL("count out of range");    \
    } while (0)

Module *module_read(const uint8_t *data, size_t len, MlwsError *err) {
    ByteReader r = {data, len, 0, false};
    Module *m = xcalloc(1, sizeof *m);
    char magic[4];
    if (!br_bytes(&r, magic, 4) || memcmp(magic, MLWSO_MAGIC, 4) != 0) FAIL("not an MLWSVM module (bad magic)");
    uint16_t ver = br_u16(&r);
    br_u16(&r);
    CHECK();
    if (ver != MLWSO_VERSION) FAIL("unsupported module version %u (expected %u)", ver, MLWSO_VERSION);
    m->name = br_str(&r);
    m->source = br_str(&r);
    CHECK();

    uint64_t n = br_uleb(&r);
    CHECK_COUNT(n, 1);
    m->nimports = (uint32_t)n;
    m->imports = xcalloc(n, sizeof(char *));
    for (uint32_t i = 0; i < m->nimports; i++) m->imports[i] = br_str(&r);
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 2);
    m->nconsts = (uint32_t)n;
    m->consts = xcalloc(n, sizeof(MConst));
    for (uint32_t i = 0; i < m->nconsts; i++) {
        MConst *c = &m->consts[i];
        c->kind = br_u8(&r);
        uint64_t l = br_uleb(&r);
        CHECK_COUNT(l, 1);
        if (c->kind != CONST_STRING) FAIL("unknown constant kind %u", c->kind);
        c->len = (uint32_t)l;
        c->data = xmalloc(l + 1);
        br_bytes(&r, c->data, l);
        c->data[l] = 0;
    }
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 2);
    m->nfuncs = (uint32_t)n;
    m->funcs = xcalloc(n, sizeof(MFunc));
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        MFunc *f = &m->funcs[i];
        f->name = br_str(&r);
        f->flags = (uint32_t)br_uleb(&r);
        CHECK();
        if (f->flags & MF_IMPORT) {
            f->import_idx = (uint32_t)br_uleb(&r);
            CHECK();
            if (f->import_idx >= m->nimports) FAIL("function %s: bad import index", f->name);
            continue;
        }
        f->nparams = br_u8(&r);
        if (f->nparams > MAX_PARAMS) FAIL("function %s: too many parameters", f->name);
        br_bytes(&r, f->params, f->nparams);
        f->nrets = br_u8(&r);
        if (f->nrets > MAX_PARAMS) FAIL("function %s: too many results", f->name);
        br_bytes(&r, f->rets, f->nrets);
        for (int k = 0; k < NBANKS; k++) {
            uint64_t nr = br_uleb(&r);
            if (nr > NREGS) FAIL("function %s: register count out of range", f->name);
            f->nregs[k] = (uint16_t)nr;
        }
        f->code_off = (uint32_t)br_uleb(&r);
        f->code_len = (uint32_t)br_uleb(&r);
        uint64_t ni = br_uleb(&r);
        CHECK_COUNT(ni, 1);
        f->ninsns = (uint32_t)ni;
        f->lines = xcalloc(ni, sizeof(uint32_t));
        for (uint32_t k = 0; k < f->ninsns; k++) f->lines[k] = (uint32_t)br_uleb(&r);
        uint64_t nh = br_uleb(&r);
        CHECK_COUNT(nh, 3);
        f->nhandlers = (uint32_t)nh;
        f->handlers = xcalloc(nh + 1, sizeof(MHandler));
        for (uint32_t k = 0; k < f->nhandlers; k++) {
            f->handlers[k].start = (uint32_t)br_uleb(&r);
            f->handlers[k].end = (uint32_t)br_uleb(&r);
            f->handlers[k].target = (uint32_t)br_uleb(&r);
        }
        CHECK();
        for (uint32_t k = 0; k < f->nhandlers; k++) {
            const MHandler *h = &f->handlers[k];
            if (h->start > h->end || h->end > f->ninsns || h->target >= f->ninsns)
                FAIL("function %s: exception handler out of range", f->name);
        }
        for (int k = 0; k < f->nparams; k++)
            if (f->params[k] == T_NONE || f->params[k] >= T__COUNT) FAIL("function %s: bad parameter type", f->name);
        for (int k = 0; k < f->nrets; k++)
            if (f->rets[k] == T_NONE || f->rets[k] >= T__COUNT) FAIL("function %s: bad result type", f->name);
    }

    n = br_uleb(&r);
    CHECK_COUNT(n, 4);
    m->nsyms = (uint32_t)n;
    m->syms = xcalloc(n, sizeof(MSym));
    for (uint32_t i = 0; i < m->nsyms; i++) {
        m->syms[i].name = br_str(&r);
        m->syms[i].offset = (uint32_t)br_uleb(&r);
        m->syms[i].size = (uint32_t)br_uleb(&r);
        m->syms[i].flags = (uint32_t)br_uleb(&r);
    }
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 2);
    m->ngrefs = (uint32_t)n;
    m->grefs = xcalloc(n, sizeof(MGref));
    for (uint32_t i = 0; i < m->ngrefs; i++) {
        MGref *g = &m->grefs[i];
        g->kind = br_u8(&r);
        if (g->kind == GREF_LOCAL) {
            g->offset = (uint32_t)br_uleb(&r);
        } else if (g->kind == GREF_IMPORT) {
            g->import_idx = (uint32_t)br_uleb(&r);
            g->sym = br_str(&r);
            CHECK();
            if (g->import_idx >= m->nimports) FAIL("global reference: bad import index");
        } else {
            FAIL("unknown global reference kind %u", g->kind);
        }
    }
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 1);
    m->nnatives = (uint32_t)n;
    m->natives = xcalloc(n, sizeof(char *));
    for (uint32_t i = 0; i < m->nnatives; i++) m->natives[i] = br_str(&r);
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 2);
    m->nstructs = (uint32_t)n;
    m->structs = xcalloc(n + 1, sizeof(MStruct));
    for (uint32_t i = 0; i < m->nstructs; i++) {
        MStruct *st = &m->structs[i];
        st->name = br_str(&r);
        st->flags = (uint32_t)br_uleb(&r);
        CHECK();
        if (st->flags & MSF_BUILTIN) continue;
        if (st->flags & MSF_IMPORT) {
            st->import_idx = (uint32_t)br_uleb(&r);
            CHECK();
            if (st->import_idx >= m->nimports) FAIL("struct %s: bad import index", st->name);
            continue;
        }
        uint64_t nf = br_uleb(&r);
        CHECK_COUNT(nf, 2);
        st->nfields = (uint32_t)nf;
        st->fields = xcalloc(nf + 1, sizeof(char *));
        st->types = xcalloc(nf + 1, 1);
        for (uint32_t k = 0; k < st->nfields; k++) {
            st->fields[k] = br_str(&r);
            st->types[k] = br_u8(&r);
            CHECK();
            if (st->types[k] == T_NONE || st->types[k] >= T__COUNT) FAIL("struct %s: bad field type", st->name);
        }
    }

    n = br_uleb(&r);
    CHECK_COUNT(n, 2);
    m->nfrefs = (uint32_t)n;
    m->frefs = xcalloc(n + 1, sizeof(MFieldRef));
    for (uint32_t i = 0; i < m->nfrefs; i++) {
        m->frefs[i].struct_idx = (uint32_t)br_uleb(&r);
        m->frefs[i].field = br_str(&r);
        CHECK();
        if (m->frefs[i].struct_idx >= m->nstructs) FAIL("field reference: bad struct index");
    }

    n = br_uleb(&r);
    CHECK_COUNT(n, 1);
    if (n % 8) FAIL("global segment size must be a multiple of 8");
    m->gsize = (uint32_t)n;
    m->gdata = xcalloc(n, 1);
    m->gtags = xcalloc(n / 8, 1);
    br_bytes(&r, m->gdata, n);
    br_bytes(&r, m->gtags, n / 8);
    CHECK();

    n = br_uleb(&r);
    CHECK_COUNT(n, 1);
    m->codelen = (uint32_t)n;
    m->code = xmalloc(n);
    br_bytes(&r, m->code, n);
    CHECK();

    for (uint32_t i = 0; i < m->nfuncs; i++) {
        MFunc *f = &m->funcs[i];
        if (f->flags & MF_IMPORT) continue;
        if ((uint64_t)f->code_off + f->code_len > m->codelen) FAIL("function %s: code out of range", f->name);
    }
    for (uint32_t i = 0; i < m->nsyms; i++) {
        if ((uint64_t)m->syms[i].offset + m->syms[i].size > m->gsize) FAIL("symbol %s out of range", m->syms[i].name);
    }
    for (uint32_t i = 0; i < m->ngrefs; i++) {
        if (m->grefs[i].kind == GREF_LOCAL && m->grefs[i].offset > m->gsize) FAIL("global reference out of range");
    }
    return m;
}
