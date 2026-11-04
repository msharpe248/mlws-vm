# How MLWSVM works

An introduction to building a virtual machine, told through one that exists.
Each chapter explains an idea from first principles, then shows it in
MLWSVM's source: the file, the function, and a short excerpt. You can read
the excerpt, then open the file and see the rest. The numbers are measured
on this VM; where a design choice was made because something else measured
worse, the chapter says what and by how much.

No knowledge of virtual machines is assumed. You should be able to read C,
and it helps to have written a little assembly language of any kind. Robert
Nystrom's [*Crafting Interpreters*](https://craftinginterpreters.com/)
builds a bytecode VM for Lox step by step, and its second half covers some
of the same ground as chapters 1–6 for a stack machine. Where it does, the
chapters point to it. (MLWSVM's Lox compiler compiles that book's Lox.)

## Chapters

| | Chapter | What it covers |
|---|---|---|
| 1 | [What a virtual machine is](01-virtual-machines.md) | Bytecode, the fetch-decode-execute loop, stack and register machines, what MLWSVM's instructions look like |
| 2 | [From text to instructions](02-assembler.md) | The assembler, the one opcode table everything is generated from, modules, the binary format, loading and the verifier |
| 3 | [The interpreter loop](03-interpreter-loop.md) | One instruction from decoding to its handler; dispatch by `switch`, computed `goto` and tail calls |
| 4 | [Making an interpreter fast](04-fast-interpreter.md) | Quickening, specialised handlers, superinstructions, inline caches, and how each was measured |
| 5 | [Memory and garbage collection](05-memory-and-gc.md) | Heap objects, marking, the mark-region heap, generations and the write barrier |
| 6 | [Calls, frames and exceptions](06-calls-frames-exceptions.md) | Register windows, frames, returns, tail calls, unwinding to a handler |
| 7 | [Processes and the scheduler](07-processes.md) | Erlang-style isolation, mailboxes, time slices, worker threads |
| 8 | [JIT 1: what a JIT is](08-jit-what.md) | Why machine code beats interpreting, when to compile, writing machine code into memory, one instruction followed into arm64 and x86-64 bytes |
| 9 | [JIT 2: running with the interpreter](09-jit-interpreter.md) | Blocks, exact instruction counts, entries, side exits, stepping what is not compiled |
| 10 | [JIT 3: making it fast](10-jit-fast.md) | The register cache, blocks reached by falling through, loops, calls and returns, compiling a closure call for its callee |
| 11 | [JIT 4: measuring and testing](11-jit-measuring.md) | Address maps, sampling profilers and callgrind, reading the machine code, the differential fuzzer, ideas measured and not done |
| 12 | [Testing a virtual machine](12-testing.md) | Tests in the VM's own assembly, check modes, sanitizers, fuzzing, mutation tests, the release check |

Read them in order the first time; later chapters use the earlier ones'
words. The reference documents ([DESIGN.md](../DESIGN.md), [ISA.md](../ISA.md),
[JIT.md](../JIT.md) and the rest) have every detail; these chapters have the
ideas, and link to the reference where you will want more.

## Trying things

Every example runs. Build the VM and run one:

```sh
make
./mlwsvm run examples/hello.masm
./mlwsvm run --stats --profile examples/fib.masm
./mlwsvm dis examples/fib.masm
```

The chapters' snippets of assembly are MLWSVM assembly
([ASSEMBLY.md](../ASSEMBLY.md)); a line starting `$` is a shell command.
