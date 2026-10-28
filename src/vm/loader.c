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
#include "../heap/numeric.h"
#include "../core/version.h"
#include "../loader/library.h"
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
    return true;
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
        if (f->target == f) interp_quicken_function(f);
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
static bool want(VM *vm, const char *lib, const char *constraint, const char *who, MlwsError *err) {
    SemVer v;
    if (!strcmp(lib, "mlwsvm")) {
        semver_parse(MLWSVM_VERSION, &v);
        if (!semver_satisfies(v, constraint))
            return mlws_err(err, "E_LINK_VERSION", "%s needs mlwsvm %s, but this is mlwsvm %s", who, constraint,
                            MLWSVM_VERSION);
        return true;
    }
    void *sel;
    if (smap_get(&vm->libsel, lib, &sel) && !semver_satisfies(((LibArchive *)sel)->version, constraint)) {
        StrBuf b = {0};
        wants_of(vm, lib, &b);
        mlws_err(err, "E_LINK_VERSION", "%s needs %s %s, but %s %s is already loaded%s%s%s", who, lib, constraint, lib,
                 ((LibArchive *)sel)->manifest->lib_version, b.n ? " (" : "", b.n ? b.s : "", b.n ? ")" : "");
        sb_free(&b);
        return false;
    }
    LibWant w = {xstrdup(lib), xstrdup(constraint), xstrdup(who)};
    VEC_PUSH(vm->wants, w);
    return true;
}

/* The newest archive of library lib that meets every constraint stated so
 * far (archives of one version: the first on the library path), chosen for
 * the rest of the run; its own requirements are then stated. NULL with err
 * set if none fits; NULL with err clear if there is none of lib at all. */
static LibArchive *choose_library(VM *vm, const char *lib, MlwsError *err) {
    void *v;
    if (smap_get(&vm->libsel, lib, &v)) return v;
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
    StrBuf who = {0};
    sb_printf(&who, "library %s %s", lib, best->manifest->lib_version);
    for (uint32_t i = 0; i < best->manifest->nrequires; i++) {
        const MRequire *r = &best->manifest->requires[i];
        if (!want(vm, r->lib, r->constraint, who.s, err)) {
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
    LModule *lm = xcalloc(1, sizeof *lm);
    lm->m = m;
    lm->lib = lib;
    lm->path = xstrdup(path);
    lm->home = home;
    lm->key = key;
    lm->index = (int)vm->mods.n;
    lm->state = LM_LOADING;
    lm->deps = xcalloc(m->nimports + 1, sizeof *lm->deps);
    for (size_t i = 0; i < vm->mods.n; i++) { /* another program's util, or the library path's */
        LModule *o = vm->mods.v[i];
        if (o->state != LM_FAILED && !strcmp(o->m->name, m->name)) {
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
        if (!want(vm, m->requires[i].lib, m->requires[i].constraint, who, err)) return NULL;
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

LModule *vm_load_text(VM *vm, const char *name, const char *text, MlwsError *err) {
    Module *m = assemble_text(name, text, name, (const char *const *)vm->libpaths.v, vm->libpaths.n, err);
    if (!m) return NULL;
    plat_lock(vm->mods_lock); /* an embedding host may load while services run */
    LModule *lm = register_module(vm, m, name, NULL, NULL, false, err);
    plat_unlock(vm->mods_lock);
    return lm;
}

LModule *vm_load_dynamic(VM *vm, const char *name, const LModule *from, MlwsError *err) {
    plat_lock(vm->mods_lock);
    size_t start = vm->mods.n, chain = vm->loading.n;
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
    }
    plat_unlock(vm->mods_lock);
    return lm;
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
            if (x->home && x->state == LM_READY && !strcmp(x->m->name, name)) v = x, n++;
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
            ok = want(vm, ms[i]->requires[k].lib, ms[i]->requires[k].constraint, paths[i], &errs[i]);
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
