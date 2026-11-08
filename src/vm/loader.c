/* loader.c - finds modules in the program's directory or on the library
 * path, assembles or reads them, loads imports recursively (rejecting
 * cycles), links symbols, pre-decodes code and runs the verifier. See
 * docs/ASSEMBLY.md ("Modules and libraries", "Where modules are found").
 *
 * A module found in a program's directory belongs to that directory: the VM
 * knows it by its directory and name (LModule.key), so programs started from
 * different directories each have their own `util`. A module found on the
 * library path, or in a library archive, is known by its name alone and is
 * shared by every program. */
#include "../asm/asm.h"
#include "../platform/platform.h"
#include "../heap/heap.h"
#include "../heap/numeric.h"
#include "../core/version.h"
#include "../loader/bundle.h"
#include "vm.h"

static char *module_rel_path(const char *name) {
    char *rel = xstrdup(name);
    for (char *c = rel; *c; c++)
        if (*c == '.') *c = '/';
    return rel;
}

/* Module a.b in dir: dir/a/b.masm or dir/a/b.mlwso. When both exist, the
 * newer one wins. */
static char *find_in_dir(const char *dir, const char *name) {
    char *rel = module_rel_path(name);
    char *base = path_join(dir, rel);
    char *found = NULL;
    StrBuf a = {0}, b = {0};
    sb_printf(&a, "%s.masm", base);
    sb_printf(&b, "%s.mlwso", base);
    int64_t ta = plat_mtime(a.s), tb = plat_mtime(b.s);
    if (ta >= 0 && ta >= tb) found = xstrdup(a.s);
    else if (tb >= 0) found = xstrdup(b.s);
    sb_free(&a);
    sb_free(&b);
    free(base);
    free(rel);
    return found;
}

/* Search the library path for a module's file. */
static char *find_module_file(VM *vm, const char *name) {
    char *found = NULL;
    for (size_t i = 0; i < vm->libpaths.n && !found; i++) found = find_in_dir(vm->libpaths.v[i], name);
    return found;
}

/* What modmap knows a module by: a library-path module (home NULL) by its
 * name; a program-local one by its directory and name. (A name cannot
 * contain the \1 that starts the second kind.) */
static char *module_key(const Home *home, const char *name) {
    if (!home) return xstrdup(name);
    StrBuf b = {0};
    sb_printf(&b, "\1%s\1%s", home->real, name);
    return b.s;
}

/* The directory of a program whose root module is the file at path: one
 * Home per directory, however its path spells it. Under mods_lock. */
static const Home *home_of(VM *vm, const char *path) {
    char *dir = path_dirname(path), *real = plat_realpath(dir);
    if (!real) real = xstrdup(dir);
    for (size_t i = 0; i < vm->homes.n; i++) {
        if (!strcmp(vm->homes.v[i]->real, real)) {
            free(dir);
            free(real);
            return vm->homes.v[i];
        }
    }
    Home *h = xcalloc(1, sizeof *h);
    h->dir = dir;
    h->real = real;
    VEC_PUSH(vm->homes, h);
    return h;
}

static Module *read_module_file(VM *vm, const char *path, const char *default_name, const Home *home,
                                MlwsError *err) {
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
            StrBuf msg = {0}; /* prefix the message with the file (truncating if need be) */
            sb_printf(&msg, "%s: %s", path, err->msg);
            snprintf(err->msg, sizeof err->msg, "%.*s", (int)(sizeof err->msg - 1), msg.s);
            sb_free(&msg);
        }
        return m;
    }
    return modcache_assemble(vm, path, default_name, home, err);
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

/* An operand that names SP or FP, as a register or a memory operand's base
 * (or its index, or inside it). */
static bool uses_stack_reg(const Operand *o) {
    if (o->mode == OM_MIND || o->mode == OM_MINDB) return o->inner && uses_stack_reg(o->inner);
    bool regs = o->mode == OM_REG || (o->mode >= OM_IND && o->mode <= OM_PREDECB) || o->mode == OM_ELEM;
    if (!regs) return false;
    if (REG_BANK(o->reg) == BANK_S && REG_IDX(o->reg) != SREG_PID) return true;
    bool idx = o->mode == OM_IDX || o->mode == OM_IDXB || o->mode == OM_ELEM;
    return idx && REG_BANK(o->ireg) == BANK_S && REG_IDX(o->ireg) != SREG_PID;
}

static bool decode_function(LModule *lm, Function *f, MlwsError *err) {
    const MFunc *mf = f->mf;
    Module *m = lm->m;
    /* its register window: the banks one after another (Function.roff) */
    uint32_t words = 0;
    for (int b = 0; b < NBANKS; b++) {
        f->roff[b] = (uint16_t)words;
        words += mf->nregs[b];
    }
    f->rsize = (uint16_t)words;
    f->zoff = 0; /* the whole window, until verify.c plans less */
    f->zlen = (uint16_t)words;
    ByteReader r = {m->code + mf->code_off, mf->code_len, 0, false};
    VEC(Insn) code = {0};
    while (r.pos < r.n) {
        Insn in;
        if (!decode_insn(&r, &in, err)) {
            StrBuf msg = {0};
            sb_printf(&msg, "%s.%s: %s", m->name, mf->name, err->msg);
            snprintf(err->msg, sizeof err->msg, "%.*s", (int)(sizeof err->msg - 1), msg.s);
            sb_free(&msg);
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
    f->stackless = true;
    for (uint32_t i = 0; i < f->ncode && f->stackless; i++) {
        const Insn *in = &f->code[i];
        if (in->op == OP_PUSH || in->op == OP_POP || in->op == OP_DROP || in->op == OP_ENTER) f->stackless = false;
        for (int k = 0; k < in->nops && f->stackless; k++)
            if (uses_stack_reg(&in->ops[k])) f->stackless = false;
    }
    return true;
}

/* The struct type of the version being replaced that st is the same as: its
 * name, fields and types. */
static const StructDef *reload_same_struct(const VM *vm, const Module *m, const MStruct *st) {
    const LModule *old = vm->reload_old;
    if (!old || strcmp(old->m->name, m->name)) return NULL;
    for (uint32_t i = 0; i < old->m->nstructs; i++) {
        const StructDef *d = old->structs[i];
        if (old->m->structs[i].flags & (MSF_IMPORT | MSF_BUILTIN) || strcmp(d->name, st->name)) continue;
        if (d->nfields != st->nfields || memcmp(d->types, st->types, st->nfields)) return NULL;
        for (uint32_t k = 0; k < st->nfields; k++)
            if (strcmp(d->fields[k], st->fields[k])) return NULL;
        return d;
    }
    return NULL;
}

/* text.intern: the VM's one string with this text, the same object as every
 * module's literal of it (const_pool), made and added if there is none yet.
 * NULL for text with a NUL in it, which literals do not share either. */
Obj *vm_intern(VM *vm, const char *s, size_t n) {
    if (memchr(s, 0, n)) return NULL;
    StrBuf key = {0};
    sb_putc(&key, (char)('0' + CONST_STRING));
    sb_putn(&key, s, n);
    void *v = NULL;
    plat_lock(vm->mods_lock);
    if (!smap_get(&vm->const_pool, key.s, &v)) {
        v = &str_new_immortal(s, n)->h;
        VEC_PUSH(vm->const_objs, v);
        smap_put(&vm->const_pool, key.s, v);
    }
    plat_unlock(vm->mods_lock);
    sb_free(&key);
    return v;
}

static bool link_module(VM *vm, LModule *lm, MlwsError *err) {
    Module *m = lm->m;
    lm->consts = xcalloc(m->nconsts + 1, sizeof(Obj *));
    for (uint32_t i = 0; i < m->nconsts; i++) {
        const MConst *c = &m->consts[i];
        /* One object per kind and text in the whole VM: every module's 3n,
         * or "name", is the same object, so identity tests (beq.r R0, 3n)
         * agree across modules. Constants are immutable, so sharing is safe.
         * (A string with a NUL in it is not a key: it gets its own object.) */
        bool keyed = !memchr(c->data, 0, c->len);
        StrBuf key = {0};
        void *v;
        if (keyed) {
            sb_putc(&key, (char)('0' + c->kind));
            sb_putn(&key, c->data, c->len);
            if (smap_get(&vm->const_pool, key.s, &v)) {
                lm->consts[i] = v;
                sb_free(&key);
                continue;
            }
        }
        Obj *o = c->kind == CONST_STRING ? &str_new_immortal(c->data, c->len)->h
                                         : num_const(c->kind == CONST_BIG ? T_BIG : T_DEC, c->data, c->len);
        if (!o) {
            sb_free(&key);
            return mlws_err(err, "E_LOAD_FORMAT", "module %s: malformed %s literal \"%s\"", m->name,
                            c->kind == CONST_BIG ? "BigInt" : "decimal", c->data);
        }
        VEC_PUSH(vm->const_objs, o);
        if (keyed) smap_put(&vm->const_pool, key.s, o);
        sb_free(&key);
        lm->consts[i] = o;
    }
    lm->grefs = xcalloc(m->ngrefs + 1, sizeof(uint64_t));
    for (uint32_t i = 0; i < m->ngrefs; i++) {
        MGref *g = &m->grefs[i];
        if (g->kind == GREF_LOCAL) {
            lm->grefs[i] = PTR_MAKE(SEG_GLOBAL0 + (uint32_t)lm->index, 1, g->offset);
            continue;
        }
        LModule *dep = lm->deps[g->import_idx];
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
        if (st->flags & MSF_BUILTIN) {
            int bi = builtin_struct_find(st->name);
            if (bi < 0)
                return mlws_err(err, "E_LINK_UNDEF", "module %s uses unknown built-in struct '%s'", m->name, st->name);
            lm->structs[i] = &VM_BUILTIN_DEFS[bi];
            continue;
        }
        if (!(st->flags & MSF_IMPORT)) {
            const StructDef *same = reload_same_struct(vm, m, st);
            if (same) { /* objects of the old version are objects of this one */
                lm->structs[i] = same;
                continue;
            }
            StructDef *d = &lm->sdefs[i];
            d->name = st->name;
            d->module = m->name;
            d->nfields = st->nfields;
            d->fields = st->fields;
            d->types = st->types;
            lm->structs[i] = d;
            continue;
        }
        LModule *dep = lm->deps[st->import_idx];
        int si = module_find_export_struct(dep->m, st->name);
        if (si < 0)
            return mlws_err(err, "E_LINK_UNDEF", "module %s does not export struct '%s' (used by %s)", dep->m->name,
                            st->name, m->name);
        lm->structs[i] = dep->structs[si];
    }
    /* Native code this module brings in: extensions, then C structs and
     * functions (.cstruct, .ffi_free, .extern), which need the struct types. */
    for (uint32_t i = 0; i < m->nextensions; i++)
        if (!ext_load(vm, m->extensions[i], err)) return false;
    for (uint32_t i = 0; i < m->ncstructs; i++)
        if (!ffi_define_struct(vm, lm, &m->cstructs[i], err)) return false;
    for (uint32_t i = 0; i < m->nffrees; i++)
        if (!ffi_define_free(vm, &m->ffrees[i], err)) return false;
    for (uint32_t i = 0; i < m->ncallbacks; i++)
        if (!ffi_define_callback(vm, &m->callbacks[i], err)) return false;
    for (uint32_t i = 0; i < m->nexterns; i++)
        if (!ffi_define(vm, &m->externs[i], err)) return false;
    lm->natives = xcalloc(m->nnatives + 1, sizeof(NativeDef *));
    for (uint32_t i = 0; i < m->nnatives; i++) {
        void *v;
        if (!smap_get(&vm->natives, m->natives[i], &v))
            return mlws_err(err, "E_LINK_NATIVE", "module %s uses unknown native function \"%s\"", m->name, m->natives[i]);
        lm->natives[i] = v;
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
        LModule *dep = lm->deps[f->mf->import_idx];
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
    if (!vm_verify_module(vm, lm, err)) return false;
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        Function *f = &lm->funcs[i];
        if (f->target == f) interp_quicken_function(f), interp_jit_function(vm, f);
    }
    return true;
}

/* ---- libraries ------------------------------------------------------------------------ */

static void scan_dir_archives(VM *vm, const char *dir) {
    char **names;
    size_t n;
    if (!plat_list_dir(dir, &names, &n)) return;
    for (size_t k = 0; k < n; k++) {
        if (str_ends_with(names[k], ".mlwsl")) {
            char *p = path_join(dir, names[k]);
            MlwsError e;
            LibArchive *a = lib_read(p, &e);
            if (a) VEC_PUSH(vm->archives, a);
            else fprintf(stderr, "mlwsvm: ignoring %s\n", e.msg);
            free(p);
        }
        free(names[k]);
    }
    free(names);
}

/* Every library archive (*.mlwsl) in the programs' directories and the
 * library path's, each directory read once. Libraries are the VM's, not a
 * program's: an archive beside one program serves them all. */
static void scan_archives(VM *vm) {
    while (vm->homes_scanned < vm->homes.n) scan_dir_archives(vm, vm->homes.v[vm->homes_scanned++]->dir);
    if (vm->archives_scanned) return;
    vm->archives_scanned = true;
    for (size_t i = 0; i < vm->libpaths.n; i++) scan_dir_archives(vm, vm->libpaths.v[i]);
}

/* What is known about a library's versions, for a message: who asked for which. */
static void wants_of(VM *vm, const char *lib, StrBuf *b) {
    for (size_t i = 0; i < vm->wants.n; i++)
        if (!strcmp(vm->wants.v[i].lib, lib))
            sb_printf(b, "%s%s needs %s", b->n ? "; " : "", vm->wants.v[i].who, vm->wants.v[i].constraint);
}

/* `who` needs library lib in a version the constraint allows: checked now if
 * lib is the VM or has been chosen, else kept for when it is. */
static bool want(VM *vm, const char *lib, const char *constraint, const char *who, const LModule *mod,
                 const LibArchive *by, MlwsError *err) {
    SemVer v;
    if (!strcmp(lib, "mlwsvm")) {
        semver_parse(MLWSVM_VERSION, &v);
        if (!semver_satisfies(v, constraint))
            return mlws_err(err, "E_LINK_VERSION", "%s needs mlwsvm %s, but this is mlwsvm %s", who, constraint,
                            MLWSVM_VERSION);
        return true;
    }
    void *sel;
    if (smap_get(&vm->libsel, lib, &sel) && sel && !semver_satisfies(((LibArchive *)sel)->version, constraint)) {
        StrBuf b = {0};
        wants_of(vm, lib, &b);
        mlws_err(err, "E_LINK_VERSION", "%s needs %s %s, but %s %s is already loaded%s%s%s", who, lib, constraint, lib,
                 ((LibArchive *)sel)->manifest->lib_version, b.n ? " (" : "", b.n ? b.s : "", b.n ? ")" : "");
        sb_free(&b);
        return false;
    }
    LibWant w = {xstrdup(lib), xstrdup(constraint), xstrdup(who), mod, by};
    VEC_PUSH(vm->wants, w);
    return true;
}

/* The newest archive of library lib that meets every constraint stated so
 * far (archives of one version: the first on the library path), chosen for
 * the rest of the run; its own requirements are then stated. NULL with err
 * set if none fits; NULL with err clear if there is none of lib at all. */
static LibArchive *choose_library(VM *vm, const char *lib, MlwsError *err) {
    void *v;
    if (smap_get(&vm->libsel, lib, &v) && v) return v;
    LibArchive *best = NULL;
    StrBuf found = {0};
    for (size_t i = 0; i < vm->archives.n; i++) {
        LibArchive *a = vm->archives.v[i];
        if (strcmp(a->manifest->lib_name, lib)) continue;
        sb_printf(&found, "%s%s", found.n ? ", " : "", a->manifest->lib_version);
        bool ok = true;
        for (size_t k = 0; k < vm->wants.n && ok; k++)
            if (!strcmp(vm->wants.v[k].lib, lib)) ok = semver_satisfies(a->version, vm->wants.v[k].constraint);
        if (ok && (!best || semver_cmp(a->version, best->version) > 0)) best = a;
    }
    if (!best) {
        if (found.n) {
            StrBuf b = {0};
            wants_of(vm, lib, &b);
            mlws_err(err, "E_LINK_VERSION", "no version of library %s meets every constraint (%s); found %s", lib, b.s,
                     found.s);
            sb_free(&b);
        }
        sb_free(&found);
        return NULL;
    }
    sb_free(&found);
    smap_put(&vm->libsel, lib, best);
    VEC_PUSH(vm->chosen, best);
    StrBuf who = {0};
    sb_printf(&who, "library %s %s", lib, best->manifest->lib_version);
    for (uint32_t i = 0; i < best->manifest->nrequires; i++) {
        const MRequire *r = &best->manifest->requires[i];
        if (!want(vm, r->lib, r->constraint, who.s, NULL, best, err)) {
            sb_free(&who);
            return NULL;
        }
    }
    sb_free(&who);
    return best;
}

static LModule *register_module(VM *vm, Module *m, const char *path, LibArchive *lib, const Home *home,
                                bool requires_stated, MlwsError *err);
static LModule *require(VM *vm, const char *name, const Home *home, MlwsError *err);

/* Whether `who` (a module of library lib, or of none) may import dep: only
 * a library's own modules may import its private ones. */
static bool may_import(const char *who, const LibArchive *lib, const LModule *dep, MlwsError *err) {
    if (!dep->lib || dep->lib == lib || lib_is_public(dep->lib, dep->m->name)) return true;
    StrBuf pub = {0};
    for (uint32_t k = 0; k < dep->lib->manifest->npublics; k++)
        sb_printf(&pub, "%s%s", k ? ", " : "", dep->lib->manifest->publics[k]);
    mlws_err(err, "E_LINK_PRIVATE", "%s imports %s, which library %s keeps private (its public modules: %s)", who,
             dep->m->name, dep->lib->manifest->lib_name, pub.n ? pub.s : "none");
    sb_free(&pub);
    return false;
}

/* Module name from a library archive: 1 loaded (*out), 0 no archive has it,
 * -1 an error. The library is the longest prefix of the name that some
 * archive on the path provides. */
static int require_from_library(VM *vm, const char *name, LModule **out, MlwsError *err) {
    scan_archives(vm);
    if (!vm->archives.n) return 0;
    char *lib = xstrdup(name);
    for (;;) {
        err->code[0] = 0;
        LibArchive *a = choose_library(vm, lib, err);
        if (a) {
            int i = lib_find(a, name);
            if (i < 0) {
                mlws_err(err, "E_LOAD_NOT_FOUND", "library %s %s (%s) has no module '%s'", lib, a->manifest->lib_version,
                         a->path, name);
                free(lib);
                return -1;
            }
            Module *m = lib_module(a, (uint32_t)i, err);
            if (!m) {
                free(lib);
                return -1;
            }
            StrBuf p = {0};
            sb_printf(&p, "%s(%s)", a->path, name);
            *out = register_module(vm, m, p.s, a, NULL, false, err);
            sb_free(&p);
            free(lib);
            return *out ? 1 : -1;
        }
        if (err->code[0]) {
            free(lib);
            return -1;
        }
        char *dot = strrchr(lib, '.');
        if (!dot) break;
        *dot = 0;
    }
    free(lib);
    return 0;
}

/* Register module m, found at path (in library archive lib, or in program
 * directory home, or neither), then load what it imports and link it. Its
 * .requires are stated first, unless the caller has (requires_stated). */
static LModule *register_module(VM *vm, Module *m, const char *path, LibArchive *lib, const Home *home,
                                bool requires_stated, MlwsError *err) {
    char *key = module_key(home, m->name);
    void *had;
    if (smap_get(&vm->modmap, key, &had) && had) { /* (NULL: a run-time load of it failed) */
        mlws_err(err, "E_LOAD_NAME",
                 "two modules named '%s': %s and %s (.module names a module; by default it is named after its file)",
                 m->name, ((LModule *)had)->path, path);
        free(key);
        module_free(m);
        return NULL;
    }
    if (vm->mods.n >= SEG_RAW0 - SEG_GLOBAL0) {
        mlws_err(err, "E_LOAD_LIMIT", "too many modules");
        free(key);
        module_free(m);
        return NULL;
    }
    if (vm->opt.passes) m = modcache_optimize(vm, m); /* --opt */
    LModule *lm = xcalloc(1, sizeof *lm);
    lm->m = m;
    lm->lib = lib;
    lm->path = xstrdup(path);
    lm->home = home;
    lm->key = key;
    lm->index = (int)vm->mods.n;
    lm->carry_from = -1;
    lm->state = LM_LOADING;
    lm->deps = xcalloc(m->nimports + 1, sizeof *lm->deps);
    for (size_t i = 0; i < vm->mods.n; i++) { /* another program's util, or the library path's */
        LModule *o = vm->mods.v[i];
        if (o->state != LM_FAILED && o != vm->reload_old && !o->replaced && !strcmp(o->m->name, m->name)) {
            atomic_store(&o->shared_name, true);
            atomic_store(&lm->shared_name, true);
        }
    }
    VEC_PUSH(vm->mods, lm);
    smap_put(&vm->modmap, key, lm);
    VEC_PUSH(vm->loading, lm);
    /* who asked, in messages: a program's module by its file (two programs
     * may both have a main), a library-path one by its name */
    const char *who = home ? path : m->name;
    for (uint32_t i = 0; i < m->nrequires && !requires_stated; i++) /* before its imports choose versions */
        if (!want(vm, m->requires[i].lib, m->requires[i].constraint, who, lm, NULL, err)) return NULL;
    for (uint32_t i = 0; i < m->nimports; i++) {
        LModule *dep = require(vm, m->imports[i], home, err);
        if (!dep) return NULL;
        if (!may_import(m->name, lib, dep, err)) return NULL;
        lm->deps[i] = dep;
    }
    if (!link_module(vm, lm, err)) return NULL;
    vm->loading.n--;
    lm->state = LM_READY;
    return lm;
}

/* A module that is being loaded or is loaded: it, if it is ready; if it is
 * still loading, it imports (indirectly) the module that imports it. */
static LModule *loaded_or_cycle(VM *vm, LModule *lm, MlwsError *err) {
    if (lm->state == LM_READY) return lm;
    StrBuf chain = {0};
    bool on = false;
    for (size_t i = 0; i < vm->loading.n; i++) {
        if (vm->loading.v[i] == lm) on = true;
        if (on) sb_printf(&chain, "%s -> ", vm->loading.v[i]->m->name);
    }
    sb_puts(&chain, lm->m->name);
    mlws_err(err, "E_LINK_CYCLE", "import cycle: %s", chain.s);
    sb_free(&chain);
    return NULL;
}

/* The module `name`, imported by a module of program directory home (NULL:
 * a library-path module, or none): found first in home, where it is that
 * directory's own, then on the library path and in library archives. */
static LModule *require(VM *vm, const char *name, const Home *home, MlwsError *err) {
    void *v;
    char *path = NULL;
    const Home *searched = home;
    int in_bundle = vm->bundle ? bundle_find(vm->bundle, name) : -1;
    if (in_bundle >= 0) { /* its requirements were met when it was bundled */
        if (smap_get(&vm->modmap, name, &v) && v) return loaded_or_cycle(vm, v, err);
        Module *m = bundle_module(vm->bundle, (uint32_t)in_bundle, err);
        if (!m) return NULL;
        StrBuf p = {0};
        sb_printf(&p, "%s(%s)", vm->bundle->path, name);
        LModule *lm = register_module(vm, m, p.s, NULL, NULL, true, err);
        sb_free(&p);
        return lm;
    }
    if (home) {
        char *key = module_key(home, name);
        bool had = smap_get(&vm->modmap, key, &v) && v;
        free(key);
        if (had) return loaded_or_cycle(vm, v, err);
        path = find_in_dir(home->dir, name);
    }
    if (!path) { /* not the program's own: the library path's */
        home = NULL;
        if (smap_get(&vm->modmap, name, &v) && v) return loaded_or_cycle(vm, v, err);
        path = find_module_file(vm, name);
    }
    if (!path) {
        LModule *lm = NULL;
        int r = require_from_library(vm, name, &lm, err);
        if (r) return r > 0 ? lm : NULL;
        StrBuf dirs = {0};
        for (size_t i = 0; i < vm->libpaths.n; i++) sb_printf(&dirs, "%s%s", i ? ", " : "", vm->libpaths.v[i]);
        mlws_err(err, "E_LOAD_NOT_FOUND", "module '%s' not found in %s%s%slibrary path [%s]", name,
                 searched ? "the program's directory " : "", searched ? searched->dir : "",
                 searched ? " or the " : "", dirs.s ? dirs.s : "");
        sb_free(&dirs);
        return NULL;
    }
    Module *m = read_module_file(vm, path, name, home, err);
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
    LModule *lm = register_module(vm, m, path, NULL, home, false, err);
    free(path);
    return lm;
}

/* module.handle: a function whose code is one tail call to lm's exported
 * function fname, in a module of its own that imports lm's by name, with lm's
 * home. As every module's calls to lm do, the call moves to each new version
 * (retarget_users), so a closure of it calls the current version; and a
 * reload that would change the function's signature, or take it away, is
 * refused, as for any user. One per module and function, for the VM's life. */
static const char *handle_type(uint8_t t) { return t == T_R ? "ref" : t == T_P ? "ptr" : type_name((MlwsType)t); }

Function *vm_handle(VM *vm, LModule *lm, const char *fname, MlwsError *err) {
    int fi = module_find_export_func(lm->m, fname);
    if (fi < 0 || (lm->m->funcs[fi].flags & MF_IMPORT)) {
        mlws_err(err, "E_LINK_UNDEF", "module.handle: module %s does not export a function '%s'", lm->m->name, fname);
        return NULL;
    }
    StrBuf key = {0};
    sb_printf(&key, "%s\x1f%s", lm->key, fname);
    void *v = NULL;
    plat_lock(vm->mods_lock);
    bool have = smap_get(&vm->handles, key.s, &v);
    plat_unlock(vm->mods_lock);
    if (have) {
        sb_free(&key);
        return v;
    }
    const MFunc *f = &lm->m->funcs[fi];
    StrBuf t = {0}, name = {0};
    sb_printf(&name, "(handle) %s.%s #%zu", lm->m->name, fname, vm->handles.n);
    sb_printf(&t, ".import %s as target\n.export h\n.func h(", lm->m->name);
    for (int i = 0; i < f->nparams; i++) sb_printf(&t, "%s%s a%d", i ? ", " : "", handle_type(f->params[i]), i);
    sb_puts(&t, ")");
    if (f->nrets) {
        sb_puts(&t, " -> (");
        for (int i = 0; i < f->nrets; i++) sb_printf(&t, "%s%s", i ? ", " : "", handle_type(f->rets[i]));
        sb_puts(&t, ")");
    }
    sb_printf(&t, "\n    tcall target.%s (", fname);
    for (int i = 0; i < f->nparams; i++) sb_printf(&t, "%sa%d", i ? ", " : "", i);
    sb_puts(&t, ")\n.endf\n");
    Module *m = assemble_text(name.s, t.s, name.s, (const char *const *)vm->libpaths.v, vm->libpaths.n, err);
    Function *h = NULL;
    if (m) {
        plat_lock(vm->mods_lock);
        if (smap_get(&vm->handles, key.s, &v)) { /* (another made it meanwhile) */
            module_free(m);
            h = v;
        } else {
            LModule *hm = register_module(vm, m, name.s, NULL, lm->home, false, err);
            if (hm) {
                h = &hm->funcs[module_find_export_func(hm->m, "h")];
                smap_put(&vm->handles, key.s, h);
            }
        }
        plat_unlock(vm->mods_lock);
    }
    sb_free(&t);
    sb_free(&name);
    sb_free(&key);
    return h;
}

LModule *vm_load_text(VM *vm, const char *name, const char *text, MlwsError *err) {
    Module *m = assemble_text(name, text, name, (const char *const *)vm->libpaths.v, vm->libpaths.n, err);
    if (!m) return NULL;
    plat_lock(vm->mods_lock); /* an embedding host may load while services run */
    LModule *lm = register_module(vm, m, name, NULL, NULL, false, err);
    plat_unlock(vm->mods_lock);
    return lm;
}

/* After a failed load: the library versions it chose and the constraints it
 * stated go too, except those of what it leaves loaded. */
static void take_back_libraries(VM *vm, size_t chosen, size_t wants) {
    size_t keep = chosen;
    for (size_t i = chosen; i < vm->chosen.n; i++) {
        LibArchive *a = vm->chosen.v[i];
        bool used = false;
        for (size_t k = 0; k < vm->mods.n && !used; k++) used = vm->mods.v[k]->lib == a && vm->mods.v[k]->state == LM_READY;
        if (used) vm->chosen.v[keep++] = a;
        else smap_put(&vm->libsel, a->manifest->lib_name, NULL);
    }
    vm->chosen.n = keep;
    keep = wants;
    for (size_t i = wants; i < vm->wants.n; i++) {
        LibWant *w = &vm->wants.v[i];
        void *sel = NULL;
        bool held = w->mod ? w->mod->state == LM_READY
                  : w->by ? smap_get(&vm->libsel, w->by->manifest->lib_name, &sel) && sel == w->by
                          : true;
        if (held) {
            vm->wants.v[keep++] = *w;
            continue;
        }
        free(w->lib);
        free(w->constraint);
        free(w->who);
    }
    vm->wants.n = keep;
}

LModule *vm_load_dynamic(VM *vm, const char *name, const LModule *from, MlwsError *err) {
    plat_lock(vm->mods_lock);
    size_t start = vm->mods.n, chain = vm->loading.n, chosen = vm->chosen.n, wants = vm->wants.n;
    LModule *lm = require(vm, name, from ? from->home : NULL, err);
    if (lm && !may_import(from ? from->m->name : "the program", from ? from->lib : NULL, lm, err)) lm = NULL;
    if (!lm) { /* what this load registered but did not link is dropped; the rest stays loaded */
        for (size_t i = start; i < vm->mods.n; i++) {
            LModule *x = vm->mods.v[i];
            if (x->state == LM_READY) continue;
            x->state = LM_FAILED;
            smap_put(&vm->modmap, x->key, NULL);
        }
        vm->loading.n = chain;
        take_back_libraries(vm, chosen, wants);
    }
    plat_unlock(vm->mods_lock);
    return lm;
}

/* ---- reloading ------------------------------------------------------------------------- */

static bool same_signature(const MFunc *a, const MFunc *b) {
    return a->nparams == b->nparams && a->nrets == b->nrets && !memcmp(a->params, b->params, a->nparams) &&
           !memcmp(a->rets, b->rets, a->nrets) && (a->flags & MF_RETS_DECLARED) == (b->flags & MF_RETS_DECLARED);
}

/* Whether module x, which may use old, links to its new version as it is:
 * its compiled operands stay, but for the functions. */
static bool still_links(const LModule *x, const LModule *old, const LModule *nw, MlwsError *err) {
    const Module *m = x->m;
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        const Function *f = &x->funcs[i];
        if (!(f->mf->flags & MF_IMPORT) || f->target->mod != old) continue;
        int fi = module_find_export_func(nw->m, f->name);
        if (fi < 0 || (nw->m->funcs[fi].flags & MF_IMPORT))
            return mlws_err(err, "E_LINK_UNDEF", "the new %s does not export function '%s' (called from %s)", old->m->name,
                            f->name, m->name);
        if (!same_signature(f->target->mf, &nw->m->funcs[fi]))
            return mlws_err(err, "E_LINK_RELOAD", "the new %s changes the signature of '%s' (called from %s)", old->m->name,
                            f->name, m->name);
    }
    for (uint32_t i = 0; i < m->ngrefs; i++)
        if (m->grefs[i].kind != GREF_LOCAL && x->deps[m->grefs[i].import_idx] == old)
            return mlws_err(err, "E_LINK_RELOAD", "%s uses data symbol '%s' of %s: its data cannot be replaced", m->name,
                            m->grefs[i].sym, old->m->name);
    for (uint32_t i = 0; i < m->nstructs; i++) {
        const MStruct *st = &m->structs[i];
        if (!(st->flags & MSF_IMPORT) || (st->flags & MSF_BUILTIN) || x->deps[st->import_idx] != old) continue;
        int si = module_find_export_struct(nw->m, st->name);
        if (si < 0 || nw->structs[si] != x->structs[i])
            return mlws_err(err, "E_LINK_RELOAD", "the new %s changes struct '%s' (used by %s)", old->m->name, st->name, m->name);
    }
    return true;
}

/* With the world stopped: every use of old's functions in other modules, of
 * every version, is a use of the new ones. */
static void retarget_users(VM *vm, const LModule *old, const LModule *nw) {
    for (size_t i = 0; i < vm->mods.n; i++) {
        LModule *x = vm->mods.v[i];
        if (x == old || x == nw || x->state != LM_READY) continue;
        for (uint32_t d = 0; d < x->m->nimports; d++)
            if (x->deps[d] == old) x->deps[d] = (LModule *)nw;
        for (uint32_t k = 0; k < x->m->nfuncs; k++) {
            Function *f = &x->funcs[k];
            if (f->target != f) {
                if (f->target->mod == old)
                    f->target = &nw->funcs[module_find_export_func(nw->m, f->name)];
                continue;
            }
            bool used = false;
            for (uint32_t pc = 0; pc < f->ncode && !used; pc++)
                for (int o = 0; o < f->code[pc].nops && !used; o++)
                    used = f->code[pc].ops[o].mode == OM_FUNC && ((const Function *)f->code[pc].ops[o].rv.ptr)->mod == old;
            if (!used) continue;
            interp_jit_forget(vm, f);
            for (uint32_t pc = 0; pc < f->ncode; pc++)
                for (int o = 0; o < f->code[pc].nops; o++) {
                    Operand *op = &f->code[pc].ops[o];
                    if (op->mode != OM_FUNC) continue;
                    const Function *t = op->rv.ptr;
                    if (t->mod == old) interp_retarget(vm, &f->code[pc], o, &nw->funcs[module_find_export_func(nw->m, t->name)]);
                }
            interp_jit_again(vm, f);
        }
    }
    for (uint32_t k = 0; k < old->m->nfuncs; k++) /* what compiled code calls must stop at the interpreter's call */
        if (old->funcs[k].target == &old->funcs[k] && (old->funcs[k].mf->flags & MF_EXPORT)) interp_jit_forget(vm, &old->funcs[k]);
}

/* The globals a new version carries from the one it replaces: each with the
 * same name and size in both. (Every data label is a symbol, to the next.) */
static void carry_globals(const LModule *old, LModule *nw) {
    const Module *a = old->m, *b = nw->m;
    if (b->flags & MOD_RELOAD_FRESH) return; /* `.reload fresh`: nothing carried */
    nw->carry_from = old->index;
    for (uint32_t i = 0; i < b->nsyms; i++)
        for (uint32_t j = 0; j < a->nsyms; j++) {
            const MSym *x = &b->syms[i], *y = &a->syms[j];
            if (strcmp(x->name, y->name) || x->size != y->size || !x->size) continue;
            nw->carry = xrealloc(nw->carry, (nw->ncarry + 1) * sizeof *nw->carry);
            nw->carry[nw->ncarry++] = (struct GlobalCarry){y->offset, x->offset, x->size};
            break;
        }
}

static LModule *reload_locked(VM *vm, LModule *old, const char *name, const char *text, MlwsError *err) {
    size_t start = vm->mods.n, chain = vm->loading.n, chosen = vm->chosen.n, wants = vm->wants.n;
    smap_put(&vm->modmap, old->key, NULL); /* for its name to load again */
    vm->reload_old = old;
    LModule *nw = NULL;
    if (text) {
        Module *m = assemble_text(name, text, name, (const char *const *)vm->libpaths.v, vm->libpaths.n, err);
        if (m && strcmp(m->name, old->m->name)) {
            mlws_err(err, "E_LOAD_NAME", "the text declares module '%s' but replaces '%s'", m->name, old->m->name);
            module_free(m);
            m = NULL;
        }
        if (m) nw = register_module(vm, m, old->path, NULL, old->home, false, err);
    } else {
        nw = require(vm, old->m->name, old->home, err);
    }
    vm->reload_old = NULL;
    bool ok = nw != NULL;
    for (size_t i = 0; i < start && ok; i++) {
        const LModule *x = vm->mods.v[i];
        if (x != old && x->state == LM_READY) ok = still_links(x, old, nw, err);
    }
    if (!ok) { /* what it registered goes, and its name is the old version's again */
        for (size_t i = start; i < vm->mods.n; i++) {
            LModule *x = vm->mods.v[i];
            if (x->state == LM_READY && x != nw) continue;
            x->state = LM_FAILED;
            smap_put(&vm->modmap, x->key, NULL);
        }
        smap_put(&vm->modmap, old->key, old);
        vm->loading.n = chain;
        take_back_libraries(vm, chosen, wants);
        return NULL;
    }
    old->replaced = true;
    carry_globals(old, nw);
    return nw;
}

LModule *vm_reload(VM *vm, const char *name, const char *text, const LModule *from, MlwsError *err) {
    LModule *old = vm_module_named(vm, name, from);
    if (!old) {
        mlws_err(err, "E_LOAD_NOT_FOUND", "module %s is not loaded", name);
        return NULL;
    }
    if (atomic_exchange(&vm->reloading, true)) {
        mlws_err(err, "E_BUSY", "a module is being reloaded");
        return NULL;
    }
    plat_lock(vm->mods_lock);
    LModule *nw = reload_locked(vm, old, name, text, err);
    plat_unlock(vm->mods_lock);
    if (nw) {
        sched_stop_world(vm);
        plat_lock(vm->mods_lock);
        retarget_users(vm, old, nw);
        plat_unlock(vm->mods_lock);
        sched_start_world(vm);
    }
    atomic_store(&vm->reloading, false);
    return nw;
}

/* ---- freeing replaced versions ---------------------------------------------------- */

/* What a version's FFI declarations made outlives it in the VM (callback
 * types, foreign functions' descriptions), so a version with any stays. */
static bool binds_foreign(const Module *m) {
    return m->nexterns || m->ncallbacks || m->nextensions || m->ncstructs || m->nffrees;
}

/* Whether anything could still run v's code: another module's code (which a
 * reload pointed at the new version, so this is only a check), or a process:
 * see gc_holds_code. The world is stopped. */
static bool version_in_use(VM *vm, const LModule *v) {
    for (size_t i = 0; i < vm->mods.n; i++) {
        const LModule *x = vm->mods.v[i];
        if (x == v || x->state != LM_READY) continue;
        for (uint32_t d = 0; d < x->m->nimports; d++)
            if (x->deps[d] == v) return true;
        for (uint32_t k = 0; k < x->m->nfuncs; k++) {
            const Function *f = &x->funcs[k];
            if (f->target->mod == v) return true;
            if (f->target != f) continue;
            for (uint32_t pc = 0; pc < f->ncode; pc++)
                for (int o = 0; o < f->code[pc].nops; o++)
                    if (f->code[pc].ops[o].mode == OM_FUNC && ((const Function *)f->code[pc].ops[o].rv.ptr)->mod == v)
                        return true;
        }
    }
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (p && gc_holds_code(p, v, (uint32_t)v->index)) return true;
    }
    for (size_t i = 0; i < vm->progs.n; i++) { /* a main that has ended: its results are still read */
        Process *p = vm->progs.v[i]->main;
        if (p && vm_proc_at(vm, p->pid) != p && gc_holds_code(p, v, (uint32_t)v->index)) return true;
    }
    return false;
}

/* Whether a process is in a blocking native call, on an I/O thread: the native
 * may use its heap, and call back into code, while the world is stopped. */
static bool blocking_call_in_flight(VM *vm) {
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        plat_lock(p->lock);
        bool busy = p->bcall.state == BCALL_PENDING || p->on_io_thread;
        plat_unlock(p->lock);
        if (busy) return true;
    }
    return false;
}

static char *keep_str(VM *vm, const char *s) {
    char *c = xstrdup(s ? s : "");
    VEC_PUSH(vm->retired, c);
    return c;
}

static void drop_globals(Process *p, uint32_t index) {
    if (index >= p->nglobals) return;
    Segment *g = &p->globals[index];
    free(g->data);
    free(g->tags);
    memset(g, 0, sizeof *g); /* not live: a pointer into it traps */
}

static const MFunc PURGED_MF = {0};

/* Free v, which nothing can run now. What stays: its LModule, for its place
 * in vm->mods (each process's global segments are numbered by it), with a
 * module of no functions; its Function records, without code, for their
 * addresses not to be taken by another function's (compiled closure calls
 * compare them; interp_forget_callees); and its struct types, which values
 * and newer versions share. Names that outlive it are copied. */
static void purge_version(VM *vm, LModule *v) {
    Module *m = v->m;
    interp_forget_callees(vm, v);
    plat_lock(vm->lock);
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        Function *f = &v->funcs[i];
        const char *old = f->name;
        char *name = keep_str(vm, old); /* a process's entry (Process.entry) and the exits shown may name it */
        for (size_t k = 0; k < vm->exits.n; k++)
            if (vm->exits.v[k].entry == old) vm->exits.v[k].entry = name;
        if (f->target == f)
            for (uint32_t k = 0; k < f->ncode; k++) insn_free(&f->code[k]);
        free(f->code);
        free(f->bp);
        free(f->prof);
        free(f->jit_entry);
        free(f->jit_xop);
        memset(f, 0, sizeof *f);
        f->name = name;
        f->mod = v;
        f->mf = &PURGED_MF;
        f->target = f;
    }
    plat_unlock(vm->lock);
    for (uint32_t i = 0; i < m->nstructs; i++) {
        StructDef *d = &v->sdefs[i];
        if (!d->name) continue; /* not defined here */
        d->name = keep_str(vm, d->name);
        d->module = keep_str(vm, d->module);
        char **fields = xcalloc(d->nfields + 1, sizeof *fields);
        for (uint32_t k = 0; k < d->nfields; k++) fields[k] = keep_str(vm, d->fields[k]);
        uint8_t *types = xmalloc(d->nfields + 1);
        memcpy(types, d->types, d->nfields);
        VEC_PUSH(vm->retired, fields);
        VEC_PUSH(vm->retired, types);
        d->fields = fields;
        d->types = types;
    }
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (p) drop_globals(p, (uint32_t)v->index);
    }
    for (size_t i = 0; i < vm->progs.n; i++)
        if (vm->progs.v[i]->main) drop_globals(vm->progs.v[i]->main, (uint32_t)v->index);
    free(v->consts);
    free(v->grefs);
    free(v->natives);
    free(v->structs);
    free(v->frefs);
    free(v->deps);
    v->consts = NULL;
    v->grefs = NULL;
    v->natives = NULL;
    v->structs = NULL;
    v->frefs = NULL;
    v->deps = NULL;
    Module *stub = xcalloc(1, sizeof *stub);
    stub->name = xstrdup(m->name);
    v->m = stub;
    v->state = LM_PURGED;
    module_free(m);
}

bool vm_purge(VM *vm, const char *name, const LModule *from, uint32_t *freed, uint32_t *kept, MlwsError *err) {
    *freed = *kept = 0;
    LModule *cur = vm_module_named(vm, name, from);
    if (!cur) return mlws_err(err, "E_LOAD_NOT_FOUND", "module %s is not loaded", name);
    if (atomic_exchange(&vm->reloading, true))
        return mlws_err(err, "E_BUSY", "a module is being reloaded or purged");
    sched_stop_world(vm);
    plat_lock(vm->mods_lock);
    bool tools = vm->hook != NULL; /* the debugger, profiler and trace hold functions */
    int unsure = -1;
    for (size_t i = 0; i < vm->mods.n; i++) {
        LModule *v = vm->mods.v[i];
        if (v == cur || !v->replaced || v->state != LM_READY || strcmp(v->key, cur->key)) continue;
        if (unsure == -1) unsure = tools || blocking_call_in_flight(vm);
        if (unsure || binds_foreign(v->m) || version_in_use(vm, v)) {
            ++*kept;
        } else {
            purge_version(vm, v);
            ++*freed;
        }
    }
    plat_unlock(vm->mods_lock);
    sched_start_world(vm);
    atomic_store(&vm->reloading, false);
    return true;
}

/* module.purge_kill: the processes that hold a replaced version of cur's
 * module, as module.purge would find them, but `self`, are killed first
 * (E_KILLED), as Erlang's code:purge does; then it purges. Processes in a
 * blocking native call, or under a tool, make it kill nobody, as they make a
 * purge keep everything. */
bool vm_purge_kill(VM *vm, const char *name, const LModule *from, uint64_t self, uint32_t *freed, uint32_t *kept,
                   uint32_t *killed, MlwsError *err) {
    *killed = 0;
    LModule *cur = vm_module_named(vm, name, from);
    if (!cur) return mlws_err(err, "E_LOAD_NOT_FOUND", "module %s is not loaded", name);
    if (atomic_exchange(&vm->reloading, true)) return mlws_err(err, "E_BUSY", "a module is being reloaded or purged");
    PidVec victims = {0};
    sched_stop_world(vm);
    plat_lock(vm->mods_lock);
    Process *me = vm_proc_at(vm, self);
    if (!vm->hook && !blocking_call_in_flight(vm))
        for (size_t i = 0; i < vm->mods.n; i++) {
            LModule *v = vm->mods.v[i];
            if (v == cur || !v->replaced || v->state != LM_READY || strcmp(v->key, cur->key) || binds_foreign(v->m)) continue;
            if (me && gc_holds_code(me, v, (uint32_t)v->index)) continue; /* kept for the caller: nobody dies for it */
            for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
                Process *p = vm_proc_at(vm, pid);
                if (!p || pid == self || p->state == PROC_DONE || p->state == PROC_FAILED) continue;
                bool listed = false;
                for (size_t k = 0; k < victims.n && !listed; k++) listed = victims.v[k] == pid;
                if (!listed && gc_holds_code(p, v, (uint32_t)v->index)) VEC_PUSH(victims, pid);
            }
        }
    plat_unlock(vm->mods_lock);
    sched_start_world(vm);
    atomic_store(&vm->reloading, false);
    char msg[256];
    snprintf(msg, sizeof msg, "it was running a replaced version of module %s, which was purged", name);
    for (size_t k = 0; k < victims.n; k++)
        if (sched_kill_from_outside(vm, victims.v[k], "E_KILLED", msg)) ++*killed;
    /* One running on another thread stops at its slice's end: wait for them all
     * (a little), so that what they held can go. */
    for (int tries = 0; tries < 2000; tries++) {
        bool any = false;
        for (size_t k = 0; k < victims.n && !any; k++) {
            const Process *p = vm_proc_at(vm, victims.v[k]); /* (a kill pending is not an end) */
            any = p && p->state != PROC_DONE && p->state != PROC_FAILED;
        }
        if (!any) break;
        plat_sleep_ns(1000000);
    }
    VEC_FREE(victims);
    return vm_purge(vm, name, from, freed, kept, err);
}

LModule *vm_module_named(VM *vm, const char *name, const LModule *from) {
    void *v = NULL;
    plat_lock(vm->mods_lock);
    bool decided = false; /* name means a module of from's directory */
    if (from && from->home) {
        char *key = module_key(from->home, name);
        decided = smap_get(&vm->modmap, key, &v);
        free(key);
        if (!decided) { /* not loaded, but there to be: the library path's does not count */
            char *path = find_in_dir(from->home->dir, name);
            decided = path != NULL;
            free(path);
        }
    }
    if (!decided && !smap_get(&vm->modmap, name, &v) && !from) {
        int n = 0; /* asked from outside any program: the only program-local module of that name */
        for (size_t i = 0; i < vm->mods.n; i++) {
            LModule *x = vm->mods.v[i];
            if (x->home && x->state == LM_READY && !x->replaced && !strcmp(x->m->name, name)) v = x, n++;
        }
        if (n != 1) v = NULL;
    }
    LModule *lm = v && ((LModule *)v)->state == LM_READY ? v : NULL;
    plat_unlock(vm->mods_lock);
    return lm;
}

bool vm_load_roots(VM *vm, const char *const *paths, int n, LModule **out, MlwsError *errs) {
    Module **ms = xcalloc((size_t)n + 1, sizeof *ms);
    const Home **homes = xcalloc((size_t)n + 1, sizeof *homes);
    bool ok = true;
    for (int i = 0; i < n; i++) out[i] = NULL;
    if (vm->bundle) { /* paths[0] is the root module's name (main.c) */
        plat_lock(vm->mods_lock);
        out[0] = require(vm, paths[0], NULL, &errs[0]);
        plat_unlock(vm->mods_lock);
        free(ms);
        free(homes);
        return out[0] != NULL;
    }
    for (int i = 0; i < n && ok; i++) {
        if (!file_exists(paths[i])) {
            mlws_err(&errs[i], "E_LOAD_NOT_FOUND", "cannot open '%s'", paths[i]);
            ok = false;
            break;
        }
        plat_lock(vm->mods_lock);
        homes[i] = home_of(vm, paths[i]);
        plat_unlock(vm->mods_lock);
        const char *base = path_basename(paths[i]);
        const char *dot = strrchr(base, '.');
        char *defname = dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
        ms[i] = read_module_file(vm, paths[i], defname, homes[i], &errs[i]);
        free(defname);
        ok = ms[i] != NULL;
    }
    plat_lock(vm->mods_lock);
    /* Every program's requirements, before any library version is chosen. */
    for (int i = 0; i < n && ok; i++)
        for (uint32_t k = 0; k < ms[i]->nrequires && ok; k++)
            ok = want(vm, ms[i]->requires[k].lib, ms[i]->requires[k].constraint, paths[i], NULL, NULL, &errs[i]);
    for (int i = 0; i < n && ok; i++) {
        out[i] = register_module(vm, ms[i], paths[i], NULL, homes[i], true, &errs[i]);
        ms[i] = NULL; /* the VM's now (or freed) */
        ok = out[i] != NULL;
    }
    plat_unlock(vm->mods_lock);
    for (int i = 0; i < n; i++) module_free(ms[i]);
    free(ms);
    free(homes);
    return ok;
}

LModule *vm_load_root(VM *vm, const char *path, MlwsError *err) {
    LModule *lm;
    return vm_load_roots(vm, &path, 1, &lm, err) ? lm : NULL;
}
