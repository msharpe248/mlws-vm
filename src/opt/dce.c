/* dce.c - dead code: pure instructions whose results nothing reads (dce, by
 * backward liveness of registers), and blocks nothing reaches (unreachable).
 *
 * Liveness counts exception handlers: an instruction in a .try region may
 * trap before it writes anything, so what the handler reads is live before
 * and after it.
 *
 * An R register is a root of the collector, so writing one can be what lets
 * an object go (`mov R3, null` before `gc.collect`, and a finalizer runs).
 * A write to an R register that nothing reads is therefore removed only where
 * the register already holds something that never dies: null, or a constant
 * (constants live as long as the VM). A forward analysis finds those. */
#include "opt_internal.h"

static void rs_or(RegSet *a, const RegSet *b) {
    for (int k = 0; k < NBANKS; k++) a->b[k] |= b->b[k];
}

static bool rs_eq(const RegSet *a, const RegSet *b) {
    for (int k = 0; k < NBANKS; k++)
        if (a->b[k] != b->b[k]) return false;
    return true;
}

/* What every handler covering pc reads on entry. */
static void handler_live(const OFunc *f, const OCfg *g, const RegSet *live_in, uint32_t pc, RegSet *out) {
    for (size_t k = 0; k < f->h.n; k++) {
        const MHandler *h = &f->h.v[k];
        if (pc >= h->start && pc < h->end && h->target < f->code.n) rs_or(out, &live_in[g->of[h->target]]);
    }
}

/* After in, which R registers hold null or a constant (given those before). */
static uint32_t immortal_after(const Insn *in, const Effect *e, uint32_t imm) {
    imm &= ~e->may.b[BANK_R];
    if (in->op == OP_MOV && in->ops[0].mode == OM_REG && REG_BANK(in->ops[0].reg) == BANK_R) {
        const Operand *s = &in->ops[1];
        bool keeps = s->mode == OM_CONST || (s->mode == OM_IMM && s->imm == 0) ||
                     (s->mode == OM_REG && REG_BANK(s->reg) == BANK_R && (imm >> REG_IDX(s->reg) & 1u));
        if (keeps && in->type != T_BIG && in->type != T_DEC) imm |= 1u << REG_IDX(in->ops[0].reg);
    }
    return imm;
}

/* For each instruction, the R registers that hold null or a constant before
 * it: forward, met by intersection; a function starts with its R parameters
 * unknown and its other R registers null, and a handler with nothing known. */
static uint32_t *immortal_regs(const OFunc *f, const OCfg *g, const Effect *eff) {
    uint32_t n = (uint32_t)f->code.n;
    uint32_t *at = xcalloc(n + 1, sizeof *at), *bin = xcalloc(g->n + 1, sizeof *bin);
    uint8_t *seen = xcalloc(g->n + 1, 1);
    uint32_t params = 0;
    int nr = 0;
    for (int k = 0; k < f->mf->nparams; k++)
        if (type_bank((MlwsType)f->mf->params[k]) == BANK_R) params |= 1u << nr++;
    bin[0] = ~params, seen[0] = 1;
    for (size_t k = 0; k < f->h.n; k++)
        if (f->h.v[k].target < n) bin[g->of[f->h.v[k].target]] = 0, seen[g->of[f->h.v[k].target]] = 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 0; b < g->n; b++) {
            if (!seen[b]) continue;
            uint32_t imm = bin[b];
            for (uint32_t i = g->b[b].start; i < g->b[b].end; i++)
                if (!f->code.v[i].dead) imm = immortal_after(&f->code.v[i].in, &eff[i], imm);
            uint32_t succ[OCFG_MAX_SUCCS];
            uint32_t ns = ocfg_succs(g, b, succ, OCFG_MAX_SUCCS);
            for (uint32_t k = 0; k < ns; k++) {
                uint32_t t = succ[k];
                uint32_t m = seen[t] ? bin[t] & imm : imm;
                if (!seen[t] || m != bin[t]) bin[t] = m, seen[t] = 1, changed = true;
            }
        }
    }
    for (uint32_t b = 0; b < g->n; b++) {
        uint32_t imm = seen[b] ? bin[b] : 0;
        for (uint32_t i = g->b[b].start; i < g->b[b].end; i++) {
            at[i] = imm;
            if (!f->code.v[i].dead) imm = immortal_after(&f->code.v[i].in, &eff[i], imm);
        }
    }
    free(bin);
    free(seen);
    return at;
}

/* Backward through block b from what is live at its end; marks dead what it
 * may remove (when `remove`). Returns what is live at its start. */
static RegSet walk(OFunc *f, const OCfg *g, const RegSet *live_in, const Effect *eff, const uint32_t *imm, uint32_t b,
                   RegSet live, bool remove, bool *ch) {
    for (uint32_t i = g->b[b].end; i-- > g->b[b].start;) {
        OInsn *oi = &f->code.v[i];
        if (oi->dead) continue;
        const Insn *in = &oi->in;
        const Effect e = eff[i];
        RegSet h = {{0}};
        if (f->h.n) handler_live(f, g, live_in, i, &h);
        rs_or(&live, &h); /* (after it, a later trap in the region) */
        if (remove && insn_pure(in)) {
            bool used = false;
            for (int k = 0; k < NBANKS; k++) used |= (e.may.b[k] & live.b[k]) != 0;
            if (e.may.b[BANK_R] & ~imm[i]) used = true; /* it may be what lets an object go */
            if (!used) {
                ochange(f, PASS_DCE, i, in, "its result is never read");
                oi->dead = true;
                *ch = true;
                continue;
            }
        }
        for (int k = 0; k < NBANKS; k++) live.b[k] &= ~e.def.b[k];
        rs_or(&live, &e.use);
        rs_or(&live, &h); /* (before it: it may trap) */
    }
    return live;
}

bool pass_dce(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    if (!n) return false;
    OCfg g;
    ocfg_build(f, &g);
    RegSet *live_in = xcalloc(g.n, sizeof *live_in);
    Effect *eff = ofn_effects(f);
    bool dummy = false, changed = true;
    while (changed) { /* to a fixpoint: blocks in reverse order converge fast */
        changed = false;
        for (uint32_t b = g.n; b-- > 0;) {
            RegSet out = {{0}};
            uint32_t succ[OCFG_MAX_SUCCS];
            uint32_t ns = ocfg_succs(&g, b, succ, OCFG_MAX_SUCCS);
            for (uint32_t k = 0; k < ns; k++) rs_or(&out, &live_in[succ[k]]);
            RegSet in = walk(f, &g, live_in, eff, NULL, b, out, false, &dummy);
            if (!rs_eq(&in, &live_in[b])) live_in[b] = in, changed = true;
        }
    }
    bool ch = false;
    uint32_t *imm = immortal_regs(f, &g, eff);
    for (uint32_t b = 0; b < g.n; b++) {
        RegSet out = {{0}};
        uint32_t succ[64];
        uint32_t ns = ocfg_succs(&g, b, succ, 64);
        for (uint32_t k = 0; k < ns; k++) rs_or(&out, &live_in[succ[k]]);
        for (uint32_t k = 64; k < g.b[b].nextra; k++) rs_or(&out, &live_in[g.b[b].extra[k]]);
        walk(f, &g, live_in, eff, imm, b, out, true, &ch);
    }
    free(imm);
    free(eff);
    free(live_in);
    ocfg_free(&g);
    return ch;
}

bool pass_unreachable(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    if (!n) return false;
    OCfg g;
    ocfg_build(f, &g);
    uint8_t *seen = xcalloc(g.n, 1);
    uint32_t *work = xmalloc((g.n + 1) * sizeof *work), nwork = 0;
    seen[0] = 1, work[nwork++] = 0;
    bool grew = true; /* a handler is reached if a reached instruction is in its region */
    while (grew) {
        grew = false;
        while (nwork) {
            uint32_t b = work[--nwork];
            uint32_t succ[64];
            uint32_t ns = ocfg_succs(&g, b, succ, 64);
            for (uint32_t k = 0; k < ns; k++)
                if (!seen[succ[k]]) seen[succ[k]] = 1, work[nwork++] = succ[k];
            for (uint32_t k = 64; k < g.b[b].nextra; k++)
                if (!seen[g.b[b].extra[k]]) seen[g.b[b].extra[k]] = 1, work[nwork++] = g.b[b].extra[k];
        }
        for (size_t k = 0; k < f->h.n; k++) {
            const MHandler *h = &f->h.v[k];
            if (h->target >= n || seen[g.of[h->target]]) continue;
            bool covered = false;
            for (uint32_t i = h->start; i < h->end && i < n && !covered; i++) covered = seen[g.of[i]] && !f->code.v[i].dead;
            if (covered) seen[g.of[h->target]] = 1, work[nwork++] = g.of[h->target], grew = true;
        }
    }
    bool ch = false;
    for (uint32_t b = 0; b < g.n; b++) {
        if (seen[b]) continue;
        for (uint32_t i = g.b[b].start; i < g.b[b].end; i++) {
            if (f->code.v[i].dead) continue;
            ochange(f, PASS_UNREACHABLE, i, &f->code.v[i].in, "nothing reaches it");
            f->code.v[i].dead = true;
            ch = true;
        }
    }
    free(seen);
    free(work);
    ocfg_free(&g);
    return ch;
}
