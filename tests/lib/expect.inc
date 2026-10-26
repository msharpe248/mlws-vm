; expect.inc - EXPECT_ERROR for the standard library tests.

; Run one instruction and check that it raises an Error with this code (a
; string literal). Uses R31 and I31.
.macro EXPECT_ERROR code, insn...
.try .Lh\@
    \insn
.endtry
    FAIL "an error was expected"
.Lh\@:
    catch R31
    strcmp I31, R31->Error.code, \code
    ASSERT_EQ I31, 0
.endm
