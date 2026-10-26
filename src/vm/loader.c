/* loader.c - finds modules on the library path, assembles or reads them,
 * loads imports recursively (rejecting cycles), links symbols, pre-decodes
 * code and runs the verifier. See docs/ASSEMBLY.md ("Modules and libraries"). */
#include "../asm/asm.h"
#include "../platform/platform.h"
#include "vm.h"

static char *module_rel_path(const char *name) {
    char *rel = xstrdup(name);
    for (char *c = rel; *c; c++)
        if (*c == '.') *c = '/';
    return rel;
}

/* Search the library path for name -> dir/a/b.masm or dir/a/b.mlwso. When both
 * exist in the same directory, the newer one wins. */
static char *find_module_file(VM *vm, const char *name) {
    char *rel = module_rel_path(name);
    char *found = NULL;
    for (size_t i = 0; i < vm->libpaths.n && !found; i++) {
        char *base = path_join(vm->libpaths.v[i], rel);
        StrBuf a = {0}, b = {0};
        sb_printf(&a, "%s.masm", base);
        sb_printf(&b, "%s.mlwso", base);
        int64_t ta = plat_mtime(a.s), tb = plat_mtime(b.s);
        if (ta >= 0 && ta >= tb) found = xstrdup(a.s);
        else if (tb >= 0) found = xstrdup(b.s);
        sb_free(&a);
        sb_free(&b);
        free(base);
    }
    free(rel);
    return found;
}

static Module *read_module_file(VM *vm, const char *path, const char *default_name, MlwsError *err) {
    if (str_ends_with(path, ".mlwso")) {
        size_t len;
        char *data = read_file(path, &len);
        if (!data) {
            mlws_err(err, "E_LOAD_NOT_FOUND", "cannot read module file '%s'", path);
            return NULL;
        }
        Module *m = module_read((const uint8_t *)data, len, err);
        free(data);
        if (!m) {
            char msg[sizeof err->msg];
            snprintf(msg, sizeof msg, "%s: %s", path, err->msg);
            snprintf(err->msg, sizeof err->msg, "%s", msg);
        }
        return m;
    }
    return assemble_file(path, default_name, (const char *const *)vm->libpaths.v, vm->libpaths.n, err);
}

static bool resolve_operand(LModule *lm, Function *f, Operand *o, MlwsError *err) {
    Module *m = lm->m;
    bool words = false;
    switch (o->mode) {
    case OM_DISP: case OM_IDX: case OM_GLOBAL: case OM_GLOBALIDX: case OM_MIND: words = true; break;
    default: break;
    }
    if (o->imm > ((int64_t)1 << 40) || o->imm < -((int64_t)1 << 40) || o->scale > ((uint64_t)1 << 32)) {
        if (o->mode != OM_IMM)
            return mlws_err(err, "E_VERIFY", "%s: displacement or scale out of range in %s", m->name, f->name);
    }
    if (o->mode != OM_IMM) {
        o->bdisp = words ? o->imm * 8 : o->imm;
        o->bscale = (int64_t)(words ? o->scale * 8 : o->scale);
    }
    switch (o->mode) {
    case OM_CONST:
        if (o->index >= m->nconsts) return mlws_err(err, "E_VERIFY", "%s: constant index out of range in %s", m->name, f->name);
        o->rv.ptr = lm->consts[o->index];
        break;
    case OM_FUNC:
        if (o->index >= m->nfuncs) return mlws_err(err, "E_VERIFY", "%s: function index out of range in %s", m->name, f->name);
        o->rv.ptr = lm->funcs[o->index].target;
        break;
    case OM_NATIVE:
        if (o->index >= m->nnatives) return mlws_err(err, "E_VERIFY", "%s: native index out of range in %s", m->name, f->name);
        o->rv.ptr = (void *)lm->natives[o->index];
        break;
    case OM_GLOBAL: case OM_GLOBALB: case OM_GLOBALIDX: case OM_GLOBALIDXB:
        if (o->index >= m->ngrefs) return mlws_err(err, "E_VERIFY", "%s: global index out of range in %s", m->name, f->name);
        o->rv.gbase = lm->grefs[o->index];
        break;
    case OM_STRUCT:
        if (o->index >= m->nstructs) return mlws_err(err, "E_VERIFY", "%s: struct index out of range in %s", m->name, f->name);
        o->rv.ptr = (void *)lm->structs[o->index];
        break;
    case OM_FIELD:
        if (o->index >= m->nfrefs) return mlws_err(err, "E_VERIFY", "%s: field index out of range in %s", m->name, f->name);
        o->rv.ptr = &lm->frefs[o->index];
        break;
    case OM_LABEL:
        if (o->index >= f->ncode) return mlws_err(err, "E_VERIFY", "%s: branch target out of range in %s", m->name, f->name);
        o->rv.target = (uint32_t)o->index;
        break;
    case OM_MIND: case OM_MINDB:
        return resolve_operand(lm, f, o->inner, err);
    default: break;
    }
    return true;
}

static bool decode_function(LModule *lm, Function *f, MlwsError *err) {
    const MFunc *mf = f->mf;
    Module *m = lm->m;
    ByteReader r = {m->code + mf->code_off, mf->code_len, 0, false};
    VEC(Insn) code = {0};
    while (r.pos < r.n) {
        Insn in;
        if (!decode_insn(&r, &in, err)) {
            char msg[sizeof err->msg];
            snprintf(msg, sizeof msg, "%s.%s: %s", m->name, mf->name, err->msg);
            snprintf(err->msg, sizeof err->msg, "%s", msg);
            for (size_t i = 0; i < code.n; i++) insn_free(&code.v[i]);
            VEC_FREE(code);
            return false;
        }
        VEC_PUSH(code, in);
    }
    f->code = code.v;
    f->ncode = (uint32_t)code.n;
    if (f->ncode != mf->ninsns)
        return mlws_err(err, "E_LOAD_FORMAT", "%s.%s: instruction count mismatch (%u decoded, %u declared)", m->name,
                      mf->name, f->ncode, mf->ninsns);
    for (uint32_t i = 0; i < f->ncode; i++)
        for (int k = 0; k < f->code[i].nops; k++)
            if (!resolve_operand(lm, f, &f->code[i].ops[k], err)) return false;
    return true;
}

static bool link_module(VM *vm, LModule *lm, MlwsError *err) {
    Module *m = lm->m;
    lm->consts = xcalloc(m->nconsts + 1, sizeof(ObjString *));
    for (uint32_t i = 0; i < m->nconsts; i++) lm->consts[i] = str_new_immortal(m->consts[i].data, m->consts[i].len);
    lm->natives = xcalloc(m->nnatives + 1, sizeof(NativeDef *));
    for (uint32_t i = 0; i < m->nnatives; i++) {
        void *v;
        if (!smap_get(&vm->natives, m->natives[i], &v))
            return mlws_err(err, "E_LINK_NATIVE", "module %s uses unknown native function \"%s\"", m->name, m->natives[i]);
        lm->natives[i] = v;
    }
    lm->grefs = xcalloc(m->ngrefs + 1, sizeof(uint64_t));
    for (uint32_t i = 0; i < m->ngrefs; i++) {
        MGref *g = &m->grefs[i];
        if (g->kind == GREF_LOCAL) {
            lm->grefs[i] = PTR_MAKE(SEG_GLOBAL0 + (uint32_t)lm->index, 1, g->offset);
            continue;
        }
        void *v;
        smap_get(&vm->modmap, m->imports[g->import_idx], &v);
        LModule *dep = v;
        int si = module_find_export_sym(dep->m, g->sym);
        if (si < 0)
            return mlws_err(err, "E_LINK_UNDEF", "module %s does not export data symbol '%s' (used by %s)", dep->m->name,
                          g->sym, m->name);
        lm->grefs[i] = PTR_MAKE(SEG_GLOBAL0 + (uint32_t)dep->index, 1, dep->m->syms[si].offset);
    }
    lm->sdefs = xcalloc(m->nstructs + 1, sizeof(StructDef));
    lm->structs = xcalloc(m->nstructs + 1, sizeof(StructDef *));
    for (uint32_t i = 0; i < m->nstructs; i++) {
        const MStruct *st = &m->structs[i];
        if (!(st->flags & MSF_IMPORT)) {
            StructDef *d = &lm->sdefs[i];
            d->name = st->name;
            d->module = m->name;
            d->nfields = st->nfields;
            d->fields = st->fields;
            d->types = st->types;
            lm->structs[i] = d;
            continue;
        }
        void *v;
        smap_get(&vm->modmap, m->imports[st->import_idx], &v);
        LModule *dep = v;
        int si = module_find_export_struct(dep->m, st->name);
        if (si < 0)
            return mlws_err(err, "E_LINK_UNDEF", "module %s does not export struct '%s' (used by %s)", dep->m->name,
                            st->name, m->name);
        lm->structs[i] = dep->structs[si];
    }
    lm->frefs = xcalloc(m->nfrefs + 1, sizeof(FieldRef));
    for (uint32_t i = 0; i < m->nfrefs; i++) {
        const StructDef *d = lm->structs[m->frefs[i].struct_idx];
        uint32_t k;
        for (k = 0; k < d->nfields && strcmp(d->fields[k], m->frefs[i].field) != 0; k++) {}
        if (k == d->nfields)
            return mlws_err(err, "E_LINK_UNDEF", "struct %s.%s has no field '%s' (used by %s)", d->module, d->name,
                            m->frefs[i].field, m->name);
        lm->frefs[i].def = d;
        lm->frefs[i].index = k;
    }
    lm->funcs = xcalloc(m->nfuncs + 1, sizeof(Function));
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        Function *f = &lm->funcs[i];
        f->mf = &m->funcs[i];
        f->name = m->funcs[i].name;
        f->mod = lm;
        if (!(f->mf->flags & MF_IMPORT)) {
            f->target = f;
            continue;
        }
        void *v;
        smap_get(&vm->modmap, m->imports[f->mf->import_idx], &v);
        LModule *dep = v;
        int fi = module_find_export_func(dep->m, f->name);
        if (fi < 0)
            return mlws_err(err, "E_LINK_UNDEF", "module %s does not export function '%s' (called from %s)", dep->m->name,
                          f->name, m->name);
        f->target = &dep->funcs[fi];
    }
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        Function *f = &lm->funcs[i];
        if (f->target == f && !decode_function(lm, f, err)) return false;
    }
    return vm_verify_module(vm, lm, err);
}

static LModule *register_module(VM *vm, Module *m, const char *path, MlwsError *err) {
    if (smap_has(&vm->modmap, m->name)) {
        mlws_err(err, "E_LOAD_NAME", "a module named '%s' is already loaded (%s)", m->name, path);
        module_free(m);
        return NULL;
    }
    if (vm->mods.n >= SEG_RAW0 - SEG_GLOBAL0) {
        mlws_err(err, "E_LOAD_LIMIT", "too many modules");
        module_free(m);
        return NULL;
    }
    LModule *lm = xcalloc(1, sizeof *lm);
    lm->m = m;
    lm->path = xstrdup(path);
    lm->index = (int)vm->mods.n;
    lm->state = LM_LOADING;
    VEC_PUSH(vm->mods, lm);
    smap_put(&vm->modmap, m->name, lm);
    VEC_PUSH(vm->loading, xstrdup(m->name));
    for (uint32_t i = 0; i < m->nimports; i++) {
        if (!vm_require(vm, m->imports[i], err)) return NULL;
    }
    if (!link_module(vm, lm, err)) return NULL;
    free(vm->loading.v[--vm->loading.n]);
    lm->state = LM_READY;
    return lm;
}

LModule *vm_require(VM *vm, const char *name, MlwsError *err) {
    void *v;
    if (smap_get(&vm->modmap, name, &v)) {
        LModule *lm = v;
        if (lm->state == LM_READY) return lm;
        StrBuf chain = {0};
        bool on = false;
        for (size_t i = 0; i < vm->loading.n; i++) {
            if (!strcmp(vm->loading.v[i], name)) on = true;
            if (on) sb_printf(&chain, "%s -> ", vm->loading.v[i]);
        }
        sb_puts(&chain, name);
        mlws_err(err, "E_LINK_CYCLE", "import cycle: %s", chain.s);
        sb_free(&chain);
        return NULL;
    }
    char *path = find_module_file(vm, name);
    if (!path) {
        StrBuf dirs = {0};
        for (size_t i = 0; i < vm->libpaths.n; i++) sb_printf(&dirs, "%s%s", i ? ", " : "", vm->libpaths.v[i]);
        mlws_err(err, "E_LOAD_NOT_FOUND", "module '%s' not found in library path [%s]", name, dirs.s ? dirs.s : "");
        sb_free(&dirs);
        return NULL;
    }
    Module *m = read_module_file(vm, path, name, err);
    if (!m) {
        free(path);
        return NULL;
    }
    if (strcmp(m->name, name) != 0) {
        mlws_err(err, "E_LOAD_NAME", "%s declares module '%s' but was loaded as '%s'", path, m->name, name);
        module_free(m);
        free(path);
        return NULL;
    }
    LModule *lm = register_module(vm, m, path, err);
    free(path);
    return lm;
}

LModule *vm_load_root(VM *vm, const char *path, MlwsError *err) {
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    char *defname = dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
    if (!file_exists(path)) {
        mlws_err(err, "E_LOAD_NOT_FOUND", "cannot open '%s'", path);
        free(defname);
        return NULL;
    }
    Module *m = read_module_file(vm, path, defname, err);
    free(defname);
    if (!m) return NULL;
    return register_module(vm, m, path, err);
}
