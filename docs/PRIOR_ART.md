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
FNV-1a hashing, the MurmurHash3 `fmix64` and SplitMix64 mixing constants, SHA-256 (FIPS 180-4), LEB128
encoding, and, since M19, CRC-32, SipHash-2-4, xoshiro256\*\* and Howard Hinnant's
civil-date conversions.

---

## Component by component

### Instruction set and execution model

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Register machine, not a stack machine | Lua 5, Dalvik/ART, Parrot; the stack-vs-register debate (Shi, Gregg, Beatty and Ertl, *Virtual Machine Showdown: Stack Versus Registers*) | Most register VMs use small, fixed-size, mostly three-operand instructions for fast decoding. MLWSVM uses variable-length instructions with 0–32 operands and decodes them once, at load time. Measured, this costs no run time, while a fixed 3-operand format would make call-heavy code 13–17% slower ([REGISTER_VS_STACK.md](REGISTER_VS_STACK.md)). |
| Four typed banks: `I` int, `F` float, `R` GC reference, `P` raw pointer, 32 each | **Parrot**'s four register sets (`I` int, `N` number, `S` string, `P` PMC) are the closest precedent; hardware int/float register files in general | The `R`/`P` split, managed references versus checked raw pointers, is a safety boundary. Narrow integer types are kept canonical (sign- or zero-extended) within the `I` bank. |
| Fresh register window per call, sized per function, on one register stack (M28) | Lua 5's register window on the value stack; SPARC register windows | Arguments are bound *per bank* (first int arg to `I0`, first float to `F0`, ...), but the banks of a window sit one after another on a single stack of words, so a call moves one stack top and copies arguments word by word between precomputed slots. There are no callee- or caller-saved conventions at all. |
| Variadic arithmetic (`add dst, s1..s31`) and the two-operand shorthand | Two-operand forms are a CISC tradition (x86, VAX); variadic operations resemble Lisp `(+ a b c)` | Unusual in a VM ISA. It lets one instruction fold a whole expression. |
| Type suffixes on one opcode (`add.i32`, `add.f64`, `add.p`) | VAX and 68k size suffixes (`.b/.w/.l`); Dalvik's typed opcodes | The type byte is separate from the opcode, so the table stays small (120 opcodes) while supporting 16 types. |
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

### Closures and numbers

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Closures with captured values (`UP[n]`), indirect calls (`calli`) | **Lua** upvalues; flat closures (Cardelli; Dybvig's Scheme compilers) | Captures are *copied values*, flat and immutable in identity, so a closure is an ordinary object that can be deep-copied into another process. Lua instead shares variables through open/closed upvalue cells; mutable sharing here is done by capturing a box. |
| Arbitrary-precision integers | GMP, Python `int`, Java `BigInteger`; division by **Knuth's Algorithm D** in the form popularised by *Hacker's Delight* (`divmnu`) | Schoolbook algorithms only (no Karatsuba yet); selected by a type suffix on ordinary arithmetic opcodes rather than separate instructions |
| Decimals: coefficient × 10^exponent, scale preserved, half-even division | **Java `BigDecimal`**, Python `decimal`, IEEE 754-2008 decimal arithmetic | One fixed rule for division (28 fractional digits) instead of a configurable context |
| `123n` / `1.5d` literals | JavaScript BigInt (`123n`); C# `decimal` (`1.5m`) | |

### Exceptions

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Handler region tables per function, zero cost until a throw | JVM exception tables; the C++ Itanium ABI's zero-cost exceptions | Selection is by the *smallest* enclosing region, so nesting works regardless of table order. There is a block form (`.try`/`.endtry`) for hand-written code and a label form for compilers. |
| Every runtime fault is a catchable `Error` struct with a stable code | Java's exception hierarchy; Erlang error reasons | Even call-depth exhaustion is recoverable. The error codes are part of the stable interface, and the tests assert on them. |

### Processes and concurrency

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Isolated processes; mailboxes; deep-copied messages; preemption by instruction count | **Erlang/BEAM**: processes, mailboxes, copying message passing, reduction counting | Our processes have *typed registers and mutable memory*. BEAM processes run a functional language with immutable terms. |
| N worker threads, per-thread run queues, work stealing; a woken receiver is queued on the sender's thread | **BEAM** SMP schedulers (one per core, run-queue migration and stealing); **Go**'s scheduler (per-P queues, stealing half a victim's queue, the `runnext` slot); work stealing (Blumofe and Leiserson, Cilk); **Pony**'s actor scheduler | Nothing in a process is shared, so there are no locks on the interpreter's fast path. Only a process's inbox, links, monitors and scheduling state are locked. Another worker is woken only when a queue has a backlog, so a message ping-pong stays on one thread. |
| Messages copied into a detached block of objects that the receiver adopts into its own heap | **BEAM** heap fragments ("m-bufs"), used when the receiver's heap is busy | Always done this way, so no heap is ever touched by two threads, and the collector needs no locks at all. |
| A poller thread (kqueue/epoll) wakes processes waiting for pipes and sockets; file I/O on a thread pool | **Go**'s netpoller (goroutines park on descriptors; file I/O blocks an OS thread); **BEAM**'s I/O polling and dirty I/O schedulers; **libuv** (epoll/kqueue for sockets, a thread pool for files) | A waiting process is just another scheduler state (`io`), woken through the same run queues as a message, and counted by deadlock detection. Natives ask to wait or to move to the pool, then run again; they are never suspended mid-call. |
| `proc.call` / `proc.reply`, `proc.register`, `std.supervisor` | **Erlang/OTP**: `gen_server:call` with a timeout, registered names, supervisors with restart intensity, `gen_tcp:controlling_process` (here `fs.give`/`fs.take`) | Replies go to a reply slot instead of the mailbox, which replaces OTP's selective receive on a reference: the caller's mailbox is never searched, and late replies are dropped by id. The supervisor is ordinary MLWSVM assembly. |
| Stop the world only for tools | Safepoints in the JVM and BEAM | The program itself never stops the world; only the monitor does, and only at time-slice boundaries. |
| `link` / `monitor` / `Down` messages / unobserved crash reports | **BEAM**: links, monitors, `DOWN` messages, OTP supervision | `Down` is an ordinary struct, inspected with the same field operands as any other. |
| Stackful coroutines inside a process (`conew`, `coresume`, `coyield`): their own frames, registers and data stack; values passed both ways; an uncaught exception kills the coroutine and is rethrown at the resume (M23) | **Lua** coroutines (asymmetric, stackful; `resume`/`yield` passing values both ways; `status` with the same four states); Wren fibers; Ruby `Fiber`; Python generators; Moura and Ierusalimschy on asymmetric coroutines; the WebAssembly stack-switching proposal | A switch exchanges about fifteen fields between the process and the coroutine object, so the rest of the VM always finds the running context in the process. Each data stack has its own pointer generation, so a pointer into a stack that is not running traps instead of reading the wrong stack. Lua instead reports errors as `false, err` from `resume`; here they are exceptions, which a compiler turns into values with a `.try`. |
| The program ends when `main` ends; deadlock detection | **Go** (main returning ends the program; "all goroutines are asleep") | The deadlock is a *catchable* exception in `main`. |
| Quotas that can only be lowered, inherited by children | Unix `setrlimit` (soft and hard limits); BEAM's `max_heap_size` | Covers heap, raw memory, mailbox size, instruction count, native code and OS access. A sandbox is just a wrapper that lowers its own limits before calling untrusted code. |

### Calling C (M10)

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Extension API: a versioned table of VM functions passed to the extension's init | **Lua**'s C API; **CPython** extension modules; **JNI**'s `JNIEnv` function table; **Erlang NIFs**; Node-API | Extensions link against nothing from the VM, and the API version is part of the init symbol's name, so a mismatch fails to load instead of crashing. No rooting is needed, because the collector only runs between instructions. Errors are recorded and raised after the call, never by unwinding foreign code. |
| `MLWS_NATIVE_BLOCKING` calls on an I/O thread pool | Erlang's **dirty schedulers** (dirty I/O NIFs) | The calling process just waits, as in `recv`, so the worker thread runs other processes. |
| `MLWS_NATIVE_SERIAL`: a per-extension lock | Python's GIL (here per library, and only for code that asks for it); Java `synchronized` natives | Opt-in, so thread-safe libraries run in parallel. |
| Foreign handles with typed finalizers | Lua full userdata with `__gc`; CPython capsules; JNI global references | Typed by the address of their descriptor, so one library's pointer can't be passed as another's. Handles can't leave their process. |
| `.extern` declarations called through libffi; `out` parameters; `ptr<tag>` | Python **ctypes** and **cffi**; **LuaJIT** FFI; Java's FFM API (Panama); libffi itself | Declared in the assembly the compiler emits, checked by the verifier like any native; tagged pointers stop pointer mix-ups at the boundary. |
| `.cstruct` layouts, structs by value / by pointer (copied back) / `out struct`; variadic `...` (M17) | ctypes `Structure` and `_fields_`; cffi's `cdef`; LuaJIT `ffi.cdef`; Panama's `MemoryLayout`; libffi `FFI_TYPE_STRUCT` and `ffi_prep_cif_var` | The struct is an ordinary MLWSVM struct, so no C memory is ever exposed: it is copied in and out around the call. C's promotions are enforced after `...` at assembly time instead of left to luck. |
| `.callback`: closures as C function pointers during a call, on a re-entrant interpreter (M18) | ctypes `CFUNCTYPE`, cffi `@ffi.callback`, LuaJIT callbacks, Java FFM upcall stubs, JNI calls back into Java; libffi closures | An uncaught exception stops at the callback and is raised when the C function returns, so C frames are never unwound (ctypes prints and drops it; LuaJIT forbids errors there). Waiting inside a callback is an error instead of a deadlock. |
| Async callbacks as messages; `api->post_*` and `api->hold` | Erlang's `enif_send` and port drivers; libuv's `uv_async_send`; GUI toolkits' "post to the main loop" | A C thread never runs MLWSVM code: its calls arrive in a process's mailbox, so process isolation holds even for multi-threaded libraries. |
| `.ffi_free` finalizers and `foreign.release` | ctypes/cffi `ffi.gc(ptr, destructor)`; Java's `Cleaner`; Rust's `Drop` | Declared once per pointer tag, so every handle of that kind is covered; a released handle refuses further calls instead of passing a dangling pointer. |
| `mlwsvm bindgen`: FFI declarations from clang's JSON syntax tree (M20) | Rust's bindgen (libclang), ctypesgen, cffi's `cdef` from headers, Zig's `@cImport`, SWIG | Writes plain assembly declarations to review, with every skipped declaration explained in a comment, and checks each symbol against the library as it goes. |
| Trusted extension directories, SHA-256 pinning, permission checks | SSH's `StrictModes` checks of `~/.ssh`; lockfile and trust-on-first-use pinning; Deno's explicit permissions | Native code is opt-in per directory and per library, and a sandboxed process can be denied native code completely with a quota (kind 4). |

### Embedding (M16)

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| `libmlwsvm`: load from files or memory, call exported functions, host natives, background runs fed with `mlws_send` | **Lua**'s embedding API; CPython's embedding API; Wasmtime and V8 embedders; Erlang ports and `enif_send` | Host natives use the same table as extensions, so one wrapper serves both. Each call is a fresh program run with its own processes, and a background run can be fed messages from any host thread. The VM's quotas make an embedded script a sandbox. |

### Standard library (M12–M19)

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| `time.*`: an `i64` of ns since the epoch, offsets in minutes, VM-computed calendar | Go's `time` (monotonic and wall clocks kept apart), Java's `java.time`, RFC 3339; Howard Hinnant's `days_from_civil` | No time-zone database: only offsets, plus the OS's local offset. Calendar results do not depend on the platform. |
| `random.*`: xoshiro256\*\* per process; `random.bytes` from the OS | Blackman and Vigna's xoshiro; Go's and Python's split between a fast generator and `crypto/rand` / `secrets` | Each process has its own generator, seeded from entropy, so processes never share a sequence. |
| `hash.siphash` | Aumasson and Bernstein's SipHash, as used by Python, Rust and Perl for hash tables | Exposed with an explicit key, for hash tables that must resist flooding. |
| `list.sort` / `list.bsearch` with closures | Python's `list.sort(key=...)` and `bisect`, Java's `Comparator`, C's `qsort`/`bsearch` | The comparator runs on the re-entrant path used by C callbacks: its exceptions come out of the native unchanged, and it cannot block. |

### Performance

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Per-process size-class allocator for small objects (M21) | BEAM's per-scheduler allocators (`erts_alloc`), tcmalloc and jemalloc size classes and thread caches, Go's per-P `mcache` | The cache is per process rather than per thread: a process's objects never leave its heap, so it needs no locks, no cross-thread frees, and its chunks are freed with it. |
| Tail-call dispatch: a function per quickened form, each tail-calling the next; one function for the generic forms (M27). A threaded loop or a `switch` where tail calls cannot be guaranteed (M22) | Bell's threaded code (1973); Ertl and Gregg on the structure of efficient interpreters; computed goto in Lua, CPython and Ruby's YARV; Haberman's `musttail` protobuf parser (upb, 2021); wasm3; CPython 3.14's tail-calling interpreter with Clang's `preserve_none` | Chosen for predictable speed more than raw speed: in one loop, any added code moved other handlers' speed by 5–50% through register allocation, differently on each compiler. The frame and its code pointer are handler arguments too, where the convention has registers for them: through memory, they put a store and a load on every call's critical path. |
| Two-word dynamic values read without a branch: `ldv I, F, slot` puts the number into the bank its tag selects (M30) | Lua's `TValue` (value plus tag); NaN-boxing (LuaJIT, JavaScriptCore), which packs both into one word | The tag stays in the slot, as everywhere in MLWSVM, so no float encoding is reserved and integers keep 64 bits; the instruction removes the branch a typed register machine would otherwise need to read a dynamically typed number. |
| Allocation and tail calls on the fast path (M29) | HotSpot's inline allocation with collection deferred to a safepoint; Lua's `OP_TAILCALL`, which reuses the caller's frame | An allocating form saves its `pc` (it may trap on a quota) and only marks a collection as due; the collection waits for the next safepoint, at most one 2,000-instruction slice away. A closure tail call reuses the frame's window, so its arguments pass through a small buffer first. |
| Specialised forms chosen from what a compiler emits, and a profiler that lists the hottest instructions still on the general path (M24) | CPython's specialisation statistics (`--enable-pystats`) and PEP 659; V8's and LuaJIT's tracing of slow paths | The forms are chosen once, at load time, from operand shapes alone, never from observed types, so they cost nothing to keep correct: a form that meets something unexpected hands that one execution to the generic instruction. |
| Quickening into specialised execution opcodes after verification | JVM "quick" bytecodes; Stefan Brunthaler, *Inline Caching Meets Quickening* (ECOOP 2010); CPython's specialising adaptive interpreter (PEP 659) | Specialisation happens once, at load time, from static operand shapes (register banks and type suffixes), not adaptively at run time. The rich operand model makes the shapes knowable statically. |

### Toolchain

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Assembler syntax, directives, macros (`\@`, `\()`, `.rept`, `.irp`, `.ifb`), numeric labels `1b`/`1f` | **GNU as** (gas); Unix assembler tradition | Assembly is the *primary* interface and the documented compiler target, rather than an afterthought behind a bytecode compiler. |
| Load-time verifier; constant pools; per-module import and export tables | **JVM** class verification and constant pool; **WebAssembly** validation | Verification includes calls *across modules* against callee signatures, and field types after linking. |
| Binary format with LEB128 integers, branch targets as instruction indices | **WebAssembly**, DWARF (LEB128) | Branch targets are indices rather than byte offsets, so code can be re-encoded freely. |
| One opcode table that generates the assembler, disassembler, verifier and docs | X-macro tables (LLVM's TableGen in spirit) | |
| Debugger (`break`/`step`/`next`/`finish`/`bt`), profiler, tracer | **gdb**; flame graphs and the collapsed-stack format (Brendan Gregg); pprof-style hot lists | Breakpoints and stepping are per instruction across many processes; debugger sessions are scriptable from test files (`; debug-script:`) |
| Live monitor, web dashboard, `top` | **Erlang's observer** and `etop`; Unix `top`; JVM JMX/VisualVM | Served by the first worker thread, polling a socket between time slices, so no extra thread; other workers pause briefly per request for a consistent snapshot; plain JSON plus a text view |
| Tests written in the VM's own assembly, with `; expect-*` comments | LLVM `lit`/FileCheck-style golden tests | Also run with a collection at every allocation (`make check-gc`), under ASan/UBSan, and on several threads under ThreadSanitizer. |

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
| Is code denser than a stack VM's? | Answered for two small programs ([REGISTER_VS_STACK.md](REGISTER_VS_STACK.md)): MLWSVM executes 55–68% fewer instructions than the JVM and CPython, and 18% fewer than Lua, but its code is 1.7–2.9× the size of the JVM's. Still to do: real compiler output |
| How much do rich operands cost the interpreter? | Partly answered: once decoded and quickened, very little. Since M22, MLWSVM runs a tight loop 2.5× faster than Lua 5.5, and `fib` as fast as Lua, since each frame's registers are one window on a single stack ([REGISTER_VS_STACK.md](REGISTER_VS_STACK.md), [PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)). Still to do: sieve, nbody and binary-trees against Lua |
| What do the tags cost? | Memory overhead (1/8 extra on the stack and globals), and the time spent on tag checks (a profile with tag checks disabled) |
| Is it easier to target? | Lines of code for a code generator from a small language to MLWSVM, compared with a JVM or WebAssembly backend |
| How do process copies scale? | Message throughput and latency against message size (deep copy cost), compared with BEAM for similar workloads |
| Is the GC simpler but competitive? | Pause times and throughput on allocation-heavy benchmarks; the cost of scanning stack tags as stacks grow |

Current data points (see [PERFORMANCE.md](PERFORMANCE.md)): about 2.1G
instructions/s on a tight register loop and about 630M/s on recursive `fib`, on an
Apple Silicon laptop with one thread. On the loop that is 14× faster than CPython
3.14, 2.5× faster than Lua 5.5 and 2.2× faster than the JVM's interpreter. On
`fib` it is 2.6× faster than CPython and 1.8× faster than the JVM's interpreter,
and level with Lua. A two-process ping-pong reaches about 18M
messages/s, and 16 independent CPU-bound processes run 7.8× faster on the
laptop's 18 cores than on one thread. Instructions with up
to 32 operands cost nothing measurable against a fixed three, and save up to 44%
of the instructions in call-heavy code.

## Milestone log

| Milestone | Notable influences added |
|---|---|
| M0–M3 | Register VM (Lua, Dalvik, Parrot), VAX addressing modes, GNU as macro syntax, JVM and WebAssembly loading and verification, LEB128 |
| M4 | Mark-sweep GC (Jones and Lins), BEAM-style per-process heaps, CPython compact dict, JVM/Dalvik field and array access (here as operands) |
| M5 | JVM exception tables, Itanium zero-cost exceptions, Java/Erlang error values |
| M6 | Erlang/BEAM processes, mailboxes, links and monitors; Go's main-ends-program rule and deadlock detection; `setrlimit`-style quotas |
| M7 | gdb's command set; Brendan Gregg's collapsed stacks for flame graphs; Erlang observer/etop and Unix top for the monitor |
| M8 | Lua upvalues and flat closures; Knuth's Algorithm D (via Hacker's Delight); Java BigDecimal / Python decimal; JavaScript's `n` literal suffix |
| M9 | Quickening: "quick" bytecodes in Smalltalk and early JVMs, Brunthaler's work on quickening, and CPython 3.11's specialising adaptive interpreter (PEP 659). Multi-threaded scheduler: BEAM SMP schedulers and heap fragments, Go's work-stealing scheduler and `runnext`, Cilk-style work stealing |
| M10 | Lua's C API, CPython extensions, JNI and Erlang NIFs (dirty schedulers) for the extension API; ctypes, cffi, LuaJIT's FFI and libffi for `.extern`; SSH `StrictModes` and hash pinning for the trust model |
| M11–M12 | Go's netpoller, BEAM's I/O polling and libuv for the I/O core; `posix_spawn` and Python's `subprocess` (argument lists, no shell) for commands |
| M13–M15 | BSD sockets with the poller; Erlang/OTP for calls, names and supervisors; Python's `str.format` and Rust's `format!` for `text.format`; RFC 8259 for JSON |
| M16 | Lua's and CPython's embedding APIs; Erlang's `enif_send` for messages from host threads |
| M17 | ctypes `Structure`, cffi `cdef` and `ffi.gc`, LuaJIT `ffi.cdef`, Java Panama `MemoryLayout` and `Cleaner`; libffi struct types and `ffi_prep_cif_var` |
| M18 | ctypes `CFUNCTYPE`, cffi callbacks, Java FFM upcalls, libffi closures; Erlang `enif_send`, libuv `uv_async_send` |
| M19 | Go's `time` and RFC 3339; Hinnant's civil-date algorithms; xoshiro256\*\* and SipHash; RFC 4648 (base64), RFC 3986 (URLs), RFC 4180 (CSV); Python's `bisect` |
| M20 | Rust's bindgen, ctypesgen, Zig's `@cImport`; clang's `-ast-dump=json` |
| M21 | BEAM's `erts_alloc`, tcmalloc/jemalloc size classes, Go's `mcache` |
| M22 | Threaded code (Bell; Ertl and Gregg), Lua's and CPython's computed-goto dispatch, Lua's register window for calls (as the measure to beat) |
| M23 | Lua's asymmetric stackful coroutines (and Moura and Ierusalimschy's account of them), Wren fibers, Ruby `Fiber`, Python generators, WebAssembly stack switching |
| M24 | CPython 3.11's specialising interpreter and Brunthaler's quickening (forms chosen from the shapes compilers emit); the inline caches of Smalltalk-80 and Self, which the Lox compiler builds from `bne.r R, @cache, L` |
| M25 | Lua's tagged values (`TValue`: a value plus its type tag, here kept in the slot's own tag and read with `tagof`) |
| M26 | Amortised growth of dynamic arrays (Lua's and CPython's list resizing), with the no-growth case as the fast path |
| M27 | Haberman's `musttail` interpreter style (upb), wasm3, CPython 3.14's tail-calling interpreter and Clang's `preserve_none` |
| M28 | Lua's single value stack (each frame's window one block on it), here holding every register bank of a window |
| M29 | Lua 5.4's `OP_TAILCALL` (a tail call reusing its caller's frame) and HotSpot's allocation fast path (bump allocation inline, collection at a safepoint); the forms chosen, as in M24, from what a compiler emits |
| M30 | Lua's `TValue` again (the tag picks how the value is read), and NaN-boxing in LuaJIT and JavaScriptCore as the alternative not taken: the tag stays in the slot, and the number in the register bank it selects |

## References

- Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes, *The Implementation of Lua 5.0*, Journal of Universal Computer Science, 2005.
- Yunhe Shi, Kevin Casey, M. Anton Ertl and David Gregg, *Virtual Machine Showdown: Stack Versus Registers*, ACM TACO, 2008 (earlier version at VEE 2005).
- James R. Bell, *Threaded Code*, Communications of the ACM 16(6), 1973.
- Josh Haberman, *Parsing Protobuf at 2+GB/s: How I Learned To Love Tail Calls in C*, 2021; the CPython 3.14 "What's New" notes on the tail-calling interpreter.
- M. Anton Ertl and David Gregg, *The Structure and Performance of Efficient Interpreters*, Journal of Instruction-Level Parallelism 5, 2003.
- The Parrot virtual machine documentation (register sets I, N, S, P).
- Erik Stenman, *The BEAM Book*, and the Erlang/OTP documentation on processes, links and monitors.
- Tim Lindholm et al., *The Java Virtual Machine Specification* (class file format, verification, exception tables).
- The WebAssembly Core Specification (validation, binary format, LEB128).
- *VAX Architecture Reference Manual* (addressing modes).
- Richard Jones and Rafael Lins, *Garbage Collection: Algorithms for Automatic Dynamic Memory Management*, 1996; Jones, Hosking and Moss, *The Garbage Collection Handbook*, 2011.
- Raymond Hettinger, the compact dict proposal for CPython (2012), adopted in CPython 3.6.
- The GNU Assembler (gas) manual: macros, `.rept`, `.irp`, local labels.
- Stefan Brunthaler, *Inline Caching Meets Quickening*, ECOOP 2010; Mark Shannon, PEP 659 *Specializing Adaptive Interpreter* (CPython 3.11).
- Donald E. Knuth, *The Art of Computer Programming, Vol. 2: Seminumerical Algorithms*, §4.3.1 (Algorithm D); Henry S. Warren, *Hacker's Delight*, 2nd ed., ch. 9 (`divmnu`).
- The Java `BigDecimal` documentation; Mike Cowlishaw, *General Decimal Arithmetic Specification*.
- The Vale language's "generational references" design notes.
- Robert D. Blumofe and Charles E. Leiserson, *Scheduling Multithreaded Computations by Work Stealing*, JACM, 1999.
- Dmitry Vyukov, *Scalable Go Scheduler Design Doc*, 2012, and the Go runtime's `proc.go` (`runqsteal`, `runnext`).
- Kenneth Lundin, *Inside the Erlang VM, with focus on SMP*, Erlang User Conference, 2008; the ERTS documentation on schedulers and heap fragments.
- Sylvan Clebsch et al., *Deny Capabilities for Safe, Fast Actors* (Pony), AGERE 2015.
- Roberto Ierusalimschy, *Programming in Lua*, part IV (the C API); the Lua 5.4 Reference Manual, §4 (the C API).
- The Erlang/OTP documentation, `erl_nif` (NIFs and dirty schedulers).
- The libffi documentation; the CPython `ctypes` documentation; the LuaJIT FFI documentation.
- FIPS 180-4, *Secure Hash Standard* (SHA-256).
- Jean-Philippe Aumasson and Daniel J. Bernstein, *SipHash: a Fast Short-Input PRF*, INDOCRYPT 2012.
- David Blackman and Sebastiano Vigna, *Scrambled Linear Pseudorandom Number Generators*, ACM TOMS, 2021 (xoshiro256\*\*).
- L. Peter Deutsch and Allan M. Schiffman, *Efficient Implementation of the Smalltalk-80 System*, POPL 1984 (inline caches); Urs Hölzle, Craig Chambers and David Ungar, *Optimizing Dynamically-Typed Object-Oriented Languages with Polymorphic Inline Caches*, ECOOP 1991.
- Ana Lúcia de Moura and Roberto Ierusalimschy, *Revisiting Coroutines*, ACM TOPLAS 31(2), 2009; the Lua 5.4 Reference Manual, §2.6 (coroutines).
- Howard Hinnant, *chrono-Compatible Low-Level Date Algorithms* (`days_from_civil`).
