/* numeric.h - arbitrary-precision integers (BigInt, `.big`) and decimals
 * (`.dec`) as immutable heap objects. Values live in R registers; the
 * instruction's type suffix selects the arithmetic. */
#ifndef MLWSVM_NUMERIC_H
#define MLWSVM_NUMERIC_H

#include "../vm/vm.h"

/* value = sign * magnitude, magnitude little-endian base 2^32, no leading zeros */
typedef struct {
    Obj h;
    int32_t sign; /* -1, 0, +1 */
    uint32_t n;   /* limbs */
    uint32_t d[];
} ObjBig;

/* value = sign * coefficient * 10^exp (the scale is -exp, as in Java's BigDecimal) */
typedef struct {
    Obj h;
    int32_t sign;
    int32_t exp;
    uint32_t n;
    uint32_t d[];
} ObjDec;

#define DEC_DIV_DIGITS 28 /* fractional digits kept by an inexact division */

bool num_kind_matches(const Obj *o, MlwsType t); /* OBJ_BIG for .big, OBJ_DEC for .dec */
Obj *num_from_i64(Process *p, MlwsType t, int64_t v, bool is_unsigned);
Obj *num_from_f64(Process *p, MlwsType t, double v);       /* traps E_OVERFLOW for inf/nan */
Obj *num_convert(Process *p, MlwsType to, const Obj *from); /* big <-> dec (dec->big truncates) */
Obj *num_arith(Process *p, int op, const Obj *a, const Obj *b); /* OP_ADD ... OP_MAX; same kinds */
Obj *num_unary(Process *p, int op, const Obj *a);           /* OP_NEG, OP_ABS */
int num_cmp(const Obj *a, const Obj *b);                    /* same kinds; -1, 0, 1 */
bool num_to_int(const Obj *a, MlwsType to, Word *out);      /* false if out of range (truncates decimals) */
double num_to_f64(const Obj *a);
void num_format(StrBuf *b, const Obj *a);
Obj *num_parse(Process *p, MlwsType t, const char *s, size_t n); /* NULL if malformed */
/* Constant-pool literal (immortal, not on any heap); NULL if malformed. */
Obj *num_const(MlwsType t, const char *s, size_t n);
uint32_t num_hash(const Obj *a);
bool num_equal(const Obj *a, const Obj *b);
size_t num_footprint(const Obj *a);

#endif
