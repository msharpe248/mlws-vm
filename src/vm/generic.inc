/* generic.inc - the generic forms: every instruction that is not quickened (or
 * whose quickened handler gave up), compiled inside interp.c's dispatch loop or
 * as a function of its own. CX is the Ctx pointer; in, p, vm and fr are the
 * instruction, the process, the VM and the current frame. It ends by breaking
 * out of its switch, after which the caller reaches the safepoint, or returns
 * a SLICE_* code (SLICE_EXT for an instruction interp_run runs). */

        const Operand *ops = in->ops;
        MlwsType T = (MlwsType)in->type;
        MlwsType TN = T; /* the type as written: .big / .dec select number arithmetic */
        if (T == T_BIG || T == T_DEC) T = T_R;
        switch (in->op) {
        case OP_NOP:
            break;
        case OP_BREAK: /* a debugger stops here; without one (or in a callback) it is a no-op */
            if (vm->hook && !p->cb_base) {
                vm->stopped = p;
                return SLICE_BREAK;
            }
            break;
        case OP_HALT:
            if (p->cb_base) vm_trap(p, "E_CALLBACK", "halt inside a callback from C");
            p->exit_code = in->nops ? rd(CX, &ops[0], T_I64).i : 0;
            return SLICE_DONE;
        case OP_ASSERT:
            if (rd(CX, &ops[0], T_I64).i == 0)
                vm_trap(p, "E_ASSERT", "%s",
                        in->nops > 1 && ((Obj *)ops[1].rv.ptr)->kind == OBJ_STRING ? ((ObjString *)ops[1].rv.ptr)->data
                                                                                  : "assertion failed");
            break;

        case OP_MOV: {
            Loc d;
            loc_of(CX, &ops[0], T ? type_size(T) : 8, &d);
            if (TN != T) { /* mov.big / mov.dec: literals become numbers */
                wr_num(CX, &d, rd_num(CX, &ops[1], TN));
                break;
            }
            MlwsType t = T;
            Word v = rdn(CX, &ops[1], &t);
            wr_loc(CX, &d, t, v);
            break;
        }
        case OP_SWAP: {
            Loc a, b;
            loc_of(CX, &ops[0], T ? type_size(T) : 8, &a);
            loc_of(CX, &ops[1], T ? type_size(T) : 8, &b);
            MlwsType ta = T, tb = T;
            Word va = rd_loc(CX, &a, &ta), vb = rd_loc(CX, &b, &tb);
            wr_loc(CX, &a, tb, vb);
            wr_loc(CX, &b, ta, va);
            break;
        }
        case OP_LEA: {
            Loc d;
            loc_of(CX, &ops[0], 8, &d);
            Word w;
            w.p = op_addr(CX, &ops[1], 8);
            wr_loc(CX, &d, T_P, w);
            break;
        }
        case OP_PUSH:
            for (int k = 0; k < in->nops; k++) {
                MlwsType t = T;
                Word v = rdn(CX, &ops[k], &t);
                push_word(CX, t, v);
            }
            break;
        case OP_POP:
            for (int k = 0; k < in->nops; k++) {
                MlwsType t = T;
                if (t == T_NONE && ops[k].mode == OM_REG) t = reg_natural_type(ops[k].reg);
                Loc top = mem_loc(p, p->sp);
                Word v = rd_loc(CX, &top, &t);
                p->sp = ptr_add(CX, p->sp, 8);
                wr(CX, &ops[k], t, v);
            }
            break;
        case OP_DROP:
            p->sp = ptr_add(CX, p->sp, (in->nops ? ops[0].imm : 1) * 8);
            break;
        case OP_ENTER: {
            int64_t n = ops[0].imm;
            if (n > (int64_t)p->stack.size) vm_trap(p, "E_STACK_OVERFLOW", "data stack overflow");
            uint64_t sp = ptr_add(CX, p->sp, -n * 8);
            Loc L = mem_loc(p, sp);
            memset(mem_ptr(CX, &L, (int)(n * 8)), 0, (size_t)n * 8);
            if (L.seg->tags) memset(L.seg->tags + L.off / 8, T_NONE, (size_t)n);
            p->sp = sp;
            note_sp(p);
            break;
        }
        case OP_CVT:
        case OP_BITCAST: {
            if (in->op == OP_CVT && (type_is_ref(in->type) || type_is_ref(in->type2))) {
                convert_num(CX, in);
                break;
            }
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            MlwsType from = (MlwsType)in->type2;
            Word v = rd(CX, &ops[in->nops - 1], from);
            wr_loc(CX, &d, T, in->op == OP_CVT ? convert(T, from, v) : bitcast(T, from, v));
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_MIN: case OP_MAX:
        case OP_AND: case OP_OR: case OP_XOR:
            if (TN != T) arith_num(CX, in, TN);
            else arith(CX, in);
            break;
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_POPCNT: case OP_CLZ: case OP_CTZ: case OP_BSWAP:
        case OP_SQRT: case OP_FLOOR: case OP_CEIL: case OP_ROUND: case OP_TRUNC: {
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            if (TN != T) { /* neg/abs of a number */
                const Obj *a;
                if (in->nops == 2) a = rd_num(CX, &ops[1], TN);
                else {
                    MlwsType rt = T_R;
                    a = rd_loc(CX, &d, &rt).r;
                    if (!num_kind_matches(a, TN)) vm_trap(p, "E_TYPE", "destination does not hold a %s", type_name(TN));
                }
                wr_num(CX, &d, num_unary(p, in->op, a));
                break;
            }
            MlwsType t = T;
            Word a = in->nops == 2 ? rd(CX, &ops[1], T) : rd_loc(CX, &d, &t);
            wr_loc(CX, &d, T, unop(CX, in->op, T, a));
            break;
        }
        case OP_INC:
        case OP_DEC: {
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            MlwsType t = T;
            Word a = rd_loc(CX, &d, &t), one;
            if (type_is_float(T)) one.f = 1.0;
            else one.i = 1;
            wr_loc(CX, &d, T, binop(CX, in->op == OP_INC ? OP_ADD : OP_SUB, T, a, one));
            break;
        }
        case OP_SHL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: {
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            MlwsType t = T;
            Word a = in->nops == 3 ? rd(CX, &ops[1], T) : rd_loc(CX, &d, &t);
            Word n = rd(CX, &ops[in->nops - 1], T);
            wr_loc(CX, &d, T, shiftop(in->op, T, a, n));
            break;
        }
        case OP_FMA: {
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            Word a = rd(CX, &ops[1], T), b = rd(CX, &ops[2], T), z = rd(CX, &ops[3], T), r;
            r.f = canon_float(T, fma(a.f, b.f, z.f));
            wr_loc(CX, &d, T, r);
            break;
        }

        case OP_SEQ: case OP_SNE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE: {
            Loc d;
            loc_of(CX, &ops[0], 1, &d);
            Word r;
            if (TN != T) { /* compare numbers: as integers -1/0/1 */
                Word x, y;
                x.i = num_cmp(rd_num(CX, &ops[1], TN), rd_num(CX, &ops[2], TN));
                y.i = 0;
                r.i = compare(in->op, T_I64, x, y);
            } else {
                Word a = rd(CX, &ops[1], T), b = rd(CX, &ops[2], T);
                r.i = compare(in->op, T, a, b);
            }
            wr_loc(CX, &d, T_B, r);
            break;
        }
        case OP_SEL: {
            Loc d;
            loc_of(CX, &ops[0], type_size(T), &d);
            int64_t cond = rd(CX, &ops[1], T_I64).i;
            wr_loc(CX, &d, T, rd(CX, &ops[cond ? 2 : 3], T));
            break;
        }

        case OP_JMP:
            fr->pc = ops[0].rv.target;
            break;
        case OP_BISA:
        case OP_BNISA: {
            Obj *ob = rd(CX, &ops[0], T_R).r;
            bool is = ob && ob->kind == OBJ_STRUCT && ((ObjStruct *)ob)->def == ops[1].rv.ptr;
            if (is == (in->op == OP_BISA)) fr->pc = ops[2].rv.target;
            break;
        }
        case OP_BZ:
        case OP_BNZ: {
            bool z = is_zero(T, rd(CX, &ops[0], T));
            if (z == (in->op == OP_BZ)) fr->pc = ops[1].rv.target;
            break;
        }
        case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT: case OP_BGE: {
            bool taken;
            if (TN != T) {
                Word x, y;
                x.i = num_cmp(rd_num(CX, &ops[0], TN), rd_num(CX, &ops[1], TN));
                y.i = 0;
                taken = compare(in->op, T_I64, x, y);
            } else {
                Word a = rd(CX, &ops[0], T), b = rd(CX, &ops[1], T);
                taken = compare(in->op, T, a, b);
            }
            if (taken) fr->pc = ops[2].rv.target;
            break;
        }
        case OP_JTAB: {
            int64_t idx = rd(CX, &ops[0], T_I64).i;
            if (idx >= 0 && idx < in->nops - 1) fr->pc = ops[1 + idx].rv.target;
            break;
        }

        case OP_CALL:
        case OP_TCALL: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            const MFunc *mf = fn->mf;
            for (int k = 0; k < argc; k++) args[k] = rd(CX, &ops[2 + k], (MlwsType)mf->params[k]);
            const Insn *ret_insn = in;
            if (in->op == OP_TCALL) {
                ret_insn = fr->ret_insn;
                proc_pop_frame(p);
            }
            proc_push_frame(p, fn, ret_insn);
            ctx_load(CX);
            bind_args(CX, NULL, mf->params, argc, args);
            break;
        }
        case OP_RET: {
            Word vals[MAX_OPERANDS];
            MlwsType vt[MAX_OPERANDS];
            const MFunc *mf = fr->fn->mf;
            bool decl = (mf->flags & MF_RETS_DECLARED) != 0;
            for (int k = 0; k < in->nops; k++) {
                vt[k] = decl ? (MlwsType)mf->rets[k] : T_NONE;
                vals[k] = rdn(CX, &ops[k], &vt[k]);
            }
            const Insn *ri = fr->ret_insn;
            const char *callee = fr->fn->name;
            proc_pop_frame(p);
            if (p->nframes == p->cb_base) { /* the entry function, a callback or a coroutine returned */
                if (!p->cb_base) p->exit_code = in->nops > 0 && type_is_int(vt[0]) ? vals[0].i : 0;
                keep_results(p, vals, vt, in->nops);
                return SLICE_DONE;
            }
            ctx_load(CX);
            if (ri) {
                int argc = (int)ri->ops[1].imm;
                int nres = ri->nops - 2 - argc;
                if (nres > in->nops)
                    vm_trap(p, "E_RET_COUNT", "%s returned %d value(s) but the caller expects %d", callee, in->nops, nres);
                for (int k = 0; k < nres; k++) wr(CX, &ri->ops[2 + argc + k], vt[k], vals[k]);
            }
            break;
        }
        case OP_NCALL: {
            const NativeDef *nd = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[NATIVE_MAX_ARGS], rets[NATIVE_MAX_RETS];
            memset(rets, 0, sizeof rets);
            if (!nd->flags) { /* built in: the fast path */
                for (int k = 0; k < argc; k++) args[k] = rd(CX, &ops[2 + k], (MlwsType)nd->pt[k]);
                nd->fn(p, args, rets);
                if (p->reentered) { /* it ran MLWSVM code (a comparator, say): the banks may have moved */
                    p->reentered = false;
                    ctx_load(CX);
                    fr = ctx_frame(CX);
                }
            } else if (p->bcall.state == BCALL_DONE && p->bcall.insn == in) { /* back from the I/O pool */
                memcpy(rets, p->bcall.rets, sizeof rets);
                p->bcall.state = BCALL_NONE;
                p->resumed = false;
                p->fdwait.deadline = 0;
                native_complete(p);
            } else {
                for (int k = 0; k < argc; k++) args[k] = rd(CX, &ops[2 + k], (MlwsType)nd->pt[k]);
                bool to_pool = (nd->flags & NF_BLOCKING) != 0;
                if (p->cb_base) { /* inside a callback: C is waiting on this thread, so run it here */
                    native_call(p, nd, args, rets);
                    if (p->native_req == NREQ_BLOCKING) { /* the blocking part: here as well */
                        p->native_req = NREQ_NONE;
                        p->resumed = true;
                        native_call(p, nd, args, rets);
                    }
                    int req = p->native_req;
                    p->native_req = NREQ_NONE;
                    p->resumed = false;
                    if (req != NREQ_NONE) {
                        p->fdwait.deadline = 0;
                        vm_trap(p, "E_CALLBACK", "%s would wait inside a callback from C", nd->name);
                    }
                    to_pool = false;
                } else if (to_pool) {
                    native_count_call(p, nd);
                } else {
                    native_call(p, nd, args, rets);
                    int req = p->native_req;
                    p->native_req = NREQ_NONE;
                    if (req == NREQ_WAIT_FD) { /* wait for the descriptor; then this instruction runs again */
                        p->resumed = true;
                        fr->pc--;
                        p->insns--;
                        return SLICE_IOWAIT;
                    }
                    if (req == NREQ_WAIT_REPLY) { /* proc.call: wait like recv; then this instruction runs again */
                        p->resumed = true;
                        fr->pc--;
                        p->insns--;
                        return SLICE_BLOCK;
                    }
                    if (req == NREQ_EXIT) { /* os.exit: the program ends (sched_request_exit) */
                        p->exit_code = vm->exit_code;
                        return SLICE_DONE;
                    }
                    to_pool = req == NREQ_BLOCKING;
                    if (to_pool) p->resumed = true;
                    else p->resumed = false, p->fdwait.deadline = 0;
                }
                if (to_pool) { /* run it on the I/O thread pool; this instruction runs again after it */
                    p->bcall.state = BCALL_PENDING;
                    p->bcall.nd = nd;
                    p->bcall.insn = in;
                    memcpy(p->bcall.args, args, sizeof args);
                    memset(p->bcall.rets, 0, sizeof p->bcall.rets);
                    fr->pc--;
                    p->insns--;
                    return SLICE_NATIVE;
                }
            }
            if (p->reentered) { /* a callback ran: the register banks and frames may have moved */
                p->reentered = false;
                ctx_load(CX);
                fr = ctx_frame(CX);
            }
            for (int k = 0; k < in->nops - 2 - argc; k++) wr(CX, &ops[2 + argc + k], (MlwsType)nd->rt[k], rets[k]);
            break;
        }

        case OP_ALLOC: {
            Loc d;
            loc_of(CX, &ops[0], 8, &d);
            ptrdiff_t ri = loc_raw_index(CX, &d);
            Word w;
            w.p = proc_alloc(p, rd(CX, &ops[1], T_I64).i);
            if (ri >= 0) d.seg = &p->raw.v[ri];
            wr_loc(CX, &d, T_P, w);
            break;
        }
        case OP_FREE:
            proc_free_block(p, rd(CX, &ops[0], T_P).p);
            break;
        case OP_REALLOC: {
            Loc d;
            loc_of(CX, &ops[0], 8, &d);
            uint64_t old = rd(CX, &ops[1], T_P).p;
            ptrdiff_t ri = loc_raw_index(CX, &d);
            Word w;
            w.p = proc_realloc(p, old, rd(CX, &ops[2], T_I64).i);
            if (ri >= 0) d.seg = &p->raw.v[ri];
            wr_loc(CX, &d, T_P, w);
            break;
        }
        case OP_MEMCPY:
        case OP_MEMSET: {
            uint64_t dst = rd(CX, &ops[0], T_P).p;
            Word second = rd(CX, &ops[1], in->op == OP_MEMCPY ? T_P : T_I64);
            int64_t n = rd(CX, &ops[2], T_I64).i;
            if (n < 0) vm_trap(p, "E_BOUNDS", "negative length %lld", (long long)n);
            if (n == 0) break;
            if (n > (int64_t)UINT32_MAX) vm_trap(p, "E_BOUNDS", "length %lld too large", (long long)n);
            Loc dl = mem_loc(p, dst);
            uint8_t *dm = mem_ptr(CX, &dl, (int)0);
            if ((uint64_t)dl.off + (uint64_t)n > dl.seg->size)
                vm_trap(p, "E_BOUNDS", "%s of %lld bytes overruns the destination", op_info[in->op].mnem, (long long)n);
            if (in->op == OP_MEMCPY) {
                Loc sl = mem_loc(p, second.p);
                const uint8_t *sm = mem_ptr(CX, &sl, 0);
                if ((uint64_t)sl.off + (uint64_t)n > sl.seg->size)
                    vm_trap(p, "E_BOUNDS", "memcpy of %lld bytes overruns the source", (long long)n);
                if (sl.seg->tags)
                    for (uint64_t w = sl.off / 8; w <= (sl.off + (uint64_t)n - 1) / 8; w++)
                        if (sl.seg->tags[w] == T_R) vm_trap(p, "E_TYPE", "memcpy cannot copy a reference slot");
                scrub_refs(dl.seg, dl.off, (uint64_t)n); /* the source has none to lose */
                memmove(dm, sm, (size_t)n);
            } else {
                scrub_refs(dl.seg, dl.off, (uint64_t)n);
                memset(dm, (int)(uint8_t)second.i, (size_t)n);
            }
            mark_raw(dl.seg, dl.off, (uint64_t)n);
            break;
        }
        /* ---- objects ---- */
        case OP_NEW:
            wr_ref(CX, &ops[0], struct_new(p, ops[1].rv.ptr));
            break;
        case OP_ANEW:
            wr_ref(CX, &ops[0], array_new(p, T, rd(CX, &ops[1], T_I64).i));
            break;
        case OP_LEN: {
            Obj *ob = rd_obj(CX, &ops[1], 0, "len");
            int64_t n;
            switch (ob->kind) {
            case OBJ_STRING: n = ((ObjString *)ob)->len; break;
            case OBJ_BYTES: n = ((ObjBytes *)ob)->len; break;
            case OBJ_ARRAY: n = ((ObjArray *)ob)->len; break;
            case OBJ_LIST: n = ((ObjList *)ob)->len; break;
            case OBJ_MAP: n = ((ObjMap *)ob)->count; break;
            default: vm_trap(p, "E_TYPE", "len of a %s", obj_desc(ob));
            }
            wr_int(CX, &ops[0], T_I64, n);
            break;
        }
        case OP_LDV: { /* the slot's own type picks the destination; the other gets zero */
            MlwsType t = T_NONE;
            Word v = rdn(CX, &ops[2], &t), z = {0}; /* never written: i64 0 */
            if (type_is_int(t)) {
                wr(CX, &ops[1], T_F64, z);
                wr(CX, &ops[0], T_I64, v);
            } else if (type_is_float(t)) {
                wr(CX, &ops[0], T_I64, z);
                wr(CX, &ops[1], T_F64, v);
            } else {
                vm_trap(p, "E_TYPE", "ldv: the value is %s, not a number", type_name(t));
            }
            break;
        }
        case OP_TAGOF: {
            int64_t tag;
            const Operand *o = &ops[1];
            if (o->mode == OM_REG) {
                tag = reg_natural_type(o->reg);
            } else if (o->mode == OM_IMM) {
                tag = T_I64;
            } else if (o->mode == OM_FIMM) {
                tag = T_F64;
            } else if (o->mode == OM_CONST) {
                tag = T_R;
            } else {
                Loc L;
                loc_of(CX, o, 1, &L);
                switch (L.is_reg) {
                case LK_SLOT: tag = L.fixed ? L.fixed : *L.tag; break;
                case LK_BYTE: tag = T_U8; break;
                default: tag = L.seg->tags && L.off % 8 == 0 ? L.seg->tags[L.off / 8] : TAG_RAW; break;
                }
            }
            wr_int(CX, &ops[0], T_I64, tag);
            break;
        }
        case OP_TYPEOF: {
            Obj *ob = rd(CX, &ops[1], T_R).r;
            wr_int(CX, &ops[0], T_I64, ob ? ob->kind : OBJ_NULL);
            break;
        }
        case OP_ISA: {
            Obj *ob = rd(CX, &ops[1], T_R).r;
            bool is = ob && ob->kind == OBJ_STRUCT && ((ObjStruct *)ob)->def == ops[2].rv.ptr;
            wr_int(CX, &ops[0], T_B, is);
            break;
        }
        case OP_RESIZE: {
            Obj *ob = rd_obj(CX, &ops[0], 0, "resize");
            int64_t n = rd(CX, &ops[1], T_I64).i;
            if (ob->kind == OBJ_BYTES) bytes_resize(p, (ObjBytes *)ob, n);
            else if (ob->kind == OBJ_LIST) list_resize(p, (ObjList *)ob, n);
            else vm_trap(p, "E_TYPE", "resize needs a byte buffer or list, got %s", obj_desc(ob));
            break;
        }

        /* ---- strings and bytes ---- */
        case OP_STRCAT: {
            /* Read every part before allocating, so a trap leaks nothing. */
            const uint8_t *part[MAX_OPERANDS];
            uint32_t plen[MAX_OPERANDS];
            uint64_t total = 0;
            for (int k = 1; k < in->nops; k++) {
                part[k] = rd_text(CX, &ops[k], "strcat", &plen[k]);
                total += plen[k];
            }
            if (total >= (uint64_t)1 << 31) vm_trap(p, "E_BAD_ALLOC", "strcat result too long");
            char *buf = xmalloc((size_t)total + 1);
            size_t at = 0;
            for (int k = 1; k < in->nops; k++) {
                memcpy(buf + at, part[k], plen[k]);
                at += plen[k];
            }
            ObjString *s = str_new(p, buf, (size_t)total);
            free(buf);
            wr_ref(CX, &ops[0], s);
            break;
        }
        case OP_SUBSTR: {
            uint32_t n;
            const uint8_t *d = rd_text(CX, &ops[1], "substr", &n);
            int64_t start = rd(CX, &ops[2], T_I64).i, cnt = rd(CX, &ops[3], T_I64).i;
            if (start < 0 || cnt < 0 || start + cnt > (int64_t)n)
                vm_trap(p, "E_BOUNDS", "substr(%lld, %lld) out of range for length %u", (long long)start, (long long)cnt, n);
            wr_ref(CX, &ops[0], str_new(p, (const char *)d + start, (size_t)cnt));
            break;
        }
        case OP_STRCMP: {
            uint32_t na, nb;
            const uint8_t *a = rd_text(CX, &ops[1], "strcmp", &na);
            const uint8_t *b = rd_text(CX, &ops[2], "strcmp", &nb);
            int r = memcmp(a, b, na < nb ? na : nb);
            if (r == 0) r = na < nb ? -1 : na > nb;
            wr_int(CX, &ops[0], T_I64, r < 0 ? -1 : r > 0);
            break;
        }
        case OP_STRFIND: {
            uint32_t nh, nn;
            const uint8_t *h = rd_text(CX, &ops[1], "strfind", &nh);
            const uint8_t *nd = rd_text(CX, &ops[2], "strfind", &nn);
            int64_t start = in->nops > 3 ? rd(CX, &ops[3], T_I64).i : 0, found = -1;
            if (start < 0) start = 0;
            for (int64_t i = start; i + nn <= nh; i++) {
                if (memcmp(h + i, nd, nn) == 0) {
                    found = i;
                    break;
                }
            }
            wr_int(CX, &ops[0], T_I64, found);
            break;
        }
        case OP_TOSTR: {
            MlwsType t = T;
            Word v = rdn(CX, &ops[1], &t);
            wr_ref(CX, &ops[0], value_to_string(p, v, t));
            break;
        }
        case OP_PARSE: {
            uint32_t n;
            const uint8_t *d = rd_text(CX, &ops[1], "parse", &n);
            Word v;
            if (TN != T) {
                v.r = num_parse(p, TN, (const char *)d, n);
                if (!v.r)
                    vm_trap(p, "E_PARSE", "cannot parse \"%.*s\" as %s", (int)(n > 60 ? 60 : n), (const char *)d, type_name(TN));
                wr(CX, &ops[0], T_R, v);
                break;
            }
            if (parse_number(CX, T, d, n, &v) < 0)
                vm_trap(p, "E_PARSE", "cannot parse \"%.*s\" as %s", (int)(n > 60 ? 60 : n), (const char *)d, type_name(T));
            wr(CX, &ops[0], T, v);
            break;
        }
        case OP_BUFNEW:
            wr_ref(CX, &ops[0], bytes_new(p, rd(CX, &ops[1], T_I64).i));
            break;
        case OP_BUFCAT: {
            ObjBytes *b = (ObjBytes *)rd_obj(CX, &ops[0], OBJ_BYTES, "bufcat");
            for (int k = 1; k < in->nops; k++) {
                uint32_t n;
                const uint8_t *d = rd_text(CX, &ops[k], "bufcat", &n);
                bytes_append(p, b, d, n);
            }
            break;
        }
        case OP_BUFPUSH: {
            ObjBytes *b = (ObjBytes *)rd_obj(CX, &ops[0], OBJ_BYTES, "bufpush");
            for (int k = 1; k < in->nops; k++) {
                uint8_t byte = (uint8_t)rd(CX, &ops[k], T_I64).i;
                bytes_append(p, b, &byte, 1);
            }
            break;
        }

        /* ---- lists and maps ---- */
        case OP_LNEW:
            wr_ref(CX, &ops[0], list_new(p, in->nops > 1 ? rd(CX, &ops[1], T_I64).i : 0));
            break;
        case OP_LPUSH: {
            ObjList *l = (ObjList *)rd_obj(CX, &ops[0], OBJ_LIST, "lpush");
            for (int k = 1; k < in->nops; k++) {
                MlwsType t = T;
                Word v = rdn(CX, &ops[k], &t);
                list_push(p, l, v, (uint8_t)t);
            }
            break;
        }
        case OP_LPOP: {
            ObjList *l = (ObjList *)rd_obj(CX, &ops[1], OBJ_LIST, "lpop");
            if (l->len == 0) vm_trap(p, "E_BOUNDS", "lpop of an empty list");
            MlwsType t = T;
            if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
            Loc L;
            memset(&L, 0, sizeof L);
            L.is_reg = LK_SLOT;
            L.slot = &l->items[l->len - 1];
            L.tag = &l->tags[l->len - 1];
            Word v = slot_read(CX, &L, &t);
            list_resize(p, l, l->len - 1);
            wr(CX, &ops[0], t, v);
            break;
        }
        case OP_LINS: {
            ObjList *l = (ObjList *)rd_obj(CX, &ops[0], OBJ_LIST, "lins");
            int64_t at = rd(CX, &ops[1], T_I64).i;
            MlwsType t = T;
            Word v = rdn(CX, &ops[2], &t);
            list_insert(p, l, at, v, (uint8_t)t);
            break;
        }
        case OP_LDEL: {
            ObjList *l = (ObjList *)rd_obj(CX, &ops[0], OBJ_LIST, "ldel");
            list_delete(p, l, rd(CX, &ops[1], T_I64).i);
            break;
        }
        case OP_MNEW:
            wr_ref(CX, &ops[0], map_new(p));
            break;
        case OP_MSET: {
            ObjMap *m = (ObjMap *)rd_obj(CX, &ops[0], OBJ_MAP, "mset");
            MlwsType kt = T_NONE, vt = T;
            Word k = rdn(CX, &ops[1], &kt);
            Word v = rdn(CX, &ops[2], &vt);
            map_set(p, m, k, (uint8_t)kt, v, (uint8_t)vt);
            break;
        }
        case OP_MGET: {
            ObjMap *m = (ObjMap *)rd_obj(CX, &ops[1], OBJ_MAP, "mget");
            MlwsType kt = T_NONE;
            Word k = rdn(CX, &ops[2], &kt);
            MapEntry *e = map_find(m, k, (uint8_t)kt);
            MlwsType t = T;
            if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
            Word v;
            if (e) {
                Loc L;
                memset(&L, 0, sizeof L);
                L.is_reg = LK_SLOT;
                L.slot = &e->val;
                L.tag = &e->vtag;
                v = slot_read(CX, &L, &t);
            } else if (in->nops > 3) {
                v = rdn(CX, &ops[3], &t);
            } else {
                char kb[96];
                describe_key(k, kt, kb, sizeof kb);
                vm_trap(p, "E_KEY", "key %s not found in map", kb);
            }
            wr(CX, &ops[0], t, v);
            break;
        }
        case OP_MHAS: {
            ObjMap *m = (ObjMap *)rd_obj(CX, &ops[1], OBJ_MAP, "mhas");
            MlwsType kt = T_NONE;
            Word k = rdn(CX, &ops[2], &kt);
            wr_int(CX, &ops[0], T_B, map_find(m, k, (uint8_t)kt) != NULL);
            break;
        }
        case OP_MDEL: {
            ObjMap *m = (ObjMap *)rd_obj(CX, &ops[0], OBJ_MAP, "mdel");
            MlwsType kt = T_NONE;
            Word k = rdn(CX, &ops[1], &kt);
            map_delete(m, k, (uint8_t)kt);
            break;
        }
        case OP_MKEYS:
            wr_ref(CX, &ops[0], map_keys(p, (ObjMap *)rd_obj(CX, &ops[1], OBJ_MAP, "mkeys")));
            break;

        /* ---- handles ---- */
        case OP_HNEW:
            wr_int(CX, &ops[0], T_I64, handle_new(p, rd(CX, &ops[1], T_R).r));
            break;
        case OP_HGET:
            wr_ref(CX, &ops[0], handle_get(p, rd(CX, &ops[1], T_I64).i));
            break;
        case OP_HFREE:
            handle_free(p, rd(CX, &ops[0], T_I64).i);
            break;

        /* ---- closures ---- */
        case OP_CLOSURE: {
            Function *fn = ops[0].rv.ptr;
            int nup = (int)ops[1].imm;
            Word vals[MAX_OPERANDS];
            uint8_t tags[MAX_OPERANDS];
            for (int k = 0; k < nup; k++) {
                MlwsType t = T_NONE;
                vals[k] = rdn(CX, &ops[2 + k], &t);
                tags[k] = (uint8_t)t;
            }
            ObjClosure *cl = closure_new(p, fn, (uint32_t)nup);
            for (int k = 0; k < nup; k++) {
                cl->up[k] = vals[k];
                cl->tags[k] = tags[k];
            }
            wr_ref(CX, &ops[2 + nup], cl);
            break;
        }
        case OP_CALLI:
        case OP_TCALLI: {
            ObjClosure *cl = rd_closure(CX, &ops[0]);
            Function *fn = cl->fn;
            const MFunc *mf = fn->mf;
            int argc = (int)ops[1].imm;
            if (argc != mf->nparams)
                vm_trap(p, "E_TYPE", "closure %s takes %d argument(s), got %d", fn->name, mf->nparams, argc);
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) args[k] = rd_param(CX, &ops[2 + k], (MlwsType)mf->params[k]);
            const Insn *ret_insn = in;
            if (in->op == OP_TCALLI) {
                ret_insn = fr->ret_insn;
                proc_pop_frame(p);
            }
            proc_push_frame(p, fn, ret_insn);
            p->frames[p->nframes - 1].closure = &cl->h;
            ctx_load(CX);
            bind_args(CX, NULL, mf->params, argc, args);
            break;
        }
        case OP_SPAWNI:
        case OP_SPAWNIL:
        case OP_SPAWNIM: {
            ObjClosure *cl = rd_closure(CX, &ops[0]);
            const MFunc *mf = cl->fn->mf;
            int argc = (int)ops[1].imm;
            if (argc != mf->nparams)
                vm_trap(p, "E_TYPE", "closure %s takes %d argument(s), got %d", cl->fn->name, mf->nparams, argc);
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) {
                if (mf->params[k] == T_P) vm_trap(p, "E_TYPE", "pointer parameters cannot cross processes");
                args[k] = rd_param(CX, &ops[2 + k], (MlwsType)mf->params[k]);
            }
            Process *child = proc_new(vm);
            child->prog = p->prog; /* the same program */
            memcpy(child->quota, p->quota, sizeof child->quota);
            p->pending_spawn = child;
            Word clw = msg_copy(p, child, (Word){.r = cl}, T_R);
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], mf->params[k]);
            p->pending_spawn = NULL;
            spawn_watch(p, child, in->op == OP_SPAWNIL, in->op == OP_SPAWNIM);
            uint64_t cpid = child->pid; /* once started, the child may run, end and be freed at once */
            proc_start(child, cl->fn, clw.r, args, argc);
            if (in->nops > 2 + argc) wr_int(CX, &ops[2 + argc], T_I64, (int64_t)cpid);
            break;
        }

        /* ---- exceptions ---- */
        case OP_THROW: {
            Obj *ex = rd(CX, &ops[0], T_R).r;
            if (!ex) vm_trap(p, "E_NULL", "throw of null");
            proc_throw(p, ex);
        }
        case OP_CATCH:
            wr_ref(CX, &ops[0], p->exception);
            break;

        /* ---- processes ---- */
        case OP_SPAWN:
        case OP_SPAWNL:
        case OP_SPAWNM: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) args[k] = rd(CX, &ops[2 + k], (MlwsType)fn->mf->params[k]);
            Process *child = proc_new(vm);
            child->prog = p->prog; /* the same program */
            memcpy(child->quota, p->quota, sizeof child->quota);
            p->pending_spawn = child; /* freed by proc_run_slice if a copy traps */
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], fn->mf->params[k]);
            p->pending_spawn = NULL;
            spawn_watch(p, child, in->op == OP_SPAWNL, in->op == OP_SPAWNM);
            uint64_t cpid = child->pid; /* once started, the child may run, end and be freed at once */
            proc_start(child, fn, NULL, args, argc);
            if (in->nops > 2 + argc) wr_int(CX, &ops[2 + argc], T_I64, (int64_t)cpid);
            break;
        }
        case OP_SEND: {
            int64_t pid = rd(CX, &ops[0], T_I64).i;
            MlwsType t = T;
            Word v = rdn(CX, &ops[1], &t);
            if (t == T_P) vm_trap(p, "E_TYPE", "pointers cannot be sent to another process (they are process-local)");
            p->msgs_sent++;
            sched_send(p, (uint64_t)pid, v, (uint8_t)t); /* to a finished or unknown process: dropped */
            break;
        }
        case OP_RECV:
        case OP_RECVT: {
            if (p->mbox_head == p->mbox.n) sched_take_inbox(p);
            if (p->mbox_head < p->mbox.n) {
                MlwsType t = T;
                if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
                Loc L = mbox_loc(&p->mbox.v[p->mbox_head]);
                Word v = slot_read(CX, &L, &t); /* a type mismatch traps and leaves the message */
                mbox_take(p);
                p->deadline_set = false;
                wr(CX, &ops[0], t, v);
                break;
            }
            if (in->op == OP_RECVT) {
                uint64_t now = plat_now_ns();
                if (p->deadline_set) {
                    if (now >= p->wake_at) { /* timed out */
                        p->deadline_set = false;
                        fr->pc = ops[2].rv.target;
                        break;
                    }
                } else {
                    int64_t ms = rd(CX, &ops[1], T_I64).i;
                    if (ms <= 0) { /* poll */
                        fr->pc = ops[2].rv.target;
                        break;
                    }
                    p->deadline_set = true;
                    p->wake_at = now + (uint64_t)ms * 1000000u;
                }
            } else if (p->deadlock) {
                p->deadlock = false;
                vm_trap(p, "E_DEADLOCK", "every process is waiting: no message can ever arrive");
            }
            if (p->cb_base) { /* C is waiting for the callback: it cannot wait in turn */
                p->deadline_set = false;
                vm_trap(p, "E_CALLBACK", "recv would wait inside a callback from C");
            }
            fr->pc--; /* re-execute this receive when woken */
            p->insns--;
            return SLICE_BLOCK;
        }
        case OP_YIELD:
            if (p->cb_base) break; /* a callback runs to completion */
            return SLICE_YIELD;
        case OP_SLEEP: {
            int64_t ms = rd(CX, &ops[0], T_I64).i;
            if (p->cb_base && ms > 0) vm_trap(p, "E_CALLBACK", "sleep inside a callback from C");
            if (ms <= 0) {
                if (p->cb_base) break;
                return SLICE_YIELD;
            }
            p->wake_at = plat_now_ns() + (uint64_t)ms * 1000000u;
            return SLICE_SLEEP;
        }
        case OP_KILL: {
            int64_t pid = rd(CX, &ops[0], T_I64).i;
            char msg[96];
            snprintf(msg, sizeof msg, "killed by process %llu", (unsigned long long)p->pid);
            sched_kill(p, (uint64_t)pid, "E_KILLED", msg);
            break; /* killing ourselves (or a linked chain back to us) stops at the next instruction */
        }
        case OP_LINK: {
            sched_link(p, (uint64_t)rd(CX, &ops[0], T_I64).i);
            break;
        }
        case OP_MONITOR: {
            sched_monitor(p, (uint64_t)rd(CX, &ops[0], T_I64).i);
            break;
        }
        case OP_ALIVE:
            wr_int(CX, &ops[0], T_B, sched_alive(vm, (uint64_t)rd(CX, &ops[1], T_I64).i));
            break;
        case OP_QUOTA: {
            int64_t kind = ops[0].imm, limit = rd(CX, &ops[1], T_I64).i;
            if (kind >= QUOTA__COUNT) vm_trap(p, "E_QUOTA", "unknown quota kind %lld", (long long)kind);
            if (limit < 0) vm_trap(p, "E_QUOTA", "a quota cannot be negative");
            sched_set_quota(p, (int)kind, (uint64_t)limit); /* only ever lowered */
            if (kind == QUOTA_HEAP) p->next_gc = 0, p->gc_pending = true;   /* re-derive the GC threshold */
            break;
        }
        case OP_MLEN:
            wr_int(CX, &ops[0], T_I64, (int64_t)p->mcount);
            break;

        default: /* the rest run outside this loop (interp_run) */
            return SLICE_EXT;
        }
