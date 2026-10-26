; std/io.inc - output macros. Include with:  .include "std/io.inc"
;
; Each macro expands to native calls in place, without a function call. The
; operand may be anything readable as the given type: a register, a literal or
; a memory operand.
;
;   PRINT_S   s     PRINTLN_S s      string (ref)
;   PRINT_I   v     PRINTLN_I v      signed integer (i64)
;   PRINT_U   v     PRINTLN_U v      unsigned integer (u64)
;   PRINT_F   v     PRINTLN_F v      float (f64)
;   PRINT_C   c                      character
;   PRINT_B   b     PRINTLN_B b      bool
;   PRINT_HEX v                      integer in hex
;   NL                               newline
;   PRINTLN   s, v...                string, then each value as i64 separated by spaces

.macro PRINT_S s
    ncall "io.print_s" (\s)
.endm

.macro PRINTLN_S s
    ncall "io.print_s" (\s)
    ncall "io.nl" ()
.endm

.macro PRINT_I v
    ncall "io.print_i" (\v)
.endm

.macro PRINTLN_I v
    ncall "io.print_i" (\v)
    ncall "io.nl" ()
.endm

.macro PRINT_U v
    ncall "io.print_u" (\v)
.endm

.macro PRINTLN_U v
    ncall "io.print_u" (\v)
    ncall "io.nl" ()
.endm

.macro PRINT_F v
    ncall "io.print_f" (\v)
.endm

.macro PRINTLN_F v
    ncall "io.print_f" (\v)
    ncall "io.nl" ()
.endm

.macro PRINT_C c
    ncall "io.print_c" (\c)
.endm

.macro PRINT_B b
    ncall "io.print_b" (\b)
.endm

.macro PRINTLN_B b
    ncall "io.print_b" (\b)
    ncall "io.nl" ()
.endm

.macro PRINT_HEX v
    ncall "io.print_hex" (\v)
.endm

.macro NL
    ncall "io.nl" ()
.endm

; PRINTLN "label:", a, b, c   ->   label: a b c
.macro PRINTLN s, vals...
    ncall "io.print_s" (\s)
    .irp v, \vals
    ncall "io.print_c" (' ')
    ncall "io.print_i" (\v)
    .endr
    ncall "io.nl" ()
.endm
