/* copy.c - copies: within a block, a register copied from another by a
 * full-width move is read from the original instead (copyprop), so the copy
 * often dies; and moves that do nothing are removed (peephole). */
#include "opt_internal.h"

/* `mov d, s` copies s exactly: registers of one bank, and either untyped or
 * of the bank's full-width type (a narrower type wraps the value). */
static bool exact_copy(const Insn *in) {
    if (in->op != OP_MOV || in->nops != 2) return false;
    const Operand *d = &in->ops[0], *s = &in->ops[1];
    if (d->mode != OM_REG || s->mode != OM_REG) return false;
    Bank b = (Bank)REG_BANK(d->reg);
    if (b >= NBANKS || REG_BANK(s->reg) != b) return false;
    MlwsType t = (MlwsType)in->type;
    if (t == T_NONE) return true;
    switch (b) {
    case BANK_I: return t == T_I64 || t == T_U64;
    case BANK_F: return t == T_F64;
    case BANK_R: return t == T_R;
    default: return t == T_P;
    }
}

/* Registers an operand uses for its address, which may be renamed: never the
 * base of [P1]+ / -[P1], which the operand also changes. */
static void rename_address(Operand *o, const uint8_t *map, bool *any) {
    switch (o->mode) {
    case OM_IND: case OM_DISP: case OM_DISPB: case OM_ELEMI: case OM_FIELD:
        if (map[o->reg] != o->reg) o->reg = map[o->reg], *any = true;
        break;
    case OM_IDX: case OM_IDXB: case OM_ELEM:
        if (map[o->reg] != o->reg) o->reg = map[o->reg], *any = true;
        if (map[o->ireg] != o->ireg) o->ireg = map[o->ireg], *any = true;
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        if (map[o->ireg] != o->ireg) o->ireg = map[o->ireg], *any = true;
        break;
    default: break;
    }
}

bool pass_copyprop(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    if (!n) return false;
    OCfg g;
    ocfg_build(f, &g);
    bool ch = false;
    uint8_t map[256]; /* register -> the register it is a copy of (itself if none) */
    uint8_t act[128]; /* the registers that are copies now */
    int nact = 0;
    for (int r = 0; r < 256; r++) map[r] = (uint8_t)r;
    for (uint32_t b = 0; b < g.n; b++) {
        for (int k = 0; k < nact; k++) map[act[k]] = act[k];
        nact = 0;
        for (uint32_t i = g.b[b].start; i < g.b[b].end; i++) {
            OInsn *oi = &f->code.v[i];
            if (oi->dead) continue;
            Insn *in = &oi->in;
            /* read through the copies */
            bool any = false;
            for (int k = 0; k < in->nops; k++) {
                Operand *o = &in->ops[k];
                if (o->mode == OM_REG) {
                    if (!is_src_pos(in, k) || map[o->reg] == o->reg) continue;
                    uint8_t was = o->reg;
                    o->reg = map[o->reg];
                    CheckErr ce;
                    if (!check_insn(in, &ce)) o->reg = was;
                    else any = true;
                } else {
                    Operand was = *o;
                    bool a = false;
                    rename_address(o, map, &a);
                    if (a) {
                        CheckErr ce;
                        if (!check_insn(in, &ce)) *o = was;
                        else any = true;
                    }
                }
            }
            if (any) {
                ch = true;
                ochange(f, PASS_COPYPROP, i, NULL, "a read of a copy reads the original");
            }
            /* what this instruction writes ends the copies of and from it */
            Effect e;
            oeffect(in, &e);
            int keep = 0;
            for (int k = 0; k < nact; k++) {
                uint8_t r = act[k];
                if (rs_has(&e.may, r) || rs_has(&e.may, map[r])) map[r] = r;
                else act[keep++] = r;
            }
            nact = keep;
            if (exact_copy(in) && in->ops[0].reg != in->ops[1].reg) {
                uint8_t d = in->ops[0].reg, src = map[in->ops[1].reg];
                if (src != d) {
                    if (map[d] == d) act[nact++] = d;
                    map[d] = src;
                }
            }
        }
    }
    ocfg_free(&g);
    return ch;
}

bool pass_peephole(OFunc *f) {
    bool ch = false;
    uint32_t n = (uint32_t)f->code.n;
    OCfg g;
    ocfg_build(f, &g);
    for (uint32_t i = 0; i < n; i++) {
        OInsn *oi = &f->code.v[i];
        if (oi->dead) continue;
        Insn *in = &oi->in;
        if (in->op == OP_NOP) {
            oi->dead = true;
            ochange(f, PASS_PEEPHOLE, i, in, "a nop removed");
            ch = true;
            continue;
        }
        if (exact_copy(in) && in->ops[0].reg == in->ops[1].reg) {
            oi->dead = true;
            ochange(f, PASS_PEEPHOLE, i, in, "a move to itself removed");
            ch = true;
            continue;
        }
        /* mov a, b; mov b, a: the second changes nothing (in one block) */
        uint32_t j = next_live(f, i + 1);
        if (exact_copy(in) && j < n && g.of[j] == g.of[i]) {
            Insn *nx = &f->code.v[j].in;
            if (exact_copy(nx) && nx->ops[0].reg == in->ops[1].reg && nx->ops[1].reg == in->ops[0].reg) {
                f->code.v[j].dead = true;
                ochange(f, PASS_PEEPHOLE, j, nx, "a move back of what was just copied removed");
                ch = true;
            }
        }
    }
    ocfg_free(&g);
    return ch;
}
