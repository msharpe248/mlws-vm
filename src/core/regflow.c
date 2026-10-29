/* regflow.c - which registers an instruction reads and writes. */
#include "regflow.h"

void rs_add(RegSet *s, uint8_t reg) {
    if (REG_BANK(reg) < NBANKS) s->b[REG_BANK(reg)] |= 1u << REG_IDX(reg);
}

/* The registers an operand reads: a source's register; and for any operand,
 * those that give its address (a register destination is only written). */
void operand_reads(const Operand *o, bool dst, RegSet *r) {
    switch (o->mode) {
    case OM_REG:
        if (!dst) rs_add(r, o->reg);
        break;
    case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC: case OM_POSTINCB: case OM_PREDECB:
    case OM_ELEMI: case OM_FIELD:
        rs_add(r, o->reg);
        break;
    case OM_IDX: case OM_IDXB: case OM_ELEM:
        rs_add(r, o->reg);
        rs_add(r, o->ireg);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB: rs_add(r, o->ireg); break;
    case OM_MIND: case OM_MINDB: operand_reads(o->inner, false, r); break;
    default: break;
    }
}

/* The registers an instruction reads, and those it always writes when it
 * completes. */
void insn_effect(const Insn *in, RegSet *rd, RegSet *wr) {
    const OpInfo *oi = &op_info[in->op];
    memset(rd, 0, sizeof *rd);
    memset(wr, 0, sizeof *wr);
    if (oi->flags & OPF_CALL) { /* target, argc, arguments, results (written by the return) */
        int argc = (int)in->ops[1].imm;
        operand_reads(&in->ops[0], false, rd);
        for (int k = 2; k < in->nops; k++) {
            bool res = k >= 2 + argc;
            operand_reads(&in->ops[k], res, rd);
            if (res && in->ops[k].mode == OM_REG) rs_add(wr, in->ops[k].reg);
        }
        return;
    }
    /* `add I0, I1` is I0 = I0 + I1, `neg I0` negates I0 and `shl I0, 3`
     * shifts it: the destination is a source too when the last source is left
     * out. So it is for inc, dec and swap. */
    bool rmw = oi->nelems && oi->elems[0] == 'D' &&
               ((oi->last_mod == '?' && in->nops == oi->nelems - 1) || (oi->last_mod == '+' && in->nops == oi->nelems));
    if (in->op == OP_INC || in->op == OP_DEC || in->op == OP_SWAP) rmw = true;
    for (int k = 0; k < in->nops; k++) {
        const Operand *o = &in->ops[k];
        bool dst = strchr("DWBEXJH", op_pattern_at(oi, k)) != NULL;
        operand_reads(o, dst, rd);
        if (!dst || o->mode != OM_REG) continue;
        if (rmw && (k == 0 || in->op == OP_SWAP)) rs_add(rd, o->reg);
        /* recvt writes nothing when it times out */
        if (in->op != OP_RECVT) rs_add(wr, o->reg);
    }
}
