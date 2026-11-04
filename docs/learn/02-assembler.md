# 2. From text to instructions

Before a VM can run anything, text has to become instructions it trusts.
This chapter follows a program from its `.masm` source to the decoded,
checked instructions the interpreter runs: the assembler's preprocessor,
lexer and parser; the one opcode table that every tool reads; the module
and its binary file; loading and linking modules by name; and the verifier,
which checks every instruction once so that the interpreter never has to.
The verifier is why MLWSVM can run a `.mlwso` file from anywhere without
crashing, and why its fastest handlers are one line of C.

## The pipeline

A program takes this path ([DESIGN.md §2](../DESIGN.md#2-the-whole-system)):

```text
 prog.masm ─► preprocessor ─► lexer and parser ─► finish() ─► Module ─► prog.mlwso
                    (src/asm)                                   │
                                                                ▼
             loader: find imports, link names, decode, verify, quicken   (src/vm)
                                                                │
                                                                ▼
                                    Function { Insn[] }  ready to run
```

The *module* is the unit of everything: what the assembler produces, what a
`.mlwso` file holds, what the loader links. The assembler's driver,
`assemble_file_deps` in [src/asm/assembler.c](../../src/asm/assembler.c),
shows the first half. The preprocessor runs over the whole file, then each
line it produced is parsed as one statement, then `finish` resolves names
and encodes the code:

```c
    pp_init(&A.pp, libpaths, nlib, err);
    A.modname = xstrdup(default_name ? default_name : "main");
    Module *m = NULL;
    if (pp_run_file(&A.pp, path)) {
        bool ok = true;
        for (size_t i = 0; ok && i < A.pp.out.n; i++) {
            A.pp.asof = i; /* .set names have the values they had at this line */
            ok = statement(&A, &A.pp.out.v[i]);
        }
        if (ok) m = finish(&A);
    }
```

The example for this chapter uses most of what the assembler does: an
include, a constant, a macro, a global and a numeric label.

```asm
; sum.masm - add up 1..N into a global, with a constant and a macro
.include "std/io.inc"

.const N = 4 * 25               ; a constant expression, evaluated by the assembler

.macro BUMP reg                 ; a macro: one parameter, used as \reg
    inc \reg
.endm

total:  .i64 0                  ; a module global, 8 bytes

.func main
    mov I0, 0                   ; i
1:  BUMP I0
    add @total, I0              ; a memory operand: the global itself
    blt I0, N, 1b
    PRINT_S "total = "
    PRINTLN_I @total
    ret 0
.endf
```

```sh
$ ./mlwsvm run sum.masm
total = 5050
```

## The preprocessor

Assemblers have long had macro preprocessors; MLWSVM's follows GNU `as`
(`\@`, `.rept`, `.irp`, numeric labels `1b` and `1f`). Its job is to turn the
file into a flat list of lines with no macros, includes or conditionals left
in it. [src/asm/preproc.c](../../src/asm/preproc.c) keeps each output
line as a `PLine` ([src/asm/preproc.h](../../src/asm/preproc.h)): its
text, where the text physically came from (`loc`), the line of your file
that expanded it (`origin`), and the innermost macro it came from
(`macro`). `./mlwsvm asm -E` prints that list. Part of it for `sum.masm`,
each line ending with the line of `sum.masm` it came from:

```text
    inc I0                                                   ; 14 via BUMP
    add @total, I0                                           ; 15
    blt I0, N, 1b                                            ; 16
    ncall "io.print_i" (@total)                              ; 18 via PRINTLN_I
    ncall "io.nl" ()                                         ; 18 via PRINTLN_I
```

Each macro call has become its body, with its arguments put in by `subst`:
`\name` becomes the argument, `\@` a number unique to this expansion (for
labels inside macros), and `\()` nothing, to join a parameter to the text
after it. The result is preprocessed again, so macros can use macros, up to
a depth of 64.

Because `origin` and `macro` travel with each line, an error inside a macro
names both places. Calling a macro `TWICE reg` (whose body is
`add \reg, \reg`) with `X9`:

```text
error[E_ASM_UNDEF_SYM]: badmac.masm:2:9: 'X9' is not a register, alias or constant
(labels and functions are only allowed as branch/call targets) (in macro 'TWICE'
expanded at badmac.masm:6)
```

Notice that `N` is still `N` in the `-E` output. A `.const` is not text
substitution, as in C's `#define`: it is a value, evaluated once by
[src/asm/expr.c](../../src/asm/expr.c), and the assembler looks the name up
when it parses `blt`. The evaluator is precedence climbing:
`parse_binary(minprec)` parses one operand, then takes operators whose level
(`||` is 1, `*` is 10) is at least `minprec`, parsing each right-hand side
one level tighter so that operators of a level group to the left:

```c
static bool parse_binary(ExprParser *ep, int minprec, ExprVal *out) {
    ExprVal lhs;
    if (!parse_primary(ep, &lhs)) return false;
    for (;;) {
        const Token *op = peek(ep);
        int prec = binprec(op);
        if (prec < 0 || prec < minprec) break;
        ep->pos++;
        ExprVal rhs;
        if (!parse_binary(ep, prec + 1, &rhs)) return false;
        if (!apply(ep, op, lhs, rhs, &lhs)) return false;
    }
    *out = lhs;
    return true;
}
```

*Crafting Interpreters* parses expressions with the same idea in
[Compiling Expressions](https://craftinginterpreters.com/compiling-expressions.html).

## Tokens and statements

A *lexer* (or scanner) cuts text into tokens: identifiers, numbers,
strings, punctuation. MLWSVM's language has one statement per line, so
`lex_line` in [src/asm/lexer.c](../../src/asm/lexer.c) works on one line at
a time, after cutting the comment at the first `;` outside quotes. Its
tokens (`TokKind` in [src/asm/lexer.h](../../src/asm/lexer.h)) are
identifiers, numbers, strings, characters, punctuation, BigInt and decimal
literals (`12n`, `1.5d`), and local label references (`1b`, `1f`). One
choice makes the rest simpler: identifiers may contain dots, so `add.i64`,
`.func` and `std.io.println` are each one token.

`statement` then takes any labels (`name:` or `1:`) off the front, and
hands the rest to `directive` if it starts with a dot, or to `instruction`.
`instruction` is where the opcode table comes in. It looks the mnemonic up,
reads the type suffix, and then parses each operand by the letter the
instruction's *pattern* gives for that position:

```c
    int op = op_lookup(m->start, blen);
    if (op < 0)
        return aerr(A, c->pl, m->col, "E_ASM_UNKNOWN_OP", "unknown instruction or macro '%.*s'", m->len, m->start);
    ...
    } else if (cur(c)->kind != TK_EOF) {
        for (int k = 0;; k++) {
            char pat = op_pattern_at(oi, k);
            if (!pat) {
                ok = aerr(A, c->pl, cur(c)->col, "E_ASM_OPERAND_COUNT", "too many operands for %s (max %d)", oi->mnem,
                          oi->maxops);
                break;
            }
            AsmOp o;
            if (!parse_operand(A, c, pat, &o)) {
```

After the operands, `infer_type` gives an instruction without a suffix its
type (from the first register operand, else the first literal), and
`check_insn` checks the whole instruction, the same function the verifier
calls later. An error then points at the operand's column:

```text
error[E_ASM_BAD_OPERAND]: bad_asm.masm:2:13: operand 1: register I0 cannot hold a value of type f64
```

Names need no back-patching. A branch keeps its label's name until `.endf`,
which resolves every label to an instruction index (`1b` is the nearest
`1:` before, `1f` the nearest after), sets each bank's register count to
the highest register used plus one (at least the parameter count), and
rejects a function whose last instruction can fall off the end. Function,
global and struct names are resolved in `finish`, which then encodes every
function into one code buffer. *Crafting Interpreters* writes a lexer in
[Scanning on Demand](https://craftinginterpreters.com/scanning-on-demand.html).

## One table for the whole instruction set

The assembler, the disassembler, the checker, the verifier, the optimizer
and the reference manual all need to know every instruction: its name, its
operands, its types. If each kept its own list, they would drift apart. The
C idiom that prevents this is the *X-macro*: write the list once, as calls
of a macro that is not yet defined, and include the list several times,
each time with a different definition of the macro.

MLWSVM's list is [src/core/opcodes.def](../../src/core/opcodes.def), 130
entries like these:

```c
/*   OP(ENUM, CODE, "mnemonic", "category", "pattern", typeset, flags, "doc") */
OP(ADD,    0x0020, "add",    "Arithmetic", "DS+", TS_NUM | TS_PTR | TS_BIGDEC, 0,
   "Sum. `add dst, s` means dst += s; `add dst, s1, s2, ...` means dst = s1 + s2 + .... For add.p the first value is a pointer and the remaining values are i64 byte offsets.")
...
OP(BLT,    0x0075, "blt",    "Branches", "SSL", TS_ORD, 0,
   "Branch if a < b, using the instruction type's signedness.")
```

The pattern `DS+` says: a destination, then one or more sources, all of
the instruction's type. `SSL` is two sources and a label. Other letters
cover memory operands (`M`), functions (`F`), call blocks (`C`) and the rest
([DESIGN.md §3.1](../DESIGN.md#31-the-opcode-table)).

The list is expanded twice. [src/core/opcodes.h](../../src/core/opcodes.h)
makes the enum every part of the VM uses:

```c
typedef enum {
#define OP(e, code, m, cat, pat, ts, fl, doc) OP_##e,
#include "opcodes.def"
#undef OP
    OP__COUNT
} Opcode;
```

and [src/core/opcodes.c](../../src/core/opcodes.c) makes the table of
descriptions, `op_info`, in the same order:

```c
OpInfo op_info[OP__COUNT] = {
#define OP(e, c, m, cat, pat, ts, fl, d) {c, m, cat, pat, ts, fl, d, {0}, 0, 0, 0, 0},
#include "opcodes.def"
#undef OP
};
```

`op_init` then derives the operand count range from each pattern (`DS+` is
2 to 32 operands), and checks that the 16-bit codes are strictly increasing.
The codes are what files store, and they are never renumbered, so an old
`.mlwso` still means the same thing; the dense enum is free to change. A
code read from a file becomes the enum by binary search (`op_from_code`).

Everything else reads `op_info`:

| Reader | What it takes from the table |
|---|---|
| `instruction` in [src/asm/assembler.c](../../src/asm/assembler.c) | the mnemonic, the type set, the pattern letter for each operand |
| `check_insn` in [src/core/check.c](../../src/core/check.c) | operand counts, allowed types, which positions are written (`D W B E X J H`) and read |
| `insn_effect` in [src/core/regflow.c](../../src/core/regflow.c), `is_dst_pos` in [src/opt/ir.c](../../src/opt/ir.c) | which registers each instruction reads and writes, for the verifier and the optimizer |
| `disasm_insn` in [src/core/disasm.c](../../src/core/disasm.c) | the mnemonic and how to print the operands |
| `gen_isa` in [src/tools/doc.c](../../src/tools/doc.c) | everything, written out as [ISA.md](../ISA.md) |

So [ISA.md](../ISA.md) is generated, not written: `make docs` runs
`./mlwsvm doc isa -o docs/ISA.md`, and regenerating it gives a file
identical to the one in the repository. What the table cannot supply is
the interpreter's handler for a new instruction; that is still written by
hand (chapter 3).

## Modules and the `.mlwso` format

A `Module` ([src/core/module.h](../../src/core/module.h)) is a set of tables
and one buffer of code. Code refers to everything by index: a constant
string is entry 0 of the constant table, a global is entry 0 of the global
references, a branch target is an instruction index. Anything outside the
module is named, not numbered: an imported function is a function-table
entry marked `IMPORT`, with its module's index and its name. So a module is
complete in itself. It can be assembled, cached or moved alone, and loaded
into any VM ([DESIGN.md §4](../DESIGN.md#4-modules-and-the-binary-format)).

Each instruction is encoded as

```text
insn    := opcode:u16le  type:u8  [type2:u8 for cvt and bitcast]  nops:u8  operand*
operand := mode:u8  payload
```

where numbers are LEB128 (little-endian base 128: seven bits per byte, the
top bit saying "more follows"), as in WebAssembly and DWARF. Two of
`sum.masm`'s instructions, from `xxd` of its `.mlwso`:

```text
add.i64 @total, I0       20 00 | 06 | 02 | 0e 00 00   | 01 00
                         add     i64  2    GLOBAL 0 +0  REG I0
blt.i64 I0, 100, .L1     75 00 | 06 | 03 | 01 00 | 02 e4 00 | 14 01
                         blt     i64  3    REG I0  IMM 100    LABEL 1
```

`100` takes two bytes, `e4 00`, because signed LEB128 needs a byte for the
sign bit once a value reaches 64. A register is one byte: the bank in the
top three bits, the index in the low five (`REG_MAKE` in
[src/core/types.h](../../src/core/types.h)), so `I0` is `00` and `F0` would
be `20`.

The file ([BINARY_FORMAT.md](../BINARY_FORMAT.md#file-layout)) is the magic
`MLWS`, a format version, and the tables in order. The start of `sum.mlwso`:

```text
4d4c5753 0900 0000        "MLWS", version 9, flags 0
03 "sum"  08 "sum.masm"   module name, source file
00  00                    no source files for .loc, no imports
01  01 08 "total = "      1 constant: a string of 8 bytes
01  04 "main" 00 00 00    1 function: main, no flags, no parameters, no results
    01 00 00 00           its window: I=1, F=0, R=0, P=0
    00 45 08 00           code at offset 0, 69 bytes, 8 instructions, defined in the source
    0d 0e 0f 10 11 12 12 13   the line of each instruction: 13, 14, ... 19
...
```

`module_write` in [src/loader/module_io.c](../../src/loader/module_io.c)
writes it; `module_read` reads it back and trusts nothing. Every count is
checked against the bytes left before anything is allocated, so a corrupt
file cannot ask for a gigabyte:

```c
/* Guard counts before allocating so a corrupt file can't request huge arrays. */
#define CHECK_COUNT(cnt, minbytes)                                             \
    do {                                                                       \
        CHECK();                                                               \
        if ((cnt) > (r.n - r.pos) / (minbytes)) FAIL("count out of range");    \
    } while (0)
```

It also checks every index into another table, every type code, register
counts (at most 32 per bank), handler and source ranges, and that the
global segment's word tags hold only numbers and raw bytes, so a file
cannot plant a reference in a global. It does not decode the code. That is
the linker's job, and checking it is the verifier's.

## Loading and linking

Linking turns names into the things they name, as a C linker turns
`printf` into an address. MLWSVM links when it loads, in
[src/vm/loader.c](../../src/vm/loader.c)
([DESIGN.md §6](../DESIGN.md#6-loading-and-linking)). `require` finds a
module by name (`a.b` is `a/b.masm` or `a/b.mlwso`, in the program's
directory, then on the library path, then in library archives), assembles
or reads it, and registers it. `register_module` loads its imports first,
depth first, and reports an import cycle with the chain that made it. Then
`link_module` resolves the module's tables in order: constants (each
string or big number becomes one object shared by the whole VM), globals
(checked pointers into each process's copy of the module's data), structs,
any native code, natives, struct fields, and functions. An imported
function is resolved by name in the exporting module:

```c
        LModule *dep = lm->deps[f->mf->import_idx];
        int fi = module_find_export_func(dep->m, f->name);
        if (fi < 0)
            return mlws_err(err, "E_LINK_UNDEF", "module %s does not export function '%s' (called from %s)", dep->m->name,
                          f->name, m->name);
        f->target = &dep->funcs[fi];
```

Then each function's bytes are decoded into its `Insn[]`
(`decode_function`), and `resolve_operand` replaces each index with the
thing itself: the constant's object, the target `Function`, the native, the
global's checked pointer. Finally the module is verified, and each function
quickened (chapter 4). Any failure stops the load before any of the
module's code runs.

Two modules show it. `shapes.masm` exports a function, and `main.masm`
imports the module and calls the function by its qualified name:

```asm
; shapes.masm - a module that exports one function
.export area

.func area(i64 w, i64 h) -> i64
    mul I2, w, h
    ret I2
.endf
```

```asm
; main.masm - imports shapes and calls shapes.area by name
.import shapes
.include "std/io.inc"

.func main
    call shapes.area (6, 7) -> (I0)
    PRINTLN_I I0
    ret 0
.endf
```

```sh
$ ./mlwsvm run main.masm
42
```

Rename `area` to `volume` in `shapes.masm`, and `main.masm` still
assembles: the assembler never reads `shapes`. The linker is the first to
notice:

```text
error[E_LINK_UNDEF]: module shapes does not export function 'area' (called from main)
```

## The verifier

A VM that accepts bytecode from a file must assume the file may be wrong or
hostile. A compiler bug, a truncated download, or an attacker could hand it
a branch into nowhere or a register that does not exist. The JVM verifies
each class before running it, and WebAssembly validates each module; Lua
dropped its bytecode verifier in 5.2 and warns that a malicious binary
chunk can crash the interpreter. The alternative to checking once is
checking on every instruction, and that cost is paid forever.

[src/vm/verify.c](../../src/vm/verify.c) checks once, at load, everything
the interpreter then relies on without checking again
([DESIGN.md §7](../DESIGN.md#7-the-verifier)). For each function,
`verify_function` checks that:

1. it has code, and each exception handler's region lies inside it;
2. each instruction passes `check_insn`: operand count, addressing modes,
   register banks against types, writability, the shape of a call;
3. every register an operand names, including the base and index of a
   memory operand, is inside the function's window;
4. a typed access to a struct field matches the field's bank;
5. calls match their callee's signature, across modules too: the argument
   count, each argument readable as the parameter's type, each result
   writable as the declared result's type;
6. the last instruction does not fall through.

Label targets and table indices were checked already by `resolve_operand`.
Check 5 is the one an assembler cannot do, because it needs the callee,
which may be in another module:

```c
        if (in->op == OP_CALL || in->op == OP_TCALL) {
            const Function *callee = in->ops[0].rv.ptr;
            const MFunc *cm = callee->mf;
            int argc = (int)in->ops[1].imm;
            if (argc != cm->nparams)
                return verr(err, lm, f, pc, "call to %s.%s passes %d argument(s); it takes %d", callee->mod->m->name,
                            callee->name, argc, cm->nparams);
            for (int k = 0; k < argc; k++)
                if (!check_operand(&in->ops[2 + k], 2 + k, (MlwsType)cm->params[k], false, &ce))
                    return verr(err, lm, f, pc, "argument %d of call to %s: %s", k + 1, callee->name, ce.msg);
```

Give `shapes.area` a third parameter (`area(i64 w, i64 h, i64 d)`), leave
`main.masm` alone, and run it again. Both modules assemble; the load
fails:

```text
error[E_VERIFY]: main.masm:6: in main.main (instruction 0): call to shapes.area passes 2 argument(s); it takes 3
```

This is what makes the interpreter both safe and simple. The quickened
`add` from chapter 1 is `IR(0) = IR(1) + IR(2)`, where `IR(k)` is
`I[in->r[k]]`: an array index with no bounds check. It is safe because check
3 proved, before the first instruction ran, that every register index is
inside the window. A call does not count its arguments, because check 5
counted them. A branch does not test its target, because `resolve_operand`
did. Only what cannot be known at load time is checked as it runs: a call
through a closure, whose callee is a value, checks its signature then;
memory accesses go through checked pointers
([DESIGN.md §10](../DESIGN.md#10-pointers-segments-and-tags)).

### Planning the register window

The verifier also plans what each call must clear. A new window must read
as zero, but clearing the whole window costs a `memset` on every call. Most
registers are written before they are read on every path, so what they held
before can never show. `plan_window` finds the others with a data-flow
analysis: for each instruction, the set of registers definitely written on
every path to it. The entry starts with the parameters; each instruction
adds the registers it always writes (`insn_effect`); where paths join, the
sets are intersected. A worklist runs until nothing changes. The heart of
it is the meet, inside the `FLOW` macro:

```c
        for (int b_ = 0; b_ < NBANKS; b_++) {                                  \
            uint32_t m_ = seen[s_] ? in[s_].b[b_] & (set_).b[b_] : (set_).b[b_]; \
            ch_ |= m_ != in[s_].b[b_];                                         \
            in[s_].b[b_] = m_;                                                 \
        }                                                                      \
```

Any register read where it is not definitely written must be cleared, and
so must every `R` register, because the collector scans the whole `R`
window. The result is one run of words, `zoff` and `zlen`, that each call
clears. The profiler shows it as `cleared`. Take a loop that relies on a
zero register, and a copy of it, `explicit`, that starts with `mov I1, 0`,
in `window.masm` with a `main` that calls each:

```asm
.func implicit(i64 n) -> i64    ; relies on I1 starting at zero
1:  add I1, n
    dec n
    bnz n, 1b
    ret I1
.endf
```

```text
$ ./mlwsvm run --profile window.masm
...
       %        insns    entries   cleared  function
   45.7%           32          1       0/2  window.explicit
   44.3%           31         10       1/2  window.implicit
   10.0%            7          1       0/2  window.main

registers read before they are written (each call clears them):
  window.implicit: I1
```

`fib` from chapter 1 clears `0/4`: none of its registers needs it.
`make check-poison` fills each new window with garbage before clearing the
planned run, so a register read too early makes a test fail.

## The module cache

Assembling is fast, but a compiler's output can be large, and the same
program is often run many times. [src/vm/modcache.c](../../src/vm/modcache.c)
keeps each assembled module, as CPython keeps `.pyc` files in
`__pycache__`. An entry is found by a SHA-256 of what decides the
assembler's output: the running executable (so a rebuilt VM never reads an
older one's entries), the default module name and the source text. A
compiler that writes the same program to a new temporary file each time
still hits:

```c
    hash_str(&h, "mlwsvm module cache");
    hash_str(&h, exe);
    sha256_update(&h, &st.size, sizeof st.size);
    sha256_update(&h, &st.mtime_ns, sizeof st.mtime_ns);
    hash_str(&h, default_name ? default_name : "main");
    sha256_update(&h, text, len);
```

An entry also lists every file the source included, with a hash of each,
so editing `std/io.inc` invalidates it. A hit skips only the assembler: the
module is still linked and verified, like any `.mlwso`. On the M5, a 940 KB
program compiled from Lua starts in 10 ms instead of 38
([TOOLING.md](../TOOLING.md#module-cache)). `MLWSVM_CACHE` chooses the
directory, and `--no-cache` turns it off. Run `sum.masm` with
`MLWSVM_CACHE=/tmp/mc` and `/tmp/mc` gets one entry, named by the hash:
`dcc6682e...d2ec.mlwsc` here (yours will differ, since the executable is
part of the key).

## A worked example: assemble, read, break

Assemble `sum.masm` to a binary module and disassemble it:

```sh
$ ./mlwsvm asm -o sum.mlwso sum.masm
$ ./mlwsvm dis sum.mlwso
```

```asm
; disassembly of module sum (source: sum.masm)
.module sum

; ---- data ----
total:
    .i64 0

.func main()
    .regs I=1, F=0, R=0, P=0
    mov.i64 I0, 0                           ; line 13
.L1:
    inc.i64 I0                              ; line 14
    add.i64 @total, I0                      ; line 15
    blt.i64 I0, 100, .L1                    ; line 16
    ncall "io.print_s" ("total = ")         ; line 17
    ncall "io.print_i" (@total)             ; line 18
    ncall "io.nl" ()                        ; line 18
    ret 0                                   ; line 19
.endf
```

The macros have expanded, `N` has become `100`, `1b` has become `.L1`
(instruction 1), and every instruction has its type. The output is
assembly again: save it as `sum.dis.masm`, assemble that, and disassemble
the result, and the instructions are the same. The two `.mlwso` files
differ only in the source file name and line numbers they record.

The verifier treats a `.mlwso` file from anywhere the same as freshly
assembled source. To see it, damage one. Assemble `fib` and find the
second instruction, `sub.i64 I1, I0, 1`:

```sh
$ ./mlwsvm asm -o /tmp/fib.mlwso examples/fib.masm
$ xxd -s 0x8d -l 10 /tmp/fib.mlwso
0000008d: 2100 0603 0101 0100 0201                 !.........
```

`21 00` is `sub`, `06` is `i64`, `03` operands, then `01 01` is the
register `I1`. Change that `01` at offset `0x92` to `09`, which is `I9`,
outside `fib`'s window of four `I` registers:

```sh
$ printf '\x09' | dd of=/tmp/fib.mlwso bs=1 seek=$((0x92)) conv=notrunc 2>/dev/null
$ ./mlwsvm dis /tmp/fib.mlwso | grep 'line 10'
    sub.i64 I9, I0, 1                       ; line 10
$ ./mlwsvm run /tmp/fib.mlwso
error[E_VERIFY]: examples/fib.masm:10: in fib.fib (instruction 1): register I9 is outside the function's register window
```

The file is well formed, so `module_read` accepts it, and the
disassembler, which reads unverified modules, prints it. The loader
refuses to run it, and names the source line the instruction came from.
Without the verifier, that `sub` would write past the end of its window,
into registers that belong to the next call, or past the end of the
register array. (If you assemble to `examples/fib.mlwso` instead, delete
it afterwards: a `.mlwso` newer than its `.masm` is the one an import
finds.)

## Where to look

| What | Where |
|---|---|
| The assembler's driver | `assemble_file_deps`, `statement`, `instruction`, `dir_endf`, `finish` in [src/asm/assembler.c](../../src/asm/assembler.c) |
| Lexer | `lex_line` in [src/asm/lexer.c](../../src/asm/lexer.c) |
| Preprocessor | `pp_lines`, `subst`, `expand_macro` in [src/asm/preproc.c](../../src/asm/preproc.c) |
| Constant expressions | `parse_binary`, `binprec` in [src/asm/expr.c](../../src/asm/expr.c) |
| The opcode table | [src/core/opcodes.def](../../src/core/opcodes.def), [opcodes.h](../../src/core/opcodes.h), `op_init` in [opcodes.c](../../src/core/opcodes.c) |
| Checking one instruction | `check_insn` in [src/core/check.c](../../src/core/check.c) |
| Instruction encoding | `encode_insn`, `decode_insn` in [src/core/insn.c](../../src/core/insn.c) |
| The `.mlwso` file | `module_write`, `module_read` in [src/loader/module_io.c](../../src/loader/module_io.c); [BINARY_FORMAT.md](../BINARY_FORMAT.md) |
| Loading and linking | `require`, `register_module`, `link_module`, `decode_function`, `resolve_operand` in [src/vm/loader.c](../../src/vm/loader.c) |
| The verifier | `verify_function`, `plan_window` in [src/vm/verify.c](../../src/vm/verify.c) |
| The module cache | `modcache_assemble` in [src/vm/modcache.c](../../src/vm/modcache.c) |
| Reference | [DESIGN.md §3–7](../DESIGN.md#3-the-instruction-set), [ASSEMBLY.md](../ASSEMBLY.md), [ISA.md](../ISA.md), [TOOLING.md](../TOOLING.md#module-cache) |

Previous: [1. What a virtual machine is](01-virtual-machines.md) · Next: [3. The interpreter loop](03-interpreter-loop.md)
