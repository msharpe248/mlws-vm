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
    p->slabs = vm->slabs;
    uint32_t bytes = vm->stack_words * 8;
    p->stack.size = bytes; /* allocated on first use (proc_segment): most processes never touch it */
    p->stack.gen = 1;
    p->stack.kind = SEGK_STACK;
    p->sp = p->fp = PTR_MAKE(SEG_STACK, 1, bytes);
    p->stack_low_off = bytes;
    p->stack_ref_lo = vm->stack_words;
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
        uint64_t *at = frame_reg(p->regs, fr, b, cnt[b]++);
        switch (b) {
        case BANK_I: *(int64_t *)at = args[k].i; break;
        case BANK_F: *(double *)at = args[k].f; break;
        case BANK_R: *(Obj **)at = args[k].r; break;
        default: *at = args[k].p; break;
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
    if (p->reply_ready) gc_free_list(p->reply.objs);
    for (size_t i = 0; i < p->names.n; i++) free(p->names.v[i]);
    VEC_FREE(p->names);
    VEC_FREE(p->inbox);
    plat_lock_free(p->lock);
    VEC_FREE(p->links);
    VEC_FREE(p->monitors);
    VEC_FREE(p->pins);
    heap_free_all(p);
    free(p->stack.data); /* the tags too: one block (and maybe a coroutine's stack, coro.c) */
    for (uint32_t i = 0; i < p->nglobals; i++) {
        free(p->globals[i].data);
        free(p->globals[i].tags);
    }
    free(p->globals);
    for (size_t i = 0; i < p->raw.n; i++) free(p->raw.v[i].data);
    VEC_FREE(p->raw);
    VEC_FREE(p->rawfree);
    free(p->regs);
    free(p->frames);
    sb_free(&p->backtrace);
    free(p);
}

void fn_where(StrBuf *b, const Function *f, uint32_t pc, bool base) {
    const char *file;
    uint32_t sline;
    if (module_srcpos(f->mod->m, f->mf, pc, &file, &sline)) sb_printf(b, "%s:%u; ", base ? path_basename(file) : file, sline);
    uint32_t line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
    const char *src = f->mod->m->source && *f->mod->m->source ? f->mod->m->source : f->mod->path;
    sb_printf(b, "%s:%u", base ? path_basename(src) : src, line);
}

static void frame_line(StrBuf *b, const Frame *fr) {
    const Function *f = fr->fn;
    sb_printf(b, "  at %s.%s (", f->mod->m->name, f->name);
    fn_where(b, f, fr->pc ? fr->pc - 1 : 0, false);
    sb_puts(b, ")\n");
}

/* Innermost frame first. Runs of identical frames (direct recursion) are
 * collapsed, and a very deep stack keeps only its innermost and outermost
 * frames, so a runaway recursion gives a short report. */
#define BT_INNER 40
#define BT_OUTER 10
static void bt_frames(StrBuf *b, const Frame *frames, uint32_t nframes, StrBuf *cur, StrBuf *next) {
    uint32_t entries = 0; /* distinct lines printed so far */
    for (uint32_t i = nframes; i-- > 0;) {
        if (entries == BT_INNER && i >= BT_OUTER) { /* skip to the outermost frames */
            sb_printf(b, "  ... %u more frames ...\n", i - BT_OUTER + 1);
            i = BT_OUTER;
            entries++;
            continue;
        }
        cur->n = 0;
        frame_line(cur, &frames[i]);
        uint32_t repeat = 0;
        while (i > 0) { /* identical callers below this frame */
            next->n = 0;
            frame_line(next, &frames[i - 1]);
            if (strcmp(next->s, cur->s) != 0) break;
            repeat++;
            i--;
        }
        sb_puts(b, cur->s);
        if (repeat) sb_printf(b, "  ... repeated %u more time%s\n", repeat, repeat == 1 ? "" : "s");
        entries++;
    }
}

static void build_backtrace(Process *p) {
    StrBuf *b = &p->backtrace;
    b->n = 0;
    if (b->s) b->s[0] = 0;
    StrBuf cur = {0}, next = {0};
    bt_frames(b, p->frames, p->nframes, &cur, &next);
    /* In a coroutine: the code that resumed it, and so on outwards. */
    for (const ObjCoro *co = p->co; co; co = co->prev) {
        sb_puts(b, "  (in a coroutine resumed by)\n");
        bt_frames(b, co->ctx.frames, co->ctx.nframes, &cur, &next);
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

static bool unwind_frames(Process *p);

/* Find the innermost handler covering the faulting instruction, popping frames
 * that have none. Returns false if the exception is uncaught. */
static bool unwind(Process *p) {
    for (;;) {
        if (unwind_frames(p)) return true;
        /* Past the first frame of a coroutine: it dies, and the exception goes
         * on from the coresume that ran it. */
        if (p->cb_base || !p->co) return false;
        coro_leave(p, CO_DEAD);
    }
}

/* The same, within the running context. */
static bool unwind_frames(Process *p) {
    while (p->nframes > p->cb_base) { /* a callback's exceptions never unwind the code that called C */
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
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: \"%.400s\"", ((ObjString *)ex)->data);
    else if (ex && ex->kind == OBJ_STRUCT)
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: a %s struct", ((ObjStruct *)ex)->def->name);
    else
        snprintf(p->err.msg, sizeof p->err.msg, "uncaught exception: a %s", ex ? obj_kind_name(ex->kind) : "null");
}

/* ---- frames ------------------------------------------------------------------ */

/* Out-of-line slow path of proc_push_frame: limits and growth. Both stacks
 * start small (8 frames, 64 register words) and double, so that a process
 * that never calls deeply stays small. */
void proc_frame_slow(Process *p, uint32_t words) {
    if (p->nframes >= p->vm->max_frames)
        vm_trap(p, "E_CALL_DEPTH", "call depth limit of %u frames exceeded (unbounded recursion?)", p->vm->max_frames);
    if (p->nframes == p->fcap) {
        p->fcap = p->fcap ? p->fcap * 2 : 8;
        p->frames = xrealloc(p->frames, p->fcap * sizeof *p->frames);
    }
    p->flimit = p->fcap < p->vm->max_frames ? p->fcap : p->vm->max_frames;
    if (p->rtop + words > p->rcap) {
        uint32_t nc = p->rcap ? p->rcap * 2 : 64;
        while (nc < p->rtop + words) nc *= 2;
        p->regs = xrealloc(p->regs, nc * sizeof *p->regs);
        p->rcap = nc;
    }
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
    volatile uint32_t slice = left < budget ? (uint32_t)left : budget; /* live across setjmp */
    p->jb = &jb;
    if (VM_SETJMP(jb)) { /* every throw and trap lands here */
        p->jb = NULL;
        p->in_foreign = false; /* a built-in checked native may trap (on a worker thread) */
        p->cur_native = NULL;
        p->resumed = false;    /* whatever call was in progress is over */
        p->fdwait.deadline = 0;
        if (p->pending_spawn) { /* spawn trapped while copying arguments */
            proc_free(p->pending_spawn);
            p->pending_spawn = NULL;
        }
        if (unwind(p)) return SLICE_YIELD; /* continue at the handler next turn */
        describe_uncaught(p);
        return SLICE_FAILED;
    }
    int r = interp_run(p, slice);
    p->jb = NULL;
    return r;
}

bool proc_callback(Process *p, ObjClosure *cl, const Word *args) {
    /* The foreign call in progress: restored afterwards. */
    jmp_buf *outer_jb = p->jb;
    uint32_t outer_base = p->cb_base;
    const NativeDef *nd = p->cur_native;
    bool in_foreign = p->in_foreign, resumed = p->resumed, ferr = p->foreign_err;
    int req = p->native_req;
    char fcode[sizeof p->foreign_code], fmsg[sizeof p->foreign_msg];
    memcpy(fcode, p->foreign_code, sizeof fcode);
    memcpy(fmsg, p->foreign_msg, sizeof fmsg);
    uint64_t deadline = p->fdwait.deadline;

    volatile uint32_t base = p->nframes;
    volatile bool entered = false, ok = false;
    jmp_buf jb;
    p->cb_base = base;
    p->cb_exception = NULL;
    p->in_foreign = false; /* the callback is MLWSVM code: it traps and is counted as usual */
    p->cur_native = NULL;
    p->resumed = false;
    p->foreign_err = false;
    p->jb = &jb;
    for (;;) {
        if (VM_SETJMP(jb)) { /* a throw or trap inside the callback */
            p->in_foreign = false;
            p->cur_native = NULL;
            p->resumed = false;
            p->fdwait.deadline = 0;
            if (p->pending_spawn) {
                proc_free(p->pending_spawn);
                p->pending_spawn = NULL;
            }
            if (unwind(p)) continue; /* caught inside the callback: run on */
            describe_uncaught(p);
            p->cb_exception = p->exception; /* for the caller to rethrow as it was */
            break;
        }
        if (!entered) {
            entered = true;
            interp_enter(p, cl, args); /* may trap (call depth) */
        }
        if (p->insns >= p->quota[QUOTA_INSNS]) {
            snprintf(p->err.code, sizeof p->err.code, "E_QUOTA");
            snprintf(p->err.msg, sizeof p->err.msg, "instruction quota of %llu exceeded",
                     (unsigned long long)p->quota[QUOTA_INSNS]);
            break;
        }
        uint64_t left = p->quota[QUOTA_INSNS] - p->insns;
        int r = interp_run(p, left < 100000 ? (uint32_t)left : 100000);
        if (r == SLICE_YIELD) continue; /* no scheduling inside a callback: just more instructions */
        if (r == SLICE_DONE && p->nframes == base) {
            ok = true;
            break;
        }
        /* killed, or the program is ending (os.exit) */
        snprintf(p->err.code, sizeof p->err.code, "E_CALLBACK");
        snprintf(p->err.msg, sizeof p->err.msg, "the callback %s did not return", cl->fn->name);
        break;
    }
    while (p->nframes > base) proc_pop_frame(p);
    /* Set last: natives inside the callback consume the flag for themselves. */
    p->reentered = true;
    p->jb = outer_jb;
    p->cb_base = outer_base;
    p->cur_native = nd;
    p->in_foreign = in_foreign;
    p->resumed = resumed;
    p->native_req = req;
    p->foreign_err = ferr;
    memcpy(p->foreign_code, fcode, sizeof fcode);
    memcpy(p->foreign_msg, fmsg, sizeof fmsg);
    p->fdwait.deadline = deadline;
    return ok;
}

/* ---- memory ------------------------------------------------------------------ */

/* A data stack's memory, at its first use: the words, then a tag per word.
 * Until then it is not live, so every access takes proc_segment's slow path. */
static void stack_alloc(Segment *s) {
    s->data = xcalloc((size_t)s->size + s->size / 8, 1);
    s->tags = s->data + s->size;
    s->live = 1;
}

Segment *proc_segment(Process *p, uint64_t ptr) {
    uint32_t seg = PTR_SEG(ptr), gen = PTR_GEN(ptr);
    Segment *s;
    if (seg == 0) vm_trap(p, "E_NULL", "null pointer dereference");
    if (seg == SEG_STACK) {
        s = &p->stack;
        if (!s->data && s->gen == gen) stack_alloc(s);
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
    if (s->gen != gen)
        vm_trap(p, "E_BAD_PTR", seg == SEG_STACK ? "pointer into a data stack that is not running (another coroutine's)"
                                                 : "invalid pointer (bad generation)");
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
