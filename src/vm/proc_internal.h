/* proc_internal.h - frame management shared by process.c and interp.c. */
#ifndef MLWSVM_PROC_INTERNAL_H
#define MLWSVM_PROC_INTERNAL_H

#include "vm.h"

void proc_frame_slow(Process *p, const uint16_t *nr);

/* Zero n registers of a bank, through the bank's own element type. Windows
 * are usually a few registers, where a call to memset costs more than the
 * stores (and compilers turn a plain loop back into that call). */
#define ZERO_REGS(regs, n)                                                     \
    do {                                                                       \
        __typeof__(&(regs)[0]) w_ = (regs);                                    \
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

/* Push a call frame with a fresh, zeroed register window (inline: every call). */
static inline void proc_push_frame(Process *p, Function *fn, const Insn *ret_insn) {
    const uint16_t *nr = fn->mf->nregs;
    if (p->nframes >= p->fcap || p->nframes >= p->vm->max_frames || p->rtop[0] + nr[0] > p->rcap[0] ||
        p->rtop[1] + nr[1] > p->rcap[1] || p->rtop[2] + nr[2] > p->rcap[2] || p->rtop[3] + nr[3] > p->rcap[3])
        proc_frame_slow(p, nr);
    Frame *fr = &p->frames[p->nframes++];
    fr->fn = fn;
    fr->pc = 0;
    fr->ret_insn = ret_insn;
    fr->closure = NULL;
    fr->entry_sp = p->sp;
    fr->saved_fp = p->fp;
    p->fp = p->sp;
    for (int b = 0; b < NBANKS; b++) {
        fr->base[b] = p->rtop[b];
        p->rtop[b] += nr[b];
    }
    /* Only the banks the function uses; most functions use one or two. */
    if (nr[BANK_I]) ZERO_REGS(p->ireg + fr->base[BANK_I], nr[BANK_I]);
    if (nr[BANK_F]) ZERO_REGS(p->freg + fr->base[BANK_F], nr[BANK_F]);
    if (nr[BANK_R]) ZERO_REGS(p->rreg + fr->base[BANK_R], nr[BANK_R]);
    if (nr[BANK_P]) ZERO_REGS(p->preg + fr->base[BANK_P], nr[BANK_P]);
    if (p->nframes > p->max_depth) p->max_depth = p->nframes;
}

static inline void proc_pop_frame(Process *p) {
    Frame *fr = &p->frames[--p->nframes];
    p->sp = fr->entry_sp;
    p->fp = fr->saved_fp;
    for (int b = 0; b < NBANKS; b++) p->rtop[b] = fr->base[b];
}
void proc_backtrace(Process *p);
void sched_unpublish(Process *p); /* remove from the process table (sched.c) */

#endif
