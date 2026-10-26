; Helper include for tests/asm/include.masm. It defines a macro, so a second
; inclusion would fail if files were not included only once.
.const ANSWER = 42
.macro LOAD_ANSWER r
    mov \r, ANSWER
.endm
