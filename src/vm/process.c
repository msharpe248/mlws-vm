/* process.c - process creation, memory segments (stack, globals, raw heap),
 * call frames, traps and backtraces. */
#include "vm.h"

#include "../heap/heap.h"
#include "../platform/platform.h"
#include "proc_internal.h"

Process *proc_new(VM *vm) {
    Process *p = xcalloc(1, sizeof *p);
    p->vm = vm;
    p->lock = plat_lock_new();
    uint32_t bytes = vm->stack_words * 8;
    p->stack.data = xcalloc(bytes, 1);
    p->stack.tags = xcalloc(vm->stack_words, 1);
    p->stack.size = bytes;
    p->stack.gen = 1;
    p->stack.kind = SEGK_STACK;
    p->stack.live = 1;
    p->sp = p->fp = PTR_MAKE(SEG_STACK, 1, bytes);
    p->stack_low_off = bytes;
    p->nglobals = (uint32_t)vm->mods.n;
    p->globals = xcalloc(p->nglobals + 1, sizeof(Segment));
    for (uint32_t i = 0; i < p->nglobals; i++) {
        const Module *m = vm->mods.v[i]->m;
        Segment *s = &p->globals[i];
        s->size = m->gsize;
        s->data = xcalloc(m->gsize + 1, 1);
        s->tags = xcalloc(m->gsize / 8 + 1, 1);
        memcpy(s->data, m->gdata, m->gsize);
        memcpy(s->tags, m->gtags, m->gsize / 8);
        s->gen = 1;
        s->kind = SEGK_GLOBAL;
        s->live = 1;
    }
    for (int b = 0; b < NBANKS; b++) p->rcap[b] = 0;
    memcpy(p->quota, vm->default_quota, sizeof p->quota);
    /* The pid is assigned now; the process joins the table in sched_start. */
    plat_lock(vm->lock);
    p->pid = vm->next_pid++;
    vm->live++;
    vm->spawned++;
    if (vm->live > vm->max_live) vm->max_live = vm->live;
    plat_unlock(vm->lock);
    return p;
}

void proc_start(Process *p, Function *fn, Obj *closure, const Word *args, int nargs) {
    p->entry = fn;
    proc_push_frame(p, fn, NULL);
    p->frames[0].closure = closure;
    const Frame *fr = &p->frames[p->nframes - 1];
    int cnt[NBANKS] = {0};
    for (int k = 0; k < nargs; k++) {
        Bank b = type_bank((MlwsType)fn->mf->params[k]);
        uint32_t at = fr->base[b] + (uint32_t)cnt[b]++;
        switch (b) {
        case BANK_I: p->ireg[at] = args[k].i; break;
        case BANK_F: p->freg[at] = args[k].f; break;
        case BANK_R: p->rreg[at] = args[k].r; break;
        default: p->preg[at] = args[k].p; break;
        }
    }
    sched_start(p->vm, p);
}

void proc_free(Process *p) {
    if (!p) return;
    VM *vm = p->vm;
    if (p->published) sched_unpublish(p);
    if (p->state != PROC_DONE && p->state != PROC_FAILED) vm->live--;
    vm->insns_retired += p->insns;
    VEC_FREE(p->mbox);
    for (size_t i = 0; i < p->inbox.n; i++) gc_free_list(p->inbox.v[i].objs);
    VEC_FREE(p->inbox);
    plat_lock_free(p->lock);
    VEC_FREE(p->links);
    VEC_FREE(p->monitors);
    heap_free_all(p);
    free(p->stack.data);
    free(p->stack.tags);
    for (uint32_t i = 0; i < p->nglobals; i++) {
        free(p->globals[i].data);
        free(p->globals[i].tags);
    }
    free(p->globals);
    for (size_t i = 0; i < p->raw.n; i++) free(p->raw.v[i].data);
    VEC_FREE(p->raw);
    VEC_FREE(p->rawfree);
    free(p->ireg);
    free(p->freg);
    free(p->rreg);
    free(p->preg);
    free(p->frames);
    sb_free(&p->backtrace);
    free(p);
}

static void frame_line(StrBuf *b, const Frame *fr) {
    const Function *f = fr->fn;
    uint32_t pc = fr->pc ? fr->pc - 1 : 0;
    uint32_t line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
    const char *src = f->mod->m->source && *f->mod->m->source ? f->mod->m->source : f->mod->path;
    sb_printf(b, "  at %s.%s (%s:%u)\n", f->mod->m->name, f->name, src, line);
}

/* Innermost frame first. Runs of identical frames (direct recursion) are
 * collapsed, and a very deep stack keeps only its innermost and outermost
 * frames, so a runaway recursion gives a short report. */
#define BT_INNER 40
#define BT_OUTER 10
static void build_backtrace(Process *p) {
    StrBuf *b = &p->backtrace;
    b->n = 0;
    if (b->s) b->s[0] = 0;
    uint32_t entries = 0; /* distinct lines printed so far */
    StrBuf cur = {0}, next = {0};
    for (uint32_t i = p->nframes; i-- > 0;) {
        if (entries == BT_INNER && i >= BT_OUTER) { /* skip to the outermost frames */
            sb_printf(b, "  ... %u more frames ...\n", i - BT_OUTER + 1);
            i = BT_OUTER;
            entries++;
            continue;
        }
        cur.n = 0;
        frame_line(&cur, &p->frames[i]);
        uint32_t repeat = 0;
        while (i > 0) { /* identical callers below this frame */
            next.n = 0;
            frame_line(&next, &p->frames[i - 1]);
            if (strcmp(next.s, cur.s) != 0) break;
            repeat++;
            i--;
        }
        sb_puts(b, cur.s);
        if (repeat) sb_printf(b, "  ... repeated %u more time%s\n", repeat, repeat == 1 ? "" : "s");
        entries++;
    }
    sb_free(&cur);
    sb_free(&next);
}

StructDef VM_BUILTIN_DEFS[BUILTIN__COUNT];

void proc_throw(Process *p, Obj *ex) {
    if (!p->jb) {
        fprintf(stderr, "mlwsvm: exception raised outside the interpreter\n");
        abort();
    }
    build_backtrace(p);
    p->exception = ex;
    p->exceptions_thrown++;
    VM_LONGJMP(*p->jb, 1);
}

void vm_trap(Process *p, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    build_backtrace(p);
    p->raising = true; /* the Error itself must not trip a heap quota (that would recurse) */
    ObjStruct *e = struct_new(p, &VM_ERROR_DEF);
    e->fields[ERRF_CODE].r = str_new(p, code, strlen(code));
    e->fields[ERRF_MESSAGE].r = str_new(p, msg, strlen(msg));
    e->fields[ERRF_TRACE].r = str_new(p, p->backtrace.s ? p->backtrace.s : "", p->backtrace.n);
    p->raising = false;
    proc_throw(p, &e->h);
}

/* Find the innermost handler covering the faulting instruction, popping frames
 * that have none. Returns false if the exception is uncaught. */
static bool unwind(Process *p) {
    while (p->nframes) {
        Frame *fr = &p->frames[p->nframes - 1];
        const MFunc *mf = fr->fn->mf;
        uint32_t pc = fr->pc ? fr->pc - 1 : 0;
        const MHandler *best = NULL;
        for (uint32_t i = 0; i < mf->nhandlers; i++) {
            const MHandler *h = &mf->handlers[i];
            if (pc >= h->start && pc < h->end && (!best || h->end - h->start < best->end - best->start)) best = h;
        }
        if (best) {
            fr->pc = best->target;
            return true;
        }
        proc_pop_frame(p);
    }
    return false;
}

/* Describe an uncaught exception in p->err. */
static void describe_uncaught(Process *p) {
    Obj *ex = p->exception;
    if (ex && ex->kind == OBJ_STRUCT && ((ObjStruct *)ex)->def == &VM_ERROR_DEF) {
        ObjStruct *e = (ObjStruct *)ex;
        Obj *code = e->fields[ERRF_CODE].r, *msg = e->fields[ERRF_MESSAGE].r;
        snprintf(p->err.code, sizeof p->err.code, "%s",
                 code && code->kind == OBJ_STRING ? ((ObjString *)code)->data : "E_UNCAUGHT");
        snprintf(p->err.msg, sizeof p->err.msg, "%s",
                 msg && msg->kind == OBJ_STRING ? ((ObjString *)msg)->data : "(no message)");
        return;
    }
    snprintf(p->err.code, sizeof p->err.code, "E_UNCAUGHT");
    if (ex && ex->kind == OBJ_STRING)
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: \"%.1000s\"", ((ObjString *)ex)->data);
    else if (ex && ex->kind == OBJ_STRUCT)
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: a %s struct", ((ObjStruct *)ex)->def->name);
    else
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: a %s", ex ? obj_kind_name(ex->kind) : "null");
}

/* ---- frames ------------------------------------------------------------------ */

static void grow_bank(Process *p, int b, uint32_t need) {
    if (need <= p->rcap[b]) return;
    uint32_t nc = p->rcap[b] ? p->rcap[b] * 2 : 256;
    while (nc < need) nc *= 2;
    switch (b) {
    case BANK_I: p->ireg = xrealloc(p->ireg, nc * sizeof *p->ireg); break;
    case BANK_F: p->freg = xrealloc(p->freg, nc * sizeof *p->freg); break;
    case BANK_R: p->rreg = xrealloc(p->rreg, nc * sizeof *p->rreg); break;
    default: p->preg = xrealloc(p->preg, nc * sizeof *p->preg); break;
    }
    p->rcap[b] = nc;
}

/* Out-of-line slow path of proc_push_frame: limits and growth. */
void proc_frame_slow(Process *p, const uint16_t *nr) {
    if (p->nframes >= p->vm->max_frames)
        vm_trap(p, "E_CALL_DEPTH", "call depth limit of %u frames exceeded (unbounded recursion?)", p->vm->max_frames);
    if (p->nframes == p->fcap) {
        p->fcap = p->fcap ? p->fcap * 2 : 64;
        p->frames = xrealloc(p->frames, p->fcap * sizeof *p->frames);
    }
    for (int b = 0; b < NBANKS; b++) grow_bank(p, b, p->rtop[b] + nr[b]);
}

void proc_backtrace(Process *p) { build_backtrace(p); }

int proc_run_slice(Process *p, uint32_t budget) {
    jmp_buf jb;
    if (p->insns >= p->quota[QUOTA_INSNS]) { /* a hard CPU limit: not catchable */
        snprintf(p->err.code, sizeof p->err.code, "E_QUOTA");
        snprintf(p->err.msg, sizeof p->err.msg, "instruction quota of %llu exceeded",
                 (unsigned long long)p->quota[QUOTA_INSNS]);
        build_backtrace(p);
        return SLICE_FAILED;
    }
    uint64_t left = p->quota[QUOTA_INSNS] - p->insns;
    if (left < budget) budget = (uint32_t)left;
    p->jb = &jb;
    if (VM_SETJMP(jb)) { /* every throw and trap lands here */
        p->jb = NULL;
        if (p->pending_spawn) { /* spawn trapped while copying arguments */
            proc_free(p->pending_spawn);
            p->pending_spawn = NULL;
        }
        if (unwind(p)) return SLICE_YIELD; /* continue at the handler next turn */
        describe_uncaught(p);
        return SLICE_FAILED;
    }
    int r = interp_run(p, budget);
    p->jb = NULL;
    return r;
}

/* ---- memory ------------------------------------------------------------------ */

Segment *proc_segment(Process *p, uint64_t ptr) {
    uint32_t seg = PTR_SEG(ptr), gen = PTR_GEN(ptr);
    Segment *s;
    if (seg == 0) vm_trap(p, "E_NULL", "null pointer dereference");
    if (seg == SEG_STACK) {
        s = &p->stack;
    } else if (seg < SEG_RAW0) {
        if (seg - SEG_GLOBAL0 >= p->nglobals) vm_trap(p, "E_BAD_PTR", "invalid pointer (unknown global segment %u)", seg);
        s = &p->globals[seg - SEG_GLOBAL0];
    } else {
        uint32_t i = seg - SEG_RAW0;
        if (i >= p->raw.n) vm_trap(p, "E_BAD_PTR", "invalid pointer (unknown segment %u)", seg);
        s = &p->raw.v[i];
        if (!s->live || s->gen != gen) vm_trap(p, "E_UAF", "use of a freed or reallocated raw block");
        return s;
    }
    if (s->gen != gen) vm_trap(p, "E_BAD_PTR", "invalid pointer (bad generation)");
    return s;
}

static void raw_quota(Process *p, uint64_t more) {
    if (p->raw_bytes + more > p->quota[QUOTA_RAW])
        vm_trap(p, "E_QUOTA", "raw memory quota of %llu bytes exceeded", (unsigned long long)p->quota[QUOTA_RAW]);
}

uint64_t proc_alloc(Process *p, int64_t size) {
    if (size < 0 || size > (int64_t)UINT32_MAX)
        vm_trap(p, "E_BAD_ALLOC", "cannot allocate %lld bytes", (long long)size);
    raw_quota(p, (uint64_t)size);
    uint32_t idx;
    if (p->rawfree.n) {
        idx = p->rawfree.v[--p->rawfree.n];
    } else {
        if (p->raw.n >= SEG_MAX - SEG_RAW0) vm_trap(p, "E_BAD_ALLOC", "too many live raw blocks");
        Segment z;
        memset(&z, 0, sizeof z);
        idx = (uint32_t)p->raw.n;
        VEC_PUSH(p->raw, z);
    }
    Segment *s = &p->raw.v[idx];
    s->gen = (uint16_t)((s->gen + 1) & 0xFFF);
    if (s->gen == 0) s->gen = 1;
    s->data = xcalloc((size_t)size + 1, 1);
    s->size = (uint32_t)size;
    s->kind = SEGK_RAW;
    s->live = 1;
    p->raw_bytes += (uint64_t)size;
    p->raw_allocs++;
    if (p->raw_bytes > p->raw_peak) p->raw_peak = p->raw_bytes;
    return PTR_MAKE(SEG_RAW0 + idx, s->gen, 0);
}

static Segment *raw_block(Process *p, uint64_t ptr, const char *what) {
    uint32_t seg = PTR_SEG(ptr);
    if (seg < SEG_RAW0 || PTR_OFF(ptr) != 0)
        vm_trap(p, "E_BAD_FREE", "%s of a pointer that is not the start of a raw block", what);
    uint32_t i = seg - SEG_RAW0;
    if (i >= p->raw.n) vm_trap(p, "E_BAD_FREE", "%s of an invalid pointer", what);
    Segment *s = &p->raw.v[i];
    if (!s->live || s->gen != PTR_GEN(ptr)) vm_trap(p, "E_DOUBLE_FREE", "%s of an already freed block", what);
    return s;
}

void proc_free_block(Process *p, uint64_t ptr) {
    if (ptr == 0) return;
    Segment *s = raw_block(p, ptr, "free");
    p->raw_bytes -= s->size;
    p->raw_frees++;
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->live = 0;
    VEC_PUSH(p->rawfree, PTR_SEG(ptr) - SEG_RAW0);
}

uint64_t proc_realloc(Process *p, uint64_t ptr, int64_t size) {
    if (ptr == 0) return proc_alloc(p, size);
    if (size < 0 || size > (int64_t)UINT32_MAX)
        vm_trap(p, "E_BAD_ALLOC", "cannot allocate %lld bytes", (long long)size);
    Segment *s = raw_block(p, ptr, "realloc");
    if ((uint32_t)size > s->size) raw_quota(p, (uint64_t)size - s->size);
    s->data = xrealloc(s->data, (size_t)size + 1);
    if ((uint32_t)size > s->size) memset(s->data + s->size, 0, (size_t)size - s->size);
    p->raw_bytes = p->raw_bytes - s->size + (uint64_t)size;
    if (p->raw_bytes > p->raw_peak) p->raw_peak = p->raw_bytes;
    s->size = (uint32_t)size;
    s->gen = (uint16_t)((s->gen + 1) & 0xFFF);
    if (s->gen == 0) s->gen = 1;
    return PTR_MAKE(PTR_SEG(ptr), s->gen, 0);
}
