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
| Type suffixes on one opcode (`add.i32`, `add.f64`, `add.p`) | VAX and 68k size suffixes (`.b/.w/.l`); Dalvik's typed opcodes | The type byte is separate from the opcode, so the table stays small (130 opcodes) while supporting 16 types. |
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
| Memory little-endian on every host, a reference in an 8-byte word on every host | **WebAssembly**'s little-endian linear memory; Java's fixed (big-endian) `DataInput` order | A program reads the same bytes on s390x as on x86-64, and the register file has the same layout on a 32-bit host as on a 64-bit one, so only the code that touches host memory (the FFI) knows the difference. |
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
| FFI types `isize`, `usize`, `long` and `ulong`; `ffi.sizeof`, `ffi.little_endian` | ctypes `c_size_t`, `c_long` and `sizeof`; Rust's `usize` and `core::ffi::c_long`; Zig's `c_long`; LuaJIT's `ffi.sizeof` and `ffi.abi` | Stored by name in the module and sized when it is linked, so one `.mlwso` calls C correctly on 32- and 64-bit hosts, and on 64-bit Windows, where a `long` is 32 bits; bindgen writes them for `size_t` and `long`. |
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
| Sweeping a heap of headered objects in address order, rebuilding free lists in address order (M31; replaced by lines in M46) | Lazy and bitmap sweeping in Boehm–Demers–Weiser and Immix; segregated-fit page sweeps in many mallocs | Done eagerly and without side bitmaps: each slot keeps its header when freed, so a chunk of mixed size classes can be walked object by object. |
| Several programs in one VM, each ending with its main and taking its processes with it (M40) | Erlang/OTP applications started together in one node; process groups and `wait` in a shell; Procfile runners (foreman) | The programs are not separate OS processes but process trees in one scheduler, so they share cores and can message each other by name, while each still ends on its own. |
| Output goes out a whole line at a time per process, so processes and programs running at once never cut each other's lines (M40) | C stdio's line buffering (`_IOLBF`); Erlang's group leader, an I/O server that applies each `io` request whole; Go's `log` package (one write per entry, under a mutex) | The line is gathered per process, without a lock, and its unfinished part goes out when the process stops running (not when it is preempted), so a prompt still appears before the program waits. |
| A module's name is what code calls it, its identity is where it was found: a program's own modules are known by their directory and name, library-path modules by name alone (M40) | Node.js's module cache, keyed by the resolved file name; Python's `sys.modules`, keyed by name alone (two scripts' `util.py` in one interpreter collide); Java's and Go's globally unique names (`com.example.util`, `github.com/x/y`) | Both kinds in one loader: a program's helper files keep short names and stay private to their directory, as in Node, while libraries keep one shared identity by name, as in Java and Go, which one version per VM needs. A library's modules never see a program's files. |
| Loading runs no code; a module that needs set-up exports an `init` that runs once per process, calls its imports' inits first, and is called by the program's `main`, its process entry function, after `module.load`, or lazily by the module's own functions (a compiler convention, `TARGETING.md` §6.1) | Java's class initialisers (`<clinit>`, run by the JVM on first use); Python's and Node's module bodies, run on first import; Erlang's `-on_load`, once per VM; C++ dynamic initialisation of `thread_local` variables | Globals are per process, so set-up must be per process too, and the VM leaves it to compilers rather than paying a check on every call; imports cannot form cycles, so unlike Python, Node and Java no module is ever seen half set up. |
| Loading modules while the program runs, called through closures; nothing is unloaded (M39) | `dlopen`/`dlsym`; Erlang's `code:load_file`; Java's `ClassLoader`; Lua's `require` | The loaded module goes through the same resolver as an import (versions, privacy, verification), and processes that already exist get its globals lazily, so no process is stopped for it. |
| Libraries: versioned archives with a manifest, constraints between them, one version each per run, private modules (M38) | Cargo and npm's semantic-version constraints (`^`, `~`); Erlang/OTP applications (`.app` files with versions and dependencies); Java's JAR manifests and the JPMS's exported packages; Go modules | The manifest is assembly (directives, with the assembler's constants and conditionals), and resolution happens in the loader as imports are met, so a library is chosen when first needed, against the constraints stated so far. |
| Unicode properties, case, normalization and segmentation from generated two-stage tables (M37) | The Unicode Standard's own algorithms (§3.13 case, UAX #15, #29, #11, #31) and its suggested multistage tables (§5.1); ICU, utf8proc, Go's `unicode` and `x/text`, Rust's `unicode-segmentation` | One table of a few hundred distinct records serves every property a lookup needs, so each character costs two array reads; the conformance tests are the Consortium's files, run as assembly programs. |
| A cache of assembled modules, keyed by a hash of the source and checked against its includes (M36) | Python's `__pycache__` (`.pyc` files checked by source hash or mtime); ccache and Go's build cache (content-addressed results) | Keyed by content rather than path, so a compiler's output written to a new temporary file each run still hits; the executable is part of the key, so a rebuilt VM starts afresh. |
| Processes that start small and grow: stacks on first use, idle threads woken one at a time (M35) | BEAM's processes (a few hundred words at spawn, heaps and stacks grown as needed); Go's scheduler, which keeps at most one thread spinning for work and lets it wake the next | The data stack is reserved at full size and allocated on its first access, which the segment check that every stack access makes already catches, so no instruction pays for it. |
| `.loc "file", line` runs: positions in the program a compiler translated, shown by backtraces and every tool (M34) | The `.loc` directive of GNU `as` and DWARF line tables; the JVM's `LineNumberTable` and `SourceFile` attributes; JavaScript source maps | Kept as runs per function in the module itself, beside the assembly line map, so a backtrace can show both the program's line and the generated code's. |
| A call clears only the registers its callee may read before writing them, and its `R` registers (M33) | Definite assignment in the JVM verifier, which rejects such reads instead; Lua, which clears nothing and lets its collector stop at the stack top | The program still sees zero in every register it reads first: the analysis decides only what the VM must clear for that to hold, and a build that fills the rest with garbage checks it. |
| Call and return plans: callee and slots as bytes, made at load time (M33) | CPython's inline caches stored next to the instruction; the argument maps of register VMs such as Dalvik's `invoke-*/range` | Made once, from the verified call's shapes, and kept in the instruction's own operand block, so it needs no invalidation. |
| Fused instructions chosen from counts of adjacent instruction pairs in compiled code (M32) | Superinstructions and superoperators (Proebsting, *Optimizing an ANSI C Interpreter with Superoperators*; Ertl and Gregg on superinstructions in Gforth); displacement addressing modes in hardware ISAs | Chosen offline, from a trace of one compiler's output, and added as ordinary instructions that compiler emits, rather than combined by the VM at run time. |
| Allocation and tail calls on the fast path (M29) | HotSpot's inline allocation with collection deferred to a safepoint; Lua's `OP_TAILCALL`, which reuses the caller's frame | An allocating form saves its `pc` (it may trap on a quota) and only marks a collection as due; the collection waits for the next safepoint, at most one 2,000-instruction slice away. A closure tail call reuses the frame's window, so its arguments pass through a small buffer first. |
| Specialised forms chosen from what a compiler emits, and a profiler that lists the hottest instructions still on the general path (M24) | CPython's specialisation statistics (`--enable-pystats`) and PEP 659; V8's and LuaJIT's tracing of slow paths | The forms are chosen once, at load time, from operand shapes alone, never from observed types, so they cost nothing to keep correct: a form that meets something unexpected hands that one execution to the generic instruction. |
| Fast forms for a dynamically typed language's tuples, floats and tail-recursive loops: an element at a constant index tested against a constant, `typeof`, `bitcast` between the integer and float banks, direct tail calls with constant arguments (M42) | BEAM's `is_tagged_tuple` (tuple, arity and tag atom tested in one instruction) and its `call_only`/`call_last` tail calls; NaN-boxing, which keeps a float's bits in an integer word | No new instruction: the compiler's existing sequence (`typeof`, `len`, then the tag compared with an element) gets a fast form for each step, and a float kept as bits in an `I` register costs one register copy each way. |
| A generational collector that moves nothing: survivors become old in place, old objects written are remembered whole; the young generation starts in the caches (64 KB) and grows with what survives and what it rescans (M44) | Demers, Weiser and others' sticky mark bits (*Combining Generational and Conservative Garbage Collection*, POPL 1990); Ungar's generation scavenging; Lua 5.4's generational mode (non-moving, with objects "touched" since the last collection); HotSpot's heap verification (`-XX:+VerifyBeforeGC`) | The write barrier checks the object, then the value, so stores of numbers, null and constants into old objects cost a load; the JIT skips it for an object made in the same block, since no collection runs inside machine code, and leaves for the interpreter to remember an object. `MLWSVM_GC_VERIFY` checks every minor collection against a walk of the whole heap and names the holder of an unremembered young object. |
| A JIT's loops found as natural loops, with calls and stepped instructions inside as holes around which the registers are written back and loaded; what a block has checked of an object kept until its register is set (M44) | Natural loops and their headers (Aho, Lam, Sethi and Ullman, *Compilers*); LuaJIT's and V8's loop handling in their optimizing tiers; V8 Maglev's known node information | Holes are capped per architecture by measurement (2 on arm64, none on x86-64, with 5 registers), and a branch back to a jump out is not taken for a loop. |
| A string's hash computed when it is first a map key (M44) | Java's `String.hashCode`, cached in the string, 0 meaning not yet computed; CPython's `str` hash, cached with -1 | Constants, which threads share, are hashed when made, so the lazy write only ever touches a process's own strings. |
| Machine code for hot functions, in mixed mode with the interpreter on the interpreter's own frames, windows and slice count; instructions it does not compile run by their interpreter handlers from inside the machine code; a block's and a loop's registers kept in machine registers (M43) | BeamAsm, Erlang/OTP's JIT since OTP 24 (one machine-code sequence per instruction, written with asmjit); V8's Sparkplug (a non-optimizing compiler from bytecode, keeping the interpreter's frame layout); HotSpot's and LuaJIT's mixed mode (interpreted until counters on calls and loops say hot); copy-and-patch (Xu and Kjolstad; CPython 3.13's JIT), the alternative not taken; Apple's `MAP_JIT` and W^X code memory | The slice is counted exactly (each block takes its length at its head, or stops before it), so preemption, quotas and instruction counts are the interpreter's; since the frames are the interpreter's, any instruction is a place to leave or enter, with no deoptimization metadata, and an instruction without machine code is *stepped* by its own handler rather than ending the run of machine code. |
| Quickening into specialised execution opcodes after verification | JVM "quick" bytecodes; Stefan Brunthaler, *Inline Caching Meets Quickening* (ECOOP 2010); CPython's specialising adaptive interpreter (PEP 659) | Specialisation happens once, at load time, from static operand shapes (register banks and type suffixes), not adaptively at run time. The rich operand model makes the shapes knowable statically. |

### Toolchain

| MLWSVM | Influences | How MLWSVM differs |
|---|---|---|
| Assembler syntax, directives, macros (`\@`, `\()`, `.rept`, `.irp`, `.ifb`), numeric labels `1b`/`1f` | **GNU as** (gas); Unix assembler tradition | Assembly is the *primary* interface and the documented compiler target, rather than an afterthought behind a bytecode compiler. |
| Load-time verifier; constant pools; per-module import and export tables | **JVM** class verification and constant pool; **WebAssembly** validation | Verification includes calls *across modules* against callee signatures, and field types after linking. |
| Binary format with LEB128 integers, branch targets as instruction indices | **WebAssembly**, DWARF (LEB128) | Branch targets are indices rather than byte offsets, so code can be re-encoded freely. |
| One opcode table that the assembler, disassembler, verifier and optimizer read, and that generates the instruction reference | X-macro tables (LLVM's TableGen in spirit) | |
| Debugger (`break`/`step`/`next`/`finish`/`bt`), profiler, tracer | **gdb**; flame graphs and the collapsed-stack format (Brendan Gregg); pprof-style hot lists | Breakpoints and stepping are per instruction across many processes; debugger sessions are scriptable from test files (`; debug-script:`) |
| Live monitor, web dashboard, `top` | **Erlang's observer** and `etop`; Unix `top`; JVM JMX/VisualVM | Served by the first worker thread, polling a socket between time slices, so no extra thread; other workers pause briefly per request for a consistent snapshot; plain JSON plus a text view |
| An optimizer over the bytecode (`mlwsvm opt`, `--opt`): folding, propagation, dead code, branches, peephole, inlining, tail calls; -O levels, each pass on or off by name, a report per pass and function | LLVM's `opt` and pass manager, and its `-O` levels; GCC's `-f`/`-fno-` flags; McKeeman's peephole optimization (1965); Wegman and Zadeck's constant propagation; ProGuard and R8, which optimize JVM bytecode offline and report what they removed; Erlang's `beam_ssa` passes; CPython's peephole optimizer | It runs on the portable module before linking, so the same optimizer works ahead of time and at every load, with results kept in the module cache; folding calls the interpreter's own arithmetic, so no folded value can differ from a run; the levels are defined by who can observe a pass (nothing but instruction counts at -O1, the tools at -O2, backtraces at -O3), because languages compiled to it read their frames. |
| Tests written in the VM's own assembly, with `; expect-*` comments | LLVM `lit`/FileCheck-style golden tests | Also run with a collection at every allocation (`make check-gc`), under ASan/UBSan, and on several threads under ThreadSanitizer. |
| A differential fuzzer (`mlwsvm fuzz`): random programs that always end, run in the interpreter and in each other mode (the JIT, the optimizer, GC stress and verification, and combinations), any difference reported and shrunk by deleting lines (M45) | Csmith (Yang, Chen, Eide and Regehr) and YARPGen, random programs for differential testing of C compilers; McKeeman's *Differential Testing for Software*; Zeller and Hildebrandt's delta debugging and C-Reduce; JavaScript engines' JIT fuzzers (Fuzzilli, jsfunfuzz), which compare a JIT with its interpreter | The reference is the VM's own interpreter, so every mode is checked against the same semantics rather than against another implementation; programs are generated per register role so that every instruction finds the type it expects, and a budget counted through loops and calls keeps each to a few milliseconds, about 300 programs a second in eleven modes on 18 cores. The generator is part of the binary, so a seed names a program on every host. |
| `make release-check` (`tests/release-check.sh`): every check, the fuzzer, and the compilers' suites in every mode, before a tag (M45) | Release checklists of compiler projects (GCC's and LLVM's release testing, Rust's crater runs over the ecosystem) | One script per machine, the compilers' suites in thirteen modes (three under AddressSanitizer) as the ecosystem run. |
| Bump allocation into the free lines of each process's chunks, a collection marking the lines its survivors are on, no sweep of the dead, and the objects that own memory on lists of their own; marking makes an object old, and a major collection flips the mark bit's meaning instead of clearing marks; the allocator's fast path inline in C and in the JIT's machine code, with one limit for the collection threshold and the heap quota (M46) | Blackburn and McKinley's Immix (mark-region collection with lines and holes, PLDI 2008) and its generational sticky form; sticky mark bits (Demers and others, POPL 1990); changing the mark's meaning at each collection instead of clearing it (the mark state of Immix in MMTk; Multicore OCaml's colours, rotated at each major cycle); bump allocation in every copying nursery (HotSpot's TLABs, BEAM's process heaps) | Immix without evacuation: objects never move, since natives that call back into VM code hold pointers across collections and maps hash objects by address. Lines are 128 bytes in chunks that start at 1 KiB, so a small process stays small; a line pinned by an old object that has died waits for the next collection of the whole heap. |
| One platform layer for POSIX and Windows behind small integer descriptors, with a readiness poller on Windows too: sockets on an I/O completion port through the AFD driver's poll requests (M71; `WSAPoll` before), a completion posted to wake it, children's output as overlapped named pipes waited for by zero-byte reads on the same port (M86), other pipes in `PIPE_NOWAIT` mode looked at every millisecond; the JIT's one x86-64 encoder serving both systems, the C functions its code calls declared `sysv_abi` (M47) | libuv (I/O completion ports; overlapped named pipes for a child's stdio, waited for by zero-byte reads; a thread per read of another pipe); wepoll (epoll's interface over the AFD driver); Python's asyncio selector loop on Windows (`select` and a socketpair to wake it) and `os.set_blocking` on pipes (`PIPE_NOWAIT`, Python 3.12); LuaJIT and Cranelift, which emit Windows' own convention | Readiness rather than completion, so the scheduler's event loop is the same on every system; and no second calling convention in the JIT: Clang converts at each boundary between C and the machine code. |
| A closure call compiled for the function its call site called last: the site records its callee as it runs; the machine code checks the closure's function against it and makes a direct call, anything else taking the general call at the same machine call (M48) | Inline caches (Deutsch and Schiffman's Smalltalk-80, 1984) and polymorphic inline caches (Hölzle, Chambers and Ungar, ECOOP 1991); call-site type profiles in HotSpot and V8 that their compilers speculate on, with deoptimization on a miss | A monomorphic guess fixed when the function is compiled, with no deoptimization: a miss is the general call, which the interpreter makes anyway, so the guess costs one compare when wrong and nothing has to be undone. |
| Frames of functions that never touch the data stack keep no stack pointers: the loader marks them, their pushes and pops skip SP and FP, and unwinding stays right because they leave both as they found them (M49) | Leaf-procedure and frame-pointer omission in native ABIs (SPARC's and MIPS's leaf routines, GCC's `-fomit-frame-pointer`, shrink-wrapping); Lua and the JVM, whose frames carry only what their bytecode needs | Decided per function from its instructions at load, for an interpreter's and a JIT's frames at once, and kept in the frame so that a pop needs no look at the function. |
| A block reached only by falling through the branch before it keeps that block's machine registers and what was checked of its object registers; the branch writes back but nothing is loaded again (M50) | Extended basic blocks and superblocks (Hwu and others' superblock scheduling, 1993), register allocation across a trace in LuaJIT and in trace compilers (Dynamo, TraceMonkey); V8's Sparkplug and BeamAsm, which keep nothing in registers across instructions | A static choice with no profile: the branch's two ways cost what they did, and only the fall-through block saves its loads, so a usually-taken branch is never made slower. |
| A value's two words (a tag or number word and the word after it) loaded as one form, with one check of the object; the string builders quickened and called from machine code; a full list grown in an out-of-line stub that reloads the register cache (M51) | Superinstructions (Proebsting; Ertl and Gregg's static superinstructions), here chosen from the compilers' output and fused only in the JIT; slow paths placed out of line (V8's and HotSpot's deferred code, LuaJIT's side exits that rejoin); Lua's `gsub` | The pair is fused by a static rule, not a profile: two loads of different banks can only both succeed on a list, so the guard is exact without speculation. |
| A compiled return leaving its results in machine registers for the continuation of the call that made the frame, a frame's flags written as one word, and the callee's window cleared with paired stores (M52) | Native calling conventions (results in `rax`/`rdx`, `x0`–`x7`); LuaJIT's and V8's register-based returns between compiled frames; `memset` with wide stores | Results in registers only where the return is known to be compiled: the continuation is reached only from a compiled return, so the interpreter's returns, and every frame's layout, are unchanged. |
| Weak references, weak maps and finalizers in a per-process, generational, non-moving collector: weakness a mode of the ordinary map, so no instruction or compiled form knows of it; a finalizer's record an object of the heap, so it counts against the quota; the closure run as a callback at the safepoint after the collection | Lua's weak tables, ephemerons and `__gc`, with the same order of clearing (weak values before finalizers, weak keys after); Hayes's ephemerons; Java's `WeakReference` and finalization; JavaScript's `WeakMap` and `FinalizationRegistry` | Generations need no barrier of their own: a weak reference is promoted with its target, and a weak map is remembered as any map is. Strings and numbers stay, as in Lua. |
| Finalizers held until the program asks, so a runtime written for the VM is never interrupted by code of its own language (M68) | Dybvig, Bruggeman and Eby's guardians (Chez Scheme), which hand dead objects to the program when it asks; Java's `ReferenceQueue.poll`; JavaScript's `FinalizationRegistry`, whose callbacks never interrupt running code, and its `cleanupSome` proposal | The same records and order as finalizers that run between instructions; only when they run is the program's choice, one setting per process. |
| The debuggers taking the run command's VM options, from their command lines and a DAP launch, but optimizing only when asked there (M69) | GDB's `--args`; the launch configurations of VS Code's debuggers (Node's `runtimeArgs`, debugpy's `python` and `args`); `go build -gcflags=all="-N -l"` for Delve and `-O0 -g` for GDB, since optimized code loses its variables | The one setting of the environment the debuggers pass over is the one that would take the names away. |
| Links, monitors and request/reply across VMs as on one: each side keeps the other's pids, control frames in order with messages, and a lost connection as the failure of everything over there (M70) | Erlang's distribution, where links and monitors cross nodes and a lost connection is `noconnection`; `gen_server:call` and its `nodedown`; Akka's remote DeathWatch | The reply still goes to the caller's reply slot, never its mailbox, across nodes as on one, and a call to a process that is not there is answered at once by its node, not left to time out. |
| Globals carried across a reload by name and size, per process and at its first use of the new version; an old struct object made one of the new type by field name (M72) | Common Lisp's `update-instance-for-redefined-class`, which keeps slots by name; Smalltalk's instance migration when a class changes; Erlang's `code_change`, where the program converts its own state | Nothing to write for the common case: what keeps its name and size keeps its value, and what changes starts as the new file says. A conversion only for struct objects, and only when the program asks. |
| A program's own log and events beside the VM's state in one read-only page, kept only while it is watched (M73) | Erlang's Observer and `logger`; the JVM's JMX consoles; structured logging | Nothing the program writes elsewhere is taken; the log is a channel it chooses to use, and without a monitor a call to it is checked and dropped. |
| Programs started and stopped from a web page in a VM that stays up, each kept within capabilities and quotas of its own, from a directory it cannot leave (M75) | Erlang/OTP's application controller in a running node; Java application servers' managers; process managers (PM2, systemd) | The programs are in one VM, sharing its scheduler and modules, yet each starts within its own bounds, set when it starts, not in its code. |
| One executable: a copy of the runtime with the program's modules after it, found from the file's last bytes | Self-extracting archives and `.zip` files, whose directory is at the end; Deno's `compile`, Bun's `--compile` and Node's single-executable applications; Lua's `srlua` | No extraction and no separate launcher: the same `mlwsvm` is the tool and the runtime, and the loader finds modules in the file before anywhere else. |
| Reloading a module under running processes, in a VM with a JIT: Erlang's rule for what moves, checked at link time against every user, with machine code left alone | Erlang's two versions of a module and its rule that a qualified call moves while a local call stays; Smalltalk and Lisp images; the JVM's `HotSwap`, which keeps a method's signature | Compiled calls already read their callee's entry, so clearing it sends them to the interpreter's call: there is no code patching or deoptimization. An old version is freed only when nothing can run it (M65): a purge walks every process's heap as the write barrier's check does, and never kills a process, unlike Erlang's `code:purge`. |
| A debugger for editors over the Debug Adapter Protocol, for whatever language compiled to the VM | Microsoft's Debug Adapter Protocol; GDB's and LLDB's adapters; source maps in browsers, whose role `.loc` plays | One adapter serves every language on the VM, in its own source files, since compilers already mark their lines with `.loc`. Processes appear as the protocol's threads. |
| A program's own formatter for its variables, a value split across registers of several banks, run by the debugger in a sandboxed process (M66) | GDB's pretty-printers and LLDB's data formatters (in Python, beside the debugger); Visual Studio's Natvis; DWARF's `DW_OP_piece` for a variable in pieces | The formatter is the language's own code, in its runtime, not a script for the debugger, so every debugger front end shows values as the language would. Process isolation makes calling it safe: it runs on copies, cannot reach the stopped program or any other process, and is bounded by quotas. |
| Large strings sent by reference between otherwise isolated heaps: promoted in place on the first send, reference-counted by heaps rather than by references, each heap finding out what it still holds at its own major collection | Erlang's reference-counted binaries, which live off the process heap; constant pools | Nothing about a string changes for those who read it, and there is no new object kind to a program: it takes the path constants already take. The count changes only when a heap gains or loses the string, not with each reference. |
| TLS on a connection, loaded at run time, with waits in the same poller as plain sockets | OpenSSL's non-blocking `SSL_read` and `SSL_write`, which say what to wait for; Erlang's `ssl`, Python's `ssl`, Go's `crypto/tls`; `curl`'s loading of TLS back ends | A connection stays one handle whatever it carries, so `fs.read_line` is the same on both, and TLS can start after plain traffic. No build or run dependency: the library is found by name when first asked for. |
| Regular expressions in linear time, inside the VM, with their work charged to the process's quota | Thompson's NFA construction; Pike's VM, and Russ Cox's account of both; RE2, Go's and Rust's `regexp`, whose syntax it follows, and Go's `(x+)?` for loops over what can match nothing | In a VM built to run untrusted code in sandboxes, a pattern must not be a way to stall it, or to spend CPU that a quota cannot see. |
| Capabilities per process, by name, deny by default, inherited and only narrowed, beside quotas and handles passed by message | Deno's `--allow-read`, `--allow-net` and the like; OpenBSD's `pledge` and `unveil`; Capsicum; object capabilities, as in E and the handles of Erlang-style message passing | It belongs to the process, not the program, so one VM can run trusted and sandboxed processes side by side, and a handle sent to a sandbox carries its own authority. |
| Processes on several VMs behind the same `send`: a pid names its node, and each connection is a pair of ordinary processes in the VM's own assembly | Erlang's distribution: node names, cookies, `spawn/4`, monitors across nodes; its external term format, which the encoding resembles | The VM adds only routing to `send`, an encoding and a challenge; the protocol is a library, in the language compilers target, so it can change without touching C. A challenge with an HMAC marked by role, not Erlang's MD5. |
| A generic server that moves its state to each new version of its callback module, in mailbox order, without being suspended (M94) | Erlang/OTP's `gen_server` and `code_change/3`, which the release handler drives by suspending the process (`sys:suspend`), changing its code, and resuming it | The reload's notice is a message, put in the mailbox while the world is stopped, so it comes after everything sent before the reload and before anything the new code sends: the mailbox's order does what suspending does. A library follows a program's module through a closure of it, without naming it. |

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
| M31 | Address-order sweeping (Boehm–Demers–Weiser, Immix); a generational collector with a remembered set (Ungar's generation scavenging, Lua 5.4's generational mode) and definite assignment (the JVM verifier) for which registers a call must clear, both built and measured but not kept |
| M32 | Superinstructions and superoperators (Proebsting; Ertl and Gregg), picked from instruction-pair counts of compiled Lua; element operands with a constant offset, as in addressing modes with a displacement |
| M33 | Definite assignment (the JVM verifier) to decide which registers a call clears, where M31's attempt had been undone by GCC expanding a small `memset` as `rep stos`; CPython's per-instruction inline caches for the call plans; Lua's interpreter as the measure, counted with its own instruction hook |
| M34 | GNU `as`'s `.loc` and DWARF line tables; the JVM's `LineNumberTable`; JavaScript source maps |
| M35 | BEAM's small, growing processes; Go's scheduler (one spinning thread at a time, which wakes the next) |
| M36 | Python's `__pycache__` and hash-checked `.pyc` files; ccache and Go's content-addressed build cache |
| M37 | The Unicode Standard's algorithms and multistage tables; ICU, utf8proc, Go's `unicode` and Rust's `unicode-segmentation` |
| M38 | Cargo's and npm's version constraints; Erlang/OTP applications; JAR manifests and the Java module system's exports; Go modules |
| M39 | `dlopen` and `dlsym`; Erlang's `code:load_file`; Java's class loaders; Lua's `require` |
| M40 | Erlang/OTP applications in one node; shell process groups and `wait`; Procfile runners; Node.js's module cache keyed by file, against Python's `sys.modules` keyed by name; stdio line buffering and Erlang's group leader for whole lines of output |
| M41 | LLVM's `opt` and `-O` levels; GCC's `-fno-*` flags; peephole optimization (McKeeman); Wegman–Zadeck constant propagation; ProGuard/R8 (offline bytecode optimizers with reports); CPython's peephole optimizer |
| M42 | BEAM's `is_tagged_tuple` and `call_only`/`call_last`, measured against Erlang/OTP on the Erlang compiler's benchmarks; NaN-boxing's floats kept as integer bits |
| M43 | BeamAsm (Erlang/OTP's JIT), V8's Sparkplug, and the hotness counters of HotSpot and LuaJIT; copy-and-patch (Xu and Kjolstad; CPython 3.13's JIT) weighed and not taken; Apple's `MAP_JIT` and W^X |
| M44 | Sticky mark bits (Demers and others), Ungar's generation scavenging and Lua 5.4's generational mode, now kept; natural loops (Aho, Lam, Sethi and Ullman); Java's lazily cached `String.hashCode`; HotSpot's heap verification |
| M45 | Csmith and YARPGen (random programs for differential testing), McKeeman's differential testing, delta debugging (Zeller and Hildebrandt; C-Reduce), Fuzzilli's JIT-against-interpreter comparison; release checklists (crater runs) |
| M46 | Immix (Blackburn and McKinley), lines and holes for bump allocation without moving; sticky mark bits, now with a flipped mark sense for collections of the whole heap |
| M47 | libuv and wepoll (readiness and completion on Windows); Python's asyncio selector loop and its socketpair wake-up; `PIPE_NOWAIT` pipes (Python 3.12's `os.set_blocking`); Clang's `sysv_abi`; MSYS2 |
| M48 | Inline caches (Deutsch and Schiffman) and polymorphic inline caches (Hölzle, Chambers and Ungar), call-site profiles that HotSpot's and V8's compilers speculate on |
| M49 | Leaf procedures and frame-pointer omission in native ABIs (SPARC, MIPS, `-fomit-frame-pointer`), shrink-wrapping |
| M50 | Extended basic blocks and superblocks (Hwu and others), register allocation along traces (LuaJIT, Dynamo); sampling profilers mapped to JIT code (Linux `perf` map files) |
| M51 | Superinstructions (Proebsting; Ertl and Gregg), out-of-line slow paths that rejoin (V8 deferred code), Lua's `string.gsub` |
| M52 | Register return conventions of native ABIs, register-based returns between compiled frames (LuaJIT, V8), wide-store `memset`; Valgrind's callgrind for counting a JIT's instructions |
| M53 | Lua's weak tables and `__gc`, Hayes's ephemerons, Java's `WeakReference` and finalization, JavaScript's `WeakMap` and `FinalizationRegistry` |
| M54 | Self-extracting archives; Deno's `compile`, Bun's `--compile`, Node's single-executable applications, Lua's `srlua` |
| M55 | Erlang's code replacement and its two versions of a module; Smalltalk and Lisp images; the JVM's `HotSwap` |
| M56 | The Debug Adapter Protocol; source maps |
| M57 | Erlang's reference-counted binaries |
| M58 | OpenSSL's non-blocking I/O; Erlang's `ssl`, Python's `ssl`, Go's `crypto/tls` |
| M59 | Thompson's construction, Pike's VM, Russ Cox's articles, RE2, Go's and Rust's `regexp` |
| M60 | Deno's permissions, OpenBSD's `pledge` and `unveil`, Capsicum, object capabilities |
| M61 | Erlang's distribution, cookies and external term format; HMAC challenge and response |
| M62 | Erlang's distribution over TLS (`inet_tls_dist`) |
| M63 | DWARF's location lists (`DW_AT_location`), the JVM's `LocalVariableTable` |
| M64 | Differential testing against a reference implementation (McKeeman); Csmith; Python's `re` as the oracle |
| M65 | Erlang's `code:soft_purge` and `check_process_code`, which refuse to purge code a process still runs or a fun still refers to; the JVM's class unloading, once a class loader is unreachable |
| M66 | GDB's pretty-printers, LLDB's data formatters, Visual Studio's Natvis, Python's `repr` in debuggers; DWARF's composite locations (`DW_OP_piece`) for a variable in several registers |
| M67 | Python's `Pattern.groupindex`, PCRE's name table, Erlang's `re:inspect(MP, namelist)` |
| M68 | Chez Scheme's guardians (Dybvig, Bruggeman and Eby, 1993); Java's `ReferenceQueue`; JavaScript's `FinalizationRegistry` and `cleanupSome` |
| M69 | GDB's `--args`; VS Code's launch configurations (Node's `runtimeArgs`); building for Delve and GDB without optimization |
| M70 | Erlang's links, monitors and `gen_server:call` across nodes (`noconnection`, `nodedown`); Akka's remote DeathWatch |
| M71 | wepoll, libuv's `uv_poll` and Rust's mio (`IOCTL_AFD_POLL` on a completion port); Windows' I/O completion ports |
| M72 | Erlang's `code_change` and record upgrades; Smalltalk's instance migration on class change (`updateInstances`); the JVM's HotSwap, which keeps fields; Common Lisp's `update-instance-for-redefined-class` |
| M73 | Erlang's Observer (process, application and table views) and `logger`; the JVM's JMX and VisualVM; Node's inspector; structured logging (JSON lines) |
| M74 | Jupyter's token in the URL; the Docker and Kubernetes APIs' bearer tokens; the defences against DNS rebinding and cross-site requests of local dev servers (Host and Origin checks, as webpack's and Vite's) |
| M75 | Erlang/OTP's release handling and application controller (`application:start`); application servers (Tomcat's manager, deploying to a running JVM); PM2 and systemd for starting and stopping services |
| M76 | Erlang's `code:purge` (kills what runs old code) beside `code:soft_purge` (refuses) |
| M77 | PM2's and systemd's restart of a service with its arguments; Erlang Observer's per-application views |
| M78 | Lisp's `intern`, Java's `String.intern`, Ruby's `to_sym`, Erlang's atom table, Lua's interned strings |
| M79 | Erlang's export entries (a fully qualified call goes to the current version); the JVM's `invokedynamic` call sites and `MethodHandle`s; Lisp's calls through a symbol's function cell |
| M80 | Erlang's literal areas, which a module's new version replaces whole; Common Lisp's `defvar` (keeps a value when the file is loaded again) against `defparameter` (sets it afresh) |
| M81 | Erlang, where an exception a process does not catch is logged as an error, and an exit signal (`exit(Pid, kill)`) ends it without one |
| M82 | Erlang's export entries, which a new version without the function leaves pointing at a stub that raises `undef`, until a version exports it again |
| M83 | Erlang's `exit(Pid, kill)` and `exit/2` across nodes, carried by the distribution protocol's exit signals |
| M84 | Erlang's node names (`name@host`, `node()`, `nodes()`) and the distribution handshake's exchange of names; its refusal of a second node of the same name |
| M85 | Erlang's `net_kernel:monitor_nodes` (`nodeup`, `nodedown`) and `monitor_node` |
| M86 | libuv's `uv_spawn` stdio pipes (an overlapped named pipe for our end, a plain handle for the child's) and its zero-byte reads on a completion port |
| M87 | curl's `--cacert`, which adds an authority but never turns checking off by itself |
| M88 | Erlang's funs in the external term format (`NEW_FUN_EXT`: module, index, a hash of the module's code, free variables), which a node without that code can hold and pass on but not call |
| M89 | Erlang's `epmd` (a name service per host, a registration held by the node's connection to it, `-sname` and `-name`) |
| M90 | Erlang's atoms in the external term format (`ATOM_UTF8_EXT`), made atoms again where they arrive |
| M91 | Erlang's code loading on demand (`code:ensure_loaded` when a call or a fun needs a module), and the external term format's `NIL_EXT`, the empty list wherever it arrives |
| M92 | Java's object serialization, which numbers each object it writes and refers back to it (`TC_REFERENCE`), and Python's `pickle` memo; Erlang's external term format, which keeps no sharing (a term shared twice is sent twice) and has no cycles to keep |
| M93 | Erlang's `is_process_alive/1`, which takes only a local pid (`rpc:call` asks another node), and its distribution's request and reply over the connection |
| M94 | Erlang/OTP's `gen_server` and `code_change/3`, with `sys:suspend` and `sys:change_code` from the release handler |
| M95 | Erlang's distribution, which encodes a message in the sending process (`erts_dsig_send`) into the connection's output queue, written by the port in as few writes as it can, `writev` batching in network servers |
| M96 | Erlang's `busy_dist_port`: a process sending on a distribution channel whose output queue is past `dist_buf_busy_limit` (1 MiB by default) is suspended until the queue drains |
| M97 | Erlang/OTP's schedulers, which poll for I/O themselves when they run out of work (`erts_check_io`), and Go's runtime, whose idle Ps call the network poller (`netpoll`) in `findrunnable` and whose `sysmon` looks when every P is busy |
| M98 | Erlang's `net_kernel:node_info/1`, whose `in` and `out` count a connection's messages, `inet:getstat/1` on its socket for bytes, and `observer`'s view of nodes |
| M99 | Erlang's `{Name, Node} ! Msg`, sent as a `REG_SEND` control message that the receiving node resolves in its registry |
| Platforms | The BSDs' `sysctl` executable paths and `EAI_NODATA`; Debian multiarch and QEMU user-mode emulation for testing 32-bit and big-endian hosts; WebAssembly's little-endian memory; ctypes' and Rust's host-sized C types; MinGW's `__mingw_setjmp`, which does not unwind, for the JIT on Windows arm64 |

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
- Alan Demers, Mark Weiser, Barry Hayes, Hans Boehm, Daniel Bobrow and Scott Shenker, *Combining Generational and Conservative Garbage Collection: Framework and Implementations*, POPL 1990; David Ungar, *Generation Scavenging*, SDE 1984.
- Stephen M. Blackburn and Kathryn S. McKinley, *Immix: A Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance*, PLDI 2008.
- Alfred Aho, Monica Lam, Ravi Sethi and Jeffrey Ullman, *Compilers: Principles, Techniques, and Tools*, 2nd edition, 2006 (natural loops).
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
- Xuejun Yang, Yang Chen, Eric Eide and John Regehr, *Finding and Understanding Bugs in C Compilers*, PLDI 2011 (Csmith); Vsevolod Livinskii, Dmitry Babokin and John Regehr, *Random Testing for C and C++ Compilers with YARPGen*, OOPSLA 2020; William M. McKeeman, *Differential Testing for Software*, Digital Technical Journal 10(1), 1998; Andreas Zeller and Ralf Hildebrandt, *Simplifying and Isolating Failure-Inducing Input*, IEEE TSE 28(2), 2002; John Regehr and others, *Test-Case Reduction for C Compiler Bugs*, PLDI 2012 (C-Reduce); Samuel Groß, *FuzzIL: Coverage Guided Fuzzing for JavaScript Engines* (Fuzzilli), 2018.
- Haoran Xu and Fredrik Kjolstad, *Copy-and-Patch Compilation: A Fast Compilation Algorithm for High-Level Languages and Bytecode*, OOPSLA 2021; Brandt Bucher, PEP 744 *JIT Compilation* (CPython 3.13).
- Lukas Larsson and John Högberg, the Erlang/OTP documentation and talks on BeamAsm, the JIT of Erlang/OTP 24; Leszek Swirski and Toon Verwaest, *Sparkplug — a non-optimizing JavaScript compiler*, the V8 blog, 2021.
