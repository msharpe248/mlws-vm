/* types.h - the MLWSVM value types, register banks and 64-bit machine word.
 *
 * Every instruction carries a type byte (an MlwsType). Integer-class types live
 * in the I bank, floats in F, references in R and raw pointers in P. Values are
 * kept canonical in 64-bit registers: signed types are sign-extended, unsigned
 * types (and bool/char) zero-extended, f32 values are rounded to float. */
#ifndef MLWSVM_TYPES_H
#define MLWSVM_TYPES_H

#include "common.h"

typedef enum {
    T_NONE = 0, /* untyped / "natural" type of each operand */
    T_B,        /* bool: 0 or 1 */
    T_C,        /* char: Unicode scalar value (u32) */
    T_I8,
    T_I16,
    T_I32,
    T_I64,
    T_U8,
    T_U16,
    T_U32,
    T_U64,
    T_F32,
    T_F64,
    T_R,   /* GC reference */
    T_P,   /* raw (checked) pointer */
    T_BIG, /* arbitrary precision integer (reference, reserved for M8) */
    T_DEC, /* decimal (reference, reserved for M8) */
    T__COUNT
} MlwsType;

/* Memory word tag for untyped bytes (strings, unaligned or partial writes).
 * Tags are otherwise MlwsType values; T_NONE marks a never-written word. */
#define TAG_RAW 0xFF

#define TM(t) (1u << (t))
#define TS_INT                                                                  \
    (TM(T_B) | TM(T_C) | TM(T_I8) | TM(T_I16) | TM(T_I32) | TM(T_I64) |         \
     TM(T_U8) | TM(T_U16) | TM(T_U32) | TM(T_U64))
#define TS_FLT (TM(T_F32) | TM(T_F64))
#define TS_NUM (TS_INT | TS_FLT)
#define TS_REF TM(T_R)
#define TS_PTR TM(T_P)
#define TS_ORD (TS_NUM | TS_PTR)
#define TS_ANY (TS_NUM | TS_REF | TS_PTR)

typedef enum { BANK_I = 0, BANK_F = 1, BANK_R = 2, BANK_P = 3, BANK_S = 4 } Bank;
#define NBANKS 4  /* banks that are allocated per call frame */
#define NREGS 32  /* registers per bank */

/* Special registers (bank S). */
enum { SREG_SP = 0, SREG_FP = 1, SREG_PID = 2, SREG__COUNT };

/* Register operand byte: bank in the top 3 bits, index in the low 5. */
#define REG_MAKE(bank, idx) ((uint8_t)(((bank) << 5) | ((idx) & 31)))
#define REG_BANK(r) ((r) >> 5)
#define REG_IDX(r) ((r) & 31)

typedef union {
    int64_t i;
    uint64_t u;
    double f;
    void *r;
    uint64_t p;
} Word;

const char *type_name(MlwsType t);           /* "i64", "f32", "r", ... */
bool type_from_name(const char *s, size_t n, MlwsType *out); /* accepts aliases */
int type_size(MlwsType t);                   /* bytes in memory */
Bank type_bank(MlwsType t);                  /* register bank for the type */
bool type_is_int(MlwsType t);
bool type_is_float(MlwsType t);
bool type_is_signed(MlwsType t);
int type_bits(MlwsType t);
MlwsType bank_natural_type(Bank b);          /* I->i64, F->f64, R->r, P->p */
const char *bank_prefix(Bank b);           /* "I", "F", "R", "P" */
int64_t canon_int(MlwsType t, int64_t v);   /* wrap/extend to t */
double canon_float(MlwsType t, double v);
/* Register name for a register operand byte, e.g. "I3" or "SP". */
const char *reg_name(uint8_t reg, char buf[8]);
/* Natural type of a register operand (I->i64, SP/FP->p, PID->i64). */
MlwsType reg_natural_type(uint8_t reg);

#endif
