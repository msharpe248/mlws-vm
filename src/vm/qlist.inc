/* qlist.inc - every quickened form with a handler in qforms.inc, as QL(form):
 * the entries of the dispatch table, for whichever QL the includer defines. */
        QL(Q_MOV_II) QL(Q_MOV_IK) QL(Q_MOV_FF) QL(Q_MOV_FK) QL(Q_MOV_RR) QL(Q_MOV_PP)
#define X(N, o) QL(Q_##N##_III) QL(Q_##N##_IIK) QL(Q_##N##_II) QL(Q_##N##_IK)
        QARITH_OPS(X)
#undef X
#define X(N, o) QL(Q_F##N##_FFF) QL(Q_F##N##_FFK) QL(Q_F##N##_FF) QL(Q_F##N##_FK) QL(Q_F##N##_FKF)
        QFARITH_OPS(X)
#undef X
#define X(N, o) QL(Q_##N##_II) QL(Q_##N##_IK)
        QBRANCH_OPS(X)
#undef X
        QL(Q_REM_IIK) QL(Q_REM_IK) QL(Q_DIV_IIK) QL(Q_DIV_IK)
        QL(Q_INC_I) QL(Q_DEC_I) QL(Q_BZ_I) QL(Q_BNZ_I) QL(Q_JMP) QL(Q_CALL_R) QL(Q_RET_R)
        QL(Q_BZ_R) QL(Q_BNZ_R) QL(Q_CALLI_R)
        QL(Q_LDF_I) QL(Q_LDF_F) QL(Q_LDF_R) QL(Q_STF_I) QL(Q_STF_F) QL(Q_STF_R)
        QL(Q_LDE_I) QL(Q_LDE_F) QL(Q_LDE_R) QL(Q_STE_I) QL(Q_STE_F) QL(Q_STE_R)
        QL(Q_CALL_RK) QL(Q_CALLI_RK) QL(Q_RET_RK) QL(Q_MOV_RK)
        QL(Q_LDG_I) QL(Q_LDG_F) QL(Q_LDG_R) QL(Q_STG_I) QL(Q_STG_F) QL(Q_STG_R)
        QL(Q_LDEK_I) QL(Q_LDEK_F) QL(Q_LDEK_R) QL(Q_STEK_I) QL(Q_STEK_F) QL(Q_STEK_R)
        QL(Q_LDU_I) QL(Q_LDU_F) QL(Q_LDU_R) QL(Q_STU_I) QL(Q_STU_F) QL(Q_STU_R)
        QL(Q_LEN) QL(Q_TAGOF_E) QL(Q_LPUSH_R) QL(Q_TCALLI_R) QL(Q_LDV_E) QL(Q_LDV_EK)
        QL(Q_NEW) QL(Q_LNEW) QL(Q_STFK) QL(Q_MGETV_I) QL(Q_MGETV_F) QL(Q_MGETV_R) QL(Q_JTAB) QL(Q_CVT_FI)
#define X(N, o) QL(Q_##N##_UU) QL(Q_##N##_UK)
        QUBRANCH_OPS(X)
#undef X
#define X(N, e) QL(Q_##N##_III) QL(Q_##N##_IIK) QL(Q_##N##_II) QL(Q_##N##_IK)
        QSHIFT_OPS(X)
#undef X
#define X(N, f) QL(Q_##N##_F) QL(Q_##N##_FF)
        QFUNARY_OPS(X)
#undef X
        QL(Q_BEQ_RR) QL(Q_BNE_RR) QL(Q_BEQ_RK) QL(Q_BNE_RK) QL(Q_BEQ_RG) QL(Q_BNE_RG)
#define X(N, o) QL(Q_##N##_FF) QL(Q_##N##_FK)
        QSET_OPS(X)
#undef X
#define X(N, o) QL(Q_##N##_FFL) QL(Q_##N##_FKL)
        QBRANCH_OPS(X)
#undef X
        QL(Q_SEQ_RR) QL(Q_SNE_RR) QL(Q_SEQ_RK) QL(Q_SNE_RK) QL(Q_ISA) QL(Q_MGET_R)
