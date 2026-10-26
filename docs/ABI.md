# MLWSVM Execution Model and ABI

This document explains what happens when MLWSVM code runs: processes, registers, the
calling convention, the data stack, memory and traps. Compiler writers should read
it alongside [TARGETING.md](TARGETING.md).

## 1. Processes

A program runs as a **process**. Each process owns:

* a call-frame stack and per-bank register stacks (for the register windows)
* a **data stack** of 8-byte words (1 MiB by default; `mlwsvm run --stack-words N`)
* its own copy of every loaded module's **global segment**
* its own **raw heap** (blocks created with `alloc`)
* its own **garbage-collected heap** (§6) and handle table
* a **mailbox** of messages, and its **quotas** and statistics

Processes share nothing mutable. Loaded code and constant-pool strings are
immutable and shared. Everything else belongs to exactly one process.

### 1.1 Scheduling

* `mlwsvm run` starts `main` as process 1. More are started with
  `spawn f (args) -> (pid)`. Process ids are positive and never reused.
* Scheduling is **preemptive**. Each process runs for a time slice of 2,000
  instructions, then the next ready process runs, round robin. A process in an
  endless loop never starves the others.
* A process that executes `recv` with an empty mailbox, or `sleep`, stops using
  the CPU until a message or its timer arrives. When only timers are pending the
  scheduler sleeps the OS thread.
* The scheduler runs on one OS thread today. Milestone M9 spreads processes
  across threads; because processes share nothing, programs won't need to change.
* **The program ends when `main` ends.** Any processes still running are stopped,
  and the exit code is `main`'s. If `main` waits in `recv` while every other process
  is also waiting, so no message can ever arrive, `main` gets a catchable
  `E_DEADLOCK`.

### 1.2 Messages

* `send pid, value` appends a copy of `value` to the receiver's mailbox, and
  `recv dst` takes the oldest message (FIFO). `recvt dst, ms, Ltimeout` gives up
  after a timeout, and `mlen` counts the waiting messages.
* A message keeps its type, as a list element does: `send.f64` / `recv F0`, a string
  received into an `R` register, and so on. Receiving into the wrong register bank
  traps with `E_TYPE` and leaves the message in the mailbox.
* References are **deep-copied** into the receiver's heap, preserving sharing and
  cycles, so the sender and receiver never share a mutable object. Constant
  strings are shared, since they are immutable. Raw pointers are process-local and
  cannot be sent (`E_TYPE`). `spawn` arguments are copied the same way.
* Sending to a finished or unknown process silently does nothing.

### 1.3 Failure: links and monitors

A process **fails** when an exception escapes it, when it is killed (`kill pid`,
`E_KILLED`), or when it exceeds its instruction quota. Otherwise it exits normally
when its entry function returns or it executes `halt`.

* `monitor pid`: when that process ends, this one receives a `Down` struct with
  fields `pid`, `exit` (the exit code), and `code` / `message`, which are null for a
  normal exit and the error otherwise. Monitoring a process that no longer exists
  delivers a `Down` immediately, with code `E_NOPROC`.
* `link pid`: the two processes fail together. When one fails, the other is
  killed with `E_LINKED`, and so on along chains of links. A normal exit just
  removes the link. If a failure reaches `main`, the program ends with that error.
* A child process that fails with nobody monitoring or linked prints a crash
  report on stderr, so failures are never silent.

### 1.4 Quotas

`quota kind, limit` lowers one of the current process's limits; a process can't
raise its own limits. Processes inherit the limits of the process that spawned
them, so to sandbox untrusted code, spawn a wrapper that sets quotas and then calls
it.

| Kind | Limit | When exceeded |
|---|---|---|
| 0 | heap bytes (the collector runs early as the limit approaches) | catchable `E_QUOTA` at the allocation |
| 1 | raw-memory bytes | catchable `E_QUOTA` at `alloc`/`realloc` |
| 2 | waiting messages in the mailbox | catchable `E_QUOTA` in the **sender** |
| 3 | instructions executed in total | the process is stopped with `E_QUOTA` (a hard CPU limit) |

`mlwsvm run --stats` reports processes spawned, the most alive at once, time
slices, and instructions across all processes. `proc.count` returns the number
of live processes.

## 2. Registers and frames

Every **call** creates a frame with a fresh, zeroed window of registers:

```
caller frame:   I0..I(n-1)   F0..   R0..   P0..       (window sized for caller)
callee frame:   I0..I(m-1)   F0..   R0..   P0..       (fresh window sized for callee)
```

* The size of each bank's window is computed by the assembler (highest register
  used + 1, at least the parameter count, at least `.regs`). It is stored in the
  module and checked by the verifier, so a function can only touch its own window.
* No register is shared between caller and callee. There are no callee-saved or
  caller-saved registers to manage: when a call returns, the caller's registers
  are exactly as it left them.
* `SP`, `FP` and `PID` are per process, not per frame.

### 2.1 Calling convention

```asm
call f (a0, a1, ...) -> (d0, d1, ...)
```

1. Each argument is read **in the caller**, left to right, using the callee's
   declared parameter type.
2. A new frame is pushed. `FP` is set to the current `SP`.
3. Arguments are bound **per bank, in order**: the first int-class parameter (any
   of `b c i8..u64`) goes to `I0`, the next to `I1`, and so on. Floats go to `F0, F1, ...`,
   references to `R0, ...` and pointers to `P0, ...`.

   ```asm
   .func f(i64 a, f64 x, ref s, i64 b, ptr p)   ; a=I0 b=I1 x=F0 s=R0 p=P0
   ```
4. `ret v0, v1, ...` reads its values in the callee, with the declared result
   types if there are any (otherwise their natural types). It pops the frame,
   restores `SP` and `FP` to their values at the call, and writes `v0, v1, ...` to
   the caller's `d0, d1, ...`. The caller may take fewer results than are returned,
   but not more (`E_RET_COUNT`).
5. Destinations can be any writable operand: registers, stack slots, globals, or
   memory through pointers.

At load time the verifier checks every call against the callee's signature: the
argument count, and the argument and result operand kinds (for example, a float
literal can't be passed to an `i64` parameter). This includes calls across modules.

### 2.2 Stack-passed arguments

Up to 30 arguments fit in a call instruction. For more, or for variadic functions,
pass values on the data stack:

```asm
; caller
    push a2, a1, a0           ; push in reverse, so ARG[0] = a0
    call sum3 -> (I0)
    drop 3                    ; the caller removes its arguments

; callee
.func sum3
    add I0, ARG[0], ARG[1], ARG[2]     ; ARG[n] = FP[n]
    ret I0
.endf
```

### 2.3 Frame layout on the data stack

```
higher addresses
    ...                caller's data
    ARG[1]   = FP[1]
    ARG[0]   = FP[0]   <- FP (= SP at the call)
    LOC[0]   = FP[-1]  \
    LOC[1]   = FP[-2]   > reserved by `enter 3`
    LOC[2]   = FP[-3]  /  <- SP after enter
    ...                pushes made by the callee
lower addresses
```

* `enter n` reserves n zeroed words: `SP -= n words`.
* There is no `leave`: `ret` restores `SP` to `FP`'s value at entry, freeing locals
  and anything else the callee pushed.
* The caller's own pushes (stack arguments) are above `FP` and survive the call.

### 2.4 Closure calls

`calli R0 (args) -> (results)` binds arguments exactly like `call`, but to the
closure's function, which is only known at run time. The argument count and each
argument's register bank are checked when the call runs (`E_TYPE`), not by the
verifier. The new frame records the closure, so `UP[n]` reaches its captures. A
closure's captures are GC roots while it runs.

### 2.5 Tail calls

`tcall f (args)` replaces the current frame with a call to `f`. `f`'s results go
directly to the current function's caller, and the stack depth does not grow, so
tail recursion runs in constant space. Because the current frame is freed first,
`tcall` supports **register arguments only**: don't pass stack arguments to a tail
call.

### 2.6 Natives

`ncall "name" (args) -> (results)` calls a host function. Parameter and result
types come from the native registry ([NATIVES.md](NATIVES.md)) and are checked by
the verifier. Natives do not create a frame.

### 2.7 Program entry and exit

* The root module must define `main` with no parameters.
* The process ends when `main` returns. If `main`'s first result is an integer,
  it is the **exit code** (the OS sees its low 8 bits). Otherwise the exit code is 0.
* `halt [code]` ends the process immediately, from any depth.
* `mlwsvm run` exits with 65 if the program fails to load and 70 on a runtime trap.

### 2.8 Limits

| Limit | Value |
|---|---|
| Registers per bank | 32 |
| Operands per instruction | 32 (calls: target + count + 30 arguments/results) |
| Call depth | 100,000 frames (`E_CALL_DEPTH`) |
| Data stack | 131,072 words (1 MiB) by default |
| Raw block size | < 4 GiB |
| Modules per VM | 4,094 |
| Elements in a string, buffer, array, list or map | < 2^31 |
| BigInt / decimal coefficient | 2^24 limbs of 32 bits (about 160 million digits) |
| Decimal exponent | ±1,000,000 |

## 3. Instruction semantics shared by many opcodes

* **Canonical integers.** Integer registers hold 64-bit values that are canonical
  for the type last written: signed narrow types are sign-extended and unsigned
  ones zero-extended. Operations wrap modulo 2^width. Reading a register as a
  narrower type truncates it first.
* **Two-operand forms.** `op dst, src` means `dst = dst op src`.
* **Variadic forms.** `op dst, s1, s2, ..., sN` folds left to right.
* **Division.** Integer division and remainder truncate toward zero and trap with
  `E_DIV_ZERO`. `INT64_MIN / -1` wraps to `INT64_MIN`. Float division never traps.
* **Comparisons.** Comparisons involving NaN are false (except `!=`). Signedness
  comes from the instruction type: `blt.u64` compares unsigned.
* **Conversions.** `cvt` from float to int truncates toward zero and saturates; NaN
  converts to 0.

## 4. Pointers

A pointer (`p`) is a checked 64-bit value:

```
 63        44 43      32 31                  0
+------------+----------+---------------------+
|  segment   |   gen    |       offset        |
+------------+----------+---------------------+
```

| Segment | What |
|---|---|
| 0 | null |
| 1 | the process's data stack |
| 2 ... 4095 | module global segments (module k is segment 2 + k) |
| 4096 ... | raw heap blocks |

* Pointers come from `SP`/`FP`, `lea`, `alloc`/`realloc` and pointer arithmetic.
  No instruction converts an integer to a pointer. Pointers stored in raw memory
  are plain bytes, though, so a program can fabricate one by writing integer bits
  there and loading them as `p`. That is harmless: every dereference is still
  segment-, generation- and bounds-checked, and the segment table is per process,
  so a pointer can never reach another process's memory or the host's.
* `add.p`/`sub.p` adjust the offset by a byte count, trapping with `E_BOUNDS` if
  the result leaves 0 .. 2^32-1. `lea P1, P0[3]` advances by words.
* Pointers compare (`beq.p`, `blt.p`, ...) as 64-bit values. Within one block that
  is the order of their offsets.
* `free`/`realloc` bump the block's generation, so stale pointers trap with `E_UAF`.
  Freeing a pointer twice traps with `E_DOUBLE_FREE`, and freeing a non-start
  pointer traps with `E_BAD_FREE`.
* A process's raw blocks are released when it ends. `--stats` reports any bytes
  still allocated as "raw leaked".

## 5. Memory

* All memory is byte-addressable and **little-endian** on every host.
* The stack and the global segments are **tagged**: every 8-byte word records the
  type it was last written with (`i64`, `f64`, `r`, `p`, ...), or "raw" after
  partial, unaligned, `memcpy` or `memset` writes. Raw heap blocks have no tags.
* Untyped `mov`, `swap`, `push` and `pop` copy a word together with its tag, so
  values keep their types when spilled and reloaded.
* **References** (`r`) may only be stored in word-aligned, tagged words (stack and
  globals), in `R` registers, and inside heap objects. A word can be read as `r` only
  if its tag is `r` or it was never written (it then reads as null). A reference word
  cannot be read as anything else, and `memcpy` refuses to copy one. This keeps the
  GC precise (§6) and makes references unforgeable. To refer to a GC object from raw
  memory, use a handle (`hnew`).

## 6. The garbage-collected heap

Objects (strings, byte buffers, arrays, structs, lists, maps) live on the heap of
the process that created them. Processes never share heap objects; the only
exception is constant-pool strings, which are immutable and immortal.

* **Collector.** Precise, non-moving mark-sweep. A collection starts when the heap
  has grown to twice its size after the previous collection (minimum 1 MiB).
  `gc.collect` requests one explicitly.
* **Safepoints.** Collections happen only *between* instructions. During one
  instruction, including a native call, nothing moves or is freed.
* **Roots.** These are every `R` register of every live frame, every
  reference-tagged word of the data stack and of every module's global segment,
  and every live handle. Nothing else keeps an object alive, so a reference held
  only in raw memory (which can't store references anyway) or in an integer
  doesn't count.
* **Precision.** The VM always knows which words are references: from the register
  bank, from the memory word tags, from array element and struct field types, and
  from list and map tags. Integers that happen to look like addresses never keep
  objects alive, and references can't be forged.
* **Cycles** are collected like any other garbage.
* **Handles.** `hnew` returns an `i64` handle that is a root until `hfree`. Use one
  to refer to an object from raw memory or from host code.
* **Statistics.** `mlwsvm run --stats` reports peak heap, objects allocated and
  collected, and GC count and time. `gc.heap_bytes` and `gc.collections` read them
  from inside a program.
* **Stress testing.** `--gc-stress` (or `MLWSVM_GC_STRESS=1`) collects at every
  instruction boundary after an allocation. `make check-gc` runs the whole test
  suite this way.

## 7. Exceptions and traps

**Raising.** `throw R0` raises any non-null reference. A runtime fault (division
by zero, bounds, null, type, call depth, a failed assertion, ...) raises a new
built-in `Error` struct with `code`, `message` and `trace` string fields.

**Finding a handler.** Each function has a table of handler regions
`[start, end) -> handler`, built from `.try` directives:

1. The VM looks at the frame that raised. The *faulting instruction* is the
   instruction executing in that frame. In the frames below it, it is the `call`
   that is still in progress.
2. Among the regions containing that instruction, the **smallest** one wins, so
   nested regions behave as expected whatever order they are declared in.
3. If no region matches, the frame is popped, which restores `SP` and `FP` exactly
   as a `ret` would, and the search continues in the caller.
4. When a handler is found, execution continues at its first instruction, in
   that frame, with that frame's registers as they were. `SP` is as it was when
   the faulting instruction started (for an outer frame, as it was at the call).
   `catch dst` then fetches the exception.

**Uncaught exceptions** end the process. An `Error` reports its own code and
message. Any other value reports `E_UNCAUGHT`. In both cases the backtrace is the
one captured where the exception was originally thrown:

```
runtime error[E_ASSERT]: ASSERT_EQ(x, 3) failed
  at fail.helper (fail.masm:3)
  at fail.main (fail.masm:7)
```

[ERRORS.md](ERRORS.md) lists every code. Because handlers are tables beside the
code, a `.try` region costs nothing at run time until an exception is actually
thrown.
