/* vm.c - VM lifetime, library path, natives registry, output. */
#include "vm.h"

#include "../platform/platform.h"

static void default_out(void *ctx, const char *s, size_t n) {
    (void)ctx;
    fwrite(s, 1, n, stdout);
}

ObjString *str_new_immortal(const char *s, size_t len) {
    ObjString *o = xmalloc(sizeof *o + len + 1);
    memset(&o->h, 0, sizeof o->h);
    o->h.kind = OBJ_STRING;
    o->h.flags = OBJF_IMMORTAL;
    o->len = (uint32_t)len;
    o->hash = hash_bytes(s, len);
    memcpy(o->data, s, len);
    o->data[len] = 0;
    return o;
}

static void builtins_init(void) {
    for (int i = 0; i < BUILTIN__COUNT; i++) {
        const BuiltinStruct *b = &BUILTIN_STRUCTS[i];
        VM_BUILTIN_DEFS[i].name = b->name;
        VM_BUILTIN_DEFS[i].module = "(builtin)";
        VM_BUILTIN_DEFS[i].nfields = b->nfields;
        VM_BUILTIN_DEFS[i].fields = (char **)b->fields;
        VM_BUILTIN_DEFS[i].types = (uint8_t *)b->types;
    }
}

VM *vm_new(void) {
    builtins_init();
    VM *vm = xcalloc(1, sizeof *vm);
    vm->out = default_out;
    vm->stack_words = 128 * 1024; /* 1 MiB data stack per process */
    vm->max_frames = 100000;
    vm->next_pid = 1;
    vm->slice_insns = 2000;
    vm->start_ns = plat_now_ns();
    for (int i = 0; i < QUOTA__COUNT; i++) vm->default_quota[i] = UINT64_MAX;
    const char *stress = getenv("MLWSVM_GC_STRESS");
    vm->gc_stress = stress && *stress && strcmp(stress, "0") != 0;
    op_init();
    natives_register_builtin(vm);
    return vm;
}

static void lmodule_free(LModule *lm) {
    Module *m = lm->m;
    if (lm->funcs) {
        for (uint32_t i = 0; i < m->nfuncs; i++) {
            Function *f = &lm->funcs[i];
            for (uint32_t k = 0; k < f->ncode; k++) insn_free(&f->code[k]);
            free(f->code);
            free(f->bp);
            free(f->prof);
        }
        free(lm->funcs);
    }
    if (lm->consts) {
        for (uint32_t i = 0; i < m->nconsts; i++) free(lm->consts[i]);
        free(lm->consts);
    }
    free(lm->grefs);
    free(lm->natives);
    free(lm->sdefs); /* field names/types are borrowed from the Module */
    free(lm->structs);
    free(lm->frefs);
    free(lm->path);
    module_free(m);
    free(lm);
}

void vm_free(VM *vm) {
    if (!vm) return;
    if (vm->tools_free) vm->tools_free(vm);
    for (size_t i = 0; i < vm->mods.n; i++) lmodule_free(vm->mods.v[i]);
    VEC_FREE(vm->mods);
    smap_free(&vm->modmap);
    for (size_t i = 0; i < vm->libpaths.n; i++) free(vm->libpaths.v[i]);
    VEC_FREE(vm->libpaths);
    for (size_t i = 0; i < vm->loading.n; i++) free(vm->loading.v[i]);
    VEC_FREE(vm->loading);
    for (size_t i = 0; i < vm->procs.n; i++) proc_free(vm->procs.v[i]);
    VEC_FREE(vm->procs);
    VEC_FREE(vm->runq);
    VEC_FREE(vm->timers);
    VEC_FREE(vm->exits);
    copy_state_free(vm);
    smap_free(&vm->natives);
    free(vm);
}

void vm_add_libpath(VM *vm, const char *dir) {
    for (size_t i = 0; i < vm->libpaths.n; i++)
        if (!strcmp(vm->libpaths.v[i], dir)) return;
    VEC_PUSH(vm->libpaths, xstrdup(dir));
}

void vm_register_native(VM *vm, const NativeDef *nd) { smap_put(&vm->natives, nd->name, (void *)nd); }

void vm_write(VM *vm, const char *s, size_t n) { vm->out(vm->out_ctx, s, n); }
