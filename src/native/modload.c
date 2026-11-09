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

static void n_purge_kill(Process *p, Word *a, Word *r) {
    const char *name = name_arg(p, a[0], "module.purge_kill");
    no_caps(p, "module.purge_kill");
    uint32_t freed, kept, killed;
    MlwsError err = {{0}, {0}};
    if (!vm_purge_kill(p->vm, name, caller_module(p), p->pid, &freed, &kept, &killed, &err)) vm_trap(p, err.code, "%s", err.msg);
    r[0].i = freed;
    r[1].i = kept;
    r[2].i = killed;
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

static LModule *closure_module(Process *p, Word a, const char *fn);

/* module.function(module, function): the module by name, or by a closure of
 * one of its functions, for that closure's version. */
static void n_function(Process *p, Word *a, Word *r) {
    const Obj *o = a[0].r;
    LModule *lm = o && o->kind == OBJ_CLOSURE ? closure_module(p, a[0], "module.function") : loaded(p, a[0], "module.function");
    const char *fname = name_arg(p, a[1], "module.function");
    int i = module_find_export_func(lm->m, fname);
    if (i < 0 || (lm->m->funcs[i].flags & MF_IMPORT))
        vm_trap(p, "E_LINK_UNDEF", "module.function: module %s does not export a function '%s'", lm->m->name, fname);
    r[0].r = &closure_new(p, &lm->funcs[i], 0)->h;
}

/* module.handle(module, function): a closure that calls the function's current
 * version, whatever reloads come (vm_handle): to keep, in a global say. */
static void n_handle(Process *p, Word *a, Word *r) {
    LModule *lm = loaded(p, a[0], "module.handle");
    const char *fname = name_arg(p, a[1], "module.handle");
    MlwsError err = {{0}, {0}};
    Function *h = vm_handle(p->vm, lm, fname, &err);
    if (!h) vm_trap(p, err.code, "%s", err.msg);
    r[0].r = &closure_new(p, h, 0)->h;
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

/* module.watch(name, on): whether this process is told, by a Reloaded
 * {module}, of each new version of a module of that name (module.reload), in
 * its mailbox before any message the new version's code sends. */
static void n_watch(Process *p, Word *a, Word *r) {
    (void)r;
    const char *name = name_arg(p, a[0], "module.watch");
    VM *vm = p->vm;
    plat_lock(vm->lock);
    size_t at = vm->mod_watchers.n;
    for (size_t i = 0; i < vm->mod_watchers.n && at == vm->mod_watchers.n; i++)
        if (vm->mod_watchers.v[i].pid == p->pid && !strcmp(vm->mod_watchers.v[i].name, name)) at = i;
    if (a[1].i && at == vm->mod_watchers.n) {
        struct ModWatch w = {xstrdup(name), p->pid};
        VEC_PUSH(vm->mod_watchers, w);
    }
    if (!a[1].i && at < vm->mod_watchers.n) {
        free(vm->mod_watchers.v[at].name);
        vm->mod_watchers.v[at] = vm->mod_watchers.v[--vm->mod_watchers.n];
    }
    plat_unlock(vm->lock);
}

/* The version a closure's function is of. */
static LModule *closure_module(Process *p, Word a, const char *fn) {
    const ObjClosure *c = a.r;
    if (!c || c->h.kind != OBJ_CLOSURE) vm_trap(p, "E_TYPE", "%s: expected a closure", fn);
    return (c->fn->target ? c->fn->target : c->fn)->mod;
}

/* module.of(closure): the name of the module its function is of, and which
 * version: a number of its own among all the VM has loaded. */
static void n_of(Process *p, Word *a, Word *r) {
    const LModule *lm = closure_module(p, a[0], "module.of");
    r[0].r = &str_new(p, lm->m->name, strlen(lm->m->name))->h;
    r[1].i = (int64_t)lm->index;
}

/* module.current(closure, function): a closure for a function that the
 * current version of the closure's module exports: the version a reload
 * brought in since, or the closure's own. So code in a library can follow
 * a program's module, which it could not name. */
static void n_current(Process *p, Word *a, Word *r) {
    LModule *lm = closure_module(p, a[0], "module.current");
    const char *fname = name_arg(p, a[1], "module.current");
    LModule *now = lm->replaced ? vm_module_named(p->vm, lm->m->name, lm) : lm;
    if (!now) now = lm;
    int i = module_find_export_func(now->m, fname);
    if (i < 0 || (now->m->funcs[i].flags & MF_IMPORT))
        vm_trap(p, "E_LINK_UNDEF", "module.current: module %s does not export a function '%s'", now->m->name, fname);
    proc_grow_globals(p);
    r[0].r = &closure_new(p, &now->funcs[i], 0)->h;
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
    N("module.purge_kill", 1, T(T_R), 3, T(T_I64, T_I64, T_I64), n_purge_kill,
      "As module.purge, after killing (E_KILLED) every process but the caller that is still running an old version, as Erlang's code:purge does. Returns how many versions were freed and kept, and how many processes were killed. A version the caller itself holds, or one that a blocking native call or a debugger, profiler or trace makes unsure, is kept, and nobody is killed for it."),
    N("module.loaded", 1, T(T_R), 1, T(T_B), n_loaded,
      "Whether a module is loaded: the module that name means to the caller, as for module.load."),
    N("module.function", 2, T(T_R, T_R), 1, T(T_R), n_function,
      "A closure for a function a loaded module exports, to call with calli (its signature is checked then). The module is a name, or a closure of one of its functions, for that closure's version."),
    N("module.exports", 1, T(T_R), 1, T(T_R), n_exports, "The names of the functions a loaded module exports."),
    N("module.handle", 2, T(T_R, T_R), 1, T(T_R), n_handle,
      "A closure for a function a loaded module exports that always calls its current version: unlike module.function's, it follows module.reload, so it can be kept. Call it with calli. A reload may remove the function or change its signature, which it may not for a module calling it: the handle then raises E_LINK_UNDEF when called, and calls the function again once a later version has it with the handle's signature."),
    N("module.watch", 2, T(T_R, T_B), 0, T(0), n_watch,
      "Whether this process is told, by a built-in Reloaded {module} struct, of each new version of a module of that name that a reload brings in; it comes before any message the new version's code sends."),
    N("module.of", 1, T(T_R), 2, T(T_R, T_I64), n_of,
      "The name of the module a closure's function is of, and its version's number, which no other version loaded has."),
    N("module.current", 2, T(T_R, T_R), 1, T(T_R), n_current,
      "A closure for a function that the current version of a closure's module exports: the version a reload has brought in since the closure was made, or the closure's own. E_LINK_UNDEF if it does not export it."),
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
