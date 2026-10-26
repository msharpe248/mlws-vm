# Register Machines and Stack Machines

MLWSVM is a register machine. Most well-known VMs are stack machines: the JVM,
the .NET CLR, CPython and WebAssembly. This document explains the
difference, what it costs and buys, and where MLWSVM sits. It also measures one
MLWSVM choice that other register VMs avoid: instructions with 0 to 32 operands
instead of a fixed three.

All measurements were taken on 2026-10-26 on the machines in
[PERFORMANCE.md](PERFORMANCE.md): an Apple M5 Pro laptop (macOS, clang) and an
AMD Ryzen 9 5900X desktop (Ubuntu, gcc). Times are the best of seven runs.

## Summary

* **A register machine executes fewer, bigger instructions.** The same `fib`
  takes 4.5 MLWSVM instructions per call against 11.5 CPython bytecodes, and a
  tight loop takes 6 per iteration against 18. Each register instruction is
  larger, 9.5 bytes on average against CPython's 2 (plus its inline caches).
* **Fewer instructions mean less dispatch.** Each dispatch costs a few cycles,
  whatever the instruction does. In MLWSVM an instruction does more of the work,
  so the work is spread over fewer dispatches.
* **The compiler does more.** It has to allocate registers. A stack machine's
  compiler just walks the expression tree.
* **Variable arity costs nothing measurable at run time.** Storing the first three
  operands inside the instruction, as a fixed format would, was no faster on
  macOS and up to 5% slower on Linux. Its costs are one byte per instruction in the
  file (about 11% of the code) and some complexity in the assembler and verifier.
* **Fixed arity would cost real time.** Without operands for arguments and
  results, `fib` needs 44% more instructions and runs 22–23% slower. A compiler
  that places arguments exactly where the callee wants them could recover most of
  that, but MLWSVM's typed register banks make that placement hard (see below).

## How each kind works

A **stack machine** has instructions that take their inputs from an operand stack
and push their results back. `a = b + c` becomes:

```
load b      ; push b
load c      ; push c
add         ; pop two, push the sum
store a     ; pop into a
```

The operands are implicit, so each instruction is small (CPython uses 2 bytes, the
JVM 1–3, WebAssembly 1–5). A compiler emits code by walking the expression tree:
operands first, then the operator. There is nothing to allocate.

A **register machine** names its inputs and output in the instruction:

```
add a, b, c
```

One instruction instead of four, but it has to encode three operands. Lua 5
(since 2003), Dalvik/ART, Parrot and MLWSVM work this way. In a VM, "registers"
are slots in the current call's frame, not hardware registers. The compiler
assigns each local and temporary to one, as a C compiler does for machine
registers, though with plenty of slots it can do so very simply.

### The same function, both ways

`fib` in CPython 3.14 (a stack machine), from `dis.dis`. That is 20 instructions
and 40 bytes, plus 62 bytes of inline caches for its specialising interpreter:

```
LOAD_FAST_BORROW  n          ; if n < 2: return n
LOAD_SMALL_INT    2
COMPARE_OP        <
POP_JUMP_IF_FALSE L1
NOT_TAKEN
LOAD_FAST_BORROW  n
RETURN_VALUE
L1: LOAD_GLOBAL   fib        ; return fib(n - 1) + fib(n - 2)
LOAD_FAST_BORROW  n
LOAD_SMALL_INT    1
BINARY_OP         -
CALL              1
LOAD_GLOBAL       fib
LOAD_FAST_BORROW  n
LOAD_SMALL_INT    2
BINARY_OP         -
CALL              1
BINARY_OP         +
RETURN_VALUE
```

The same function in MLWSVM (`bench/fib.masm`) is 8 instructions and 76 bytes:

```
.func fib(i64 n) -> i64
    blt n, 2, 1f
    sub I1, n, 1
    call fib (I1) -> (I2)
    sub I1, n, 2
    call fib (I1) -> (I3)
    add I0, I2, I3
    ret I0
1:  ret n
.endf
```

Every `LOAD` in the CPython version exists only to move a value onto the stack.
The register version reads values where they already are.

## Measured: MLWSVM against CPython

CPython 3.14 is a well-optimised stack VM: it specialises instructions as they
run (PEP 659), so it is a fair stack-machine opponent for dispatch. It is not a
fair one for arithmetic: its integers are heap objects of arbitrary size, and
MLWSVM's are 64-bit registers.

| | fib (per call) | loop (per iteration) |
|---|---|---|
| CPython instructions executed | 11.5 | 18 |
| MLWSVM instructions executed | 4.5 | 6 |
| MLWSVM executes | 61% fewer | 67% fewer |
| CPython code size | 40 bytes (20 instructions) + 62 bytes of caches | 36 bytes per iteration + caches |
| MLWSVM code size | 76 bytes (8 instructions) | 54 bytes per iteration |
| Wall time, CPython | 126 ms (`fib(32)`) | 2,390 ms (30M iterations) |
| Wall time, MLWSVM | 112 ms | 237 ms |
| Time per instruction, CPython | 1.6 ns | 4.4 ns |
| Time per instruction, MLWSVM | 3.5 ns | 1.3 ns |

CPython's counts come from `sys.monitoring` instruction events on `fib(20)` and
on 100,000 iterations of the loop. MLWSVM's come from `--stats`.

What the numbers show:

* **Instruction counts fall by 60–70%.** That is more than the 47% that Shi,
  Gregg, Beatty and Ertl measured when translating JVM code to a register form
  (*Virtual Machine Showdown*, 2005/2008), because MLWSVM's instructions also take
  constants and branch targets directly (`blt n, 2, 1f` is one instruction).
* **Code grows by 50–90% in bytes.** Shi et al. measured 25% for their simpler
  register format. MLWSVM's encoding is generous: every operand carries a mode
  byte, and every instruction carries a type byte and an operand count.
* **Time per instruction rises** where the instructions do more. A `fib` call in
  MLWSVM binds arguments into a fresh register window for each of four banks,
  and that is where `fib`'s time goes. In the loop, CPython's per-instruction cost
  is dominated by its integer objects, not by dispatch.

The comparison that would isolate the stack-versus-register question is the one
Shi et al. made: the same VM, the same runtime, two instruction formats. Lua's
switch from a stack VM (Lua 4) to a register VM (Lua 5.0) is the best-known
real case. Ierusalimschy, de Figueiredo and Celes report that Lua 5.0 ran their
benchmarks faster than Lua 4.0, and credit the register VM for much of that.

## The trade-offs

| | Stack machine | Register machine |
|---|---|---|
| Instructions executed | More: every value is pushed and popped | Fewer: values are read in place |
| Code size | Smaller per instruction, usually smaller overall | Larger per instruction, usually larger overall |
| Decoding | Trivial: an opcode and perhaps an index | Operand fields to extract (MLWSVM does this once, at load) |
| Compiler | Walks the expression tree. No register allocation | Must allocate registers. With 32 per bank and a fresh window per call, a simple allocator suffices ([TARGETING.md](TARGETING.md)) |
| Verifier | Must track stack depth and the type in each slot at every instruction (the JVM's type inference) | Checks register indices and operand modes per instruction. MLWSVM's banks give each register a fixed type, so the check is local |
| GC roots | The verifier has to compute which stack slots hold references at each point (stack maps) | MLWSVM's `R` bank holds only references, so the roots are simply the `R` registers plus tagged slots |
| JIT compiling | Stack code converts easily to SSA form, so JITs don't mind | Registers map to machine registers, but a JIT re-allocates them anyway |
| Debugging | Values are anonymous stack slots | Values have homes. The debugger can show `I3` and name it |
| Interpreter dispatch | Paid per instruction, so paid more often | Paid less often. Each instruction does more |

Neither design is better in general. Stack machines win on code size and on the
simplicity of their compilers, which is why they suit code shipped over a network
(WebAssembly, Java applets) or compiled by a JIT anyway. Register machines win
when the bytecode is interpreted, because dispatch is the interpreter's fixed cost
and they dispatch less.

## Where MLWSVM sits

MLWSVM is a register machine with three unusual choices:

1. **Typed register banks** (`I`, `F`, `R`, `P`) instead of one register file of
   tagged values. Lua and Dalvik use a single file. Banks make GC roots exact and
   keep `f64` and pointers out of integer registers, but a call's arguments are
   spread across up to four banks.
2. **Memory operands.** Any operand can be `SP[42]`, `P1[I2*4+8]`, `R2->Vec2.x` or
   `@global`. This follows CISC machines like the VAX rather than the
   load/store discipline of Lua and Dalvik, so `add @counter, 1` is one instruction.
3. **Variable arity, 0–32 operands.** Lua packs three fields into a 32-bit word,
   and Dalvik's arithmetic takes at most three registers, while its calls list up
to five argument registers or name a range of consecutive ones. MLWSVM
   writes a call as a single instruction with all its arguments and results
   (`call f (I1, F0, R2) -> (I3)`), and `add` takes any number of sources.

The rest of this document measures the third choice.

## Up to 32 operands against a fixed 3

### What a fixed-3 format would look like

In Lua 5.4 every instruction is 32 bits: a 7-bit opcode and fields `A`, `B`
and `C`. A call is `CALL A B C`. The function and its arguments must be in
consecutive registers starting at `A`. `B` gives the argument count, and the
results come back starting at `A`. The compiler arranges for the arguments to be
computed straight into those registers, so usually no extra moves are needed.

MLWSVM cannot simply copy this: its arguments go to separate banks (the first
integer argument to the callee's `I0`, the first float to `F0`), so a
"consecutive range" would be needed in each bank. Each call would need up to four
ranges, and the compiler would have to place values for all of them.

### How often MLWSVM uses more than three operands

Statically, across the 5,739 instructions in the tests, examples, libraries and
benchmarks (each function counted once):

| Operands | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8+ |
|---|---|---|---|---|---|---|---|---|---|
| Share | 1.6% | 15.0% | 29.0% | 44.9% | 5.8% | 2.4% | 0.9% | 0.2% | 0.3% |

9.5% of instructions have more than three operands. 88% of those are calls of some
kind: `ncall` 317, `call` 100, the `spawn` forms 37, `closure` 17, `calli` 9 and
`tcall` 1. The rest are variadic arithmetic (`add`, `mul`, `min`, `max` and the
bitwise operations: 20), `lpush` with several values (20), `strcat` with several
parts (13) and a few others (`sel`, `substr`, `mget`, `jtab`, `fma`, `strfind`). This
code is hand-written. A compiler's output may differ, especially in how often it
uses variadic arithmetic.

Dynamically (instructions executed, from an instrumented build, one thread):

| Benchmark | Instructions | With more than 3 operands | Which |
|---|---|---|---|
| fib | 31.7M | 22.2% | `call` (1 argument, 1 result) |
| binarytrees | 26.1M | 23.8% | `call` (1 argument, 1 result); `add I0, I0, I1, 1` |
| closures | 30.0M | 16.7% | `calli` |
| connections | 0.36M | 36.2% | `ncall`, `spawn` |
| loop, sieve, nbody, maps, pingpong, parallel | | 0% | |

In call-heavy code, a fifth of all instructions have more than three operands.
Elsewhere none do.

### What variable arity costs

**At run time: nothing measurable.** Instructions are decoded once, at load
time, into an `Insn` (16 bytes) with a pointer to its array of decoded
operands. A fixed format would keep the operands inside the instruction and
save following that pointer. To measure that, a test build stored up to three
operands inline in each `Insn`, and the quickened handlers (the fast paths for
common shapes; see [PERFORMANCE.md](PERFORMANCE.md)) read them from there.

| Benchmark | macOS: now | Inline | Inline, read directly | Linux: now | Inline | Inline, read directly |
|---|---|---|---|---|---|---|
| fib | 117 | 119 | 120 | 206 | 209 | 208 |
| loop | 237 | 249 | 238 | 476 | 482 | 485 |
| sieve | 698 | 698 | 699 | 1,276 | 1,288 | 1,280 |
| nbody | 132 | 133 | 132 | 220 | 217 | 217 |
| binarytrees | 192 | 191 | 193 | 356 | 354 | 361 |
| closures | 149 | 147 | 147 | 261 | 269 | 268 |
| maps | 62 | 60 | 61 | 103 | 104 | 103 |
| pingpong | 34 | 34 | 34 | 94 | 96 | 95 |
| parallel | 70 | 68 | 69 | 131 | 134 | 138 |

Milliseconds, best of seven, interleaved. The differences are within noise on
macOS. On Linux the inline versions are mostly slower, by up to 5% (`parallel`),
because each `Insn` grew from 16 to 240 bytes and the hot loops touch more cache
lines. Following the
pointer is cheap because the load does not depend on anything slow: the pointer
sits in the same cache line as the opcode, which has just been read.

The interpreter does not otherwise pay for arity. Quickening checks the operand
count once, at load time: `add I0, I1, I2` gets a three-register handler, and
only the rare variadic forms take the generic path, which loops over the
operands.

**In the file: one byte per instruction.** Each encoded instruction carries
its operand count. The average instruction is 9.47 bytes, so the count is about
11% of the code. A fixed format could drop it, but MLWSVM's encoding would still
be variable-length, since each operand carries its own mode byte and an
LEB128-encoded payload.

**In complexity: some.** The assembler parses call blocks (`(args) -> (results)`),
the verifier checks each opcode's minimum and maximum operand count, and the call
and return paths count arguments and results and copy them through a `Word[32]`
array.

### What fixed arity would cost

To measure that, `fib` was rewritten the way a fixed 3-operand ISA would need it
in the worst case. Each argument is moved into a dedicated argument register before
the call, and each result is moved out afterwards:

```
    sub  I1, n, 1
    mov  I4, I1                ; argument into place
    call fib (I4) -> (I5)
    mov  I2, I5                ; result out of place
```

| | fib | fib, fixed-3 style |
|---|---|---|
| Instructions executed | 31.7M | 45.8M (+44%) |
| macOS | 114 ms | 139 ms (+22%) |
| Linux | 206 ms | 252 ms (+23%) |

Across the benchmarks, the extra instructions a fixed format would need range
from 0% (best case, with every argument computed in place) to 43–44% of all
instructions executed (`fib`, `binarytrees`: worst case, with a move per argument
and per result). Even in the best case, `binarytrees` needs 4.8% more
instructions, because `add I0, I0, I1, 1` becomes two adds.

### Conclusion

Supporting up to 32 operands costs about a byte per instruction in the file and
some code in the toolchain, but nothing measurable when running. Restricting
instructions to three operands would save that byte and that code, and would make
call-heavy code up to a fifth slower unless the compiler places every argument
exactly, which typed banks make hard. The variable format stays.

The measurement did point at a different cost: the decoded form is large. Each
decoded operand is a 72-byte `Operand`, because it has room for every addressing
mode. `fib`'s 8 instructions occupy about 1.7 KB in memory, against CPython's
102 bytes. The benchmarks fit in cache, so this does not show in them yet. The
Linux slowdown from a larger `Insn` above suggests that it would show in programs
with a lot of hot code. The fix would be a compact decoded form for the quickened
shapes, not fewer operands. It is listed in [ROADMAP.md](ROADMAP.md) §4.

## Reproducing

The experiments are temporary builds, not part of the tree:

* **Instruction counts by operand count:** count `in->nops` per opcode in
  `interp_run` and in `interp_quicken_function`, and run each benchmark with
  `--threads 1`.
* **Inline operands:** add `Operand inl[3]` to `Insn` (`src/core/insn.h`). In
  `decode_function` (`src/vm/loader.c`), after the operands are resolved, copy
  the operands of instructions with up to three into it and point `ops` at it. Then
  make the quickened handlers in `interp_run` read `in->inl`.
* **Fixed-3 `fib`:** the listing above, with `main` from `bench/fib.masm`.
* **CPython counts:** `sys.monitoring` with `events.INSTRUCTION` enabled on the
  function's code object.

## References

- Yunhe Shi, David Gregg, Andrew Beatty and M. Anton Ertl, *Virtual Machine
  Showdown: Stack Versus Registers*, VEE 2005; extended version with Kevin Casey
  in ACM TACO, 2008.
- Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes, *The
  Implementation of Lua 5.0*, Journal of Universal Computer Science, 2005.
- PEP 659, *Specializing Adaptive Interpreter* (CPython 3.11 and later).
- [PRIOR_ART.md](PRIOR_ART.md) for where MLWSVM's other ideas come from.
