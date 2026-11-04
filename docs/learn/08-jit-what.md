# 8. JIT 1: what a JIT is

An interpreter runs a program by looking at each instruction and doing what
it says. A *just-in-time compiler* (JIT) translates the program, while it
runs, into the processor's own machine code, and then runs that instead. This
chapter explains why that is faster, what kinds of JIT there are, when
MLWSVM decides to compile a function, how it produces machine code at all,
and how the code gets into memory where the processor will run it. It ends
by following one loop from MLWSVM assembly into arm64 and x86-64 bytes.

Chapters 9 to 11 continue: how the machine code and the interpreter share one
running program (9), what makes the machine code fast (10), and how a JIT is
measured and tested (11).

## Where an interpreter's time goes

Take one instruction, `add I1, I1, I2`: add two integer registers. Chapter 3
showed how the interpreter runs it. Its handler, `h_Q_ADD_III`, is one line
of C, from the arithmetic forms in
[src/vm/qforms.inc](../../src/vm/qforms.inc):

```c
QCASE(Q_##N##_III) IR(0) = (int64_t)(U(IR(1)) op_ U(IR(2))); NEXT;
```

`IR(k)` is `QI[in->r[k]]`: the window's integer register whose number is the
instruction's k-th operand. `NEXT` counts the instruction against the time
slice and jumps to the next instruction's handler. Here is what the C
compiler made of it on the M5 (`objdump --disassemble-symbols=_h_Q_ADD_III
mlwsvm`, comments added):

```text
ldrb  w8, [x21, #0x11]        ; operand 1's register number, from the Insn
ldr   x8, [x23, x8, lsl #3]   ; its value, from the register window
ldrb  w9, [x21, #0x12]        ; operand 2's register number
ldr   x9, [x23, x9, lsl #3]   ; its value
add   x8, x9, x8              ; the work
ldrb  w9, [x21, #0x10]        ; operand 0's register number
str   x8, [x23, x9, lsl #3]   ; the result, back to the window
cbz   w22, ...                ; the slice used up?
sub   w22, w22, #0x1          ; count this instruction
ldrh  w8, [x21, #0x22]        ; the next instruction's handler number
adrp  x9, ...                 ; the handler table
add   x9, x9, #0xf08
ldr   x0, [x9, x8, lsl #3]    ; the next handler's address
add   x21, x21, #0x20         ; the next instruction
br    x0                      ; go there
```

Fifteen machine instructions, and one of them is the addition. The rest is
the cost of interpreting:

* **Decoding.** The operands' register numbers are data, read from the
  instruction on every execution (`ldrb`).
* **Memory traffic.** VM registers live in memory, so each operand is a load
  and the result a store, even when the value was computed by the instruction
  just before.
* **Dispatch.** Finding and jumping to the next handler takes a table load
  and an indirect branch, which the processor must predict.

This handler is already about as good as an interpreter's gets: tail-call
dispatch keeps the window pointer and the slice count in machine registers
(chapter 3), and quickening has chosen a form for exactly this operand shape
(chapter 4). What is left can only go if the program stops being data.

A JIT removes all three. The machine code for this `add` has the register
numbers built into it, so there is nothing to decode. If both values are
already in machine registers, there are no loads or stores. And the next
instruction's code simply follows, so there is no dispatch. In the loop at the
end of this chapter, the same `add` becomes one machine instruction:
`add x1, x1, x2`.

## Kinds of JIT

JITs differ in what they compile and how hard they work at it.

* **Method JITs** compile a whole function at a time. The JVM's HotSpot, .NET
  and V8 are method JITs. **Tracing JITs** record the path a loop actually
  takes, with the types it saw, and compile that path. LuaJIT is the best
  known; it is why LuaJIT runs compiled Lua's `nbody` 14 times faster than
  MLWSVM's JIT does (the README's table).
* **Baseline JITs** translate each bytecode instruction into a fixed pattern
  of machine code, quickly, without analysing the program much.
  **Optimizing JITs** build an intermediate form, analyse it (types, inlining,
  register allocation across the function) and spend far longer compiling.
  V8 has both (Sparkplug, then Maglev and TurboFan), as HotSpot has C1 and
  C2, and they hand a function on to the next tier as it gets hotter.
* **Copy-and-patch** JITs, such as CPython's since 3.13, stitch together
  machine-code templates that the C compiler produced at build time, filling
  in the holes (operands, addresses).

MLWSVM's is a baseline method JIT with a few targeted optimizations. It
compiles a whole function, one quickened form at a time, keeps values in
machine registers within a block (chapter 10), and runs mixed with the
interpreter: anything it does not compile is left to the interpreter's
handlers (chapter 9). [JIT.md](../JIT.md#design) explains why it writes
machine code itself instead of copying and patching: templates need Clang and
a relocation extractor in the build, and cannot keep a value in a machine
register from one instruction to the next, which is most of the gain on loops.

## When to compile

Compiling costs time and memory, and most of a program's code runs rarely,
so a JIT compiles only what has proved hot. MLWSVM counts. With `--jit on`,
the first instruction of each function and the head of each loop are given
an extra opcode, `Q_JITC`, whose handler counts and then runs the instruction
as usual. In [src/vm/jit.inc](../../src/vm/jit.inc):

```c
static QCC int h_Q_JITC(QPARAMS) {
    Function *fn = (Function *)QFRAME->fn;
    uint32_t pc = (uint32_t)(in - QCODE);
    if (!c->hooked && __atomic_add_fetch(&fn->jit_count, 1, __ATOMIC_RELAXED) == c->vm->jit_hot) {
        plat_lock(c->vm->jit_lock);
        jit_compile(c->vm, fn);
        plat_unlock(c->vm->jit_lock);
    }
    uint16_t x = __atomic_load_n(&in->xop, __ATOMIC_ACQUIRE);
    if (x == Q_JITC || (x == Q_JIT && c->hooked)) x = fn->jit_xop[pc];
    MUSTTAIL return qhandlers[x](QARGS);
}
```

When calls plus loop iterations reach `--jit-hot` (1000 unless you say
otherwise), the thread that got there compiles the function, under a lock
because other threads may be running it too. The handler then carries on
with the instruction's own form (`jit_xop` keeps it), so counting costs one
atomic add per call or iteration until the function is compiled, and nothing
after. Counting loop heads as well as calls matters: a `main` called once
that loops a hundred million times is still hot.

`--jit all` compiles every function as soon as it is loaded and enters the
machine code at every block. Nothing runs fast that way, but every form gets
compiled and run, which is what tests want (chapter 11).

## Writing machine code

Machine code is bytes. A JIT is, at bottom, a program that appends the right
bytes to a buffer. MLWSVM has one encoder per processor,
[src/vm/jit_a64.inc](../../src/vm/jit_a64.inc) and
[src/vm/jit_x64.inc](../../src/vm/jit_x64.inc), behind one set of
operations named `jm_*` (load, store, add, compare, branch...). The forms
themselves are written once, in [src/vm/jit.inc](../../src/vm/jit.inc), in
those operations. Here is the integer arithmetic form, which compiles `add`,
`sub`, `mul`, `and`, `or` and `xor`:

```c
case JX_ARITH: { /* (two operands: the destination is the first) */
    static const int ops[] = {[OP_ADD] = JA_ADD, [OP_SUB] = JA_SUB, [OP_MUL] = JA_MUL,
                              [OP_AND] = JA_AND, [OP_OR] = JA_OR, [OP_XOR] = JA_XOR};
    const Operand *a = &in->ops[in->nops == 3 ? 1 : 0], *c = &in->ops[in->nops - 1];
    s1 = jit_gen_src(j, a, pc, JR_S3);
    s2 = jit_gen_src(j, c, pc, JR_S4);
    d = jc_def(j, 0, REG_IDX(in->ops[0].reg));
    jm_alu(b, ops[in->op], d, s1, s2);
    return;
}
```

`jit_gen_src` puts each source operand in a machine register (loading it if
it is not there already), `jc_def` picks the machine register the result
goes in, and `jm_alu` emits the one instruction.

### arm64: fixed-size words

Every arm64 instruction is one 32-bit word. A register-to-register `add` has
a fixed pattern with three 5-bit register fields, so encoding is an OR of
shifted numbers. From `jit_a64.inc`:

```c
#define A_ADD 0x8B000000u
static uint32_t a_rrr(uint32_t op, int d, int n, int m) { return op | (unsigned)m << 16 | (unsigned)n << 5 | (unsigned)d; }
static void jm_alu(JBuf *b, int op, int d, int a, int c) {
    static const uint32_t ops[] = {A_ADD, A_SUB, A_AND, A_ORR, A_EOR, A_MUL, A_LSLV, A_LSRV, A_ASRV};
    jw(b, a_rrr(ops[op], d, a, c)); /* (a shift's count modulo 64, as shiftop) */
}
```

`add x1, x1, x2` is `0x8B000000 | 2 << 16 | 1 << 5 | 1` = `0x8B020021`, and
that is the word in the dump below. `jw` appends it to the buffer.

### x86-64: variable-length bytes

x86-64 instructions are 1 to 15 bytes long, built from parts: optional
prefixes, a REX byte that widens to 64 bits and extends register numbers to
four bits, the opcode, and a ModRM byte naming the registers. The same `add`,
on registers `rdi` (7) and `r9` (9), is `49 03 F9`:

| Byte | Meaning |
|---|---|
| `49` | REX: W=1 (64-bit operands), B=1 (the r/m register is r8–r15) |
| `03` | `add r64, r/m64` |
| `F9` | ModRM: mode 11 (both are registers), reg = 7 (`rdi`), r/m = 1, plus B, so 9 (`r9`) |

`x_rr` in `jit_x64.inc` builds exactly that: the REX byte, the opcode, then
`0xC0 | (reg & 7) << 3 | (rm & 7)`.

### Branches to code not written yet

A branch forward has to say how far to jump before the code it jumps to
exists. Assemblers solve this with *fix-ups*, and so does a JIT. Every
branch is emitted with a zero offset and recorded (`jfix`: where it is, and
what it should reach: an instruction's code, a stub, the exit). When the
whole function has been written, the fix-ups are patched with the real
distances (`jm_patch_at`), just before the code is installed.

## Putting the code where it can run

Bytes in a buffer cannot be executed: memory from `malloc` is not
executable. Operating systems also refuse, for security, to let memory be
writable and executable at the same time (W^X, "write xor execute"), since
that is what an attacker who can write into a process would want.

So code memory has a life cycle: reserve address space, write the code while
the pages are writable, switch them to executable, and tell the processor
that the bytes there changed. The last step matters because processors keep
a separate cache of instructions, which does not see ordinary stores. In
[src/platform/posix.c](../../src/platform/posix.c):

```c
bool plat_code_write(void *at, const void *src, size_t n) {
#ifdef CODE_MAP_JIT
    pthread_jit_write_protect_np(0);
    memcpy(at, src, n);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(at, n);
#else
    size_t page = plat_code_align(), len = (n + page - 1) & ~(page - 1);
    if (mprotect(at, len, PROT_READ | PROT_WRITE)) return false;
    memcpy(at, src, n);
    if (mprotect(at, len, PROT_READ | PROT_EXEC)) return false;
    __builtin___clear_cache((char *)at, (char *)at + n);
#endif
    return true;
}
```

On Apple silicon, the region is mapped once with `MAP_JIT`, and each thread
switches it between writable and executable for itself
(`pthread_jit_write_protect_np`); elsewhere each piece of code gets pages of
its own, which `mprotect` makes writable, then executable. Windows does the
same with `VirtualProtect` ([src/platform/win32.c](../../src/platform/win32.c)).
Code is written once and never changed, so the switch happens once per
compiled function.

## Making it visible to every thread

Several threads may run the same function. Once its code is installed, they
must all start using it, and none may see half of it. MLWSVM publishes in an
order that makes that safe, at the end of `jit_compile`:

```c
__atomic_store_n(&f->jit_entry, ent, __ATOMIC_RELEASE);
for (uint32_t pc = 0; pc < n; pc++)
    __atomic_store_n(&f->code[pc].xop, enter[pc] ? (uint16_t)Q_JIT : xop[pc], __ATOMIC_RELEASE);
```

First the table of entry addresses, then the opcodes that use it. A thread
that loads a `Q_JIT` opcode with acquire order is guaranteed to see the
table, and the code the table points to. The interpreter never stops: a
thread in the middle of the function simply finds `Q_JIT` the next time it
reaches the start of a block, and goes into the machine code there
(chapter 9).

## Worked example: a loop, into machine code

This function sums 0 to n−1:

```asm
.func sum(i64 n) -> i64
    mov I1, 0                 ; total
    mov I2, 0                 ; i
1:  add I1, I1, I2
    inc I2
    blt I2, n, 1b
    ret I1
.endf

.func main
    call sum (100000000) -> (I0)
    PRINTLN_I I0
    ret 0
.endf
```

(Put `.include "std/io.inc"` at the top for `PRINTLN_I`.) Interpreted, and
with the JIT:

```sh
$ ./mlwsvm run --stats sum.masm
4999999950000000
instructions:     300000007
wall time:        190.347 ms
$ ./mlwsvm run --jit on --stats sum.masm
4999999950000000
wall time:        30.535 ms
```

300 million instructions in 190 ms on the M5 is 0.63 ns each, which is fast
for an interpreter; the machine code takes a sixth of the time. On the i7 it
is 308 ms and 51 ms. `--jit-report -` says what happened:

```text
mlwsvm jit (arm64): a function compiled after 1000 calls or loop iterations
  functions compiled  1
  instructions        6 of 6 compiled (100.0%)
  loops               1 with their registers in machine registers
  machine code        284 bytes, in 0.16 ms
  entries             150000, 299697009 instructions run in machine code (1998.0 per entry)
```

The function was compiled in 0.16 ms, during its loop, and the rest of the
loop ran in machine code. It was entered 150,000 times because a process
runs for a time slice of 2,000 instructions and then gives way (chapter 7);
each time, the interpreter takes over at the slice's end and enters the
machine code again.

To see the code, `MLWSVM_JIT_DUMP=FILE` writes each compiled function as
assembler `.byte` lines, which any assembler and disassembler can read
back:

```sh
$ MLWSVM_JIT_DUMP=dump.s ./mlwsvm run --jit on sum.masm
$ cc -c dump.s -o dump.o && objdump -d dump.o      # on Linux: as -o dump.o dump.s
```

On the M5 (comments added):

```text
_f0_sum:
  60: subs  w23, w23, #0x2          ; block header: 2 instructions from the slice
  64: b.lo  ...                     ;   not enough left: stop for the interpreter
  68: mov   x0, #0x0                ; mov I1, 0
  6c: mov   x1, #0x0                ; mov I2, 0
  70: str   x0, [x20, #0x8]         ; written back to the window
  74: str   x1, [x20, #0x10]
  78: ldr   x0, [x20]               ; the loop's registers into machine registers:
  7c: ldr   x1, [x20, #0x8]         ;   n in x0, I1 in x1, I2 in x2
  80: ldr   x2, [x20, #0x10]
  84: subs  w23, w23, #0x3          ; loop: 3 instructions from the slice
  88: b.lo  ...
  8c: add   x1, x1, x2              ; add I1, I1, I2
  90: add   x2, x2, #0x1            ; inc I2
  94: cmp   x2, x0                  ; blt I2, n, 1b
  98: b.lt  0x84
  9c: str   x1, [x20, #0x8]         ; leaving the loop: its registers back
  a0: str   x2, [x20, #0x10]
  ...                               ; ret I1
```

and on the i7, the loop alone:

```text
  8f: sub   r13d,0x3
  93: jb    ...
  99: add   rdi,r9
  9c: add   r9,0x1
  a0: cmp   r9,rsi
  a3: jl    8f
```

Each iteration is six machine instructions for three VM instructions, where
the interpreter needed about fifteen per instruction. `x20` (`r12` on
x86-64) holds the register window's address, so VM register `I1` is the word
at offset 8; `w23` (`r13d`) holds what is left of the slice. In the loop,
the VM's registers stay in machine registers and the window is touched only
on the way in and out. How the JIT decides that, and how it stays correct
when the slice runs out in the middle, are chapters 9 and 10.

## Where to look

| What | Where |
|---|---|
| Counting and compiling once hot | `h_Q_JITC`, `jit_compile` in [src/vm/jit.inc](../../src/vm/jit.inc) |
| The forms, written once | `jit_insn` and the helpers above it, in `jit.inc` |
| arm64 encoding | `a_rrr`, `jm_alu`, `jm_emit_branch`, `jm_patch_at` in [src/vm/jit_a64.inc](../../src/vm/jit_a64.inc) |
| x86-64 encoding | `x_rex`, `x_rr`, `x_memx`, `jm_alu` in [src/vm/jit_x64.inc](../../src/vm/jit_x64.inc) |
| Fix-ups | `jfix`, `jbranch`, and the patching loop at the end of `jit_compile` |
| Code memory | `plat_code_reserve`, `plat_code_write` in [src/platform/posix.c](../../src/platform/posix.c) and [src/platform/win32.c](../../src/platform/win32.c) |
| Publishing to other threads | the end of `jit_compile` (`jit_entry`, then `Q_JIT`) |
| Options and the report | [JIT.md](../JIT.md#options), [JIT.md](../JIT.md#the-report) |
| Design | [DESIGN.md §8.8](../DESIGN.md#88-the-jit) |

Previous: [7. Processes and the scheduler](07-processes.md) · Next: [9. JIT 2: running with the interpreter](09-jit-interpreter.md)
