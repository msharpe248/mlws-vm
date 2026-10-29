/* tail.c - `call g (...) -> (r1, ..., rk); ret r1, ..., rk` becomes
 * `tcall g (...)`: g's results are this function's, so g can return them
 * straight to this function's caller, and this frame is gone during the call
 * (deep recursion through it no longer grows the stack, and a backtrace no
 * longer shows it). Only for a g of this module that declares the same
 * results, and never inside a .try region, whose handler must still see a
 * fault in g. The ret stays where other paths may reach it. */
#include "opt_internal.h"

static bool same_rets(const MFunc *a, const MFunc *b) {
    if (!(a->flags & MF_RETS_DECLARED) || !(b->flags & MF_RETS_DECLARED) || a->nrets != b->nrets) return false;
    return !memcmp(a->rets, b->rets, a->nrets);
}

bool pass_tailcall(OFunc *f) {
    bool ch = false;
    Module *m = f->om->m;
    uint32_t n = (uint32_t)f->code.n;
    for (uint32_t i = 0; i < n; i++) {
        OInsn *oi = &f->code.v[i];
        Insn *in = &oi->in;
        if (oi->dead || in->op != OP_CALL || in_try(f, i)) continue;
        int64_t gi = func_index_of(&in->ops[0]);
        if (gi < 0 || (uint64_t)gi >= m->nfuncs || (m->funcs[gi].flags & MF_IMPORT)) continue;
        if (!same_rets(f->mf, &m->funcs[gi])) continue;
        int argc = (int)in->ops[1].imm, nres = in->nops - 2 - argc;
        uint32_t j = next_live(f, i + 1);
        if (j >= n) continue;
        const Insn *r = &f->code.v[j].in;
        if (r->op != OP_RET || r->nops != nres || nres != f->mf->nrets) continue;
        bool match = true;
        for (int k = 0; k < nres && match; k++) {
            const Operand *res = &in->ops[2 + argc + k], *val = &r->ops[k];
            match = res->mode == OM_REG && val->mode == OM_REG && res->reg == val->reg;
            for (int q = 0; q < k && match; q++) match = in->ops[2 + argc + q].reg != res->reg; /* each once */
        }
        if (!match) continue;
        ochange(f, PASS_TAILCALL, i, in, "a call whose results are returned is a tail call");
        Insn t = insn_make(OP_TCALL, T_NONE, 2 + argc);
        for (int k = 0; k < 2 + argc; k++) {
            t.ops[k] = in->ops[k];
            in->ops[k].inner = NULL; /* now t's */
        }
        t.ops[1].imm = argc;
        insn_free(in);
        *in = t;
        ch = true;
    }
    return ch;
}
