/* modload.c - module.*: loading modules while the program runs (docs/ABI.md,
 * "Loading modules at run time"). A module is named by its string, looked up
 * as an import from the calling module would be (a program's own module
 * looks in its directory first). A version a reload replaced stays until
 * module.purge finds nothing that can run it. */
#include "../heap/heap.h"
#include "../platform/platform.h"

static const char *name_arg(Process *p, Word a, const char *fn) {
    Obj *o = a.r;
    if (!o) vm_trap(p, "E_NULL", "%s: null where a module name was expected", fn);
    if (o->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "%s: a module name is a string, not %s", fn, obj_kind_name(o->kind));
    return ((ObjString *)o)->data;
}

/* The module of the function that called the native: whose import this is. */
static const LModule *caller_module(Process *p) { return p->nframes ? p->frames[p->nframes - 1].fn->mod : NULL; }

static LModule *loaded(Process *p, Word a, const char *fn) {
    const char *name = name_arg(p, a, fn);
    LModule *lm = vm_module_named(p->vm, name, caller_module(p));
    if (!lm) vm_trap(p, "E_LOAD_NOT_FOUND", "%s: module %s is not loaded (module.load loads it)", fn, name);
    return lm;
}

/* Code from a file, or text that may .include one, reaches what a restricted
 * process could not read. */
static void no_caps(Process *p, const char *fn) {
    if (p->caps) vm_trap(p, "E_PERMISSION", "%s: not allowed in a restricted process", fn);
}

static void n_load(Process *p, Word *a, Word *r) {
    (void)r;
    const char *name = name_arg(p, a[0], "module.load");
    no_caps(p, "module.load");
    /* Reading code from the file system, and maybe native extensions: an OS call (quota 5). */
    if (p->os_calls >= p->quota[QUOTA_OS])
        vm_trap(p, "E_QUOTA", "OS call quota of %llu exceeded (module.load reads files)",
                (unsigned long long)p->quota[QUOTA_OS]);
    p->os_calls++;
    MlwsError err = {{0}, {0}};
    if (!vm_load_dynamic(p->vm, name, caller_module(p), &err)) vm_trap(p, err.code, "%s", err.msg);
    proc_grow_globals(p);
}

/* Charged as module.load is. */
static void charge_load(Process *p, const char *fn) {
    if (p->os_calls >= p->quota[QUOTA_OS])
        vm_trap(p, "E_QUOTA", "OS call quota of %llu exceeded (%s reads files)", (unsigned long long)p->quota[QUOTA_OS], fn);
    p->os_calls++;
}

static void n_reload(Process *p, Word *a, Word *r) {
    (void)r;
    const char *name = name_arg(p, a[0], "module.reload");
    no_caps(p, "module.reload");
    charge_load(p, "module.reload");
    MlwsError err = {{0}, {0}};
    if (!vm_reload(p->vm, name, NULL, caller_module(p), &err)) vm_trap(p, err.code, "%s", err.msg);
    proc_grow_globals(p);
}

static void n_purge(Process *p, Word *a, Word *r) {
    const char *name = name_arg(p, a[0], "module.purge");
    no_caps(p, "module.purge");
    uint32_t freed, kept;
    MlwsError err = {{0}, {0}};
    if (!vm_purge(p->vm, name, caller_module(p), &freed, &kept, &err)) vm_trap(p, err.code, "%s", err.msg);
    r[0].i = freed;
    r[1].i = kept;
}

static void n_reload_text(Process *p, Word *a, Word *r) {
    (void)r;
    const char *name = name_arg(p, a[0], "module.reload_text");
    const char *text = name_arg(p, a[1], "module.reload_text");
    no_caps(p, "module.reload_text");
    charge_load(p, "module.reload_text");
    MlwsError err = {{0}, {0}};
    if (!vm_reload(p->vm, name, text, caller_module(p), &err)) vm_trap(p, err.code, "%s", err.msg);
    proc_grow_globals(p);
}

static void n_loaded(Process *p, Word *a, Word *r) {
    r[0].i = vm_module_named(p->vm, name_arg(p, a[0], "module.loaded"), caller_module(p)) != NULL;
}

static void n_function(Process *p, Word *a, Word *r) {
    LModule *lm = loaded(p, a[0], "module.function");
    const char *fname = name_arg(p, a[1], "module.function");
    int i = module_find_export_func(lm->m, fname);
    if (i < 0 || (lm->m->funcs[i].flags & MF_IMPORT))
        vm_trap(p, "E_LINK_UNDEF", "module.function: module %s does not export a function '%s'", lm->m->name, fname);
    r[0].r = &closure_new(p, &lm->funcs[i], 0)->h;
}

static void n_exports(Process *p, Word *a, Word *r) {
    LModule *lm = loaded(p, a[0], "module.exports");
    ObjList *l = list_new(p, 0);
    r[0].r = &l->h;
    for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
        const MFunc *f = &lm->m->funcs[i];
        if ((f->flags & MF_EXPORT) && !(f->flags & MF_IMPORT))
            list_push(p, l, (Word){.r = &str_new(p, f->name, strlen(f->name))->h}, T_R);
    }
}

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
/* The struct type its module's current version gives def's name, or def if
 * none (or def is that type): def's own version found among the loaded ones,
 * then the version its name now means. */
static const StructDef *current_def(VM *vm, const StructDef *def) {
    const LModule *owner = NULL;
    plat_lock(vm->mods_lock);
    for (size_t i = 0; i < vm->mods.n && !owner; i++) {
        const LModule *lm = vm->mods.v[i];
        if (lm->sdefs && def >= lm->sdefs && def < lm->sdefs + lm->m->nstructs) owner = lm;
    }
    plat_unlock(vm->mods_lock);
    if (!owner || !owner->replaced) return def;
    const LModule *now = vm_module_named(vm, owner->m->name, owner);
    if (!now || now == owner || now->state != LM_READY) return def;
    for (uint32_t i = 0; i < now->m->nstructs; i++)
        if (!(now->m->structs[i].flags & (MSF_IMPORT | MSF_BUILTIN)) && !strcmp(now->structs[i]->name, def->name))
            return now->structs[i];
    return def;
}

/* module.upgrade(v): a struct object of a type that a reload changed, as an
 * object of the type its module's current version has under that name: each
 * field that keeps its name and type keeps its value, and the rest start as
 * new fields do. Anything else comes back as it is. */
static void n_upgrade(Process *p, Word *a, Word *r) {
    const ObjStruct *o = a[0].r;
    r[0] = a[0];
    if (!o || o->h.kind != OBJ_STRUCT) return;
    const StructDef *def = o->def, *nd = current_def(p->vm, def);
    if (nd == def) return;
    ObjStruct *n = struct_new(p, nd); /* (o is an argument, so live; and objects do not move) */
    for (uint32_t i = 0; i < nd->nfields; i++)
        for (uint32_t j = 0; j < def->nfields; j++)
            if (!strcmp(nd->fields[i], def->fields[j])) {
                if (nd->types[i] == def->types[j]) n->fields[i] = o->fields[j];
                break;
            }
    r[0].r = n;
}

#define T(...) {__VA_ARGS__}

static const NativeDef MODULE_NATIVES[] = {
    N("module.load", 1, T(T_R), 0, T(0), n_load,
      "Load a module (and what it imports) while the program runs: as if the calling module imported it, so "
      "found in the program's directory first if the caller is one of the program's own modules, with the same "
      "library versions, privacy and verification. Loading a loaded module does nothing."),
    N("module.reload", 1, T(T_R), 0, T(0), n_reload,
      "Load a new version of a loaded module from its file: calls from other modules go to it from now on, while code already running, closures made from the old version and the old version's calls to itself stay on the old one. Fails with nothing changed if a module using it would not link to the new version (E_LINK_RELOAD, E_LINK_UNDEF)."),
    N("module.reload_text", 2, T(T_R, T_R), 0, T(0), n_reload_text,
      "The same, with the new version's assembly text: a program's own compiler or deployment can supply it."),
    N("module.purge", 1, T(T_R), 2, T(T_I64, T_I64), n_purge,
      "Free the old versions of a module that reloads replaced and that nothing can run now: no process has one of their functions on its stack, or reaches a closure or a suspended coroutine of theirs. Returns how many were freed, and how many were kept (still in use, or not to be checked now: a process is in a blocking native call, or a debugger, profiler or trace is on). Struct types stay, and a pointer into a freed version's data traps."),
    N("module.loaded", 1, T(T_R), 1, T(T_B), n_loaded,
      "Whether a module is loaded: the module that name means to the caller, as for module.load."),
    N("module.function", 2, T(T_R, T_R), 1, T(T_R), n_function,
      "A closure for a function a loaded module exports, to call with calli (its signature is checked then)."),
    N("module.exports", 1, T(T_R), 1, T(T_R), n_exports, "The names of the functions a loaded module exports."),
    N("module.upgrade", 1, T(T_R), 1, T(T_R), n_upgrade,
      "A struct object of a type that a reload changed, as an object of the type its module's current version has under that name: fields that keep their name and type keep their values, new ones start empty. Anything else is returned as it is."),
};

void natives_register_modules(VM *vm) {
    for (size_t i = 0; i < sizeof MODULE_NATIVES / sizeof MODULE_NATIVES[0]; i++)
        vm_register_native(vm, &MODULE_NATIVES[i]);
}

const NativeDef *natives_modules_table(size_t *n) {
    *n = sizeof MODULE_NATIVES / sizeof MODULE_NATIVES[0];
    return MODULE_NATIVES;
}
