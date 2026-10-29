/* verify.c - load-time bytecode verifier. Everything the interpreter relies on
 * without re-checking is established here: operand shapes, register bounds,
 * branch targets, call/return arity and argument types. See docs/ABI.md. */
#include "../core/check.h"
#include "../core/regflow.h"
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
    case OM_ELEM:
        *bad = o->reg;
        if (!reg_in_frame(mf, o->reg)) return false;
        *bad = o->ireg;
        return reg_in_frame(mf, o->ireg);
    case OM_ELEMI: case OM_FIELD:
        *bad = o->reg;
        return reg_in_frame(mf, o->reg);
    case OM_MIND: case OM_MINDB: return operand_regs_ok(mf, o->inner, bad);
    default: return true;
    }
}

static bool verify_function(LModule *lm, Function *f, MlwsError *err) {
    const MFunc *mf = f->mf;
    if (f->ncode == 0) return verr(err, lm, f, 0, "function has no code");
    for (uint32_t i = 0; i < mf->nhandlers; i++) {
        const MHandler *h = &mf->handlers[i];
        if (h->start > h->end || h->end > f->ncode || h->target >= f->ncode)
            return verr(err, lm, f, 0, "exception handler %u is out of range", i);
    }
    for (uint32_t pc = 0; pc < f->ncode; pc++) {
        const Insn *in = &f->code[pc];
        CheckErr ce;
        if (!check_insn(in, &ce)) return verr(err, lm, f, pc, "%s: %s", op_info[in->op].mnem, ce.msg);
        for (int k = 0; k < in->nops; k++) {
            uint8_t bad;
            char b[8];
            if (!operand_regs_ok(mf, &in->ops[k], &bad))
                return verr(err, lm, f, pc, "register %s is outside the function's register window", reg_name(bad, b));
            /* A typed access to a struct field must match the field's register bank. */
            if (in->ops[k].mode == OM_FIELD && !(op_info[in->op].flags & OPF_CALL)) {
                const FieldRef *fr = in->ops[k].rv.ptr;
                MlwsType ft = (MlwsType)fr->def->types[fr->index];
                MlwsType ot = operand_type(in, k);
                if (ot == T_BIG || ot == T_DEC) ot = T_R; /* numbers are references */
                if (ot != T_NONE && (type_bank(ot) != type_bank(ft) || type_is_ref(ot) != type_is_ref(ft)))
                    return verr(err, lm, f, pc, "%s: field %s.%s is %s but is accessed as %s", op_info[in->op].mnem,
                                fr->def->name, fr->def->fields[fr->index], type_name(ft), type_name(ot));
            }
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
        } else if (op_is_spawn(in->op)) {
            const Function *callee = in->ops[0].rv.ptr;
            const MFunc *cm = callee->mf;
            int argc = (int)in->ops[1].imm;
            if (argc != cm->nparams)
                return verr(err, lm, f, pc, "spawn of %s passes %d argument(s); it takes %d", callee->name, argc,
                            cm->nparams);
            for (int k = 0; k < argc; k++) {
                if (cm->params[k] == T_P)
                    return verr(err, lm, f, pc, "spawn of %s: pointer parameters cannot cross processes", callee->name);
                if (!check_operand(&in->ops[2 + k], 2 + k, (MlwsType)cm->params[k], false, &ce))
                    return verr(err, lm, f, pc, "argument %d of spawn %s: %s", k + 1, callee->name, ce.msg);
            }
            int nres = in->nops - 2 - argc;
            if (nres > 1) return verr(err, lm, f, pc, "spawn has one result: the new process id");
            if (nres == 1 && !check_operand(&in->ops[2 + argc], 2 + argc, T_I64, true, &ce))
                return verr(err, lm, f, pc, "spawn result: %s", ce.msg);
        } else if (in->op == OP_CLOSURE) {
            int argc = (int)in->ops[1].imm;
            if (in->nops - 2 - argc != 1) return verr(err, lm, f, pc, "closure has exactly one result: the closure");
            if (!check_operand(&in->ops[2 + argc], 2 + argc, T_R, true, &ce)) return verr(err, lm, f, pc, "closure result: %s", ce.msg);
        } else if (op_is_spawni(in->op)) {
            int argc = (int)in->ops[1].imm;
            int nres = in->nops - 2 - argc;
            if (nres > 1) return verr(err, lm, f, pc, "spawni has one result: the new process id");
            if (nres == 1 && !check_operand(&in->ops[2 + argc], 2 + argc, T_I64, true, &ce))
                return verr(err, lm, f, pc, "spawni result: %s", ce.msg);
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

/* ---- what a call clears ----------------------------------------------------------------
 *
 * A call's new window reads as zero until written (docs/ABI.md), but only two
 * kinds of register need clearing for that: the R registers, which the
 * collector scans at any instruction boundary, and the I, F and P registers
 * the function may read before it writes them. Every other register is
 * written before it is read on every path, so what it held before never
 * shows. plan_window finds the second kind by a forward analysis of the
 * registers definitely written before each instruction (sets that only grow
 * along a path, met by intersection where paths join), from the parameters at
 * entry (every call binds them all); an exception handler starts from the
 * entry set, which every path includes. A call then clears one run of words,
 * from the first register that needs it to the last (the banks lie I, F, R, P
 * in the window). */

static void whole_window(Function *f) {
    f->zoff = 0;
    f->zlen = f->rsize;
}

static void plan_window(Function *f) {
    const MFunc *mf = f->mf;
    uint32_t n = f->ncode;
    if (!n) return;
    RegSet *in = xcalloc(n, sizeof *in);
    uint8_t *seen = xcalloc(n, 2); /* reached; queued (at seen + n) */
    uint32_t *work = xmalloc(n * sizeof *work), nwork = 0;
    RegSet entry = {{0}};
    int cnt[NBANKS] = {0};
    for (int k = 0; k < mf->nparams; k++) {
        Bank b = type_bank((MlwsType)mf->params[k]);
        entry.b[b] |= 1u << cnt[b]++;
    }
#define FLOW(pc_, set_)                                                        \
    do {                                                                       \
        uint32_t s_ = (pc_);                                                   \
        bool ch_ = !seen[s_];                                                  \
        for (int b_ = 0; b_ < NBANKS; b_++) {                                  \
            uint32_t m_ = seen[s_] ? in[s_].b[b_] & (set_).b[b_] : (set_).b[b_]; \
            ch_ |= m_ != in[s_].b[b_];                                         \
            in[s_].b[b_] = m_;                                                 \
        }                                                                      \
        seen[s_] = 1;                                                          \
        if (ch_ && !seen[n + s_]) seen[n + s_] = 1, work[nwork++] = s_;       \
    } while (0)
    FLOW(0, entry);
    for (uint32_t i = 0; i < mf->nhandlers; i++) FLOW(mf->handlers[i].target, entry);
    while (nwork) {
        uint32_t pc = work[--nwork];
        seen[n + pc] = 0;
        const Insn *ins = &f->code[pc];
        RegSet rd, out;
        insn_effect(ins, &rd, &out);
        for (int b = 0; b < NBANKS; b++) out.b[b] |= in[pc].b[b];
        if (!(op_info[ins->op].flags & OPF_TERM) && pc + 1 < n) FLOW(pc + 1, out);
        for (int k = 0; k < ins->nops; k++)
            if (ins->ops[k].mode == OM_LABEL) FLOW(ins->ops[k].rv.target, out);
    }
#undef FLOW
    RegSet need = {{0}}; /* read where not yet written */
    for (uint32_t pc = 0; pc < n; pc++) {
        if (!seen[pc]) continue;
        RegSet rd, wr;
        insn_effect(&f->code[pc], &rd, &wr);
        for (int b = 0; b < NBANKS; b++) need.b[b] |= rd.b[b] & ~in[pc].b[b];
    }
    free(in);
    free(seen);
    free(work);
    for (int b = 0; b < NBANKS; b++) f->read_first[b] = b == BANK_R ? 0 : need.b[b];
    need.b[BANK_R] = 0xFFFFFFFFu; /* the collector scans them all */
    uint32_t lo = f->rsize, hi = 0;
    for (int b = 0; b < NBANKS; b++) {
        uint32_t m = need.b[b];
        if (mf->nregs[b] < 32) m &= (1u << mf->nregs[b]) - 1;
        if (!m) continue;
        uint32_t first = 0, last = 32;
        while (!(m >> first & 1)) first++;
        while (!(m >> (last - 1) & 1)) last--;
        if (f->roff[b] + first < lo) lo = f->roff[b] + first;
        if (f->roff[b] + last > hi) hi = f->roff[b] + last;
    }
    f->zoff = (uint16_t)(lo < hi ? lo : 0);
    f->zlen = (uint16_t)(lo < hi ? hi - lo : 0);
}

bool vm_verify_module(VM *vm, LModule *lm, MlwsError *err) {
    for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
        Function *f = &lm->funcs[i];
        if (f->target != f) continue;
        if (!verify_function(lm, f, err)) return false;
        if (vm->whole_windows) whole_window(f);
        else plan_window(f);
    }
    return true;
}

void vm_whole_windows(VM *vm) {
    vm->whole_windows = true;
    for (size_t i = 0; i < vm->mods.n; i++) {
        LModule *lm = vm->mods.v[i];
        for (uint32_t k = 0; k < lm->m->nfuncs; k++)
            if (lm->funcs[k].target == &lm->funcs[k]) whole_window(&lm->funcs[k]);
    }
}
