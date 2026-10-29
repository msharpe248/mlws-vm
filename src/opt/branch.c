/* branch.c - branches: a branch to a jmp goes straight to where that jmp
 * goes; a jmp (or a branch) to the next instruction goes; a branch on
 * constants is decided; `bcc L1; jmp L2; L1:` becomes `bncc L2`; and a jmp to
 * a ret becomes the ret. */
#include "opt_internal.h"

#include "../core/arith.h"

static uint32_t resolve(const OFunc *f, uint32_t t) { /* where control lands */
    return next_live(f, t);
}

static uint32_t thread(const OFunc *f, uint32_t t) {
    for (int hops = 0; hops < 32; hops++) {
        uint32_t at = resolve(f, t);
        if (at >= f->code.n) return t;
        const Insn *in = &f->code.v[at].in;
        if (in->op != OP_JMP) return t;
        uint32_t nt = in->ops[0].index;
        if (resolve(f, nt) == at) return t; /* a loop of jumps */
        t = nt;
    }
    return t;
}

static bool plain(const Operand *o) {
    return o->mode == OM_REG || o->mode == OM_IMM || o->mode == OM_FIMM || o->mode == OM_CONST || o->mode == OM_LABEL;
}

static bool all_plain(const Insn *in) {
    for (int k = 0; k < in->nops; k++)
        if (!plain(&in->ops[k])) return false;
    return true;
}

static bool cond_branch(int op) {
    return op == OP_BZ || op == OP_BNZ || op == OP_BEQ || op == OP_BNE || op == OP_BLT || op == OP_BLE ||
           op == OP_BGT || op == OP_BGE || op == OP_BISA || op == OP_BNISA;
}

/* The constant an immediate operand reads as type t. */
static bool imm_val(const Operand *o, MlwsType t, Word *w) {
    if (o->mode == OM_IMM) {
        if (type_is_float(t)) w->f = canon_float(t, (double)o->imm);
        else w->i = canon_int(t, o->imm);
        return true;
    }
    if (o->mode == OM_FIMM && type_is_float(t)) {
        w->f = canon_float(t, o->f);
        return true;
    }
    return false;
}

/* 1: taken, 0: not taken, -1: not known. */
static int decide(const Insn *in) {
    MlwsType t = (MlwsType)in->type;
    Word a, b;
    switch (in->op) {
    case OP_BZ: case OP_BNZ:
        if (t == T_R && in->ops[0].mode == OM_CONST) return in->op == OP_BNZ; /* a constant is never null */
        if (!(type_is_int(t) || t == T_R || t == T_P) || in->ops[0].mode != OM_IMM) return -1;
        a.i = canon_int(type_is_int(t) ? t : T_I64, in->ops[0].imm);
        return (a.i == 0) == (in->op == OP_BZ);
    case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT: case OP_BGE:
        if (t == T_R) {
            if (in->op != OP_BEQ && in->op != OP_BNE) return -1;
            const Operand *x = &in->ops[0], *y = &in->ops[1];
            bool eq;
            if (x->mode == OM_IMM && y->mode == OM_IMM) eq = true; /* null and null */
            else if ((x->mode == OM_IMM && y->mode == OM_CONST) || (x->mode == OM_CONST && y->mode == OM_IMM)) eq = false;
            else if (x->mode == OM_CONST && y->mode == OM_CONST && x->index == y->index) eq = true;
            else return -1;
            return eq == (in->op == OP_BEQ);
        }
        if (!(type_is_int(t) || type_is_float(t))) return -1;
        if (!imm_val(&in->ops[0], t, &a) || !imm_val(&in->ops[1], t, &b)) return -1;
        return arith_compare(in->op, t, a, b);
    default: return -1;
    }
}

static int inverse(const Insn *in) {
    MlwsType t = (MlwsType)in->type;
    switch (in->op) {
    case OP_BZ: return OP_BNZ;
    case OP_BNZ: return OP_BZ;
    case OP_BISA: return OP_BNISA;
    case OP_BNISA: return OP_BISA;
    case OP_BEQ: return OP_BNE;
    case OP_BNE: return OP_BEQ;
    default: break;
    }
    if (type_is_float(t)) return -1; /* with a NaN, neither a < b nor a >= b */
    switch (in->op) {
    case OP_BLT: return OP_BGE;
    case OP_BGE: return OP_BLT;
    case OP_BLE: return OP_BGT;
    case OP_BGT: return OP_BLE;
    default: return -1;
    }
}

static bool targeted(const OFunc *f, uint32_t pc) { /* by any label or handler */
    for (size_t i = 0; i < f->code.n; i++) {
        if (f->code.v[i].dead) continue;
        const Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].mode == OM_LABEL && resolve(f, in->ops[k].index) == pc) return true;
    }
    for (size_t k = 0; k < f->h.n; k++)
        if (resolve(f, f->h.v[k].target) == pc) return true;
    return false;
}

bool pass_branches(OFunc *f) {
    bool ch = false;
    uint32_t n = (uint32_t)f->code.n;
    for (uint32_t i = 0; i < n; i++) {
        OInsn *oi = &f->code.v[i];
        if (oi->dead) continue;
        Insn *in = &oi->in;
        /* threading */
        for (int k = 0; k < in->nops; k++) {
            if (in->ops[k].mode != OM_LABEL) continue;
            uint32_t t = in->ops[k].index, nt = thread(f, t);
            if (resolve(f, nt) != resolve(f, t)) {
                in->ops[k].index = nt;
                ochange(f, PASS_BRANCHES, i, NULL, "a branch to a jmp goes where the jmp goes");
                ch = true;
            }
        }
        uint32_t next = next_live(f, i + 1);
        /* decided by constants */
        if (cond_branch(in->op) || in->op == OP_JTAB) {
            int d = -1;
            uint32_t target = 0;
            if (in->op == OP_JTAB) {
                if (in->ops[0].mode == OM_IMM) {
                    int64_t idx = in->ops[0].imm;
                    d = idx >= 0 && idx < in->nops - 1;
                    if (d) target = in->ops[1 + idx].index;
                }
            } else {
                d = decide(in);
                if (d == 1) target = in->ops[in->nops - 1].index;
            }
            if (d == 0) {
                ochange(f, PASS_BRANCHES, i, in, "a branch that is never taken removed");
                oi->dead = true;
                ch = true;
                continue;
            }
            if (d == 1) {
                Insn j = insn_make(OP_JMP, T_NONE, 1);
                j.ops[0] = op_label(target);
                ochange(f, PASS_BRANCHES, i, in, "a branch that is always taken is a jmp");
                insn_free(in);
                *in = j;
                ch = true;
            }
        }
        /* to the next instruction */
        if ((in->op == OP_JMP || (cond_branch(in->op) && all_plain(in))) &&
            resolve(f, in->ops[in->nops - 1].index) == next) {
            ochange(f, PASS_BRANCHES, i, in, "a branch to the next instruction removed");
            oi->dead = true;
            ch = true;
            continue;
        }
        /* bcc L1; jmp L2; L1:  ->  bncc L2 */
        if (cond_branch(in->op) && next < n && f->code.v[next].in.op == OP_JMP) {
            int inv = inverse(in);
            uint32_t after = next_live(f, next + 1);
            if (inv >= 0 && resolve(f, in->ops[in->nops - 1].index) == after && !targeted(f, next)) {
                Insn *j = &f->code.v[next].in;
                ochange(f, PASS_BRANCHES, i, in, "a branch around a jmp inverted");
                in->op = in->xop = (uint16_t)inv;
                in->ops[in->nops - 1].index = j->ops[0].index;
                f->code.v[next].dead = true;
                ch = true;
                continue;
            }
        }
        /* jmp to a ret: the ret */
        if (in->op == OP_JMP) {
            uint32_t t = resolve(f, in->ops[0].index);
            if (t < n && f->code.v[t].in.op == OP_RET && f->code.v[t].in.nops <= 4 && all_plain(&f->code.v[t].in)) {
                Insn r;
                insn_copy(&r, &f->code.v[t].in);
                ochange(f, PASS_BRANCHES, i, in, "a jmp to a ret is the ret");
                insn_free(in);
                *in = r;
                oi->line = f->code.v[t].line;
                ch = true;
            }
        }
    }
    return ch;
}
