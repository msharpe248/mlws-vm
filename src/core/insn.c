/* insn.c - instruction encode/decode. */
#include "insn.h"

bool mode_is_memory(uint8_t mode) {
    return (mode >= OM_IND && mode <= OM_MINDB);
}

bool mode_is_writable(uint8_t mode) { return mode == OM_REG || mode_is_memory(mode); }

void encode_operand(ByteBuf *b, const Operand *o) {
    bb_u8(b, o->mode);
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_POSTINC: case OM_PREDEC:
    case OM_POSTINCB: case OM_PREDECB:
        bb_u8(b, o->reg);
        break;
    case OM_IMM: bb_sleb(b, o->imm); break;
    case OM_FIMM: bb_f64(b, o->f); break;
    case OM_CONST: case OM_LABEL: case OM_FUNC: case OM_NATIVE:
        bb_uleb(b, o->index);
        break;
    case OM_DISP: case OM_DISPB:
        bb_u8(b, o->reg);
        bb_sleb(b, o->imm);
        break;
    case OM_IDX: case OM_IDXB:
        bb_u8(b, o->reg);
        bb_u8(b, o->ireg);
        bb_uleb(b, o->scale);
        bb_sleb(b, o->imm);
        break;
    case OM_GLOBAL: case OM_GLOBALB:
        bb_uleb(b, o->index);
        bb_sleb(b, o->imm);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        bb_uleb(b, o->index);
        bb_u8(b, o->ireg);
        bb_uleb(b, o->scale);
        bb_sleb(b, o->imm);
        break;
    case OM_MIND: case OM_MINDB:
        encode_operand(b, o->inner);
        bb_sleb(b, o->imm);
        break;
    default: break;
    }
}

void encode_insn(ByteBuf *b, const Insn *in) {
    const OpInfo *oi = &op_info[in->op];
    bb_u16(b, oi->code);
    bb_u8(b, in->type);
    if (oi->flags & OPF_TYPE2) bb_u8(b, in->type2);
    bb_u8(b, in->nops);
    for (int i = 0; i < in->nops; i++) encode_operand(b, &in->ops[i]);
}

static bool decode_operand(ByteReader *r, Operand *o, int depth, MlwsError *err) {
    memset(o, 0, sizeof *o);
    o->mode = br_u8(r);
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_POSTINC: case OM_PREDEC:
    case OM_POSTINCB: case OM_PREDECB:
        o->reg = br_u8(r);
        break;
    case OM_IMM: o->imm = br_sleb(r); break;
    case OM_FIMM: o->f = br_f64(r); break;
    case OM_CONST: case OM_LABEL: case OM_FUNC: case OM_NATIVE:
        o->index = br_uleb(r);
        break;
    case OM_DISP: case OM_DISPB:
        o->reg = br_u8(r);
        o->imm = br_sleb(r);
        break;
    case OM_IDX: case OM_IDXB:
        o->reg = br_u8(r);
        o->ireg = br_u8(r);
        o->scale = br_uleb(r);
        o->imm = br_sleb(r);
        break;
    case OM_GLOBAL: case OM_GLOBALB:
        o->index = br_uleb(r);
        o->imm = br_sleb(r);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        o->index = br_uleb(r);
        o->ireg = br_u8(r);
        o->scale = br_uleb(r);
        o->imm = br_sleb(r);
        break;
    case OM_MIND: case OM_MINDB:
        if (depth > 0) return mlws_err(err, "E_LOAD_FORMAT", "nested memory-indirect operand");
        o->inner = xmalloc(sizeof *o->inner);
        if (!decode_operand(r, o->inner, depth + 1, err)) return false;
        if (!mode_is_memory(o->inner->mode))
            return mlws_err(err, "E_LOAD_FORMAT", "memory-indirect operand needs a memory inner operand");
        o->imm = br_sleb(r);
        break;
    default:
        return mlws_err(err, "E_LOAD_FORMAT", "unknown operand mode 0x%02x", o->mode);
    }
    if (r->err) return mlws_err(err, "E_LOAD_FORMAT", "truncated operand");
    return true;
}

bool decode_insn(ByteReader *r, Insn *out, MlwsError *err) {
    memset(out, 0, sizeof *out);
    uint16_t code = br_u16(r);
    int op = op_from_code(code);
    if (r->err) return mlws_err(err, "E_LOAD_FORMAT", "truncated instruction");
    if (op < 0) return mlws_err(err, "E_LOAD_FORMAT", "unknown opcode 0x%04x", code);
    out->op = (uint16_t)op;
    out->type = br_u8(r);
    if (op_info[op].flags & OPF_TYPE2) out->type2 = br_u8(r);
    out->nops = br_u8(r);
    if (r->err) return mlws_err(err, "E_LOAD_FORMAT", "truncated instruction header");
    if (out->nops > MAX_OPERANDS)
        return mlws_err(err, "E_LOAD_FORMAT", "%s: too many operands (%d)", op_info[op].mnem, out->nops);
    out->ops = xcalloc(out->nops ? out->nops : 1, sizeof(Operand));
    for (int i = 0; i < out->nops; i++) {
        if (!decode_operand(r, &out->ops[i], 0, err)) {
            insn_free(out);
            return false;
        }
    }
    return true;
}

void insn_free(Insn *in) {
    if (!in->ops) return;
    for (int i = 0; i < in->nops; i++) free(in->ops[i].inner);
    free(in->ops);
    in->ops = NULL;
}
