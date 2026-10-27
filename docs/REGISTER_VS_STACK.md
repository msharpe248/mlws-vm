# Register Machines and Stack Machines

Every bytecode VM has to decide where an instruction finds its inputs and puts
its result. A **stack machine** keeps them on an operand stack: instructions
take values from the top and push results back. A **register machine** names
them: each instruction says which slots to read and which to write. The JVM,
the .NET CLR, CPython, Ruby and WebAssembly are stack machines. Lua, Dalvik
(Android), BEAM (Erlang) and MLWSVM are register machines.

Part 1 explains the two designs and their trade-offs in general. Part 2
measures them: the same two programs on MLWSVM, Lua, the JVM and CPython,
with WebAssembly listings. Part 3 places MLWSVM among them. Part 4 measures one
MLWSVM choice that other register VMs avoid: instructions with 0 to 32 operands
instead of a fixed three.

## Summary

* **Register machines execute fewer instructions.** For the same programs,
  MLWSVM and Lua execute 4.5–7 instructions where the JVM and CPython execute
  10–19: 45–68% fewer.
* **Their code is bigger.** Each instruction encodes its operands, so it is larger.
  MLWSVM's `fib` is 76 bytes, Lua's 60, the JVM's 26.
* **Fewer instructions is not the same as faster.** On the Mac, the JVM's
  interpreter runs the loop faster than both register VMs while executing nearly
  three times as many instructions, because each of its instructions costs a third
  of a nanosecond. How cheaply a VM executes each instruction matters as much as
  how many it needs.
* **MLWSVM is the fastest of these interpreters on the loop, but not on calls.**
  On the Mac it runs the loop 2.5× faster than Lua 5.5 and 2.2× faster than the
  JVM's interpreter. Lua runs `fib` 1.4× faster (on Linux too, where the JVM's
  interpreter is faster too), because an MLWSVM call sets up a register window
  in each bank the callee uses.
* **Variable arity costs nothing measurable at run time.** Storing the first three
  operands inside the instruction, as a fixed format would, was no faster on
  macOS and up to 5% slower on Linux. Its costs are one byte per instruction in the
  file (about 11% of the code) and some complexity in the toolchain. Restricting
  calls to three operands would make `fib` 13–17% slower, unless the compiler
  computes each argument in place as Lua's does, which MLWSVM's typed register banks
  make hard.

## Part 1: The two designs

### Evaluating an expression

Take `a = (b + c) * (b - d)`. A stack machine evaluates it the way a
Reverse Polish calculator does: operands first, then the operator, which
consumes them.

```
load b        ; stack: b
load c        ; stack: b c
add           ; stack: (b+c)
load b        ; stack: (b+c) b
load d        ; stack: (b+c) b d
sub           ; stack: (b+c) (b-d)
mul           ; stack: (b+c)*(b-d)
store a       ; stack: empty
```

A register machine names every input and output. Intermediate results need
somewhere to live, so the compiler picks temporary registers (`t1`, `t2`):

```
add t1, b, c
sub t2, b, d
mul a, t1, t2
```

Three instructions instead of eight. Each register instruction does the work of
one stack instruction plus the loads and stores around it. That is the whole
trade: a stack machine spends instructions moving values to and from the stack,
and a register machine spends bits naming where values are.

In a VM, "registers" are not hardware registers. They are numbered slots in the
current call's frame, an array in memory. `add t1, b, c` reads two array slots
and writes a third. A stack machine's operand stack is an array in memory too.
The difference is how instructions address it: implicitly at the top, or
explicitly by number.

### What each design needs from a compiler

**For a stack machine**, the compiler walks the expression tree in post-order:
emit the code for each operand, then the operator. Each variable has a slot
(the JVM's "local variables", WebAssembly's "locals") that `load` and `store`
copy to and from the stack. No analysis is needed to produce correct code, which
is why stack bytecode is the traditional target for simple compilers and for
languages compiled on the fly.

**For a register machine**, the compiler must decide which register holds each
variable and each temporary: register allocation. On a CPU with 16 registers this
is one of the hardest parts of a compiler. A VM can make it easy by offering many
registers: Lua offers up to 255 per function, MLWSVM 32 per type. With that many,
a compiler can give each local variable its own register for the whole function,
and use the rest as a stack of temporaries, which is what Lua's own compiler does
in a single pass.

**Choosing good instructions.** A stack machine's instruction set is closed under
composition: any expression is just its operands' code followed by its
operator's. A register machine has more choices, for example operating directly on
a variable's register (`add a, a, 1`) instead of a temporary, or putting a constant
straight into an instruction. Each choice saves instructions, but the compiler has to
look for it.

### Calls and frames

**Stack machines** pass arguments on the operand stack. In the JVM, the caller
pushes the arguments and executes `invokestatic`, and the arguments become the
callee's first local variables. Its result is pushed on the caller's operand
stack. Each frame has a local-variable array and an operand stack, both sized by
the compiler.

**Register machines** need a convention for which registers receive the arguments.
Lua's is elegant: the frame is a window onto one big value stack. The caller puts
the function and its arguments in consecutive registers at the top of its own
window. The callee's window starts right there, so its parameters are already in
place, with no copying at all. Results come back the same way. A compiler that
computes each argument directly into its register makes a call cost one instruction.

MLWSVM gives each call a fresh register window, with separate banks for
integers, floats, references and pointers. A `call` instruction lists its
arguments and the registers for its results (`call f (I1, F0, R2) -> (I3)`), and
copies each argument into the callee's bank. That is one instruction too, but the
copy is real work, and Part 2 shows it costs more than Lua's sliding window.

### Instruction encoding and code size

Stack instructions are small because their operands are implicit. CPython's
instructions are 2 bytes (an opcode and an argument byte). The JVM uses a 1-byte
opcode usually followed by 0–4 bytes of operands, and WebAssembly does the same with
LEB128-encoded integers. Register instructions are larger because they name their
registers:

| VM | Format |
|---|---|
| Lua 5.4 and 5.5 | Every instruction is 32 bits: a 7-bit opcode, then fields `A`, `B` and `C` (8 bits each) and a flag bit, or one wide field |
| Dalvik | 16-bit units, 1–5 per instruction. Arithmetic names 2 or 3 registers; calls list up to five argument registers or a range |
| MLWSVM | 2-byte opcode, type byte and operand count, then per operand a mode byte and a payload: 9.5 bytes on average |

A register program has fewer instructions, but more bytes. Shi et al. measured
register code 25% larger than the same programs as JVM bytecode. In Part 2,
MLWSVM's code is 1.7–2.9× the JVM's size.

Code size matters when code is shipped over a network or kept in memory in
large amounts. It matters less for an interpreter's speed, which depends on how
many instructions it dispatches and what each one costs.

### Dispatch: where an interpreter's time goes

An interpreter repeats one cycle: fetch the next instruction, decode it, jump to
its handler, execute it. Everything except "execute" is overhead, paid once per
instruction. A simple `switch` loop in C costs several cycles per instruction,
mostly in the indirect branch to the handler, which the CPU must predict.

This is the case for register machines. If the overhead per instruction is
fixed, executing half as many instructions halves it. Shi et al. found that
translating JVM bytecode into a register form removed 47% of the instructions
executed, and that the register version ran 26–32% faster, depending on the dispatch
technique.

The overhead is not fixed, though. Decades of interpreter work reduce it:

* **Threaded code and computed goto:** each handler jumps directly to the next one
  (`goto *table[op]`), instead of returning to a central `switch`. Each
  instruction then gets its own indirect branch, which the CPU predicts better.
  Lua uses computed goto when compiled with GCC or Clang.
* **Tail calls between handlers:** each handler is a function that ends by
  tail-calling the next one (`musttail` in Clang and GCC 15), with the
  interpreter's state in argument registers. Branches are predicted as with
  threaded code, and the compiler allocates each handler's registers separately,
  so no handler's speed depends on the others' code. CPython 3.14 and MLWSVM
  dispatch this way.
* **Top-of-stack caching:** the stack's top value is kept in a CPU register, so
  most stack instructions never touch the stack in memory (Ertl, 1995).
* **Template interpreters:** HotSpot (the standard JVM) generates each bytecode's
  handler as machine code when it starts, with the top of the stack cached in a
  register. Its interpreter is fast enough that stack overhead almost disappears,
  as Part 2 shows.
* **Superinstructions and quickening:** frequent sequences become single
  instructions, and generic instructions are replaced by type-specialised ones
  once their operand types are known. CPython 3.11+ does this as it runs (PEP 659),
  and MLWSVM does it at load time.

A register machine's instructions also have more to decode: which registers,
which addressing modes. Lua extracts its fields with shifts and masks.
MLWSVM decodes all operands once, at load time, into structures the
interpreter reads directly.

### Verification and safety

A VM that accepts bytecode from untrusted sources must check it before running it.

* **Stack machines** must prove that the stack is used consistently. At every
  instruction, the stack must have the same depth and the same types on every
  path that reaches it. The JVM's verifier infers this with a data-flow analysis.
  Since Java 6 (and required since Java 7), compilers emit "stack map frames"
  that state the types at branch targets, so the check takes a single pass. WebAssembly's structured control flow
  (`block`, `loop` and `if` instead of arbitrary jumps) makes the check a single
  linear pass by design.
* **Register machines** must check that each register is used consistently. In a
  dynamically typed VM like Lua, every register holds any value, so there is little
  to check. Lua dropped its bytecode verifier in 5.2 and warns that a malicious
  binary chunk can crash the interpreter; `load` can be told to accept source
  text only. Dalvik verifies types per
  register, much like the JVM. MLWSVM's banks fix each register's type, so its
  verifier's checks are local to each instruction.

The garbage collector needs a related piece of information: which slots hold
references. A JVM needs stack maps for that too. In MLWSVM only the `R` bank holds
references, so the roots are simply the `R` registers.

### When to choose which

| Choose a stack machine when | Choose a register machine when |
|---|---|
| Code size matters (sent over a network, many small programs) | Code is interpreted, and dispatch is a large part of the run time |
| The front end should be as simple as possible | The compiler can afford a register allocator (with many registers, a simple one suffices) |
| A JIT will compile hot code anyway (it converts the stack code to SSA form, so the format hardly matters) | Values should have stable homes, for debuggers and for precise GC roots |
| Verification must be simple and standard (WebAssembly) | Instructions should do more each: memory operands, constants, fused compare-and-branch |

Many VMs end up in between. V8's Ignition interpreter (2016) is a register machine
with an accumulator: most instructions read one register and the accumulator,
which keeps instructions short. HotSpot keeps stack bytecode but caches the top
of the stack in a CPU register. Every serious JIT ignores the source format and
allocates machine registers itself.

### A short history

* **Stack machines first.** The Burroughs B5000 (1961) was a stack-based
  hardware architecture. Forth (1970), Pascal's p-code (from 1973; UCSD Pascal made it
  widespread in 1978) and Smalltalk-80's bytecode were all stack machines, chosen
  for compact code and simple compilers. The JVM (1995), the .NET CLR (2002),
  CPython, Ruby's YARV and WebAssembly (2017) followed.
* **Register VMs for speed.** Lua switched from a stack VM (Lua 4) to a register
  VM in Lua 5.0 (2003), and its authors report that 5.0 ran their benchmarks faster.
  Parrot, the planned VM for Perl 6, was a register machine. Erlang's BEAM is
  one too.
* **Dalvik** (2008) chose registers for Android: fewer instructions to interpret
  on slow phones, and less memory traffic. Android's later runtime, ART, compiles
  ahead of time but still loads Dalvik's register bytecode.
* **The debate was settled by measurement.** Shi, Gregg, Beatty and Ertl (2005,
  2008) translated the JVM's stack code into register form inside the same VM, and
  found the register version faster despite larger code. Since then, interpreters
  built for speed have mostly used registers, and formats built for portability and
  validation (WebAssembly) have mostly used stacks.

## Part 2: Measured

The same two programs run on five VMs. All of them use 64-bit integers.

* **`fib`:** naive recursive Fibonacci, `fib(32)`, which makes 7,049,155 calls.
  This measures calls.
* **`loop`:** `acc = (acc + (i*i) % 7) ^ 0x55` for 30 million values of `i`. This
  measures arithmetic and branches.

The sources are `bench/fib.masm`, `bench/loop.masm` and their translations in
`bench/compare/`. `python3 bench/compare/compare.py` reruns everything below
except the listings.

### The same function, five ways

**MLWSVM** (register machine; 8 instructions, 76 bytes):

```
.func fib(i64 n) -> i64
    blt n, 2, 1f                  ; if n < 2 goto 1
    sub I1, n, 1
    call fib (I1) -> (I2)
    sub I1, n, 2
    call fib (I1) -> (I3)
    add I0, I2, I3
    ret I0
1:  ret n
.endf
```

**Lua 5.5** (register machine with fixed 3-operand instructions; 15 instructions,
60 bytes), from `luac -l`:

```
1  LTI       0 2 0      ; if not (R0 < 2) then skip the next instruction
2  JMP       1          ; to 4
3  RETURN1   0          ; return n
4  GETUPVAL  1 0        ; R1 = fib (the function itself is a value)
5  ADDI      2 0 -1     ; R2 = n - 1: the argument, computed in place
6  MMBINI    0 1 7 0    ; only for non-numbers (__sub)
7  CALL      1 2 2      ; R1 = R1(R2): 1 argument, 1 result
8  GETUPVAL  2 0
9  ADDI      3 0 -2
10 MMBINI    0 2 7 0
11 CALL      2 2 2      ; R2 = R2(R3)
12 ADD       1 1 2
13 MMBIN     1 2 6      ; only for non-numbers (__add)
14 RETURN1   1
15 RETURN0
```

Lua's arithmetic instructions skip the `MMBIN` that follows them when both
operands are numbers, so the `MMBIN` instructions never run here. Note how the
call works: `GETUPVAL 1` puts the function in `R1`, `ADDI 2 0 -1` computes the
argument straight into `R2`, and `CALL 1 2 2` calls `R1` with arguments from `R2`
on. No values are moved to make the call.

**JVM** (stack machine; 16 instructions, 26 bytes), from `javap -c`:

```
 0: lload_0                 ; push n
 1: ldc2_w  2L              ; push 2
 4: lcmp                    ; compare: push -1, 0 or 1
 5: ifge    10              ; if n >= 2 goto 10
 8: lload_0
 9: lreturn                 ; return n
10: lload_0
11: lconst_1
12: lsub                    ; push n - 1
13: invokestatic fib        ; pop the argument, push the result
16: lload_0
17: ldc2_w  2L
20: lsub
21: invokestatic fib
24: ladd
25: lreturn
```

**WebAssembly** (stack machine with structured control flow; 17 instructions,
27 bytes), from `wasm-objdump -d`:

```
local.get 0
i64.const 2
i64.lt_s
if i64
  local.get 0
else
  local.get 0
  i64.const 1
  i64.sub
  call 0 <fib>
  local.get 0
  i64.const 2
  i64.sub
  call 0 <fib>
  i64.add
end
end
```

**CPython 3.14** (stack machine; 20 instructions, 40 bytes plus 62 bytes of inline
caches for its specialising interpreter), from `dis.dis`:

```
RESUME             0
LOAD_FAST_BORROW   n          ; if n < 2: return n
LOAD_SMALL_INT     2
COMPARE_OP         <
POP_JUMP_IF_FALSE  L1
NOT_TAKEN
LOAD_FAST_BORROW   n
RETURN_VALUE
L1: LOAD_GLOBAL    fib        ; return fib(n - 1) + fib(n - 2)
LOAD_FAST_BORROW   n
LOAD_SMALL_INT     1
BINARY_OP          -
CALL               1
LOAD_GLOBAL        fib
LOAD_FAST_BORROW   n
LOAD_SMALL_INT     2
BINARY_OP          -
CALL               1
BINARY_OP          +
RETURN_VALUE
```

The stack versions spend most of their instructions on `load` and `const`. The
register versions read `n` where it is and write results where they are needed.
The JVM needs three instructions (`lcmp`, then `ifge`, after two loads) for what
MLWSVM does in one `blt`. Lua needs two (`LTI` and `JMP`), although it executes the
`JMP` as part of `LTI`.

### Instructions executed and code size

| | MLWSVM | Lua 5.5 | JVM | CPython 3.14 | WebAssembly |
|---|---|---|---|---|---|
| Kind | register | register, fixed 3 | stack | stack | stack |
| Executed per `fib` call | **4.5** | 5.5 | 10 | 11.5¹ | |
| Executed per `loop` iteration | **6** | 7 | 19 | 18¹ | |
| `fib` code | 8 insns, 76 bytes | 15 insns, 60 bytes | 16 insns, **26 bytes** | 20 insns, 40 bytes + 62 of caches | 17 insns, 27 bytes |
| `loop` body | 6 insns, 54 bytes | 13 insns (7 run), 52 bytes | 19 insns, **32 bytes** | 18 insns, 36 bytes + caches | 19 insns, 33 bytes |

¹ 11 and 17 in CPython 3.13, which does not have 3.14's `NOT_TAKEN` instruction.

How they were counted: MLWSVM with `--stats`, Lua with a count hook that fires on
every instruction (`bench/compare/count.lua`), CPython with `sys.monitoring`
instruction events. A standard JVM cannot count bytecodes, so the JVM's numbers
are counted along the paths through the listing above: 6 instructions for
`n < 2`, 14 otherwise, and half the calls take each path.

The two register machines execute 45–68% fewer instructions than the two stack
machines, and their code is 1.6–2.9× larger than the JVM's. Both results agree
with Shi et al.

### Time

Wall time in milliseconds, best of five, with each VM's start-up time subtracted:

| | Kind | `fib` (Mac) | `loop` (Mac) | `fib` (Linux) | `loop` (Linux) |
|---|---|---|---|---|---|
| MLWSVM | register | 68 | **84** | 143 | **149** |
| Lua 5.5 | register, fixed 3 | **49** | 210 | **100** | 288 |
| JVM, interpreter only (`-Xint`) | stack | 87 | 184 | 104 | 396 |
| CPython | stack | 128 | 1,177 | 186 | 2,376 |
| JVM with its JIT compiler | (machine code) | 3 | 16 | 8 | 31 |

The Mac is an Apple M5 Pro with Lua 5.5.1, OpenJDK 27 and CPython 3.14. Linux is
an Intel i7-11700KF running Ubuntu 26.04 with Lua 5.5.0, OpenJDK 25 and CPython
3.13 (a pyenv build). The JIT row is there for scale: compiled machine code is
another 5–25× faster than the fastest interpreter.

Time per instruction executed, in nanoseconds (Mac):

| | MLWSVM | Lua 5.5 | JVM `-Xint` | CPython |
|---|---|---|---|---|
| `fib` | 2.2 | 1.3 | 1.2 | 1.6 |
| `loop` | 0.46 | 1.0 | 0.32 | 2.1 |

### What the measurements show

1. **The instruction counts favour registers, as expected.** Both register VMs
   need about half as many instructions per call, and a third as many per loop
   iteration.
2. **The cost of each instruction varies by more than the counts do.** The JVM's
   template interpreter executes a bytecode in a third of a nanosecond (one or two
   clock cycles) in the loop on the Mac, fast enough to beat Lua there while
   executing nearly three times as many instructions. A stack machine with an
   excellent interpreter beats a register machine with an ordinary one.
3. **With cheap instructions, the register machine wins the loop.** MLWSVM's
   loop costs about half a nanosecond per instruction, within 1.5× of the JVM's
   template interpreter. Since it needs a third of the JVM's instructions, it
   finishes in under half the time, and 2.5× faster than Lua on the Mac (1.9× on
   Linux). Its program counter stays in a CPU register, common instructions
   dispatch straight to the next handler (with tail calls between handlers,
   the effect of Lua's threaded code), and
   each carries a compact copy of its operands
   ([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)).
4. **Calls are where Lua stays ahead.** A `fib` call costs about 10 ns in MLWSVM
   against 7 ns in Lua, so Lua runs `fib` 1.4× faster on the Mac while executing
   22% more instructions. On the Intel machine with GCC, the JVM's interpreter is
   faster on `fib` too. Lua's callee window starts at the caller's argument
   registers, on a single stack of values, so a call copies nothing. An MLWSVM
   call copies its arguments straight into the callee's registers, but it still
   sets up and zeroes a separate window in each register bank the callee uses,
   and restores four bank tops on return.
5. **CPython is slow on the loop for a different reason.** Its integers are heap
   objects of arbitrary size, so each arithmetic operation does more work. Its
   cost per instruction on `fib` is close to Lua's.

The general lesson: the register-versus-stack choice sets how many instructions
a program needs. How fast each instruction runs depends on the interpreter's
implementation, and that difference can be larger. Register machines win when
each instruction is cheap; their extra decoding work has to be paid once, at
load time, not on every instruction.

## Part 3: Where MLWSVM sits

MLWSVM is a register machine with three unusual choices:

1. **Typed register banks** (`I`, `F`, `R`, `P`) instead of one register file of
   tagged values. Lua and Dalvik use a single file. Banks make GC roots exact and
   keep floats and pointers out of integer registers, but a call's arguments are
   spread across up to four banks. That rules out Lua's sliding window, which is
   one reason calls cost more.
2. **Memory operands.** Any operand can be `SP[42]`, `P1[I2*4+8]`, `R2->Vec2.x` or
   `@global`. This follows CISC machines like the VAX rather than the
   load/store discipline of Lua and Dalvik, so `add @counter, 1` is one instruction.
3. **Variable arity, 0–32 operands.** Lua packs three fields into a 32-bit word,
   and Dalvik's arithmetic takes at most three registers, while its calls list up
   to five argument registers or name a range of consecutive ones. MLWSVM writes a
   call as a single instruction with all its arguments and results
   (`call f (I1, F0, R2) -> (I3)`), and `add` takes any number of sources.

The rest of this document measures the third choice.

## Part 4: Up to 32 operands against a fixed 3

### What a fixed-3 format would look like

In Lua every instruction is 32 bits: a 7-bit opcode and fields `A`, `B` and `C`.
A call is `CALL A B C`. The function and its arguments must be in consecutive
registers starting at `A`. `B` gives the argument count, and the results come
back starting at `A`. Lua's compiler computes each argument straight into its
register, so in Part 2's `fib`, a fixed 3-operand format needs no extra moves at
all.

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
time, into an `Insn` with a pointer to its array of decoded operands. A fixed
format would keep the operands inside the instruction and save following that
pointer. To measure that, a test build of the interpreter (with `switch`
dispatch, and 16-byte `Insn`s without the compact copy described below) stored
up to three full operands inline in each `Insn`, and the quickened handlers (the
fast paths for common shapes; see [PERFORMANCE.md](PERFORMANCE.md)) read them
from there.

| Benchmark | macOS: pointer | Inline | Inline, read directly | Linux: pointer | Inline | Inline, read directly |
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
lines. Following the pointer is cheap because the load does not depend on anything
slow: the pointer sits in the same cache line as the opcode, which has just been
read.

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
the verifier checks each opcode's minimum and maximum operand count, and the
generic call and return paths count arguments and results and copy them through
a `Word[32]` array. (Quickened calls copy straight between register windows.)

### What fixed arity would cost

`bench/compare/fib3.masm` is `fib` written the way a fixed 3-operand ISA would
need it in the worst case. Each argument is moved into a dedicated argument
register before the call, and each result is moved out afterwards:

```
    sub  I1, n, 1
    mov  I4, I1                ; argument into place
    call fib (I4) -> (I5)
    mov  I2, I5                ; result out of place
```

| | fib | fib, fixed-3 style |
|---|---|---|
| Instructions executed | 31.7M | 45.8M (+44%) |
| macOS, Apple M5 Pro | 69 ms | 80 ms (+17%) |
| Linux, Intel i7, GCC | 152 ms | 172 ms (+13%) |
| Linux, AMD Ryzen, Clang | 137 ms | 160 ms (+17%) |

Across the benchmarks, the extra instructions a fixed format would need range
from 0% (best case, with every argument computed in place, as Lua does) to 43–44%
of all instructions executed (`fib`, `binarytrees`: worst case, with a move per
argument and per result). Even in the best case, `binarytrees` needs 4.8% more
instructions, because `add I0, I0, I1, 1` becomes two adds.

### Conclusion

Supporting up to 32 operands costs about a byte per instruction in the file and
some code in the toolchain, but nothing measurable when running. Restricting
instructions to three operands would save that byte and that code. It would also
make call-heavy code up to a sixth slower, unless the compiler places every
argument exactly, which typed banks make hard. The variable format stays.

The measurements did point at two other costs:

* **The decoded form is large.** Each decoded operand is a 72-byte `Operand`,
  because it has room for every addressing mode. `fib`'s 8 instructions occupy
  about 1.7 KB in memory, against Lua's 60 bytes. The benchmarks fit in cache, so
  this does not show in them yet. The Linux slowdown from a larger `Insn` above
  suggests that it would show in programs with a lot of hot code. The fix is a
  compact decoded form, not fewer operands. The quickened shapes have one: a
  16-byte copy of the instruction's register numbers, branch target and
  immediate inside its 32-byte `Insn`. With `switch` dispatch it made no
  difference, but with threaded dispatch it saves 18% on the loop: once
  dispatch is cheap, the pointer chase shows.
* **Calls cost more than Lua's** (Part 2). Setting up a window in each register
  bank is where `fib`'s time goes, and Lua stays 1.4× faster there.

What remains of both is listed in [ROADMAP.md](ROADMAP.md) §4.

## Reproducing

* **Part 2:** `python3 bench/compare/compare.py` (needs `lua`, a JDK and a
  built `./mlwsvm`) prints the timings and instruction counts. The listings come
  from `luac -l -p bench/compare/fib.lua`, `javap -c` on the compiled
  `Fib.class`, `wat2wasm` and `wasm-objdump -d` on `fib.wat`, `python3 -m dis`
  on `fib.py`, and `./mlwsvm dis bench/fib.masm`.
* **Fixed-3 `fib`:** `./mlwsvm run --stats bench/compare/fib3.masm` against
  `bench/fib.masm`.
* **Instruction counts by operand count:** a temporary build that counts
  `in->nops` per opcode in `interp_run` and in `interp_quicken_function`, run on
  each benchmark with `--threads 1`.
* **Inline operands:** a temporary build that adds `Operand inl[3]` to `Insn`
  (`src/core/insn.h`). After the operands are resolved, `decode_function`
  (`src/vm/loader.c`) copies the operands of instructions with up to three into
  `inl` and points `ops` at it. The quickened handlers in `interp_run` then read
  `in->inl`.

## References

- Yunhe Shi, David Gregg, Andrew Beatty and M. Anton Ertl, *Virtual Machine
  Showdown: Stack Versus Registers*, VEE 2005; extended version with Kevin Casey
  in ACM TACO, 2008.
- Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes, *The
  Implementation of Lua 5.0*, Journal of Universal Computer Science, 2005.
- M. Anton Ertl, *Stack Caching for Interpreters*, PLDI 1995.
- M. Anton Ertl and David Gregg, *The Structure and Performance of Efficient
  Interpreters*, Journal of Instruction-Level Parallelism, 2003.
- Philip Koopman, *Stack Computers: The New Wave*, 1989.
- Tim Lindholm, Frank Yellin, Gilad Bracha and Alex Buckley, *The Java Virtual
  Machine Specification*, chapter 4.10 (verification and stack map frames).
- Andreas Haas et al., *Bringing the Web up to Speed with WebAssembly*, PLDI 2017.
- Dan Bornstein, *Dalvik VM Internals*, Google I/O 2008.
- The V8 team, *Firing up the Ignition interpreter*, 2016.
- PEP 659, *Specializing Adaptive Interpreter* (CPython 3.11 and later).
- [PRIOR_ART.md](PRIOR_ART.md) for where MLWSVM's other ideas come from.
