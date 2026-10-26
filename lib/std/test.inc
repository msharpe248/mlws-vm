; std/test.inc - assertion macros for tests. Include with:  .include "std/test.inc"
;
; A failing assertion traps with E_ASSERT. The message names the operands, and
; the backtrace gives the source line. The optional `type` argument sets the
; comparison type (i32, u64, f64, r, p, ...); when it is omitted, the type is
; inferred from the first register operand, as for any instruction.
;
;   ASSERT_EQ a, b [, type]      a == b
;   ASSERT_NE a, b [, type]      a != b
;   ASSERT_LT a, b [, type]      a <  b
;   ASSERT_LE a, b [, type]      a <= b
;   ASSERT_GT a, b [, type]      a >  b
;   ASSERT_GE a, b [, type]      a >= b
;   ASSERT_TRUE v [, type]       v != 0 / non-null
;   ASSERT_FALSE v [, type]      v == 0 / null
;   FAIL "message"               unconditional failure

.macro _ASSERT_CMP op, a, b, type, name
.ifb \type
    \op \a, \b, .Lassert_ok\@
.else
    \op\().\type \a, \b, .Lassert_ok\@
.endif
    ncall "test.fail" ("\name(\a, \b) failed")
.Lassert_ok\@:
.endm

.macro ASSERT_EQ a, b, type
    _ASSERT_CMP beq, \a, \b, \type, ASSERT_EQ
.endm

.macro ASSERT_NE a, b, type
    _ASSERT_CMP bne, \a, \b, \type, ASSERT_NE
.endm

.macro ASSERT_LT a, b, type
    _ASSERT_CMP blt, \a, \b, \type, ASSERT_LT
.endm

.macro ASSERT_LE a, b, type
    _ASSERT_CMP ble, \a, \b, \type, ASSERT_LE
.endm

.macro ASSERT_GT a, b, type
    _ASSERT_CMP bgt, \a, \b, \type, ASSERT_GT
.endm

.macro ASSERT_GE a, b, type
    _ASSERT_CMP bge, \a, \b, \type, ASSERT_GE
.endm

.macro ASSERT_TRUE v, type
.ifb \type
    bnz \v, .Lassert_ok\@
.else
    bnz.\type \v, .Lassert_ok\@
.endif
    ncall "test.fail" ("ASSERT_TRUE(\v) failed")
.Lassert_ok\@:
.endm

.macro ASSERT_FALSE v, type
.ifb \type
    bz \v, .Lassert_ok\@
.else
    bz.\type \v, .Lassert_ok\@
.endif
    ncall "test.fail" ("ASSERT_FALSE(\v) failed")
.Lassert_ok\@:
.endm

.macro FAIL msg
    ncall "test.fail" (\msg)
.endm
