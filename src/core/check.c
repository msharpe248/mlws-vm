/* check.c - see check.h */
#include "check.h"

static bool fail(CheckErr *ce, int k, const char *fmt, ...) MLWS_PRINTF(3, 4);
static bool fail(CheckErr *ce, int k, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ce->msg, sizeof ce->msg, fmt, ap);
    va_end(ap);
    ce->operand = k;
    return false;
}

static bool reg_valid(uint8_t reg) {
    int bank = REG_BANK(reg);
    if (bank > BANK_S) return false;
    return bank != BANK_S || REG_IDX(reg) < SREG__COUNT;
}

static bool reg_holds(uint8_t reg, MlwsType t) {
    if (REG_BANK(reg) == BANK_S) {
        MlwsType nt = reg_natural_type(reg);
        return nt == T_P ? t == T_P : type_is_int(t);
    }
    return REG_BANK(reg) == type_bank(t);
}

static bool base_ok(uint8_t reg) {
    return REG_BANK(reg) == BANK_P ||
           (REG_BANK(reg) == BANK_S && (REG_IDX(reg) == SREG_SP || REG_IDX(reg) == SREG_FP));
}

static bool check_mem(const Operand *o, int k, CheckErr *ce) {
    char b1[8], b2[8];
    switch (o->mode) {
    case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC:
    case OM_POSTINCB: case OM_PREDECB:
        if (!reg_valid(o->reg) || !base_ok(o->reg))
            return fail(ce, k, "operand %d: base register %s must be a pointer (P register, SP or FP)", k + 1,
                        reg_name(o->reg, b1));
        return true;
    case OM_IDX: case OM_IDXB:
        if (!reg_valid(o->reg) || !base_ok(o->reg))
            return fail(ce, k, "operand %d: base register %s must be a pointer (P register, SP or FP)", k + 1,
                        reg_name(o->reg, b1));
        /* fallthrough */
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        if (REG_BANK(o->ireg) != BANK_I)
            return fail(ce, k, "operand %d: index register %s must be an I register", k + 1, reg_name(o->ireg, b2));
        return true;
    case OM_GLOBAL: case OM_GLOBALB:
        return true;
    case OM_MIND: case OM_MINDB:
        if (!o->inner || !mode_is_memory(o->inner->mode) || o->inner->mode == OM_MIND || o->inner->mode == OM_MINDB)
            return fail(ce, k, "operand %d: memory-indirect operand needs a plain memory operand inside", k + 1);
        return check_mem(o->inner, k, ce);
    default:
        return fail(ce, k, "operand %d: expected a memory operand", k + 1);
    }
}

bool check_operand(const Operand *o, int k, MlwsType t, bool write, CheckErr *ce) {
    char b[8];
    if (write && !mode_is_writable(o->mode))
        return fail(ce, k, "operand %d must be a register or memory location", k + 1);
    switch (o->mode) {
    case OM_REG:
        if (!reg_valid(o->reg)) return fail(ce, k, "operand %d: invalid register", k + 1);
        if (write && REG_BANK(o->reg) == BANK_S && REG_IDX(o->reg) == SREG_PID)
            return fail(ce, k, "operand %d: PID is read-only", k + 1);
        if (t != T_NONE && !reg_holds(o->reg, t))
            return fail(ce, k, "operand %d: register %s cannot hold a value of type %s", k + 1, reg_name(o->reg, b),
                        type_name(t));
        return true;
    case OM_IMM:
        if ((t == T_R || t == T_P) && o->imm != 0)
            return fail(ce, k, "operand %d: the only %s literal is 0 (null)", k + 1, type_name(t));
        return true;
    case OM_FIMM:
        if (t != T_NONE && !type_is_float(t) && t != T_DEC)
            return fail(ce, k, "operand %d: float literal used where %s is expected", k + 1, type_name(t));
        return true;
    case OM_CONST:
        if (t != T_NONE && t != T_R && t != T_BIG && t != T_DEC)
            return fail(ce, k, "operand %d: string literal used where %s is expected", k + 1, type_name(t));
        return true;
    case OM_LABEL: return fail(ce, k, "operand %d: a label is not a value", k + 1);
    case OM_FUNC: return fail(ce, k, "operand %d: a function is not a value (yet)", k + 1);
    case OM_NATIVE: return fail(ce, k, "operand %d: a native name is not a value", k + 1);
    case OM_STRUCT: return fail(ce, k, "operand %d: a struct type is not a value", k + 1);
    case OM_UPVAL: return true; /* checked against the running closure at run time */
    case OM_ELEM: case OM_ELEMI: case OM_FIELD:
        if (REG_BANK(o->reg) != BANK_R)
            return fail(ce, k, "operand %d: element and field access need an R register base, not %s", k + 1,
                        reg_name(o->reg, b));
        if (o->mode == OM_ELEM && REG_BANK(o->ireg) != BANK_I)
            return fail(ce, k, "operand %d: index register %s must be an I register", k + 1, reg_name(o->ireg, b));
        return true;
    default:
        if (mode_is_memory(o->mode)) return check_mem(o, k, ce);
        return fail(ce, k, "operand %d: unknown addressing mode", k + 1);
    }
}

MlwsType operand_type(const Insn *in, int k) {
    const OpInfo *oi = &op_info[in->op];
    if (oi->flags & OPF_CALL) return T_NONE;
    MlwsType t = (MlwsType)in->type;
    if ((in->op == OP_ADD || in->op == OP_SUB) && t == T_P) {
        /* pointer arithmetic: the pointer operand(s) are p, the offsets i64 */
        if (k == 0) return T_P;
        if (in->nops == 2) return T_I64;
        return k == 1 ? T_P : T_I64;
    }
    if ((oi->flags & OPF_TYPE2) && k == 1) return (MlwsType)in->type2;
    /* map keys are always read with their natural type */
    if ((in->op == OP_MSET && k == 1) || (in->op == OP_MGET && k == 2)) return T_NONE;
    switch (op_pattern_at(oi, k)) {
    case 'D': case 'S': case 'V': case 'W': return t;
    case 'B': return T_B;
    case 'Q': return T_I64;
    case 'J': return T_I64;
    case 'A': case 'E': return T_P;
    case 'O': case 'X': return T_R;
    default: return T_NONE;
    }
}

bool check_insn(const Insn *in, CheckErr *ce) {
    const OpInfo *oi = &op_info[in->op];
    ce->operand = -1;
    if (in->type >= T__COUNT || in->type2 >= T__COUNT) return fail(ce, -1, "invalid type byte");
    if (oi->types == 0) {
        if (in->type != T_NONE) return fail(ce, -1, "%s does not take a type suffix", oi->mnem);
    } else if (in->type == T_NONE) {
        if (!(oi->flags & (OPF_NATURAL | OPF_PERVAL)))
            return fail(ce, -1, "%s needs a type", oi->mnem);
    } else if (!(oi->types & TM(in->type))) {
        if (in->type == T_R && (oi->types & TS_BIGDEC))
            return fail(ce, -1, "%s does not support type .r (for BigInt or decimal arithmetic write %s.big or %s.dec)",
                        oi->mnem, oi->mnem, oi->mnem);
        return fail(ce, -1, "%s does not support type .%s", oi->mnem, type_name((MlwsType)in->type));
    }
    if (oi->flags & OPF_TYPE2) {
        uint32_t from_ok = in->op == OP_BITCAST ? TS_NUM : TS_NUM | TS_BIGDEC;
        if (in->type == T_NONE || !(from_ok & TM(in->type2)))
            return fail(ce, -1, "%s needs two numeric types, e.g. %s.f64.i64", oi->mnem, oi->mnem);
        if (in->op == OP_BITCAST && type_size((MlwsType)in->type) != type_size((MlwsType)in->type2))
            return fail(ce, -1, "bitcast.%s.%s: types must have the same size", type_name((MlwsType)in->type),
                        type_name((MlwsType)in->type2));
    }
    if (in->nops < oi->minops || in->nops > oi->maxops) {
        if (oi->minops == oi->maxops)
            return fail(ce, -1, "%s takes %d operand(s), got %d", oi->mnem, oi->minops, in->nops);
        return fail(ce, -1, "%s takes %d to %d operands, got %d", oi->mnem, oi->minops, oi->maxops, in->nops);
    }
    if (oi->flags & OPF_CALL) {
        bool indirect = op_calls_closure(in->op);
        uint8_t want = in->op == OP_NCALL ? OM_NATIVE : OM_FUNC;
        if (indirect) {
            if (!check_operand(&in->ops[0], 0, T_R, false, ce)) return false;
        } else if (in->ops[0].mode != want) {
            return fail(ce, 0, "%s target must be a %s", oi->mnem, in->op == OP_NCALL ? "native name string" : "function");
        }
        if (in->ops[1].mode != OM_IMM || in->ops[1].imm < 0 || in->ops[1].imm > in->nops - 2)
            return fail(ce, 1, "malformed call block (argument count)");
        int argc = (int)in->ops[1].imm;
        if ((in->op == OP_TCALL || in->op == OP_TCALLI) && in->nops != 2 + argc)
            return fail(ce, -1, "tcall cannot have result operands (results go to the current caller)");
        for (int k = 2; k < in->nops; k++)
            if (!check_operand(&in->ops[k], k, T_NONE, k >= 2 + argc, ce)) return false;
        return true;
    }
    for (int k = 0; k < in->nops; k++) {
        const Operand *o = &in->ops[k];
        char pat = op_pattern_at(oi, k);
        MlwsType t = operand_type(in, k);
        switch (pat) {
        case 'D': case 'W': case 'B': case 'E': case 'X': case 'J':
            if (!check_operand(o, k, t, true, ce)) return false;
            break;
        case 'S': case 'V': case 'Q': case 'A': case 'O':
            if (!check_operand(o, k, t, false, ce)) return false;
            break;
        case 'T':
            if (o->mode != OM_STRUCT) return fail(ce, k, "operand %d must be a struct type name", k + 1);
            break;
        case 'M':
            if (!mode_is_memory(o->mode)) return fail(ce, k, "operand %d must be a memory operand", k + 1);
            if (!check_mem(o, k, ce)) return false;
            break;
        case 'K':
            if (o->mode != OM_CONST) return fail(ce, k, "operand %d must be a string literal", k + 1);
            break;
        case 'I':
            if (o->mode != OM_IMM || o->imm < 0) return fail(ce, k, "operand %d must be a non-negative integer constant", k + 1);
            break;
        case 'L':
            if (o->mode != OM_LABEL) return fail(ce, k, "operand %d must be a label", k + 1);
            break;
        case 'F':
            if (o->mode != OM_FUNC) return fail(ce, k, "operand %d must be a function", k + 1);
            break;
        case 'N':
            if (o->mode != OM_NATIVE) return fail(ce, k, "operand %d must be a native name", k + 1);
            break;
        default:
            return fail(ce, k, "too many operands for %s", oi->mnem);
        }
    }
    return true;
}
