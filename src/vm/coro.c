/* coro.c - coroutines: more than one execution context in a process.
 *
 * A coroutine has its own register stacks, call frames and data stack, and
 * shares everything else with its process: the heap, globals, raw memory and
 * mailbox. Process always holds the running context, so a switch exchanges it
 * with the one a coroutine object holds (coro_swap) and the interpreter just
 * reloads its frame. After coresume the object holds its resumer's context,
 * and after coyield (or the end) its own again: the resumers of the running
 * coroutine form a chain through ObjCoro.prev, each holding the context below.
 *
 * The instructions themselves are in interp.c; unwinding an exception out of a
 * coroutine is in process.c (unwind). See docs/ABI.md ("Coroutines"). */
#include "vm.h"

#include "../heap/heap.h"

/* 8 KiB, unless conew asks for another size: making a coroutine costs about as
 * much again for every further 8 KiB, since its stack must start zeroed. */
#define CORO_STACK_WORDS 1024u

#define SWAP(a, b)                                                             \
    do {                                                                       \
        __typeof__(a) t_ = (a);                                                \
        (a) = (b);                                                             \
        (b) = t_;                                                              \
    } while (0)

void coro_swap(Process *p, ObjCoro *co) {
    ExecCtx *x = &co->ctx;
    SWAP(p->ireg, x->ireg);
    SWAP(p->freg, x->freg);
    SWAP(p->rreg, x->rreg);
    SWAP(p->preg, x->preg);
    for (int b = 0; b < NBANKS; b++) {
        SWAP(p->rcap[b], x->rcap[b]);
        SWAP(p->rtop[b], x->rtop[b]);
    }
    SWAP(p->frames, x->frames);
    SWAP(p->nframes, x->nframes);
    SWAP(p->fcap, x->fcap);
    SWAP(p->stack, x->stack);
    SWAP(p->sp, x->sp);
    SWAP(p->fp, x->fp);
    SWAP(p->stack_low_off, x->stack_low_off);
    SWAP(p->stack_ref_lo, x->stack_ref_lo);
}

ObjCoro *coro_new(Process *p, ObjClosure *fn, int64_t words) {
    if (words == 0) words = CORO_STACK_WORDS;
    if (words < 16 || words > (int64_t)(UINT32_MAX / 8))
        vm_trap(p, "E_BAD_ALLOC", "a coroutine stack of %lld words is out of range (16 to %u)", (long long)words,
                UINT32_MAX / 8);
    ObjCoro *co = (ObjCoro *)gc_alloc_unaccounted(p, OBJ_CORO, sizeof(ObjCoro));
    co->fn = fn;
    co->words = (uint32_t)words;
    co->status = CO_SUSPENDED;
    uint32_t bytes = (uint32_t)words * 8;
    Segment *s = &co->ctx.stack;
    s->data = xcalloc((size_t)bytes + (size_t)words, 1); /* the words, then a tag per word (as a process's) */
    s->tags = s->data + bytes;
    s->size = bytes;
    /* Each stack has its own generation, so a pointer into one stack traps
     * (E_BAD_PTR) while another runs. Generations are 12 bits and wrap. */
    p->stack_gen = (uint16_t)(p->stack_gen % 0xFFF + 1);
    if (p->stack_gen == 1) p->stack_gen = 2; /* 1 is the process's own stack */
    s->gen = p->stack_gen;
    s->kind = SEGK_STACK;
    s->live = 1;
    co->ctx.sp = co->ctx.fp = PTR_MAKE(SEG_STACK, s->gen, bytes);
    co->ctx.stack_low_off = bytes;
    co->ctx.stack_ref_lo = (uint32_t)words;
    /* Charged last: if the heap quota traps, the object is complete garbage. */
    gc_account(p, (int64_t)(sizeof(ObjCoro) + (size_t)words * 9));
    return co;
}

static void free_ctx(ExecCtx *x) {
    free(x->ireg);
    free(x->freg);
    free(x->rreg);
    free(x->preg);
    free(x->frames);
    free(x->stack.data); /* and the tags, in the same block */
    memset(x, 0, sizeof *x);
}

void coro_free_ctx(Process *p, ObjCoro *co) {
    free_ctx(&co->ctx);
    p->heap_bytes -= (uint64_t)co->words * 9; /* obj_footprint no longer counts the stack */
    co->words = 0;
}

void coro_release(ObjCoro *co) { free_ctx(&co->ctx); }

ObjCoro *coro_leave(Process *p, int status) {
    ObjCoro *co = p->co;
    co->status = (uint8_t)status;
    p->co = co->prev;
    if (p->co) p->co->status = CO_RUNNING;
    co->prev = NULL;
    coro_swap(p, co);
    if (status == CO_DEAD) coro_free_ctx(p, co);
    return co;
}
