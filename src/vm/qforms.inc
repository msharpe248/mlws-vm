/* qforms.inc - the quickened forms' handlers (see interp_quicken in interp.c),
 * written once and compiled either as the cases of interp.c's dispatch loop or
 * as one function per handler that tail-calls the next one. The handlers use
 * this vocabulary, which each includer defines:
 *
 *   QCASE(x)        start the handler for quickened form x
 *   NEXT            go on with the next instruction
 *   JUMP()          make the next instruction the branch target (in->target)
 *   JUMPTO(t)       make the next instruction the one at index t of the code
 *   GENERIC()       hand this instruction to the generic forms
 *   SPILL()/RELOAD() write the fast path's state back / read it again
 *   QI QF QR QP     the current register windows of each bank; QB(I) is QI
 *   QCTX            the Ctx pointer; IR(k), FR(k) the I/F register operand k
 *   in              the instruction
 *   QPRC, QFRM      the process and the current frame
 *   QFORMS_RET_INLINE  1: Q_RET_R also returns to generic calls itself
 *   QPUSH_FRAME(f, ri) push f's frame (or, where it would grow, leave the call
 *                   to the generic form) */

        /* ---- quickened forms (see interp_quicken) ---- */
        QCASE(Q_MOV_II) IR(0) = IR(1); NEXT;
        QCASE(Q_MOV_IK) IR(0) = in->k.i; NEXT;
        QCASE(Q_MOV_FF) FR(0) = FR(1); NEXT;
        QCASE(Q_MOV_FK) FR(0) = in->k.f; NEXT;
        QCASE(Q_MOV_RR) QR[in->r[0]] = QR[in->r[1]]; NEXT;
        QCASE(Q_MOV_PP) QP[in->r[0]] = QP[in->r[1]]; NEXT;
#define X(N, op_)                                                             \
        QCASE(Q_##N##_III) IR(0) = (int64_t)(U(IR(1)) op_ U(IR(2))); NEXT;     \
        QCASE(Q_##N##_IIK) IR(0) = (int64_t)(U(IR(1)) op_ U(in->k.i)); NEXT; \
        QCASE(Q_##N##_II) IR(0) = (int64_t)(U(IR(0)) op_ U(IR(1))); NEXT;      \
        QCASE(Q_##N##_IK) IR(0) = (int64_t)(U(IR(0)) op_ U(in->k.i)); NEXT;
        QARITH_OPS(X)
#undef X
#define X(N, op_)                                                             \
        QCASE(Q_F##N##_FFF) FR(0) = FR(1) op_ FR(2); NEXT;                     \
        QCASE(Q_F##N##_FFK) FR(0) = FR(1) op_ in->k.f; NEXT;                   \
        QCASE(Q_F##N##_FF) FR(0) = FR(0) op_ FR(1); NEXT;                      \
        QCASE(Q_F##N##_FK) FR(0) = FR(0) op_ in->k.f; NEXT;                   \
        QCASE(Q_F##N##_FKF) FR(0) = in->k.f op_ FR(2); NEXT;
        QFARITH_OPS(X)
#undef X
        QCASE(Q_DIV_IIK) IR(0) = IR(1) / in->k.i; NEXT; /* divisor is a non-zero, non -1 constant */
        QCASE(Q_DIV_IK) IR(0) = IR(0) / in->k.i; NEXT;
        QCASE(Q_REM_IIK) IR(0) = IR(1) % in->k.i; NEXT;
        QCASE(Q_REM_IK) IR(0) = IR(0) % in->k.i; NEXT;
        QCASE(Q_INC_I) IR(0) = (int64_t)(U(IR(0)) + 1); NEXT;
        QCASE(Q_DEC_I) IR(0) = (int64_t)(U(IR(0)) - 1); NEXT;
        QCASE(Q_BZ_I) if (IR(0) == 0) JUMP(); NEXT;
        QCASE(Q_BNZ_I) if (IR(0) != 0) JUMP(); NEXT;
        QCASE(Q_JMP) JUMP(); NEXT;
#define X(N, op_)                                                             \
        QCASE(Q_##N##_II) if (IR(0) op_ IR(1)) JUMP(); NEXT;     \
        QCASE(Q_##N##_IK) if (IR(0) op_ in->k.i) JUMP(); NEXT;
        QBRANCH_OPS(X)
#undef X
#undef U
        QCASE(Q_CALL_R) {
            SPILL(); /* the caller's pc is its return address; a depth trap unwinds from here */
            const QPlan *pl = in->k.p;
            Function *fn = pl->fn;
            int argc = pl->argc;
            QPUSH_FRAME(fn, in); /* may move the frames and the banks */
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            const Frame *of = nf - 1;
            nf->qret = 1;
            /* caller slot -> callee slot, a word each (quicken_plan) */
            uint64_t *nw = QPRC->regs + nf->base;
            const uint64_t *ow = QPRC->regs + of->base;
            for (int k = 0; k < argc; k++) memcpy(&nw[pl->slot[argc + k]], &ow[pl->slot[k]], 8);
            ctx_set(QCTX, nf, fn, nw);
            RELOAD();
            NEXT;
        }
        QCASE(Q_RET_R) {
            SPILL();
            const Insn *ri = QFRM->ret_insn;
            if (QFRM->qret) {
                /* Back to a quickened call, whose result registers match this
                 * function's declared result types (and so the registers this
                 * ret names): copy each one straight across, then pop. */
                const QPlan *cp = ri->k.p, *rp = in->k.p;
                int nres = cp->nres;
                const uint8_t *rs = cp->slot + 2 * cp->argc;
                const Frame *of = QFRM - 1;
                uint64_t *cw = QPRC->regs + of->base;
                const uint64_t *fw = QPRC->regs + QFRM->base;
                for (int k = 0; k < nres; k++) memcpy(&cw[rs[k]], &fw[rp->slot[k]], 8);
                proc_pop_frame(QPRC);
                ctx_set(QCTX, (Frame *)of, of->fn, cw);
                RELOAD();
                NEXT;
            }
#if !QFORMS_RET_INLINE /* to a generic call or out of the entry function: the generic ret */
            GENERIC();
#else
            const Operand *qo = in->ops;
            Word vals[MAX_OPERANDS];
            int n = in->nops;
            for (int k = 0; k < n; k++) {
                uint8_t r = qo[k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: vals[k].i = QI[REG_IDX(r)]; break;
                case BANK_F: vals[k].f = QF[REG_IDX(r)]; break;
                case BANK_R: vals[k].r = QR[REG_IDX(r)]; break;
                default: vals[k].p = QP[REG_IDX(r)]; break;
                }
            }
            const MFunc *mf = QFRM->fn->mf;
            const char *callee = QFRM->fn->name;
            proc_pop_frame(QPRC);
            if (QPRC->nframes == QPRC->cb_base) { /* the entry function, a callback or a coroutine returned */
                if (!QPRC->cb_base) QPRC->exit_code = n > 0 && type_is_int((MlwsType)mf->rets[0]) ? vals[0].i : 0;
                MlwsType rt[MAX_OPERANDS];
                for (int k = 0; k < n; k++) rt[k] = (MlwsType)mf->rets[k];
                keep_results(QPRC, vals, rt, n);
                return SLICE_DONE;
            }
            ctx_load(QCTX);
            RELOAD();
            if (!ri) NEXT;
            int argc = (int)ri->ops[1].imm, nres = ri->nops - 2 - argc;
            if (nres > n)
                vm_trap(QPRC, "E_RET_COUNT", "%s returned %d value(s) but the caller expects %d", callee, n, nres);
            /* A quickened call to this function: both sides are registers of
             * the right banks. (Not if a tail call replaced the callee's frame:
             * the function returning is another, with other result types.) */
            if (ri->xop == Q_CALL_R && ((const Function *)ri->ops[0].rv.ptr)->mf == mf) {
                for (int k = 0; k < nres; k++) {
                    uint8_t r = ri->ops[2 + argc + k].reg;
                    switch (REG_BANK(r)) {
                    case BANK_I: QI[REG_IDX(r)] = vals[k].i; break;
                    case BANK_F: QF[REG_IDX(r)] = vals[k].f; break;
                    case BANK_R: QR[REG_IDX(r)] = vals[k].r; break;
                    default: QP[REG_IDX(r)] = vals[k].p; break;
                    }
                }
            } else {
                for (int k = 0; k < nres; k++)
                    wr(QCTX, &ri->ops[2 + argc + k], n ? (MlwsType)mf->rets[k] : T_NONE, vals[k]);
            }
#endif
            NEXT;
        }
        QCASE(Q_BZ_R) if (!QR[in->r[0]]) JUMP(); NEXT;
        QCASE(Q_BNZ_R) if (QR[in->r[0]]) JUMP(); NEXT;
        /* Fields and elements: anything unexpected goes to the generic mov. */
#define STRUCT_OF(reg) ((ObjStruct *)QR[reg])
#define IS_STRUCT(o) ((o) && (o)->h.kind == OBJ_STRUCT && (o)->def == in->k.p)
/* An array element of exactly the instruction's type, or a list element
 * tagged with it (a store retags a list element) or never written (zero of
 * any type), at index i; into an I
 * register (the instruction's type is i64 or u64), also a byte of a string or
 * byte buffer. */
#define ELEM_BYTE_I(N, ob, i) ELEM_BYTE_LOAD(N, ob, i)
#define ELEM_BYTE_F(N, ob, i) GENERIC();
#define ELEM_BYTE_R(N, ob, i) GENERIC();
#define ELEM_LOAD(N, bank, w, i_)                                             \
        {                                                                     \
            Obj *ob = QR[in->r[1]];                                          \
            uint64_t i = (i_);                                                \
            if (!ob) GENERIC();                                            \
            if (ob->kind == OBJ_ARRAY) {                                      \
                ObjArray *a = (ObjArray *)ob;                                 \
                if (i >= a->len || a->elem != in->type) GENERIC();         \
                QB(bank)[in->r[0]] = a->data[i].w;                              \
            } else if (ob->kind == OBJ_LIST) {                                \
                ObjList *l = (ObjList *)ob;                                   \
                if (i >= l->len) GENERIC();                                   \
                if (__builtin_expect(l->tags[i] == in->type, 1))              \
                    QB(bank)[in->r[0]] = l->items[i].w;                         \
                else if (l->tags[i] == T_NONE) /* never written: zero (null) */ \
                    QB(bank)[in->r[0]] = 0;                                   \
                else                                                          \
                    GENERIC();                                                \
            } else {                                                          \
                ELEM_BYTE_##bank(N, ob, i)                                    \
            }                                                                 \
            NEXT;                                                             \
        }
/* The write barrier (heap.h, gc_write) after a store of R register r[1]
 * into object o (R register r[0], or the frame's closure: N). */
#define GC_WRITE_I(o, N)
#define GC_WRITE_F(o, N)
#define GC_WRITE_R(o, N)                                                      \
            if (__builtin_expect(((o)->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD, 0) && \
                gc_young(QR[in->r[1]]))                                       \
                GC_REMEMBER(N, o);
#define ELEM_STORE(bank, w, i_)                                               \
        {                                                                     \
            Obj *ob = QR[in->r[0]];                                          \
            uint64_t i = (i_);                                                \
            if (!ob) GENERIC();                                            \
            if (ob->kind == OBJ_ARRAY) {                                      \
                ObjArray *a = (ObjArray *)ob;                                 \
                if (i >= a->len || a->elem != in->type) GENERIC();         \
                a->data[i].w = QB(bank)[in->r[1]];                              \
            } else if (ob->kind == OBJ_LIST) {                                \
                ObjList *l = (ObjList *)ob;                                   \
                if (i >= l->len) GENERIC();                                \
                l->tags[i] = in->type;                                        \
                l->items[i].w = QB(bank)[in->r[1]];                             \
            } else {                                                          \
                GENERIC();                                                 \
            }                                                                 \
            GC_WRITE_##bank(ob, R0)                                           \
            NEXT;                                                             \
        }
#define X(B, bank, w)                                                         \
        QCASE(Q_LDF_##B) {                                                    \
            ObjStruct *so = STRUCT_OF(in->r[1]);                              \
            if (!IS_STRUCT(so)) GENERIC();                                 \
            QB(bank)[in->r[0]] = so->fields[in->target].w;                      \
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_STF_##B) {                                                    \
            ObjStruct *so = STRUCT_OF(in->r[0]);                              \
            if (!IS_STRUCT(so)) GENERIC();                                 \
            so->fields[in->target].w = QB(bank)[in->r[1]];                      \
            GC_WRITE_##bank(&so->h, R0)                                       \
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_LDE_##B) ELEM_LOAD(E, bank, w, (uint64_t)QI[in->r[2]])            \
        QCASE(Q_STE_##B) ELEM_STORE(bank, w, (uint64_t)QI[in->r[2]])           \
        QCASE(Q_LDED_##B) ELEM_LOAD(ED, bank, w, (uint64_t)QI[in->r[2]] + (uint64_t)in->k.i) \
        QCASE(Q_STED_##B) ELEM_STORE(bank, w, (uint64_t)QI[in->r[2]] + (uint64_t)in->k.i) \
        QCASE(Q_LDEK_##B) ELEM_LOAD(EK, bank, w, (uint64_t)in->target)              \
        QCASE(Q_STEK_##B) ELEM_STORE(bank, w, (uint64_t)in->target)             \
        QCASE(Q_LDU_##B) {                                                    \
            const ObjClosure *cl = (const ObjClosure *)QFRM->closure;          \
            uint32_t u = in->target;                                          \
            if (!cl || u >= cl->nup || cl->tags[u] != in->type) GENERIC(); \
            QB(bank)[in->r[0]] = cl->up[u].w;                                   \
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_STU_##B) {                                                    \
            ObjClosure *cl = (ObjClosure *)QFRM->closure;                       \
            uint32_t u = in->target;                                          \
            if (!cl || u >= cl->nup) GENERIC();                            \
            cl->tags[u] = in->type;                                           \
            cl->up[u].w = QB(bank)[in->r[1]];                                   \
            GC_WRITE_##bank(&cl->h, UP)                                       \
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_LDG_##B) {                                                    \
            uint32_t g = (uint32_t)((uint64_t)in->k.i >> 32), off = (uint32_t)in->k.i; \
            if (g >= QPRC->nglobals) GENERIC();                               \
            const Segment *s = &QPRC->globals[g];                                \
            /* a reference only from a word tagged r; nothing else from one */ \
            if (off + 8 > s->size || (s->tags[off >> 3] == T_R) != (in->type == T_R)) GENERIC(); \
            memcpy(&QB(bank)[in->r[0]], s->data + off, 8);                      \
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_STG_##B) {                                                    \
            uint32_t g = (uint32_t)((uint64_t)in->k.i >> 32), off = (uint32_t)in->k.i; \
            if (g >= QPRC->nglobals) GENERIC();                               \
            Segment *s = &QPRC->globals[g];                                      \
            if (off + 8 > s->size) GENERIC();                              \
            memcpy(s->data + off, &QB(bank)[in->r[1]], 8);                      \
            s->tags[off >> 3] = in->type;                                     \
            NEXT;                                                             \
        }
        X(I, I, i) X(F, F, f) X(R, R, r)
#undef X
        QCASE(Q_STFK) { /* mov R0->S.f, constant: the word in Operand.kw */
            ObjStruct *so = STRUCT_OF(in->r[0]);
            if (!IS_STRUCT(so)) GENERIC();
            memcpy(&so->fields[in->target], &in->ops[1].kw, 8);
            NEXT;
        }
/* mov R0[i], constant: an array element of exactly the instruction's type, or
 * a list element, which takes the type's tag; the word in Operand.kw. */
#define ELEM_STORE_K(i_)                                                      \
        {                                                                     \
            Obj *ob = QR[in->r[0]];                                          \
            uint64_t i = (i_);                                                \
            if (!ob) GENERIC();                                               \
            if (ob->kind == OBJ_LIST) {                                       \
                ObjList *l = (ObjList *)ob;                                   \
                if (i >= l->len) GENERIC();                                   \
                l->tags[i] = in->type;                                        \
                memcpy(&l->items[i], &in->ops[1].kw, 8);                      \
            } else if (ob->kind == OBJ_ARRAY) {                               \
                ObjArray *a = (ObjArray *)ob;                                 \
                if (i >= a->len || a->elem != in->type) GENERIC();            \
                memcpy(&a->data[i], &in->ops[1].kw, 8);                       \
            } else {                                                          \
                GENERIC();                                                    \
            }                                                                 \
            NEXT;                                                             \
        }
        QCASE(Q_STEC) ELEM_STORE_K((uint64_t)QI[in->r[2]] + (uint64_t)in->k.i)
        QCASE(Q_STECK) ELEM_STORE_K((uint64_t)in->target)
#undef ELEM_STORE_K
        QCASE(Q_STGK) { /* mov @g, constant */
            uint32_t g = (uint32_t)((uint64_t)in->k.i >> 32), off = (uint32_t)in->k.i;
            if (g >= QPRC->nglobals) GENERIC();
            Segment *s = &QPRC->globals[g];
            if (off + 8 > s->size) GENERIC();
            memcpy(s->data + off, &in->ops[1].kw, 8);
            s->tags[off >> 3] = in->type;
            NEXT;
        }
#undef ELEM_LOAD
#undef ELEM_STORE
#undef ELEM_BYTE_I
#undef ELEM_BYTE_F
#undef ELEM_BYTE_R
#undef GC_WRITE_I
#undef GC_WRITE_F
#undef GC_WRITE_R
#undef STRUCT_OF
#undef IS_STRUCT
        QCASE(Q_CALLI_R) {
            const Operand *qo = in->ops;
            Obj *ob = QR[in->r[0]];
            if (!ob || ob->kind != OBJ_CLOSURE) GENERIC();
            ObjClosure *cl = (ObjClosure *)ob;
            Function *fn = cl->fn;
            const MFunc *mf = fn->mf;
            int argc = (int)qo[1].imm, nres = in->nops - 2 - argc;
            if (argc != mf->nparams || nres > mf->nrets || (nres && !(mf->flags & MF_RETS_DECLARED))) GENERIC();
            for (int k = 0; k < argc; k++)
                if (!wide_type(mf->params[k]) || REG_BANK(qo[2 + k].reg) != (int)type_bank((MlwsType)mf->params[k])) GENERIC();
            for (int k = 0; k < nres; k++)
                if (!wide_type(mf->rets[k]) || REG_BANK(qo[2 + argc + k].reg) != (int)type_bank((MlwsType)mf->rets[k])) GENERIC();
            plan_saw(in, fn);
            SPILL();
            QPUSH_FRAME(fn, in);
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            const Frame *of = nf - 1;
            nf->closure = &cl->h;
            nf->qret = 1;
            /* the next parameter slot of each bank in the callee's window
             * (scalars: an array here costs a stack frame and a stack-protector
             * check per call) */
            uint64_t *nw = QPRC->regs + nf->base;
            const uint64_t *ow = QPRC->regs + of->base;
            uint32_t ni = 0, nfl = fn->roff[BANK_F], nr = fn->roff[BANK_R], np = fn->roff[BANK_P];
            for (int k = 0; k < argc; k++) {
                const Operand *a = &qo[2 + k];
                uint32_t d;
                switch (REG_BANK(a->reg)) {
                case BANK_I: d = ni++; break;
                case BANK_F: d = nfl++; break;
                case BANK_R: d = nr++; break;
                default: d = np++; break;
                }
                memcpy(&nw[d], &ow[a->pad], 8);
            }
            ctx_set(QCTX, nf, fn, nw);
            RELOAD();
            NEXT;
        }
        QCASE(Q_TCALLI_R) {
            /* A closure tail call: the callee's frame replaces this one, at
             * the same base, and returns to this frame's caller. */
            const Operand *qo = in->ops;
            Obj *ob = QR[in->r[0]];
            if (!ob || ob->kind != OBJ_CLOSURE) GENERIC();
            ObjClosure *cl = (ObjClosure *)ob;
            Function *fn = cl->fn;
            const MFunc *mf = fn->mf;
            int argc = (int)qo[1].imm;
            if (argc != mf->nparams || QFRM->base + fn->rsize > QPRC->rcap) GENERIC();
            for (int k = 0; k < argc; k++)
                if (!wide_type(mf->params[k]) || REG_BANK(qo[2 + k].reg) != (int)type_bank((MlwsType)mf->params[k])) GENERIC();
            plan_saw(in, fn);
            SPILL();
            /* The arguments, in order, before the callee's window replaces this one. */
            uint64_t args[MAX_OPERANDS];
            const uint64_t *ow = QPRC->regs + QFRM->base;
            for (int k = 0; k < argc; k++) memcpy(&args[k], &ow[qo[2 + k].pad], 8);
            const Insn *ri = QFRM->ret_insn;
            uint8_t qret = tail_qret(ri, QFRM->qret, mf);
            proc_pop_frame(QPRC);
            proc_push_frame_fitting(QPRC, fn, ri);
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            nf->closure = &cl->h;
            nf->qret = qret;
            uint64_t *nw = QPRC->regs + nf->base;
            uint32_t ni = 0, nfl = fn->roff[BANK_F], nr = fn->roff[BANK_R], np = fn->roff[BANK_P];
            for (int k = 0; k < argc; k++) {
                uint32_t d;
                switch (REG_BANK(qo[2 + k].reg)) {
                case BANK_I: d = ni++; break;
                case BANK_F: d = nfl++; break;
                case BANK_R: d = nr++; break;
                default: d = np++; break;
                }
                memcpy(&nw[d], &args[k], 8);
            }
            ctx_set(QCTX, nf, fn, nw);
            RELOAD();
            NEXT;
        }
        QCASE(Q_TCALL_R) {
            /* A direct tail call, as Q_TCALLI_R: the callee's frame replaces
             * this one. A constant argument's word is its operand's kw. */
            const Operand *qo = in->ops;
            Function *fn = qo[0].rv.ptr;
            const MFunc *mf = fn->mf;
            int argc = (int)qo[1].imm;
            if (QFRM->base + fn->rsize > QPRC->rcap) GENERIC();
            SPILL();
            uint64_t args[MAX_OPERANDS];
            const uint64_t *ow = QPRC->regs + QFRM->base;
            for (int k = 0; k < argc; k++) memcpy(&args[k], qo[2 + k].mode == OM_REG ? &ow[qo[2 + k].pad] : &qo[2 + k].kw, 8);
            const Insn *ri = QFRM->ret_insn;
            uint8_t qret = tail_qret(ri, QFRM->qret, mf);
            proc_pop_frame(QPRC);
            proc_push_frame_fitting(QPRC, fn, ri);
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            nf->qret = qret;
            uint64_t *nw = QPRC->regs + nf->base;
            uint32_t ni = 0, nfl = fn->roff[BANK_F], nr = fn->roff[BANK_R], np = fn->roff[BANK_P];
            for (int k = 0; k < argc; k++) {
                uint32_t d;
                switch (type_bank((MlwsType)mf->params[k])) {
                case BANK_I: d = ni++; break;
                case BANK_F: d = nfl++; break;
                case BANK_R: d = nr++; break;
                default: d = np++; break;
                }
                memcpy(&nw[d], &args[k], 8);
            }
            ctx_set(QCTX, nf, fn, nw);
            RELOAD();
            NEXT;
        }
        /* Allocation: a collection may run, so the frame's pc is saved first
         * (as for any instruction that can fail). */
        QCASE(Q_NEW) {
            SPILL();
            QR[in->r[0]] = &struct_new(QPRC, in->k.p)->h;
            NEXT;
        }
        QCASE(Q_LNEW) {
            SPILL();
            QR[in->r[0]] = &list_new(QPRC, in->k.i)->h;
            NEXT;
        }
        QCASE(Q_JTAB) { /* out of range: on to the next instruction */
            uint64_t i = (uint64_t)IR(0);
            if (i < (uint64_t)in->nops - 1) JUMPTO(in->ops[1 + i].rv.target);
            NEXT;
        }
        QCASE(Q_CVT_FI) FR(0) = (double)IR(1); NEXT;
        /* ldv I0, F0, R1[i]: an i64 or f64 element of a list (by its tag, or
         * a never-written one, as 0) or of an array (by its element type);
         * the other destination gets zero. */
#define LDV_ELEM(i_)                                                          \
        {                                                                     \
            const Obj *ob = QR[in->k.i];                                      \
            uint64_t i = (i_);                                                \
            if (!ob) GENERIC();                                               \
            uint8_t tag;                                                      \
            const Word *w;                                                    \
            if (ob->kind == OBJ_LIST) {                                       \
                const ObjList *l = (const ObjList *)ob;                       \
                if (i >= l->len) GENERIC();                                   \
                tag = l->tags[i], w = &l->items[i];                           \
            } else if (ob->kind == OBJ_ARRAY) {                               \
                const ObjArray *a = (const ObjArray *)ob;                     \
                if (i >= a->len) GENERIC();                                   \
                tag = a->elem, w = &a->data[i];                               \
            } else {                                                          \
                GENERIC();                                                    \
            }                                                                 \
            if (tag == T_I64) IR(0) = w->i, FR(1) = 0.0;                      \
            else if (tag == T_F64) IR(0) = 0, FR(1) = w->f;                   \
            else if (tag == T_NONE) IR(0) = 0, FR(1) = 0.0;                   \
            else GENERIC();                                                   \
            NEXT;                                                             \
        }
        QCASE(Q_LDV_E) LDV_ELEM((uint64_t)QI[in->target])
        QCASE(Q_LDV_ED) LDV_ELEM((uint64_t)QI[in->target] + (uint64_t)(int64_t)(int8_t)in->r[2])
        QCASE(Q_LDV_EK) LDV_ELEM((uint64_t)in->target)
#undef LDV_ELEM
#define X(N, op_)                                                             \
        QCASE(Q_##N##_UU) if ((uint64_t)IR(0) op_ (uint64_t)IR(1)) JUMP(); NEXT;             \
        QCASE(Q_##N##_UK) if ((uint64_t)IR(0) op_ (uint64_t)in->k.i) JUMP(); NEXT;
        QUBRANCH_OPS(X)
#undef X
#define X(N, e)                                                               \
        QCASE(Q_##N##_III) { int64_t a = IR(1), n = IR(2); IR(0) = (int64_t)(e); NEXT; } \
        QCASE(Q_##N##_IIK) { int64_t a = IR(1), n = in->k.i; IR(0) = (int64_t)(e); NEXT; } \
        QCASE(Q_##N##_II) { int64_t a = IR(0), n = IR(1); IR(0) = (int64_t)(e); NEXT; } \
        QCASE(Q_##N##_IK) { int64_t a = IR(0), n = in->k.i; IR(0) = (int64_t)(e); NEXT; }
        QSHIFT_OPS(X)
#undef X
        /* ---- M24: the shapes of dynamically typed languages' code ---- */
        QCASE(Q_MOV_RK) QR[in->r[0]] = (Obj *)in->k.p; NEXT;
        QCASE(Q_BEQ_RR) if (QR[in->r[0]] == QR[in->r[1]]) JUMP(); NEXT;
        QCASE(Q_BNE_RR) if (QR[in->r[0]] != QR[in->r[1]]) JUMP(); NEXT;
        QCASE(Q_BEQ_RK) if (QR[in->r[0]] == (Obj *)in->k.p) JUMP(); NEXT;
        QCASE(Q_BNE_RK) if (QR[in->r[0]] != (Obj *)in->k.p) JUMP(); NEXT;
        QCASE(Q_SEQ_RR) QI[in->r[0]] = QR[in->r[1]] == QR[in->r[2]]; NEXT;
        QCASE(Q_SNE_RR) QI[in->r[0]] = QR[in->r[1]] != QR[in->r[2]]; NEXT;
        QCASE(Q_SEQ_RK) QI[in->r[0]] = QR[in->r[1]] == (Obj *)in->k.p; NEXT;
        QCASE(Q_SNE_RK) QI[in->r[0]] = QR[in->r[1]] != (Obj *)in->k.p; NEXT;
#define X(N, op_)                                                             \
        QCASE(Q_##N##_FF) QI[in->r[0]] = QF[in->r[1]] op_ QF[in->r[2]]; NEXT;     \
        QCASE(Q_##N##_FK) QI[in->r[0]] = QF[in->r[1]] op_ in->k.f; NEXT;
        QSET_OPS(X)
#undef X
#define X(N, op_)                                                             \
        QCASE(Q_##N##_FFL) if (QF[in->r[0]] op_ QF[in->r[1]]) JUMP(); NEXT; \
        QCASE(Q_##N##_FKL) if (QF[in->r[0]] op_ in->k.f) JUMP(); NEXT;
        QBRANCH_OPS(X)
#undef X
        QCASE(Q_LEN) {
            const Obj *ob = QR[in->r[1]];
            if (!ob) GENERIC();
            switch (ob->kind) {
            case OBJ_LIST: QI[in->r[0]] = ((const ObjList *)ob)->len; break;
            case OBJ_STRING: QI[in->r[0]] = ((const ObjString *)ob)->len; break;
            case OBJ_MAP: QI[in->r[0]] = ((const ObjMap *)ob)->count; break;
            case OBJ_ARRAY: QI[in->r[0]] = ((const ObjArray *)ob)->len; break;
            case OBJ_BYTES: QI[in->r[0]] = ((const ObjBytes *)ob)->len; break;
            default: GENERIC();
            }
            NEXT;
        }
        QCASE(Q_LPUSH_R) { /* grows (allocates) only on the generic path */
            Obj *ob = QR[in->r[0]];
            if (!ob || ob->kind != OBJ_LIST) GENERIC();
            ObjList *l = (ObjList *)ob;
            uint32_t n = (uint32_t)in->nops - 1;
            if (l->cap - l->len < n) GENERIC();
            const Operand *qo = in->ops;
            bool young = false;
            for (uint32_t k = 1; k <= n; k++, l->len++) {
                uint8_t r = qo[k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: l->items[l->len].i = QI[REG_IDX(r)]; l->tags[l->len] = T_I64; break;
                case BANK_F: l->items[l->len].f = QF[REG_IDX(r)]; l->tags[l->len] = T_F64; break;
                default:
                    l->items[l->len].r = QR[REG_IDX(r)], l->tags[l->len] = T_R;
                    young |= gc_young(QR[REG_IDX(r)]);
                    break;
                }
            }
            if (__builtin_expect(young && (ob->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD, 0)) GC_REMEMBER(R0, ob);
            NEXT;
        }
        QCASE(Q_LPUSH_RK) { /* the same, with constants (interp_quicken; a constant is never young) */
            Obj *ob = QR[in->r[0]];
            if (!ob || ob->kind != OBJ_LIST) GENERIC();
            ObjList *l = (ObjList *)ob;
            uint32_t n = (uint32_t)in->nops - 1;
            if (l->cap - l->len < n) GENERIC();
            const Operand *qo = in->ops;
            bool young = false;
            for (uint32_t k = 1; k <= n; k++, l->len++) {
                const Operand *a = &qo[k];
                if (a->mode != OM_REG) {
                    memcpy(&l->items[l->len], &a->kw, 8);
                    l->tags[l->len] = a->pad;
                    continue;
                }
                uint8_t r = a->reg;
                switch (REG_BANK(r)) {
                case BANK_I: l->items[l->len].i = QI[REG_IDX(r)]; l->tags[l->len] = T_I64; break;
                case BANK_F: l->items[l->len].f = QF[REG_IDX(r)]; l->tags[l->len] = T_F64; break;
                default:
                    l->items[l->len].r = QR[REG_IDX(r)], l->tags[l->len] = T_R;
                    young |= gc_young(QR[REG_IDX(r)]);
                    break;
                }
            }
            if (__builtin_expect(young && (ob->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD, 0)) GC_REMEMBER(R0, ob);
            NEXT;
        }
#define X(N, f)                                                               \
        QCASE(Q_##N##_F) FR(0) = f(FR(0)); NEXT;                                \
        QCASE(Q_##N##_FF) FR(0) = f(FR(1)); NEXT;
        QFUNARY_OPS(X)
#undef X
        QCASE(Q_TAGOF_E) {
            const Obj *ob = QR[in->r[1]];
            uint64_t i = (uint64_t)QI[in->r[2]];
            if (ob && ob->kind == OBJ_LIST && i < ((const ObjList *)ob)->len)
                QI[in->r[0]] = ((const ObjList *)ob)->tags[i];
            else if (ob && ob->kind == OBJ_ARRAY && i < ((const ObjArray *)ob)->len)
                QI[in->r[0]] = ((const ObjArray *)ob)->elem;
            else
                GENERIC();
            NEXT;
        }
        QCASE(Q_TYPEOF) {
            const Obj *ob = QR[in->r[1]];
            IR(0) = ob ? ob->kind : OBJ_NULL;
            NEXT;
        }
        QCASE(Q_BITCAST_FI) memcpy(&FR(0), &IR(1), 8); NEXT;
        QCASE(Q_BITCAST_IF) memcpy(&IR(0), &FR(1), 8); NEXT;
/* R0[i] (i in r[1..2]) against the constant in k: a list element tagged r,
 * or an element of an r array; anything else goes to the generic branch. */
#define ELEM_REF_BRANCH(cmp)                                                  \
        {                                                                     \
            const Obj *ob = QR[in->r[0]];                                     \
            uint32_t i = in->r[1] | (uint32_t)in->r[2] << 8;                  \
            const Obj *v;                                                     \
            if (!ob) GENERIC();                                               \
            if (ob->kind == OBJ_LIST) {                                       \
                const ObjList *l = (const ObjList *)ob;                       \
                if (i >= l->len || l->tags[i] != T_R) GENERIC();              \
                v = l->items[i].r;                                            \
            } else if (ob->kind == OBJ_ARRAY) {                               \
                const ObjArray *a = (const ObjArray *)ob;                     \
                if (i >= a->len || a->elem != T_R) GENERIC();                 \
                v = a->data[i].r;                                             \
            } else {                                                          \
                GENERIC();                                                    \
            }                                                                 \
            if (v cmp (const Obj *)in->k.p) JUMP();                           \
            NEXT;                                                             \
        }
        QCASE(Q_BEQ_EKK) ELEM_REF_BRANCH(==)
        QCASE(Q_BNE_EKK) ELEM_REF_BRANCH(!=)
#undef ELEM_REF_BRANCH
        QCASE(Q_ISA) {
            const Obj *ob = QR[in->r[1]];
            QI[in->r[0]] = ob && ob->kind == OBJ_STRUCT && ((const ObjStruct *)ob)->def == in->k.p;
            NEXT;
        }
        QCASE(Q_BISA) {
            const Obj *ob = QR[in->r[0]];
            if (ob && ob->kind == OBJ_STRUCT && ((const ObjStruct *)ob)->def == in->k.p) JUMP();
            NEXT;
        }
        QCASE(Q_BNISA) {
            const Obj *ob = QR[in->r[0]];
            if (!(ob && ob->kind == OBJ_STRUCT && ((const ObjStruct *)ob)->def == in->k.p)) JUMP();
            NEXT;
        }
        /* A global reference word: segment and offset in k. Anything but a word
         * tagged r (never written, another type, a module loaded later) is the
         * generic path's. */
#define GLOBAL_REF(dst)                                                       \
        {                                                                     \
            uint32_t g_ = (uint32_t)((uint64_t)in->k.i >> 32), off_ = (uint32_t)in->k.i; \
            if (g_ >= QPRC->nglobals) GENERIC();                              \
            const Segment *s_ = &QPRC->globals[g_];                              \
            if (off_ + 8 > s_->size || s_->tags[off_ >> 3] != T_R) GENERIC(); \
            memcpy(&(dst), s_->data + off_, sizeof(Obj *));                   \
        }
        QCASE(Q_BEQ_RG) {
            Obj *v;
            GLOBAL_REF(v);
            if (QR[in->r[0]] == v) JUMP();
            NEXT;
        }
        QCASE(Q_BNE_RG) {
            Obj *v;
            GLOBAL_REF(v);
            if (QR[in->r[0]] != v) JUMP();
            NEXT;
        }
#undef GLOBAL_REF
        QCASE(Q_MGET_R) { /* mget.r R0, R1, "key" [, null] */
            const Obj *ob = QR[in->r[1]];
            if (!ob || ob->kind != OBJ_MAP) GENERIC();
            Word key;
            key.r = (Obj *)in->k.p;
            const MapEntry *e = map_find((ObjMap *)ob, key, T_R);
            if (e) {
                if (e->vtag != T_R) GENERIC();
                QR[in->r[0]] = e->val.r;
            } else {
                if (in->nops < 4) GENERIC(); /* no default: E_KEY */
                QR[in->r[0]] = NULL;
            }
            NEXT;
        }
        /* mget I0, R1, R2 [, k]: a key in any register (its type in target),
         * a value of exactly the bank's 64-bit type, or the constant default
         * (in k). */
#define X(B, bank, w, tag)                                                    \
        QCASE(Q_MGETV_##B) {                                                  \
            const Obj *ob = QR[in->r[1]];                                     \
            if (!ob || ob->kind != OBJ_MAP) GENERIC();                        \
            Word key;                                                         \
            if (in->target == T_I64) key.i = QI[in->r[2]];                    \
            else if (in->target == T_F64) key.f = QF[in->r[2]];               \
            else key.r = QR[in->r[2]];                                        \
            const MapEntry *e = map_find((ObjMap *)ob, key, (uint8_t)in->target); \
            if (e) {                                                          \
                if (e->vtag != (tag)) GENERIC();                              \
                QB(bank)[in->r[0]] = e->val.w;                                \
            } else {                                                          \
                if (in->nops < 4) GENERIC(); /* no default: E_KEY */          \
                memcpy(&QB(bank)[in->r[0]], &in->k, 8);                       \
            }                                                                 \
            NEXT;                                                             \
        }
        X(I, I, i, T_I64) X(F, F, f, T_F64) X(R, R, r, T_R)
#undef X
        /* Calls and rets with constants among their values: Q_CALL_R and
         * Q_RET_R with each constant converted at load time (Operand.kw). */
        QCASE(Q_CALL_RK) {
            SPILL();
            const QPlan *pl = in->k.p;
            Function *fn = pl->fn;
            int argc = pl->argc;
            QPUSH_FRAME(fn, in);
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            const Frame *of = nf - 1;
            nf->qret = 1;
            uint64_t *nw = QPRC->regs + nf->base;
            const uint64_t *ow = QPRC->regs + of->base;
            for (int k = 0; k < argc; k++) {
                uint8_t s = pl->slot[k];
                memcpy(&nw[pl->slot[argc + k]], s != QPLAN_K ? &ow[s] : &in->ops[2 + k].kw, 8);
            }
            ctx_set(QCTX, nf, fn, nw);
            RELOAD();
            NEXT;
        }
        QCASE(Q_RET_RK) {
            if (!QFRM->qret) GENERIC();
            SPILL();
            const Insn *ri = QFRM->ret_insn;
            const QPlan *cp = ri->k.p, *rp = in->k.p;
            int nres = cp->nres;
            const uint8_t *rs = cp->slot + 2 * cp->argc;
            const Frame *of = QFRM - 1;
            uint64_t *cw = QPRC->regs + of->base;
            const uint64_t *fw = QPRC->regs + QFRM->base;
            for (int k = 0; k < nres; k++) {
                uint8_t s = rp->slot[k];
                memcpy(&cw[rs[k]], s != QPLAN_K ? &fw[s] : &in->ops[k].kw, 8);
            }
            proc_pop_frame(QPRC);
            ctx_set(QCTX, (Frame *)of, of->fn, cw);
            RELOAD();
            NEXT;
        }
        /* A closure call with constants: checked against the callee per call,
         * out of line (call_rk). */
        QCASE(Q_CALLI_RK) {
            SPILL();
            if (!call_rk(QPRC, in)) GENERIC();
            ctx_load(QCTX);
            RELOAD();
            NEXT;
        }
