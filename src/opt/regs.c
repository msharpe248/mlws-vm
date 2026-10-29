/* regs.c - compact: the registers a function uses are renumbered so that each
 * bank's are the lowest, and the window shrinks to them. Parameters keep
 * their places (a call binds them there). A smaller window is cheaper to push
 * and to clear at every call. */
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

bool pass_compact(OFunc *f) {
    MFunc *mf = f->mf;
    uint32_t used[NBANKS];
    ofn_used_regs(f, used);
    int params[NBANKS] = {0};
    for (int k = 0; k < mf->nparams; k++) params[type_bank((MlwsType)mf->params[k])]++;
    uint8_t map[NBANKS][NREGS];
    uint16_t want[NBANKS];
    bool moved = false;
    for (int b = 0; b < NBANKS; b++) {
        int next = params[b];
        for (int r = 0; r < NREGS; r++) {
            if (r < params[b]) map[b][r] = (uint8_t)r;
            else if (used[b] >> r & 1u) {
                map[b][r] = (uint8_t)next++;
                moved |= map[b][r] != r;
            } else {
                map[b][r] = (uint8_t)r;
            }
        }
        want[b] = (uint16_t)next;
    }
    bool smaller = false;
    for (int b = 0; b < NBANKS; b++) smaller |= want[b] < mf->nregs[b];
    if (!smaller) return false;
    if (moved)
        for (size_t i = 0; i < f->code.n; i++) {
            Insn *in = &f->code.v[i].in;
            for (int k = 0; k < in->nops; k++) rename_regs(&in->ops[k], map);
        }
    uint32_t before = ofn_regs(f);
    for (int b = 0; b < NBANKS; b++)
        if (want[b] < mf->nregs[b]) mf->nregs[b] = want[b];
    char what[96];
    snprintf(what, sizeof what, "its window is %u registers, not %u", ofn_regs(f), before);
    ochange(f, PASS_COMPACT, 0, NULL, what);
    return true;
}
