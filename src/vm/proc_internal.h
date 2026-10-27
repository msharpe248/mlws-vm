/* proc_internal.h - frame management shared by process.c and interp.c. */
#ifndef MLWSVM_PROC_INTERNAL_H
#define MLWSVM_PROC_INTERNAL_H

#include "vm.h"

void proc_frame_slow(Process *p, uint32_t words);

/* Zero a register window of n words. Windows are usually a few registers,
 * where a call to memset costs more than the stores (and compilers turn a
 * plain loop back into that call). */
#define ZERO_REGS(regs, n)                                                     \
    do {                                                                       \
        uint64_t *w_ = (regs);                                                 \
        switch (n) {                                                           \
        case 8: w_[7] = 0; /* fall through */                                  \
        case 7: w_[6] = 0; /* fall through */                                  \
        case 6: w_[5] = 0; /* fall through */                                  \
        case 5: w_[4] = 0; /* fall through */                                  \
        case 4: w_[3] = 0; /* fall through */                                  \
        case 3: w_[2] = 0; /* fall through */                                  \
        case 2: w_[1] = 0; /* fall through */                                  \
        case 1: w_[0] = 0; /* fall through */                                  \
        case 0: break;                                                         \
        default: memset(w_, 0, (n) * sizeof *w_); break;                       \
        }                                                                      \
    } while (0)

#if defined(__GNUC__) || defined(__clang__)
#define PROC_INLINE static inline __attribute__((always_inline))
#else
#define PROC_INLINE static inline
#endif

/* Room for a frame with a window of `words` registers, without growing
 * anything or reaching the depth limit. */
PROC_INLINE bool proc_frame_fits(const Process *p, uint32_t words) {
    return p->nframes < p->flimit && p->rtop + words <= p->rcap;
}

/* Push a call frame with a fresh, zeroed register window (inline: every
 * call), when proc_frame_fits is known to hold. */
PROC_INLINE void proc_push_frame_fitting(Process *p, Function *fn, const Insn *ret_insn) {
    Frame *fr = &p->frames[p->nframes++];
    fr->fn = fn;
    fr->pc = 0;
    fr->ret_insn = ret_insn;
    fr->closure = NULL;
    fr->qret = 0;
    fr->entry_sp = p->sp;
    fr->saved_fp = p->fp;
    p->fp = p->sp;
    fr->base = p->rtop;
    p->rtop += fn->rsize;
    ZERO_REGS(p->regs + fr->base, fn->rsize);
    if (p->nframes > p->max_depth) p->max_depth = p->nframes;
}

PROC_INLINE void proc_push_frame(Process *p, Function *fn, const Insn *ret_insn) {
    if (!proc_frame_fits(p, fn->rsize)) proc_frame_slow(p, fn->rsize);
    proc_push_frame_fitting(p, fn, ret_insn);
}

PROC_INLINE void proc_pop_frame(Process *p) {
    Frame *fr = &p->frames[--p->nframes];
    p->sp = fr->entry_sp;
    p->fp = fr->saved_fp;
    p->rtop = fr->base;
}
void proc_backtrace(Process *p);
void sched_unpublish(Process *p); /* remove from the process table (sched.c) */

#endif
