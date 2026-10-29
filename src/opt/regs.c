/* regs.c - compact: the registers a function uses are renumbered so that each
 * bank's are the lowest, and the window shrinks to them. Parameters keep
 * their places (a call binds them there). A smaller window is cheaper to push
 * and to clear at every call.
 *
 * A call clears one run of its new window's words (src/vm/verify.c,
 * plan_window): from the first register that needs clearing to the last, the
 * banks lying I, F, R, P. Every R register needs it (the collector scans
 * them), and so does any other the function may read before it writes it.
 * So within each bank the registers that need clearing are put next to the R
 * bank: last among the I and F registers, first among the P registers. The
 * run then covers as few that do not need it as it can. */
#include "opt_internal.h"

static void note(const Operand *o, uint32_t *used) {
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC: case OM_POSTINCB:
    case OM_PREDECB: case OM_ELEMI: case OM_FIELD:
        if (REG_BANK(o->reg) < NBANKS) used[REG_BANK(o->reg)] |= 1u << REG_IDX(o->reg);
        break;
    case OM_IDX: case OM_IDXB: case OM_ELEM:
        if (REG_BANK(o->reg) < NBANKS) used[REG_BANK(o->reg)] |= 1u << REG_IDX(o->reg);
        if (REG_BANK(o->ireg) < NBANKS) used[REG_BANK(o->ireg)] |= 1u << REG_IDX(o->ireg);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        if (REG_BANK(o->ireg) < NBANKS) used[REG_BANK(o->ireg)] |= 1u << REG_IDX(o->ireg);
        break;
    case OM_MIND: case OM_MINDB:
        if (o->inner) note(o->inner, used);
        break;
    default: break;
    }
}

static uint8_t mapped(uint8_t r, uint8_t map[NBANKS][NREGS]) {
    if (REG_BANK(r) >= NBANKS) return r;
    return REG_MAKE(REG_BANK(r), map[REG_BANK(r)][REG_IDX(r)]);
}

static void rename_regs(Operand *o, uint8_t map[NBANKS][NREGS]) {
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC: case OM_POSTINCB:
    case OM_PREDECB: case OM_ELEMI: case OM_FIELD:
        o->reg = mapped(o->reg, map);
        break;
    case OM_IDX: case OM_IDXB: case OM_ELEM:
        o->reg = mapped(o->reg, map);
        o->ireg = mapped(o->ireg, map);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB: o->ireg = mapped(o->ireg, map); break;
    case OM_MIND: case OM_MINDB:
        if (o->inner) rename_regs(o->inner, map);
        break;
    default: break;
    }
}

void ofn_used_regs(const OFunc *f, uint32_t used[NBANKS]) {
    memset(used, 0, NBANKS * sizeof *used);
    for (size_t i = 0; i < f->code.n; i++) {
        if (f->code.v[i].dead) continue;
        const Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++) note(&in->ops[k], used);
    }
}

/* The I, F and P registers f may read before writing them (on some path from
 * its entry, or from a handler's: both start with only the parameters
 * written). As plan_window works it out, at load. */
static void read_first(const OFunc *f, uint32_t need[NBANKS]) {
    uint32_t n = (uint32_t)f->code.n;
    memset(need, 0, NBANKS * sizeof *need);
    if (!n) return;
    RegSet entry = {{0}};
    int cnt[NBANKS] = {0};
    for (int k = 0; k < f->mf->nparams; k++) {
        Bank b = type_bank((MlwsType)f->mf->params[k]);
        entry.b[b] |= 1u << cnt[b]++;
    }
    RegSet *in = xcalloc(n, sizeof *in);
    uint8_t *seen = xcalloc(n, 1);
    Effect *eff = ofn_effects(f);
    bool changed = true;
#define FLOWTO(pc_, set_)                                                                     \
    do {                                                                                      \
        uint32_t t_ = (pc_);                                                                  \
        for (int b_ = 0; b_ < NBANKS; b_++) {                                                 \
            uint32_t m_ = seen[t_] ? in[t_].b[b_] & (set_).b[b_] : (set_).b[b_];              \
            if (!seen[t_] || m_ != in[t_].b[b_]) in[t_].b[b_] = m_, changed = true;           \
        }                                                                                     \
        seen[t_] = 1;                                                                         \
    } while (0)
    FLOWTO(0, entry);
    for (size_t k = 0; k < f->h.n; k++)
        if (f->h.v[k].target < n) FLOWTO(f->h.v[k].target, entry);
    while (changed) {
        changed = false;
        for (uint32_t pc = 0; pc < n; pc++) {
            if (!seen[pc]) continue;
            const Insn *ins = &f->code.v[pc].in;
            RegSet out = in[pc];
            for (int b = 0; b < NBANKS; b++) out.b[b] |= eff[pc].def.b[b];
            if (!(op_info[ins->op].flags & OPF_TERM) && pc + 1 < n) FLOWTO(pc + 1, out);
            for (int k = 0; k < ins->nops; k++)
                if (ins->ops[k].mode == OM_LABEL && ins->ops[k].index < n) FLOWTO(ins->ops[k].index, out);
        }
    }
#undef FLOWTO
    for (uint32_t pc = 0; pc < n; pc++)
        if (seen[pc])
            for (int b = 0; b < NBANKS; b++) need[b] |= eff[pc].use.b[b] & ~in[pc].b[b];
    need[BANK_R] = 0;
    free(eff);
    free(seen);
    free(in);
}

/* The words a call clears, for these banks' sizes and registers to clear. */
static uint32_t clear_run(const uint16_t nregs[NBANKS], const uint32_t need[NBANKS]) {
    uint32_t off[NBANKS], at = 0, lo = UINT32_MAX, hi = 0;
    for (int b = 0; b < NBANKS; b++) off[b] = at, at += nregs[b];
    for (int b = 0; b < NBANKS; b++) {
        uint32_t m = b == BANK_R ? (nregs[b] >= 32 ? 0xFFFFFFFFu : (1u << nregs[b]) - 1) : need[b];
        if (nregs[b] < 32) m &= (1u << nregs[b]) - 1;
        for (int r = 0; r < 32; r++)
            if (m >> r & 1u) {
                if (off[b] + r < lo) lo = off[b] + r;
                if (off[b] + r + 1 > hi) hi = off[b] + r + 1;
            }
    }
    return lo < hi ? hi - lo : 0;
}

bool pass_compact(OFunc *f) {
    MFunc *mf = f->mf;
    uint32_t used[NBANKS], need[NBANKS];
    ofn_used_regs(f, used);
    read_first(f, need);
    int params[NBANKS] = {0};
    for (int k = 0; k < mf->nparams; k++) params[type_bank((MlwsType)mf->params[k])]++;
    uint8_t map[NBANKS][NREGS];
    uint16_t want[NBANKS];
    uint32_t newneed[NBANKS] = {0};
    bool moved = false;
    for (int b = 0; b < NBANKS; b++) {
        int next = params[b];
        for (int r = 0; r < NREGS; r++) map[b][r] = (uint8_t)r;
        /* those to clear next to the R bank: after the others in I and F, before them in P */
        for (int pass = 0; pass < 2; pass++) {
            bool clears_now = b == BANK_P ? pass == 0 : pass == 1;
            for (int r = params[b]; r < NREGS; r++) {
                if (!(used[b] >> r & 1u) || ((need[b] >> r & 1u) != 0) != clears_now) continue;
                map[b][r] = (uint8_t)next;
                if (clears_now) newneed[b] |= 1u << next;
                moved |= next != r;
                next++;
            }
        }
        want[b] = (uint16_t)next;
    }
    uint32_t run_before = clear_run(mf->nregs, need), run_after = clear_run(want, newneed);
    bool smaller = false;
    for (int b = 0; b < NBANKS; b++) smaller |= want[b] < mf->nregs[b];
    if (!smaller && run_after >= run_before) return false;
    if (moved)
        for (size_t i = 0; i < f->code.n; i++) {
            Insn *in = &f->code.v[i].in;
            for (int k = 0; k < in->nops; k++) rename_regs(&in->ops[k], map);
        }
    uint32_t before = ofn_regs(f);
    for (int b = 0; b < NBANKS; b++)
        if (want[b] < mf->nregs[b]) mf->nregs[b] = want[b];
    char what[128];
    snprintf(what, sizeof what, "its window is %u registers, not %u; a call clears %u, not %u", ofn_regs(f), before,
             run_after, run_before);
    ochange(f, PASS_COMPACT, 0, NULL, what);
    return true;
}
