/* qforms.inc - the quickened forms' handlers (see interp_quicken in interp.c),
 * written once and compiled either as the cases of interp.c's dispatch loop or
 * as one function per handler that tail-calls the next one. The handlers use
 * this vocabulary, which each includer defines:
 *
 *   QCASE(x)        start the handler for quickened form x
 *   QCASE2(x, y)    start one handler for the two forms x and y
 *   NEXT            go on with the next instruction
 *   JUMP()          make the next instruction the branch target (in->target)
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
        QCASE(Q_F##N##_FK) FR(0) = FR(0) op_ in->k.f; NEXT;
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
            const Operand *qo = in->ops;
            Function *fn = qo[0].rv.ptr;
            int argc = (int)qo[1].imm;
            QPUSH_FRAME(fn, in); /* may move the frames and the banks */
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            const Frame *of = nf - 1;
            nf->qret = 1;
            for (int k = 0; k < argc; k++) { /* caller register -> callee parameter, same bank */
                uint8_t r = qo[2 + k].reg, d = qo[2 + k].ireg;
                switch (REG_BANK(r)) {
                case BANK_I: QPRC->ireg[nf->base[BANK_I] + d] = QPRC->ireg[of->base[BANK_I] + REG_IDX(r)]; break;
                case BANK_F: QPRC->freg[nf->base[BANK_F] + d] = QPRC->freg[of->base[BANK_F] + REG_IDX(r)]; break;
                case BANK_R: QPRC->rreg[nf->base[BANK_R] + d] = QPRC->rreg[of->base[BANK_R] + REG_IDX(r)]; break;
                default: QPRC->preg[nf->base[BANK_P] + d] = QPRC->preg[of->base[BANK_P] + REG_IDX(r)]; break;
                }
            }
            ctx_load(QCTX);
            RELOAD();
            NEXT;
        }
        QCASE(Q_RET_R) {
            SPILL();
            const Operand *qo = in->ops;
            const Insn *ri = QFRM->ret_insn;
            if (QFRM->qret) {
                /* Back to a quickened call, whose result registers match this
                 * function's declared result types (and so the registers this
                 * ret names): copy each one straight across, then pop. */
                int argc = (int)ri->ops[1].imm, nres = ri->nops - 2 - argc;
                const Frame *of = QFRM - 1;
                for (int k = 0; k < nres; k++) {
                    uint8_t sr = qo[k].reg, dr = ri->ops[2 + argc + k].reg;
                    switch (REG_BANK(sr)) {
                    case BANK_I: QPRC->ireg[of->base[BANK_I] + REG_IDX(dr)] = QI[REG_IDX(sr)]; break;
                    case BANK_F: QPRC->freg[of->base[BANK_F] + REG_IDX(dr)] = QF[REG_IDX(sr)]; break;
                    case BANK_R: QPRC->rreg[of->base[BANK_R] + REG_IDX(dr)] = QR[REG_IDX(sr)]; break;
                    default: QPRC->preg[of->base[BANK_P] + REG_IDX(dr)] = QP[REG_IDX(sr)]; break;
                    }
                }
                proc_pop_frame(QPRC);
                ctx_load(QCTX);
                RELOAD();
                NEXT;
            }
#if !QFORMS_RET_INLINE /* to a generic call or out of the entry function: the generic ret */
            GENERIC();
#else
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
            if (ri->xop == Q_CALL_R) { /* both sides are registers of the right banks */
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
 * tagged with it (a store retags a list element), at index i. */
#define ELEM_LOAD(bank, w, i_)                                                \
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
                if (i >= l->len || l->tags[i] != in->type) GENERIC();      \
                QB(bank)[in->r[0]] = l->items[i].w;                             \
            } else {                                                          \
                GENERIC();                                                 \
            }                                                                 \
            NEXT;                                                             \
        }
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
            NEXT;                                                             \
        }                                                                     \
        QCASE(Q_LDE_##B) ELEM_LOAD(bank, w, (uint64_t)QI[in->r[2]])            \
        QCASE(Q_STE_##B) ELEM_STORE(bank, w, (uint64_t)QI[in->r[2]])           \
        QCASE(Q_LDEK_##B) ELEM_LOAD(bank, w, (uint64_t)in->target)              \
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
#undef ELEM_LOAD
#undef ELEM_STORE
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
            SPILL();
            QPUSH_FRAME(fn, in);
            Frame *nf = &QPRC->frames[QPRC->nframes - 1];
            const Frame *of = nf - 1;
            nf->closure = &cl->h;
            nf->qret = 1;
            /* the next parameter register of each bank (scalars: an array here
             * costs a stack frame and a stack-protector check per call) */
            uint32_t ni = nf->base[BANK_I], nfl = nf->base[BANK_F], nr = nf->base[BANK_R], np = nf->base[BANK_P];
            for (int k = 0; k < argc; k++) {
                uint8_t r = qo[2 + k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: QPRC->ireg[ni++] = QPRC->ireg[of->base[BANK_I] + REG_IDX(r)]; break;
                case BANK_F: QPRC->freg[nfl++] = QPRC->freg[of->base[BANK_F] + REG_IDX(r)]; break;
                case BANK_R: QPRC->rreg[nr++] = QPRC->rreg[of->base[BANK_R] + REG_IDX(r)]; break;
                default: QPRC->preg[np++] = QPRC->preg[of->base[BANK_P] + REG_IDX(r)]; break;
                }
            }
            ctx_load(QCTX);
            RELOAD();
            NEXT;
        }
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
            for (uint32_t k = 1; k <= n; k++, l->len++) {
                uint8_t r = qo[k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: l->items[l->len].i = QI[REG_IDX(r)]; l->tags[l->len] = T_I64; break;
                case BANK_F: l->items[l->len].f = QF[REG_IDX(r)]; l->tags[l->len] = T_F64; break;
                default: l->items[l->len].r = QR[REG_IDX(r)]; l->tags[l->len] = T_R; break;
                }
            }
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
        QCASE(Q_ISA) {
            const Obj *ob = QR[in->r[1]];
            QI[in->r[0]] = ob && ob->kind == OBJ_STRUCT && ((const ObjStruct *)ob)->def == in->k.p;
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
        /* Calls with constants among the arguments, and rets with constants
         * back to a quickened call: out of line (call_rk, ret_rk), since their
         * code in this loop slowed the other call forms. */
        QCASE2(Q_CALL_RK, Q_CALLI_RK) {
            SPILL();
            if (!call_rk(QPRC, in)) GENERIC();
            ctx_load(QCTX);
            RELOAD();
            NEXT;
        }
        QCASE(Q_RET_RK) {
            if (!QFRM->qret) GENERIC();
            SPILL();
            ret_rk(QPRC, in);
            ctx_load(QCTX);
            RELOAD();
            NEXT;
        }
