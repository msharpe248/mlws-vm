# Prior Art, Influences, and What Might Be New

This is a living record of where MLWSVM's ideas come from, and of what, if
anything, is distinctive about putting them together. It is updated at every
milestone.

**Summary.** Almost no individual technique in MLWSVM is new. Each part comes from
a well-known system, credited below. What *may* be unusual is the **combination**:

- a CISC-style register instruction set (VAX-like addressing modes, up to 32
  operands, memory operands anywhere);
- typed register banks with per-call register windows (Parrot, Lua);
- a type tag on every memory word (tagged architectures), which makes the GC precise
  and references unforgeable without stack maps;
- Erlang/BEAM-style isolated processes;
- an assembler with a macro preprocessor as the primary interface, aimed at being
  an easy compiler target.

Whether that combination is valuable is an open question.
[What we could measure](#what-we-could-measure) lists experiments that would tell
us.

All code in this repository was written for this project. Nothing was ported or
translated from another VM. A few standard public algorithms appear verbatim:
FNV-1a hashing, the MurmurHash3 `fmix64` and SplitMix64 mixing constants, and LEB128
encoding.

---

## Component by component

### Instruction set and execution model

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Register machine, not a stack machine | Lua 5, Dalvik/ART, Parrot; the stack-vs-register debate (Shi, Gregg, Beatty and Ertl, *Virtual Machine Showdown: Stack Versus Registers*) | Most register VMs use small, fixed-size, mostly three-operand instructions for fast decoding. MLWSVM uses variable-length instructions with 0–32 operands, trading decode speed for density and ease of targeting. |
| Four typed banks: `I` int, `F` float, `R` GC reference, `P` raw pointer, 32 each | **Parrot**'s four register sets (`I` int, `N` number, `S` string, `P` PMC) are the closest precedent; hardware int/float register files in general | The `R`/`P` split, managed references versus checked raw pointers, is a safety boundary. Narrow integer types are kept canonical (sign- or zero-extended) within the `I` bank. |
| Fresh register window per call, sized per function | Lua 5's register window on the value stack; SPARC register windows | Arguments are bound *per bank* (first int arg to `I0`, first float to `F0`, ...). There are no callee- or caller-saved conventions at all. |
| Variadic arithmetic (`add dst, s1..s31`) and the two-operand shorthand | Two-operand forms are a CISC tradition (x86, VAX); variadic operations resemble Lisp `(+ a b c)` | Unusual in a VM ISA. It lets one instruction fold a whole expression. |
| Type suffixes on one opcode (`add.i32`, `add.f64`, `add.p`) | VAX and 68k size suffixes (`.b/.w/.l`); Dalvik's typed opcodes | The type byte is separate from the opcode, so the table stays small (about 110 opcodes) while supporting 14 types. |
| Untyped (`natural`) moves that copy a word together with its type tag | Tagged architectures (Burroughs B5000, Lisp machines) | Used so that spills and reloads preserve types for the GC. |

### Addressing modes

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| `[P]`, `P[d]`, `P[I*s+d]`, `[P]+`, `-[P]`, memory-indirect `[mem][d]` | **VAX** (autoincrement, autodecrement, deferred/indirect modes), PDP-11, Motorola 68000, x86 SIB addressing | Every mode is bounds-checked against a segment and generation-checked. Word units `[..]` and byte units `{..}` are explicitly distinguished in the syntax. |
| `SP[n]`, `FP[n]`, `ARG[n]`, `LOC[n]` | Frame-pointer-relative addressing in every C ABI | The data stack is separate from the register windows and the call frames. |
| Object element `R[I]` and field `R->Type.field` operands | Field and array instructions in the JVM (`getfield`, `iaload`), Dalvik (`iget`, `aget`) | They are *operands*, not instructions, so any instruction can read or write a field or an element directly, for example `add R0->P.x, R1[I2]`. This seems uncommon in VM designs. |

### Memory safety and the garbage collector

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Checked pointers: segment, generation and offset; use-after-free detection | Generational references (the Vale language), handle tables, CHERI capabilities (in spirit), fat pointers | Done in software for every raw memory access. Raw memory is per process, and the pointers can't reach another process's memory. |
| A type tag on every stack and global word | Tagged memory: Lisp machines, the Burroughs B5000, Erlang term tags | Tags exist *alongside* untagged 64-bit values, rather than stealing bits from them. They make the GC precise without stack maps, and they forbid forged or leaked references. |
| Precise, non-moving mark-sweep, one heap per process, collecting only at instruction boundaries | Classic mark-sweep (McCarthy; Jones and Lins, *Garbage Collection*); per-process heaps in **BEAM** | Collecting only between instructions removes the need for C code inside an instruction to register roots, which is a big simplification. |
| Insertion-ordered maps (entry array plus open-addressing index) | **CPython**'s compact dict (Raymond Hettinger's 2012 design) | Keys compare by value across integer widths, and strings by content. |
| Lists with a type tag per element, arrays with a fixed element type | Dynamic arrays in every scripting VM; typed arrays in the JVM and JavaScript | Both are offered, so dynamically typed and statically typed source languages each get a natural fit. |

### Exceptions

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Handler region tables per function, zero cost until a throw | JVM exception tables; the C++ Itanium ABI's zero-cost exceptions | Selection is by the *smallest* enclosing region, so nesting works regardless of table order. There is a block form (`.try`/`.endtry`) for hand-written code and a label form for compilers. |
| Every runtime fault is a catchable `Error` struct with a stable code | Java's exception hierarchy; Erlang error reasons | Even call-depth exhaustion is recoverable. The error codes are part of the stable interface, and the tests assert on them. |

### Processes and concurrency

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Isolated processes; mailboxes; deep-copied messages; preemption by instruction count | **Erlang/BEAM**: processes, mailboxes, copying message passing, reduction counting | Our processes have *typed registers and mutable memory*. BEAM processes run a functional language with immutable terms. |
| `link` / `monitor` / `Down` messages / unobserved crash reports | **BEAM**: links, monitors, `DOWN` messages, OTP supervision | `Down` is an ordinary struct, inspected with the same field operands as any other. |
| The program ends when `main` ends; deadlock detection | **Go** (main returning ends the program; "all goroutines are asleep") | The deadlock is a *catchable* exception in `main`. |
| Quotas that can only be lowered, inherited by children | Unix `setrlimit` (soft and hard limits); BEAM's `max_heap_size` | Covers heap, raw memory, mailbox size and instruction count. A sandbox is just a wrapper that lowers its own limits before calling untrusted code. |

### Toolchain

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Assembler syntax, directives, macros (`\@`, `\()`, `.rept`, `.irp`, `.ifb`), numeric labels `1b`/`1f` | **GNU as** (gas); Unix assembler tradition | Assembly is the *primary* interface and the documented compiler target, rather than an afterthought behind a bytecode compiler. |
| Load-time verifier; constant pools; per-module import and export tables | **JVM** class verification and constant pool; **WebAssembly** validation | Verification includes calls *across modules* against callee signatures, and field types after linking. |
| Binary format with LEB128 integers, branch targets as instruction indices | **WebAssembly**, DWARF (LEB128) | Branch targets are indices rather than byte offsets, so code can be re-encoded freely. |
| One opcode table that generates the assembler, disassembler, verifier and docs | X-macro tables (LLVM's TableGen in spirit) | |
| Tests written in the VM's own assembly, with `; expect-*` comments | LLVM `lit`/FileCheck-style golden tests | Also run with a collection at every allocation (`make check-gc`) and under ASan/UBSan. |

---

## Possibly distinctive combinations

These are hypotheses, not claims. Each needs the experiments in the next section.

1. **A CISC-style VM ISA as a compiler target.** Most VM designers chose simple
   instructions for interpreter speed. MLWSVM chooses expressive ones: memory
   operands everywhere, up to 32 operands, and field and element operands. The bet
   is that code generation is simpler and code is denser, at some cost in decode
   speed. Pre-decoding at load time (already done) recovers part of that cost.
2. **Typed banks plus word tags as the basis for a precise GC.** Typed registers
   give precision for registers and tags give it for memory, with no stack maps
   and no conservative scanning. The cost is a tag byte per word and a tag check
   on reference reads.
3. **BEAM-style processes with a mutable, typed, register-based core.** Erlang
   gets isolation from immutability. MLWSVM gets it from separate heaps and
   copying at the boundary, while allowing ordinary mutation inside a process.
4. **Object fields and elements as addressing modes.** This unifies "memory" and
   "object" access in a single operand model.
5. **Safety without a type-checked bytecode language.** Bounds, generation and tag
   checks plus a structural verifier give memory safety and isolation, without the
   JVM's full type-inferring verifier.

## What we could measure

Experiments that would show whether the combination pays off:

| Question | Experiment |
|---|---|
| Is code denser than a stack VM's? | Compile the same programs for MLWSVM, the JVM (javac) and Lua (luac); compare instruction counts and bytes |
| How much do rich operands cost the interpreter? | Instructions per second, and time per program, against Lua 5.4 on small benchmarks (fib, sieve, nbody, binary-trees), before and after the M9 dispatch work |
| What do the tags cost? | Memory overhead (1/8 extra on the stack and globals), and the time spent on tag checks (a profile with tag checks disabled) |
| Is it easier to target? | Lines of code for a code generator from a small language to MLWSVM, compared with a JVM or WebAssembly backend |
| How do process copies scale? | Message throughput and latency against message size (deep copy cost), compared with BEAM for similar workloads |
| Is the GC simpler but competitive? | Pause times and throughput on allocation-heavy benchmarks; the cost of whole-stack tag scanning as stacks grow |

Current data points: about 100M instructions/s on recursive `fib` (unoptimised
`switch` dispatch on an Apple Silicon laptop), and about 4M messages/s in a
two-process ping-pong.

## Milestone log

| Milestone | Notable influences added |
|---|---|
| M0–M3 | Register VM (Lua, Dalvik, Parrot), VAX addressing modes, GNU as macro syntax, JVM and WebAssembly loading and verification, LEB128 |
| M4 | Mark-sweep GC (Jones and Lins), BEAM-style per-process heaps, CPython compact dict, JVM/Dalvik field and array access (here as operands) |
| M5 | JVM exception tables, Itanium zero-cost exceptions, Java/Erlang error values |
| M6 | Erlang/BEAM processes, mailboxes, links and monitors; Go's main-ends-program rule and deadlock detection; `setrlimit`-style quotas |

## References

- Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes, *The Implementation of Lua 5.0*, Journal of Universal Computer Science, 2005.
- Yunhe Shi, Kevin Casey, M. Anton Ertl and David Gregg, *Virtual Machine Showdown: Stack Versus Registers*, ACM TACO, 2008 (earlier version at VEE 2005).
- The Parrot virtual machine documentation (register sets I, N, S, P).
- Erik Stenman, *The BEAM Book*, and the Erlang/OTP documentation on processes, links and monitors.
- Tim Lindholm et al., *The Java Virtual Machine Specification* (class file format, verification, exception tables).
- The WebAssembly Core Specification (validation, binary format, LEB128).
- *VAX Architecture Reference Manual* (addressing modes).
- Richard Jones and Rafael Lins, *Garbage Collection: Algorithms for Automatic Dynamic Memory Management*, 1996; Jones, Hosking and Moss, *The Garbage Collection Handbook*, 2011.
- Raymond Hettinger, the compact dict proposal for CPython (2012), adopted in CPython 3.6.
- The GNU Assembler (gas) manual: macros, `.rept`, `.irp`, local labels.
- The Vale language's "generational references" design notes.
