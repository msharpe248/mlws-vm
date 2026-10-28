/* modload.c - module.*: loading modules while the program runs (docs/ABI.md,
 * "Loading modules at run time"). A module is named by its string, looked up
 * as an import from the calling module would be (a program's own module
 * looks in its directory first); once loaded it stays loaded until the VM
 * ends, so a closure for one of its functions never outlives its code. */
#include "../heap/heap.h"

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

static void n_load(Process *p, Word *a, Word *r) {
    (void)r;
    const char *name = name_arg(p, a[0], "module.load");
    /* Reading code from the file system, and maybe native extensions: an OS call (quota 5). */
    if (p->os_calls >= p->quota[QUOTA_OS])
        vm_trap(p, "E_QUOTA", "OS call quota of %llu exceeded (module.load reads files)",
                (unsigned long long)p->quota[QUOTA_OS]);
    p->os_calls++;
    MlwsError err = {{0}, {0}};
    if (!vm_load_dynamic(p->vm, name, caller_module(p), &err)) vm_trap(p, err.code, "%s", err.msg);
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
#define T(...) {__VA_ARGS__}

static const NativeDef MODULE_NATIVES[] = {
    N("module.load", 1, T(T_R), 0, T(0), n_load,
      "Load a module (and what it imports) while the program runs: as if the calling module imported it, so "
      "found in the program's directory first if the caller is one of the program's own modules, with the same "
      "library versions, privacy and verification. Loading a loaded module does nothing."),
    N("module.loaded", 1, T(T_R), 1, T(T_B), n_loaded,
      "Whether a module is loaded: the module that name means to the caller, as for module.load."),
    N("module.function", 2, T(T_R, T_R), 1, T(T_R), n_function,
      "A closure for a function a loaded module exports, to call with calli (its signature is checked then)."),
    N("module.exports", 1, T(T_R), 1, T(T_R), n_exports, "The names of the functions a loaded module exports."),
};

void natives_register_modules(VM *vm) {
    for (size_t i = 0; i < sizeof MODULE_NATIVES / sizeof MODULE_NATIVES[0]; i++)
        vm_register_native(vm, &MODULE_NATIVES[i]);
}

const NativeDef *natives_modules_table(size_t *n) {
    *n = sizeof MODULE_NATIVES / sizeof MODULE_NATIVES[0];
    return MODULE_NATIVES;
}
