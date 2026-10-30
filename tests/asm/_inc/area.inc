; A function defined in an included file, for tests/asm/include_lines.masm:
; the fault in `area` is on line 6 of this file.

.func area(ref shape) -> i64
    mov I0, 2
    mov I1, shape[I0]           ; E_BOUNDS for a short list
    ret I1
.endf
