/* verify.c - load-time bytecode verifier. Everything the interpreter relies on
 * without re-checking is established here: operand shapes, register bounds,
 * branch targets, call/return arity and argument types. See docs/ABI.md. */
#include "../core/check.h"
#include "vm.h"

static bool verr(MlwsError *err, const LModule *lm, const Function *f, uint32_t pc, const char *fmt, ...)
    MLWS_PRINTF(5, 6);
static bool verr(MlwsError *err, const LModule *lm, const Function *f, uint32_t pc, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    uint32_t line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
    return mlws_err(err, "E_VERIFY", "%s:%u: in %s.%s (instruction %u): %s", lm->m->source ? lm->m->source : lm->m->name,
                  line, lm->m->name, f->name, pc, msg);
}

static bool reg_in_frame(const MFunc *mf, uint8_t reg) {
    int b = REG_BANK(reg);
    return b >= NBANKS || REG_IDX(reg) < mf->nregs[b];
}

static bool operand_regs_ok(const MFunc *mf, const Operand *o, uint8_t *bad) {
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC:
    case OM_POSTINCB: case OM_PREDECB:
        *bad = o->reg;
        return reg_in_frame(mf, o->reg);
    case OM_IDX: case OM_IDXB:
        *bad = o->reg;
        if (!reg_in_frame(mf, o->reg)) return false;
        *bad = o->ireg;
        return reg_in_frame(mf, o->ireg);
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        *bad = o->ireg;
        return reg_in_frame(mf, o->ireg);
    case OM_MIND: case OM_MINDB: return operand_regs_ok(mf, o->inner, bad);
    default: return true;
    }
}

static bool verify_function(LModule *lm, Function *f, MlwsError *err) {
    const MFunc *mf = f->mf;
    if (f->ncode == 0) return verr(err, lm, f, 0, "function has no code");
    for (uint32_t pc = 0; pc < f->ncode; pc++) {
        const Insn *in = &f->code[pc];
        CheckErr ce;
        if (!check_insn(in, &ce)) return verr(err, lm, f, pc, "%s: %s", op_info[in->op].mnem, ce.msg);
        for (int k = 0; k < in->nops; k++) {
            uint8_t bad;
            char b[8];
            if (!operand_regs_ok(mf, &in->ops[k], &bad))
                return verr(err, lm, f, pc, "register %s is outside the function's register window", reg_name(bad, b));
        }
        if (in->op == OP_CALL || in->op == OP_TCALL) {
            const Function *callee = in->ops[0].rv.ptr;
            const MFunc *cm = callee->mf;
            int argc = (int)in->ops[1].imm;
            if (argc != cm->nparams)
                return verr(err, lm, f, pc, "call to %s.%s passes %d argument(s); it takes %d", callee->mod->m->name,
                            callee->name, argc, cm->nparams);
            for (int k = 0; k < argc; k++)
                if (!check_operand(&in->ops[2 + k], 2 + k, (MlwsType)cm->params[k], false, &ce))
                    return verr(err, lm, f, pc, "argument %d of call to %s: %s", k + 1, callee->name, ce.msg);
            int nres = in->nops - 2 - argc;
            if (cm->flags & MF_RETS_DECLARED) {
                if (nres > cm->nrets)
                    return verr(err, lm, f, pc, "call to %s expects %d result(s), but %s returns %d", callee->name,
                                nres, callee->name, cm->nrets);
                for (int k = 0; k < nres; k++)
                    if (!check_operand(&in->ops[2 + argc + k], 2 + argc + k, (MlwsType)cm->rets[k], true, &ce))
                        return verr(err, lm, f, pc, "result %d of call to %s: %s", k + 1, callee->name, ce.msg);
            }
        } else if (in->op == OP_NCALL) {
            const NativeDef *nd = in->ops[0].rv.ptr;
            int argc = (int)in->ops[1].imm;
            if (argc != nd->np)
                return verr(err, lm, f, pc, "native \"%s\" takes %d argument(s), got %d", nd->name, nd->np, argc);
            for (int k = 0; k < argc; k++)
                if (!check_operand(&in->ops[2 + k], 2 + k, (MlwsType)nd->pt[k], false, &ce))
                    return verr(err, lm, f, pc, "argument %d of native \"%s\": %s", k + 1, nd->name, ce.msg);
            int nres = in->nops - 2 - argc;
            if (nres > nd->nr)
                return verr(err, lm, f, pc, "native \"%s\" returns %d result(s), %d requested", nd->name, nd->nr, nres);
            for (int k = 0; k < nres; k++)
                if (!check_operand(&in->ops[2 + argc + k], 2 + argc + k, (MlwsType)nd->rt[k], true, &ce))
                    return verr(err, lm, f, pc, "result %d of native \"%s\": %s", k + 1, nd->name, ce.msg);
        } else if (in->op == OP_RET && (mf->flags & MF_RETS_DECLARED)) {
            if (in->nops != mf->nrets)
                return verr(err, lm, f, pc, "ret has %d value(s) but the function declares %d result(s)", in->nops,
                            mf->nrets);
            for (int k = 0; k < in->nops; k++)
                if (!check_operand(&in->ops[k], k, (MlwsType)mf->rets[k], false, &ce))
                    return verr(err, lm, f, pc, "ret: %s", ce.msg);
        }
    }
    if (!(op_info[f->code[f->ncode - 1].op].flags & OPF_TERM))
        return verr(err, lm, f, f->ncode - 1, "execution can run off the end of the function");
    return true;
}

bool vm_verify_module(VM *vm, LModule *lm, MlwsError *err) {
    (void)vm;
    for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
        Function *f = &lm->funcs[i];
        if (f->target != f) continue;
        if (!verify_function(lm, f, err)) return false;
    }
    return true;
}
