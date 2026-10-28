/* vm.c - VM lifetime, library path, natives registry, output. */
#include "vm.h"
#include "../heap/heap.h"

#include "../loader/library.h"
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
    const char *no_slabs = getenv("MLWSVM_NO_SLABS");
    vm->slabs = gc_slabs_enabled() && !(no_slabs && *no_slabs && strcmp(no_slabs, "0"));
    const char *stress = getenv("MLWSVM_GC_STRESS");
    vm->gc_stress = stress && *stress && strcmp(stress, "0") != 0;
    vm_options_from_env(vm); /* MLWSVM_THREADS, MLWSVM_MAX_FRAMES, ... */
    plat_ignore_sigpipe();   /* writes to a closed pipe or socket fail with E_RESET */
    plat_raise_fd_limit();   /* many connections need many descriptors */
    vm->lock = plat_lock_new();
    vm->mods_lock = plat_lock_new();
    const char *unsafe = getenv("MLWSVM_UNSAFE_NATIVE");
    vm->unsafe_native = unsafe && *unsafe && strcmp(unsafe, "0") != 0;
    vm->out_lock = plat_lock_new();
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
        free(lm->consts); /* the objects are the VM's (const_pool) */
    }
    free(lm->grefs);
    free(lm->natives);
    free(lm->sdefs); /* field names/types are borrowed from the Module */
    free(lm->structs);
    free(lm->frefs);
    free(lm->path);
    free(lm->key);
    free(lm->deps);
    module_free(m);
    free(lm);
}

void vm_free(VM *vm) {
    if (!vm) return;
    if (vm->tools_free) vm->tools_free(vm);
    sched_free(vm); /* every remaining process */
    if (sched_abandoned(vm)) { /* a native call is still running on this VM's memory */
        fprintf(stderr, "mlwsvm: a blocking native call did not return; leaving it running\n");
        return;
    }
    ext_free_all(vm);
    for (size_t i = 0; i < vm->mods.n; i++) lmodule_free(vm->mods.v[i]);
    VEC_FREE(vm->mods);
    smap_free(&vm->modmap);
    for (size_t i = 0; i < vm->archives.n; i++) lib_free(vm->archives.v[i]);
    VEC_FREE(vm->archives);
    smap_free(&vm->libsel);
    for (size_t i = 0; i < vm->const_objs.n; i++) free(vm->const_objs.v[i]);
    VEC_FREE(vm->const_objs);
    smap_free(&vm->const_pool);
    for (size_t i = 0; i < vm->wants.n; i++) {
        free(vm->wants.v[i].lib);
        free(vm->wants.v[i].constraint);
        free(vm->wants.v[i].who);
    }
    VEC_FREE(vm->wants);
    vm_programs_free(vm); /* (a run frees its programs; this frees the list) */
    VEC_FREE(vm->progs);
    free(vm->cache_dir);
    for (size_t i = 0; i < vm->libpaths.n; i++) free(vm->libpaths.v[i]);
    VEC_FREE(vm->libpaths);
    VEC_FREE(vm->loading);
    for (size_t i = 0; i < vm->homes.n; i++) {
        free(vm->homes.v[i]->dir);
        free(vm->homes.v[i]->real);
        free(vm->homes.v[i]);
    }
    VEC_FREE(vm->homes);
    VEC_FREE(vm->exits);
    for (size_t i = 0; i < vm->prog_args.n; i++) free(vm->prog_args.v[i]);
    VEC_FREE(vm->prog_args);
    for (size_t i = 0; i < vm->tickets.n; i++) plat_fd_close(vm->tickets.v[i].fd); /* given, never taken */
    VEC_FREE(vm->tickets);
    smap_free(&vm->registry);
    copy_state_free();
    smap_free(&vm->natives);
    plat_lock_free(vm->lock);
    plat_lock_free(vm->mods_lock);
    plat_lock_free(vm->out_lock);
    free(vm);
}

void vm_add_libpath(VM *vm, const char *dir) {
    for (size_t i = 0; i < vm->libpaths.n; i++)
        if (!strcmp(vm->libpaths.v[i], dir)) return;
    VEC_PUSH(vm->libpaths, xstrdup(dir));
}

void vm_register_native(VM *vm, const NativeDef *nd) { smap_put(&vm->natives, nd->name, (void *)nd); }

void vm_flush(VM *vm) {
    if (vm->out != default_out) return; /* captured output (tests, tools) has no buffer */
    plat_lock(vm->out_lock);
    fflush(stdout);
    plat_unlock(vm->out_lock);
}

void vm_write(VM *vm, const char *s, size_t n) {
    plat_lock(vm->out_lock);
    vm->out(vm->out_ctx, s, n);
    plat_unlock(vm->out_lock);
}

void proc_write(Process *p, const char *s, size_t n) {
    size_t head = n; /* through the last newline */
    while (head && s[head - 1] != '\n') head--;
    if (!head) { /* no newline: the line goes on */
        sb_putn(&p->outline, s, n);
        if (p->outline.n >= OUTLINE_MAX) proc_write_line_out(p);
        return;
    }
    VM *vm = p->vm;
    plat_lock(vm->out_lock);
    if (p->outline.n) vm->out(vm->out_ctx, p->outline.s, p->outline.n);
    vm->out(vm->out_ctx, s, head);
    plat_unlock(vm->out_lock);
    p->outline.n = 0;
    if (head < n) sb_putn(&p->outline, s + head, n - head);
}

void proc_write_line_out(Process *p) {
    if (!p->outline.n) return;
    vm_write(p->vm, p->outline.s, p->outline.n);
    p->outline.n = 0;
}
