# MLWSVM Tooling

Tools for seeing what a program is doing: resource statistics, a profiler, an
instruction tracer, an interactive debugger with an adapter for editors, and a live
monitor with a web dashboard and a `top`-style terminal view. For shipping one, a
single executable. And, for the VM itself, a differential fuzzer and the checks to
run before a release.

| Tool | Command | Overhead when off |
|---|---|---|
| Statistics | `mlwsvm run --stats prog.masm` | none |
| Profiler | `mlwsvm run --profile [--flame FILE] prog.masm` | none |
| Tracer | `mlwsvm run --trace FILE\|- [--trace-limit N] prog.masm` | none |
| Debugger | `mlwsvm debug prog.masm` | — |
| Debugger for editors | `mlwsvm dap` (Debug Adapter Protocol) | — |
| Debugger for a running process | the monitor's dashboard: **Debug**, or `/debug#pid=N` | none |
| Monitor + dashboard | `mlwsvm run --monitor [HOST:]PORT [--hold] [--monitor-actions] prog.masm` | none |
| A VM that stays up | `mlwsvm serve --programs DIR [--monitor [HOST:]PORT]` | — |
| Terminal view | `mlwsvm top [https://]HOST:PORT [--ca FILE] [--once] [--interval MS] [--log]` | — |
| Name service for nodes | `mlwsvm names [--port N]` | — |
| VM options | `mlwsvm run --max-frames N --quota heap=64M --cap read=/data ... prog.masm` | — |
| Several programs | `mlwsvm run a.masm args --also b.masm args ...` | — |
| Native code | `mlwsvm ext list\|install\|remove\|allow-ffi\|deny-ffi` ([EXTENDING.md](EXTENDING.md)) | — |
| Bindings generator | `mlwsvm bindgen [-l LIB] [-p PREFIX] [-o OUT] header.h` | — |
| Libraries | `mlwsvm lib build DIR \| info FILE \| list` | — |
| One executable | `mlwsvm bundle [-o out] [--with MODULE] [VM options] prog.masm` | — |
| Optimizer | `mlwsvm opt -O2 prog.masm`, or `mlwsvm run --opt 2 prog.masm` ([OPTIMIZER.md](OPTIMIZER.md)) | none |
| JIT | `mlwsvm run --jit on [--jit-report -] prog.masm` ([JIT.md](JIT.md)) | none |
| Differential fuzzer | `mlwsvm fuzz [-n N \| --time S]`, `--reduce FILE` | — |
| Before a release | `make release-check` (`tests/release-check.sh`) | — |

The profiler, tracer and debugger use one per-instruction hook, which is
installed only while one of them is active. A program run under them is
interpreted (the JIT compiles nothing) on a single worker thread, so traces
and profiles are deterministic and the debugger can stop the whole program.
The monitor needs no hook: the first worker thread polls its socket about once
a millisecond between time slices, and while it waits when idle, so the
monitor needs no thread of its own. To answer a request it
briefly pauses the other worker threads at their next time-slice boundary ("stop
the world"), so the numbers it reports are a consistent snapshot.

---

## Module cache

`mlwsvm run`, `test` and `debug` assemble a `.masm` file once and keep the result,
so that later runs load it without assembling: a 940 KB program compiled from Lua
starts in 10 ms instead of 38 on the M5. An entry is found by a SHA-256 of the
source text, the default module name (from the file name) and the running
`mlwsvm` executable, so the same program written to a fresh temporary file
each time still finds it, and a rebuilt VM never uses another build's entries.
An entry also lists every file the source included, with a hash of its
contents, and what their names were resolved against (the working directory,
the source's directory, the program's directory and the library path): if any of them differs, the source
is assembled again and the entry replaced. Backtraces name the file that was
run, wherever the entry was made.

With `--opt` ([OPTIMIZER.md](OPTIMIZER.md)), each module's optimized form is
kept as well, found by a SHA-256 of the executable, the passes and the module
as loaded (but not its file's name, so a program a compiler writes to a fresh
temporary directory each time is still found). That holds for every module,
from a `.masm` file, a `.mlwso` file or a library archive, so a module is
optimized once per set of passes. A run
asked for `--opt-report` optimizes afresh, so that its report says what the
passes did.

| Setting | Effect |
|---|---|
| `MLWSVM_CACHE=DIR` | where the entries go (default: `$XDG_CACHE_HOME/mlwsvm`, else `~/.cache/mlwsvm`; on Windows `%LOCALAPPDATA%\mlwsvm`) |
| `MLWSVM_NO_CACHE=1`, `mlwsvm run --no-cache` | assemble every time, and keep nothing |

The directory is created usable by its owner only, and is not used at all if it
is writable by anyone else or owned by another user (the checks native code
gets: a module can declare foreign functions). Entries are written to a
temporary file and renamed into place, so runs at the same time never see half
of one; past 512 entries, the oldest are removed. `mlwsvm asm` and `dis` never
use the cache.

## Several programs: `--also`

```
$ mlwsvm run server.masm 8080 --also client.masm localhost 8080 --also client.masm localhost 8080
```

Each `--also` starts another program in the same VM, with the arguments after
it as its `os.args` (so `--also` cannot itself be one of a program's
arguments); the same file may appear more than once. Every program is loaded
before any starts, and the run ends when the last program's `main` ends
([ABI.md §1](ABI.md#1-processes) has the rules). An uncaught error in one
program is reported with its file name, and the others run on. `--stats` and
`--profile` count every process, and `--stats`'s figures for `main` are the
first program's.

Each program finds its own modules in its own directory first, so programs
from different directories can have modules with the same names: with
`mlwsvm run a/main.masm --also b/main.masm`, both programs are `main` and an
`.import util` in each gets that program's `util`. Modules on the library
path, and library versions, are shared by all programs. Every program's
`.requires` are taken into account before any library version is chosen, so
the order of the files does not matter. [ASSEMBLY.md §8](ASSEMBLY.md#8-modules-and-libraries)
has the rules.

## Libraries: `mlwsvm lib`

```
$ mlwsvm lib build json5/                 # json5/library.masm and the modules under json5/
json5-1.2.0.mlwsl: library json5 1.2.0, 3 modules (public: json5, json5.parse)
$ mlwsvm lib info json5-1.2.0.mlwsl       # manifest, requirements and modules
$ mlwsvm lib list -L ~/mlws-libs          # the archives on the library path
```

`lib build DIR [-o FILE] [-L dir]` assembles `DIR/library.masm`, the manifest
([ASSEMBLY.md §8.1](ASSEMBLY.md#81-libraries)), and every module under `DIR`,
named by its path (`json5/parse.masm` is `json5.parse`; directories starting
with `_` or `.` are skipped, for include files). Each module must belong to the
library (its name is the library's or starts with it and a dot), each `.public`
module must exist, and the archive goes to `name-version.mlwsl` unless `-o`
says otherwise. To use it, put it in a directory of the library path (`-L`,
`--lib-path`, `$MLWSVM_PATH`, or `lib/` next to `mlwsvm`) or in the
program's directory; several versions can sit side by side. A damaged archive on the path is reported and ignored.

## One executable: `mlwsvm bundle`

```
$ mlwsvm bundle -o wc --opt 2 --jit on examples/wordcount.masm
wc: wordcount, 1 module, 1288493 bytes
$ ./wc
```

`bundle` loads a program as `run` would, with all it imports from the program's
directory, the library path and library archives, and writes one executable:
a copy of `mlwsvm` with the modules after it. It needs nothing beside it: not
`lib/`, the assembler or the module cache. Its whole command line is the
program's (`os.args`).

* **VM options** given to `bundle` are kept and used at every start, with
  `MLWSVM_<NAME>` still overriding them. `--opt` is the exception: the
  optimizer runs when bundling, on the modules that go into the file.
* **`--with MODULE`** adds a module that the program only asks for with
  `module.load`. A `module.load` of a module that is not in the bundle looks on
  the library path, as usual.
* **Modules are found by name**, so two modules of the same name from
  different directories cannot be in one bundle.
* **Not bundled:** native extensions (`lib/ext`) and the C libraries an
  `.extern` names. They are looked for as usual.
* **macOS:** the result runs, but `codesign --verify` reports that it fails
  strict validation, since the program comes after the signed part. A bundle
  cannot be notarized. Do not name it `*.app`: macOS kills any program file so
  named as it starts (there, an `.app` is an application bundle, a directory),
  and `mlwsvm bundle` warns of it.

A bundle is for the machine type and system of the `mlwsvm` that wrote it.
The layout is in [BINARY_FORMAT.md](BINARY_FORMAT.md#bundles).

## VM options

Settings for the VM itself. Each can be given on the command line (`mlwsvm run`,
`test`, `debug` and `dap`), as an environment variable (`MLWSVM_` plus the name in
capitals with `_` for `-`, e.g. `MLWSVM_MAX_FRAMES=500000`), or in a test file as
`; option: --max-frames 500000`. The command line overrides the environment, and
a test file's own lines override both.

| Option | Default | Meaning |
|---|---|---|
| `--threads N` | one per core | worker threads (1 to 256) |
| `--stack-words N` | 131072 | each process's data stack, in 8-byte words |
| `--max-frames N` | 100000 | call depth limit (`E_CALL_DEPTH`) |
| `--slice N` | 2000 | time slice, in instructions, before another process gets a turn |
| `--gc-min BYTES` | 64K | the young generation's starting size: what a process allocates between minor collections (it grows while too much of it survives) |
| `--env FILE` | (none) | a file of `name = JSON` lines that every program's environment starts with; again for more ([STDLIB.md](STDLIB.md#environments-env)) |
| `--cap LIST` | everything | what every process may reach, e.g. `read=/data,connect=example.org:443`, or `none` ([STDLIB.md](STDLIB.md#sandboxing)) |
| `--quota LIST` | unlimited | starting limits for every process, e.g. `heap=64M,raw=1G,mailbox=1000,insns=5000000,foreign=0,os=0`; any subset, `unlimited` resets one |
| `--ext-path DIR[:DIR]` | (none) | trusted native-extension directories, searched before `lib/ext` ([EXTENDING.md](EXTENDING.md)) |
| `--lib-path DIR[:DIR]` | (none) | more library directories (for modules and library archives), searched after the program's own |
| `--opt LIST` | `0` (off) | optimize every module as it is loaded: a level `0` to `3`, and passes to add or (`-name`) leave out, e.g. `2`, `3`, `3,tailcall`, `fold,dce` ([OPTIMIZER.md](OPTIMIZER.md)) |
| `--opt-report FILE\|-` | (none) | when the run ends, write the optimizer's report of the modules it loaded to FILE (`-`: stderr) |
| `--jit off\|on\|all` | `off` | machine code for each function once it is hot (`on`), or for every function as it is loaded, entered at every block (`all`, for tests); `0` and `1` too ([JIT.md](JIT.md)) |
| `--jit-hot N` | 1000 | with `--jit on`, the calls and loop iterations after which a function is compiled |
| `--jit-report FILE\|-` | (none) | when the run ends, write what the JIT compiled, and where its machine code stopped for the interpreter, to FILE (`-`: stderr) |

Sizes take an optional `K`, `M` or `G` suffix (powers of 1024). A bad value is
an error (exit code 2) on the command line, a failing test in a test file, and a
warning that is otherwise ignored in the environment.

```
$ mlwsvm run --max-frames 1000000 deep_recursion.masm
$ MLWSVM_QUOTA=heap=256M,insns=10000000000 mlwsvm run untrusted.masm
```

Three environment variables are for testing the VM itself:
`MLWSVM_GC_STRESS=1` (or `mlwsvm run --gc-stress`) collects garbage at the
first safepoint after every allocation, after the next generic instruction or
at the next slice's start (every eighth collection of the whole heap, the rest
minor); `MLWSVM_GC_VERIFY=1` (or `--gc-verify`) checks each minor
collection against a walk of the whole heap, and aborts naming the object
that holds a young one without having been remembered (a missing write
barrier), with a young generation of 16 KiB unless `--gc-min` is given; `MLWSVM_NO_SLABS=1` allocates every object with
`malloc` instead of the small-object allocator
([PERFORMANCE.md](PERFORMANCE.md#the-small-object-allocator)); and
`MLWSVM_SLABS=1` uses that allocator even in an AddressSanitizer build, which
otherwise uses `malloc`, so that its leak check sees what dead objects own. For
development, `mlwsvm run --unsafe-native` (or `MLWSVM_UNSAFE_NATIVE=1`) loads
native code without the trust checks ([EXTENDING.md](EXTENDING.md#security)).

A frame whose code has a `.loc` position shows the line of the translated program
before the assembly line, as in `at shapes.L4$area (shapes.lua:12; shapes.masm:4)`
([ASSEMBLY.md §5.4](ASSEMBLY.md#54-source-positions-of-a-translated-program));
so do `Error.trace`, the tracer and the monitor (`source` and `source_line` in the
JSON API's process entries).

A backtrace from a runaway recursion stays short: runs of identical frames are
collapsed (`... repeated 99998 more times`), and a very deep stack shows only its
innermost 40 and outermost 10 frames.

## Statistics: `--stats`

Printed on stderr when the program ends: instructions and speed of `main`, call
depth, stack peak, raw-memory allocations and anything leaked, heap peak and final
size, objects allocated and collected, GC count and time, the memory the
small objects' chunks take and how much of it the last collection found in
use (DESIGN.md §11), processes spawned,
the most alive at once, time slices, and instructions across all processes.

## Profiler: `--profile`

```
$ mlwsvm run --profile examples/fib.masm
--- mlwsvm profile: 2860297 instructions ---

functions (instructions executed in the function itself; ...):
       %        insns    entries   cleared  function
  100.0%      2860292     635621       0/4  fib.fib
...
hot lines:
   22.2%       635621  examples/fib.masm:9 (fib.fib)
...
opcodes (fast = the share run by a specialised handler, chosen at load time):
       %        insns   fast  opcode
   22.2%       635622 100.0%  ret
...
hottest instructions on the general path:
       %        insns  instruction
    0.0%            1  ncall "io.print_i" (I0)                  ; fib.main line 22
...
```

* **functions**: instructions executed in each function itself (exclusive).
  `entries` counts how often the function's first instruction ran: calls, plus any
  loop that jumps back to it. `cleared` is how many words of its register window
  each call clears, of the window's size: its `R` registers and any `I`, `F` or
  `P` register it may read before writing it ([TARGETING.md](TARGETING.md#2-values-and-registers)).
  For the functions listed that read some register first, a further list
  names those registers: writing them first, or passing a constant instead of
  an unused one, shortens every call's clear.
* **hot lines**: instructions per source line.
* **hot program lines**: with `.loc`, instructions per line of the translated
  program, summed over every function whose code came from it.
* **opcodes**: the instruction mix, and for each opcode the share of executions
  that used a specialised handler ([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)).
  The handler is chosen when the code is loaded, from the instruction's shape; one
  that meets something unexpected at run time (a null, another type) still hands
  that execution to the general path, and is counted as fast.
* **hottest instructions on the general path**: the instructions that ran most
  often without a specialised handler, disassembled, with their function and
  line. For generated code, these are the shapes to change, or to report: see
  [TARGETING.md §12](TARGETING.md#12-what-runs-fastest).
* `--flame FILE` also writes sampled call stacks (one sample every 997
  instructions) in the *collapsed* format, `main;f;g 123`, which
  `flamegraph.pl` and speedscope read directly. A stack keeps its innermost
  128 frames, under one frame named `[deeper frames]` for the rest.

Counts are exact, not sampled; only the flame stacks are sampled.

## Tracer: `--trace`

Logs every instruction before it executes, with process, function, offset,
source line and disassembly. `-` means stderr, and `--trace-limit N` stops after
N lines.

```
$ mlwsvm run --trace - --trace-limit 3 examples/hello.masm
[1] hello.main+0 (examples/hello.masm:9)  call std.io.println ("Hello, MLWSVM!")
[1] std.io.println+0 (lib/std/io.masm:21)  ncall "io.print_s" (R0)
[1] std.io.println+1 (lib/std/io.masm:22)  ncall "io.nl" ()
```

## Debugger: `mlwsvm debug`

A gdb-style command line. The program starts stopped, so you can set breakpoints
first and then `run`. A `break` instruction in the code also stops the program
here (outside the debugger it does nothing).

```
mlwsvm debug [-L dir] [VM options] prog.masm|prog.mlwso
```

It takes `run`'s [VM options](#vm-options), e.g. `--max-frames 50000000` for a
deep recursion, from the command line and the environment, but optimizes only
for `--opt` on its own command line, not for `MLWSVM_OPT`: a function the
optimizer changes loses its variables' names. The JIT options have no effect,
since the debugger interprets.

| Command | Short | Action |
|---|---|---|
| `run` | `r` | start the program |
| `continue` | `c` | resume until a breakpoint or the end |
| `step` | `s` | execute one instruction, entering calls |
| `next` | `n` | execute one instruction, stepping over calls |
| `finish` | `fin` | run until the current function returns |
| `break SPEC` | `b` | breakpoint at `FUNCTION`, `module.FUNCTION`, `LINE` (main file) or `FILE:LINE`; the file may be one `.loc` names (the first instruction of each function from that line) |
| `delete [N]` | `d` | delete breakpoint N, or all |
| `info break` | `i` | list breakpoints with hit counts |
| `print REG` or `print NAME` | `p` | a register (`I3`, `F0`, `R1`, `P2`, `SP`, `FP`, `PID`), or a variable by name; objects are shown with their contents, and a variable with a formatter as it shows it |
| `locals` | `lo` | the current frame's named variables: `.local` names, parameters and aliases, each with its registers, shown by the program's formatter for them (`.debug_format`) where it has one ([ASSEMBLY.md §5.4](ASSEMBLY.md#54-source-positions-of-a-translated-program)) |
| `regs` | | every register in the current frame's window |
| `stack [N]` | `st` | the top N data-stack words, with their type tags |
| `bt` | `where` | backtrace |
| `list` | `l`, `dis` | disassembly around the current instruction |
| `procs` | `pr` | processes; `*` marks the one being inspected |
| `process PID` | | inspect another process |
| `quit` | `q` | stop the program and exit |

```
(mdb) break fact
Breakpoint 1 at demo.fact+0 (demo.masm:6)
(mdb) run
Breakpoint 1, process 1 at demo.fact+0 (demo.masm:6)
=>    0  line 6     ble.i64 I0, 1, .L5
(mdb) print R0
R0 = Point {x: 1.5, y: 0}
```

* Stepping is per **instruction**: one assembly line can be several instructions
  when it comes from a macro.
* With several processes, a stop selects the process that stopped. `step`/`next`
  follow the process being inspected, while the other processes keep running
  normally in between.
* Commands are read from stdin. When stdin is not a terminal, each command is
  echoed after the prompt, so a piped script produces a readable transcript.
* **Tests can drive it.** `; debug-script: CMD` lines in a `.masm` test run it
  under the debugger, and `; expect-stdout:` then checks the transcript (see
  `tests/tools/`).
* Not yet available: printing parameters or aliases by name (they are not kept
  in the binary), memory expressions such as `print SP[2]`, watchpoints, and
  attaching to a running VM.

## Debugger for editors: `mlwsvm dap`

`mlwsvm dap` speaks the [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
on stdin and stdout, so VS Code, Neovim, Emacs and others can debug MLWSVM
programs. It is `mlwsvm debug`'s engine, so a program is interpreted on one
worker thread, and it works for any language compiled to MLWSVM, in that
language's own files, wherever its compiler marked the lines with `.loc`.

* **Breakpoints** by file and line, in `.masm` files or files named by `.loc`. On
  a line without code, one moves to the next line that has some.
* **Stepping** by lines: step over, step in, step out.
* **Threads** are processes. All of them stop together, and each has its stack.
* **Variables:** a frame's **Locals**, the program's own names for its registers
  (`.local`, parameters, aliases) where it has any, then its registers by bank.
  A variable of several registers shows what the program's formatter
  (`.debug_format`) makes of them, and opens to show the registers.
  Structs, lists, arrays and maps
  open to show their contents, up to a thousand entries.
* **Watch and hover**: a variable's name, or a register's (`I3`, `R0`), in the selected frame.
* **Pause** stops a running program, within 65,536 instructions.
* **Output** comes as output events. The adapter has stdin, so a program being
  debugged reads end of input.

The `launch` request takes `program`, `args` (a list), `stopOnEntry`, and
`vmOptions`, a list of VM options as on the command line, e.g. `["--max-frames",
"50000000"]`; there is no `attach`. `mlwsvm dap` itself takes `-L` and the VM
options too, applied before a launch's own, and like `mlwsvm debug` ignores
`MLWSVM_OPT`.

`editors/vscode/` is a small VS Code extension that declares the debugger:
copy it into `~/.vscode/extensions/`, or run it from there with `code
--extensionDevelopmentPath`. With `mlwsvm` on your `PATH`, this goes in
`launch.json`:

```json
{ "type": "mlwsvm", "request": "launch", "name": "Debug", "program": "${file}" }
```

For Neovim's nvim-dap, the adapter is `{ type = 'executable', command = 'mlwsvm',
args = { 'dap' } }`.

For Vim (8.2 and later) or Neovim with [vimspector](https://github.com/puremourning/vimspector),
put this in `.vimspector.json` at the top of the project, then `:call
vimspector#Launch()` (F5 with its mappings) in a `.masm` file:

```json
{
  "adapters": {
    "mlwsvm": { "command": [ "mlwsvm", "dap" ] }
  },
  "configurations": {
    "Debug": {
      "adapter": "mlwsvm",
      "configuration": { "request": "launch", "program": "${file}", "stopOnEntry": true }
    }
  }
}
```

## Monitor: `--monitor`

```
$ mlwsvm run --monitor 7070 server.masm
mlwsvm: monitor at http://127.0.0.1:7070/  (or: mlwsvm top 127.0.0.1:7070)
```

`--monitor PORT` binds to 127.0.0.1. `--monitor HOST:PORT` binds elsewhere; use
it with care, because what it shows needs no key unless there is a tokens file
(`--monitor-tokens`, [Keys](#keys), below). It is
**read-only** unless `--monitor-actions` is given (below). `--hold` keeps
serving after `main` ends, so you can inspect the final state (Ctrl-C to quit).

```
$ mlwsvm run --monitor 7070 --monitor-actions server.masm
mlwsvm: monitor at http://127.0.0.1:7070/#token=4f1c...  (actions allowed with that token)
$ mlwsvm run --monitor 0.0.0.0:7443 --monitor-cert cert.pem --monitor-key key.pem server.masm
```

* **Actions** (`--monitor-actions`): kill a process, send it a message, and
  reload or purge a module, from the dashboard or the API. Each needs an
  admin's [key](#keys): the one printed at the start (128 random bits, or
  `MLWSVM_MONITOR_TOKEN`'s value), or an admin's from the tokens file. Open
  the printed address: the key is in its `#` part, which a browser never sends
  to a server, and the page keeps it for that tab only, and only then shows the
  buttons. An action also needs, when the monitor listens on this machine only,
  a `Host` of that address (so a page from another site cannot reach it through
  a name of its own pointing here), and no `Origin` but the monitor's own. Every
  action is written to the program's log (`monitor.kill`, `monitor.send`,
  `monitor.reload`, `monitor.purge`, `monitor.env_set`, `monitor.env_delete`:
  the environment and name, never the value), with `by`, the label of the key
  that did it. Reads need no key, unless there is a tokens file.
* **TLS** (`--monitor-cert FILE --monitor-key FILE`, PEM): the dashboard and the
  API over HTTPS, and nothing over plain HTTP. It needs OpenSSL or LibreSSL at
  run time, as `std.tls` does; so does `mlwsvm top https://HOST:PORT`.
* **Clients** hold nothing up. A request is read as its bytes come and its
  answer written as its client takes it, between the program's time slices, so
  one that sends slowly, or does not read, waits by itself: a request has 2
  seconds to arrive and its answer 10 to be taken, and then the connection is
  dropped. The monitor serves 32 connections at once (others wait their turn),
  takes 8 KiB of headers (431 for more) and a 64 KiB body (413), and answers
  503 while more than 64 MiB of answers wait to be taken. After a 431 or 413
  it lets the rest of the request come for a second, unread, so that the
  answer reaches its client.
* **The page** is sent with headers that keep it out of any other site's
  frame, and let it run and fetch only what is its own
  (`Content-Security-Policy`, `X-Frame-Options`).

### Web dashboard: `GET /`

Summary tiles (live processes, instructions per second, heap, time slices per
second), two-minute charts of instructions per second and heap size, a sortable
process table (click a row for details: quotas, links and monitors, here and on
other nodes, a backtrace, and the first messages waiting), the program's log
(below; filtered by level and by text, with the newest line at the bottom or the
top, which the browser remembers; it can be paused, and the lines shown
downloaded as text), a graph of who is linked with and who monitors whom, the modules loaded
with each version, the registered names, the nodes connected and each connection's
traffic and backlog, the [environments](STDLIB.md#environments-env) (names
to anyone who may read; values, and setting and deleting them, to an admin),
the keys (to an admin), and the most recent
exits with their results. It is a single self-contained page, compiled into the
binary from `web/dashboard.html`, and it polls the JSON API once a second (the
modules, names, nodes, graph, environments and keys every five). To an admin,
a process's details have **Debug**, which opens [its
debugger](#debugging-a-process), and the processes being debugged are listed
above the table.

### JSON API

| Endpoint | Returns |
|---|---|
| `GET /api/vm` | `vm` (`"mlwsvm"`), `uptime_ns`, `processes` (live), `spawned`, `max_live`, `slices`, `insns` (all processes), `heap_bytes`, `raw_bytes`, `modules`, `main_pid` |
| `GET /api/procs` | an array, per live process: `pid`, `program` and `program_name` (the program it belongs to: its number, from 0, and file, as `--also` or `mlwsvm serve` started it), `env` (the named environment it is in, or null: its program's), `state` (`ready` / `running` / `waiting` / `sleeping` / `native`: in a blocking native call / `io`: waiting for a pipe or socket / `paused`: by [its debugger](#debugging-a-process)), `away` (true while an I/O thread is still making that blocking call: the rest is then as it was when the call began), `entry`, `function`, `line`, `insns`, `cpu_ns`, `heap_bytes`, `heap_peak`, `raw_bytes`, `stack_bytes`, `depth`, `mailbox`, `msgs_sent`, `msgs_received`, `gc_count`, `gc_ns`, `exceptions`, `objects`, `links`, `monitors`, and `far_links`, `far_monitors` and `far_watching`, its ties with processes on other nodes (`node` and `pid`: a whole pid can be larger than JavaScript's numbers hold exactly) |
| `GET /api/proc/PID` | the same for one process, plus `quota` (`heap`, `raw`, `mailbox`, `insns`, `foreign`, `os`; `null` = unlimited), `messages` (the first 20 waiting: `type`, and `size` or a short `preview`) and `backtrace`. A process in a blocking call that is still being made (`away`) is shown as it was when the call began: no `messages`, and for `backtrace` where it made the call |
| `GET /api/exits` | the last 64 finished processes, newest first: `pid`, `entry`, `program`, `program_name`, `main` (its program's main), `failed`, `code`, `exit`, `insns`, `cpu_ns`, `ago_ns` |
| `GET /api/log?since=N` | the log's entries after number `N` (0: all it has), oldest first: `seq`, `t_ns` (since the VM started), `pid`, `program` and `program_name` (null for the monitor's own), and `level` and `text`, or `event` and `value` (JSON); with `next`, the number to ask from next time, and `lost`, how many were overwritten before being asked for |
| `GET /api/modules` | every module loaded, each version in the order loaded: `name`, `path`, `version` (1, 2, ...), `current`, `state` (`ready`, `failed`, `purged`), `functions`, `globals_bytes` |
| `GET /api/names` | the registry: `name` and `pid` |
| `GET /api/nodes` | `self`, this VM's node number (0: none), its `name` (`""`: none), and `connected`: `node`, its `name`, `carrier` (the process carrying that connection), and its traffic since it came: `up_ms`, messages `sent` there and `received` from there, the frames' `bytes_out` and `bytes_in`, `queued` (bytes not yet written), `waiting` (senders waiting now for it to have room, as more than 1 MiB is queued) and `waits` (how many times one has) |
| `GET /api/graph` | `links` (`[a, b]`, each once), `monitors` (`[watcher, watched]`), and `far`: ties with other nodes, `pid`, `kind` (`link`, `watched_by`, `watches`) and `other` |
| `GET /api/envs` | each environment, the programs' and then the named ones: `env` (a named one's name, or null), `program` and `program_name` (a program's, or null), `processes` (in it), `watchers` (`env.watch`), and `values`, by name: `key` and `secret`, and, to an admin's key, `value` as JSON (null for a secret, or with `note` for one that has no JSON form or is over 16 KiB of it) |
| `GET /top` | the plain-text table that `mlwsvm top` shows |
| `GET /log?since=N` | the log's entries after number `N` as plain text, which `mlwsvm top --log` shows: a first line with the number to ask from next time, then each entry: its time, pid, program, level (or `event`) and text (or an event's name and value) |

Actions (with `--monitor-actions`; `Authorization: Bearer KEY`, an admin's).
Each answers `{"ok": true}`, or `{"error": ...}` with 400 (a bad body, or a
module that will not reload), 401 (no key, or no such key), 403 (actions off, a
key that may only read, another `Host`, another site) or 404 (no such process,
module or action):

| Endpoint | Does |
|---|---|
| `POST /api/proc/PID/kill` | the process fails with `E_KILLED` (so its links and monitors hear of it) |
| `POST /api/proc/PID/send` | the body, JSON of up to 64 KiB, as a message: an object is a map, an array a list, a string a string, a number an `i64` or `f64` |
| `POST /api/modules/NAME/reload` | a new version of the module from its file (`module.reload`) |
| `POST /api/modules/NAME/purge` | frees the old versions nothing runs (`module.purge`); answers `freed` and `kept` |
| `POST /api/envs/set` | set a value of an environment: `{"env": "billing", "key": "rate", "value": 4.5}`, or `"program": 0` for a program's own; `"secret": true` marks it secret. Its watchers hear of it, as of `env.set`; 404 for no such environment |
| `POST /api/envs/delete` | the same, with no `value`: delete one; 404 for no such value |

`GET /api/vm` also says whether actions are on (`actions`), whether every
request needs a key (`keys`), and whose key asked (`you`: its `label` and
`scope`, or null).

### Keys

```
$ mlwsvm token new --label ops-dashboard --file monitor.tokens
4be1c09a7d2f...                       (the token: shown only now)
$ mlwsvm run --monitor 0.0.0.0:7443 --monitor-cert cert.pem --monitor-key key.pem \
      --monitor-tokens monitor.tokens --monitor-actions server.masm
mlwsvm: monitor at https://0.0.0.0:7443/#token=9a3e...  (actions allowed with that token)
```

A key is a token with a scope and a label:

* **`read`** sees all that the monitor shows.
* **`admin`** reads, acts (with `--monitor-actions`, or under `mlwsvm
  serve`), and makes and revokes keys.

The key printed at the start, `(start)`, is an admin's. It is made when
actions are on or there is a tokens file, and lasts as long as the VM.

A **tokens file** (`--monitor-tokens FILE`, for `run` and `serve`) holds one
key to a line, `SCOPE LABEL SHA256`: each token's SHA-256, never the token, so
that reading the file gives no one access. Lines that start with `#`, and
blank ones, are skipped; a label is 1 to 63 letters, digits, `_`, `.`, `-` or
`@`. With a tokens file, every request but the page's needs a key, so the
monitor may listen on a network (best over TLS: a key goes with every
request). The monitor reads the file again whenever it changes, so a key added
or removed takes effect at once; a file that no longer reads leaves the keys as
they were, and the log says why (`monitor.keys_unread`). A file that is not
there yet has no keys.

Whoever can write the file can make a key. So the monitor takes none from a
file that other users can write, or that another user owns (the administrator
aside): it does not start on one, and one that becomes so while it runs leaves
the keys as they were. `mlwsvm token` writes a file that only its owner can
write, whatever the umask. Keep its directory yours as well.

**Wrong keys** are counted by the address they come from. An address may send
ten, and one more every three seconds; past that, a request with a key from
there is refused unread (429) until the next is due, so a key cannot be found
by trying, and the log says so once a minute (`monitor.keys_refused`, with
`from`). The same wrong key again counts once, so a page left open with a key
since revoked uses up nothing. A request with no key is not counted.

`mlwsvm token` keeps the file:

* `new --label L [--scope read|admin] [--file F]` makes a key, prints its
  token (the only time it is shown) and adds its line to the file; with no
  file, it prints the line to add.
* `revoke --label L --file F` removes one.
* `list --file F` lists them.

An admin does the same from the dashboard's Keys card, or the API. Each is in
the log, by its key (`monitor.key_new`, `monitor.key_revoke`):

| Endpoint | Does |
|---|---|
| `GET /api/keys` | `file` (the tokens file, or null), and `keys`: each one's `label`, `scope`, `from` (`start`, `file`, or `api`: made here with no tokens file, lasting as long as the VM) and `used_ago_ms` (null: never) |
| `POST /api/keys` | make one: `{"label": "ops", "scope": "read"}` (`scope` may be left out: `read`); answers its `token`, the only time it is shown, and writes its hash into the tokens file; 409 for a label in use |
| `POST /api/keys/LABEL/revoke` | it no longer works; 409 for the key asking |

These need an admin's key, the monitor's own `Host` and `Origin` as actions
do, but not `--monitor-actions`. On the dashboard, a monitor that needs a key
asks for one, and the header says whose key is in use, with a way to sign out.

Counters are cumulative; compute rates by differencing two samples, as the
dashboard does. `cpu_ns` is the time spent executing that process's time slices.

### Debugging a process

`/api/debug` debugs one process of a VM that goes on running: the process is
interpreted, an instruction at a time, and when it stops it alone is paused
(`paused`, among the states); every other process runs as it did, machine code
and all. `mlwsvm debug` and `mlwsvm dap` debug a whole program from its start,
on one thread; this debugs what is already running, and several processes at
once, each by itself.

**The debugger's page**, `/debug#pid=N`, is the way to use it: a page for
one process, in a tab of its own, so that several can be debugged side by
side with the dashboard still there. **Debug**, in a process's details on the
dashboard, pauses the process and opens it; under `mlwsvm serve`, **Debug**
beside **Start** starts a program paused, its main stopped before its first
instruction, in its debugger. The page shows the source with the line the process is at (the
compiled language's own file where there is one, or the assembly, and the
function's instructions on request), the call stack, the variables of the
frame chosen, which open to show what they hold and are marked when a step
changed them, the messages waiting in its mailbox, the
[environment](STDLIB.md#environments-env) it reads (the named one it has
joined, or its program's: each value as JSON, a secret's never), the
breakpoints, and the process's own lines of the log (the newest at the bottom or at the top, by
the dashboard's own choice), and it names the other processes being
debugged, each a click away. A click beside a line sets or removes a
breakpoint there. A message can be sent to the process from its mailbox, as
JSON; sent to a paused process, it waits there. The bar under a pane (the
call stack, the variables, the mailbox, the environment, the log), dragged, makes it taller or
shorter, which the browser remembers; a double-click gives it back its own
size. Continue is F8 or `c`, step over F10 or `n`, step into `s`,
step out `o`, pause `p`. Closing the page changes nothing: a paused process
stays paused, and the dashboard says so. The page is compiled into the
binary from `web/debugger.html`, and asks for the process's state five times
a second while it runs, once a second while it is paused.

A process is **attached** to, and from then:

* **Pause** stops it before its next instruction. One that is waiting, for a
  message or a time, pauses when it next would run, and can be read meanwhile.
* **Steps** go by the lines of its source (the compiled language's, where its
  compiler marked them with `.loc`, else the assembly's) or by instructions:
  into a call, over one, or out of the function.
* **Breakpoints** are the VM's: at a line of a file, or at a function's first
  instruction, they stop whichever attached process reaches one. A line
  without code moves to the next that has some, up to ten lines on. One for
  code that is not loaded waits for it, and each follows a reload. A `break`
  instruction stops an attached process too.
* **Continue** lets it go on; **detach** as well, and from then it runs as it
  did before, its machine code where it has some. Continue also lets off one
  that was asked to pause and is not there yet.
* **What it spawns** can be debugged too (`children`): each process it spawns
  from then on, and theirs, so that a breakpoint stops whichever of them
  reaches it. A program started paused, in the debugger, is debugged so from
  its first process.
* **An error that nothing catches** pauses an attached process where it was
  raised, its frames and variables still there, instead of ending it
  (`reason` `error`, with the `error`'s `code` and `message`). It cannot step;
  let go on, it ends as it would have, with the same report. An error that a
  `.try` catches stops nothing.
* **Its whole program** can be paused: every process of the program is
  attached to and asked to pause, and what they spawn is debugged; and let go
  on, all at once.

Paused, the process keeps its mailbox: messages sent to it wait. A call to it
may time out meanwhile, as in Erlang's debugger. It can be killed, and it ends
with its program. What it is paused in is one instruction: a breakpoint on a
`recv` stops it once, though the `recv` runs again when a message comes.

Everything here needs an admin's key, since what a process holds is shown; the
actions also need `--monitor-actions`, as others do. Attaching, detaching,
pausing and each new breakpoint go into the log (`monitor.debug_attach`,
`monitor.debug_detach`, `monitor.debug_pause`, `monitor.debug_break`).

| Endpoint | Returns |
|---|---|
| `GET /api/debug` | `available` (false, with `why`, in a VM run under the profiler, the tracer or `mlwsvm debug`), `breakpoints`, and `processes`: each one attached to, as below without its frames |
| `GET /api/debug/PID` | `debugged`, `state`, `away`, `pausing` (asked to pause, and not yet there), `stepping`, `reason` (why it is paused: `pause`, `step`, `breakpoint`, `break`, `entry`, `error`), `error` (what it failed with, paused there: `code` and `message`; else null), `seq` (its pauses so far: a new one is a new stop), `children` (what it spawns is debugged too), `at` (where it is) and `frames`, the innermost first, 200 at most: each a `function`, `pc`, `file` and `line` (the assembly's), and `source` and `source_line` where `.loc` gives them |
| `GET /api/debug/PID/vars?frame=N` | frame N's `locals` (the names the program gave its registers, each as its formatter shows it) and `registers` by bank: each a `name`, `value`, `kind`, how many `children` it opens to show, and its `path` |
| `GET /api/debug/PID/vars?frame=N&path=P` | the `children` of the value at a path: a struct's fields, a list's or an array's elements, a map's values by key, a closure's captures, the registers of a variable of several; 1,000 at most, from `from=K` |
| `GET /api/debug/PID/code?frame=N` | the frame's function as instructions: each a `pc`, `line`, `text`, `source` and `source_line`, and whether a `breakpoint` is there; with `pc`, the frame's own |
| `GET /api/debug/source?file=F` | the `text` of a file that loaded code came from, as the frames name it: the file of that name, or one beside the module's own file (a compiler names a program's file as it was given it); 404 for any other file, or one that is not there |
| `GET /api/debug/breakpoints` | each breakpoint: `id`, `file` and `line`, or `function`; `line_at` (the line with code it is at) and `at`, the places it is at now (none: waiting for its code) |

| Action | Does |
|---|---|
| `POST /api/debug/PID/attach` | debug it; with `{"pause": true}`, pause it too; with `{"children": true}` (or `false`), what it spawns from now as well (or no longer); 403 for `mlwsvm serve`'s own process |
| `POST /api/debug/PID/detach` | no longer; it goes on if it was paused |
| `POST /api/debug/PID/pause` | pause it before its next instruction; with `{"program": true}`, every process of its program, each attached to |
| `POST /api/debug/PID/continue` | a paused one goes on, and one asked to pause is let off; 409 if it is neither; with `{"program": true}`, every one of its program |
| `POST /api/debug/PID/step` | `{"kind": "into" \| "over" \| "out", "by": "line" \| "insn"}` (`by` may be left out: `line`); 409 if it is not paused, or is paused where it failed |
| `POST /api/debug/breakpoints` | `{"file": "app.masm", "line": 12}` (a file by its name, with or without its directories) or `{"function": "mod.f"}`; answers the breakpoint, the one there already if it is the same |
| `POST /api/debug/breakpoints/ID/delete` | it is gone |

Each action on a process answers its state as `GET /api/debug/PID` does,
without the frames.

Limits to know. A process in a blocking call that is still being made
(`away`) cannot be read until the call returns (409), and pauses then.
Nothing stops inside a callback from C, and an error raised inside a
coroutine, or in such a callback, ends its process as it always did.
Breakpoints stop only processes being debugged: one not attached to runs
past them. A register its function has not
written yet shows whatever was there. Values are read, not changed.

### The program's log

A program writes to the monitor's log with `monitor.log(level, text)`, a line at a
level (0 debug, 1 info, 2 warn, 3 error), and `monitor.event(name, value)`, a named
event whose value is kept as JSON; `monitor.on()` says whether a monitor is there
to keep them. Nothing else goes there: the program's output and its files are its
own. The log keeps the last 1,000 entries, each line up to 4 KiB. Without
`--monitor` the calls check their arguments and keep nothing, and an event's value
is not turned into JSON, so it costs next to nothing; with one, a value with no
JSON form is `E_JSON`.

```asm
    ncall "monitor.log" (1, "listening")
    ncall "monitor.event" ("request", R0)    ; R0: a map, say
```

## A VM that stays up: `mlwsvm serve`

```
$ mlwsvm serve --programs ./services --monitor 7070 --quota heap=256M
mlwsvm: serving at http://127.0.0.1:7070/#token=9a3e...  (programs from ./services)
```

`mlwsvm serve` starts a VM that runs until it is shut down, with no program of
its own, and its monitor with actions on (an admin's key is printed, or is
`MLWSVM_MONITOR_TOKEN`; `--monitor-tokens FILE` as for `run`). Programs are started from the dashboard's Programs
card, or the API, each with its own `os.args` and, if given, capabilities and
quotas of its own in `--cap`'s and `--quota`'s syntax, within the VM's. Each
has an [environment](STDLIB.md#environments-env) of its own, with `--env`'s
values and those of the `NAME.mlws.env` beside its file, if there is one (one
that is not right is reported, and left out). A
program is a file in the `--programs` directory: a path that leads out of it,
through `..` or a link, is refused. The same file started again is another main
of the same module, as with `--also`. When a program's main ends, its other
processes are stopped and it is freed, heap and all: only how it ended stays,
in `/api/programs` and Recent exits. Programs share the VM's output, as with
`--also`; what one wants to show in the dashboard it writes to the monitor's log
(`monitor.log`): [examples/serve/echo.masm](../examples/serve/echo.masm) is
one to try, which writes there each message the dashboard sends it. VM options
apply to all of them. `--monitor-cert` and
`--monitor-key` serve the monitor over HTTPS, as for `run`; it listens on
127.0.0.1:7070 unless told otherwise.

| Endpoint | Does |
|---|---|
| `GET /api/programs` | the programs started: `id`, `path` (in the programs' directory), `cap` and `quota` (as started, or null), `args`, `pid` (of its main), `state` (`running`, `done` with `exit`, `failed` with `code` and `message`) |
| `GET /api/programs/files` | the programs in the directory, for the dashboard's picker: each `.masm` or `.mlwso` file by its path within it, at most four directories down |
| `POST /api/programs` | start one: `{"path": "app.masm", "args": ["-v"], "cap": "read=/data", "quota": "heap=64M"}` (all but `path` may be left out); with `"debug": true`, its main is paused before its first instruction, [its debugger's](#debugging-a-process); answers its `id` and `pid`; 400 for a program that does not load or a bad `cap` or `quota`, 403 for a path not in the directory |
| `POST /api/programs/ID/stop` | its main fails with `E_KILLED`, and its other processes stop, as when a program's main ends |
| `POST /api/programs/ID/rerun` | the same file again, with the same arguments, capabilities and quotas, as a new program (paused, with `{"debug": true}`); answers its `id` and `pid` |
| `POST /api/shutdown` | every program is stopped, and `mlwsvm serve` exits 0 |

These need an admin's key, as every action does, and each is written to the log
(`monitor.start`, `monitor.rerun`, `monitor.stop`, `monitor.shutdown`). `GET
/api/vm` says `"serve": true`. On the dashboard, a program is chosen from the
directory's, by sub-directory; a program that has ended can be run again from
the Programs card or from Recent exits; every process, exit and log line names
its program; and the log can be narrowed to the process selected, or to its
program.

## Name service for nodes: `mlwsvm names`

```
$ mlwsvm names
mlwsvm names: on port 4368
```

One per machine, as Erlang's `epmd`: a node that `std.node`'s `start_named`
starts registers its name (the part before its `@`) and port with this
machine's service, and another finds that port by asking the service of the
name's host (`connect_named("db@host", cookie)`). A registration lasts while
the node's connection to the service stays open, so a node that ends takes
its name with it. Registrations are taken only from this machine; anyone may
ask. The service listens on 4368 (`--port N`; nodes use `$MLWSVM_NAMES_PORT`
when it is set), one below Erlang's 4369, so that both can run. It is
`lib/std/names.masm`, run as a program; its protocol is a line and an
answer: `REGISTER name port` (`OK`, `TAKEN` or `REFUSED`), `PORT name` (the
port, or `0`) and `NAMES` (`name port` lines, then `END`).

## Terminal view: `mlwsvm top`

```
MLWSVM  up 12.0s  processes 4 (max 4, spawned 4)  insns 33.9M  heap 712.0KB

   PID STATE     %CPU    INSNS     HEAP      RAW   STACK  MBOX MSGS IN/OUT   GC  FUNCTION
     2 ready     46.5    16.9M   356.0K        0       0     0         0/0   97  busy.spinner+7 (busy.masm:9)
     1 sleeping   0.0        4        0        0       0     0         0/0    0  busy.main+4 (busy.masm:20)

nodes (this VM: node 1, one@host):
  NODE NAME                       UP     SENT RECEIVED      OUT       IN   QUEUED WAIT  WAITS
     2 two@host                 8.4s     1.2M     3.0K   35.7MB   88.1KB    1.2MB    1     14

recent exits:
     7 worker           failed E_BOUNDS        1.2K insns
```

`top` connects to `127.0.0.1:7070` unless given another address. It redraws
every second (`--interval MS`) and sorts processes by CPU. A VM connected to
other nodes has a line for each connection: how long it has been up, the
messages sent and received, the bytes written and read, what is queued to be
written, and the senders waiting for room now (`WAIT`) and in all (`WAITS`). `--once`
prints one snapshot and exits; there, %CPU is measured over the VM's whole uptime.
It fetches `/top` from the monitor, so it works against any VM started with
`--monitor`. A monitor over TLS is `https://HOST:PORT` (the address the run
prints will do): `top`
checks its certificate against the host, with the authorities in `--ca FILE`
(for a certificate of your own making) or else the system's, and never
without checking. A monitor with a tokens file needs a key: `--token KEY`, the
address's `#token=` part, or `MLWSVM_MONITOR_TOKEN`, in that order.

`mlwsvm top --log` shows [the program's log](#the-programs-log) instead, and
follows it as `tail -f` follows a file, until the VM ends; with `--once` it
prints what the log holds and exits. A line is the time since the VM started,
the pid (`-` for the monitor's own lines), the program, the level (or `event`)
and the text (or an event's name and its value as JSON); the dashboard's
Download writes the same lines.

```
    0.004 s     1  watched.masm  info   watched: started
    0.004 s     1  watched.masm  event  ready [42,"x"]
    3.120 s     -  monitor  event  monitor.kill {"by":"ops","pid":2}
```

## Bindings generator: `mlwsvm bindgen`

```
mlwsvm bindgen [-l LIB] [-p PREFIX] [-o OUT] [--all] [--no-check] [--cc CLANG]
               [--ast FILE.json] header.h [-- clang options...]
```

`bindgen` writes the FFI declarations for a C header ([EXTENDING.md](EXTENDING.md#the-declarative-ffi)):
`.extern` for its functions, `.cstruct` for its structs, `.callback` for
function-pointer parameters, and `.const` for enum constants. Clang does the
parsing (`clang -Xclang -ast-dump=json -fsyntax-only`, so the header is read
exactly as a C compiler reads it, macros, conditionals and includes included).
`-l` names the library (`""`, the default, is the program's own symbols, which
include the C library), and `-p` the natives' prefix (default: the header's
name). Pass include paths and defines after `--`. `--ast` reads a syntax tree
saved earlier with that clang command, and `--cc` names another clang.

```sh
mlwsvm bindgen -l libsqlite3 -p sql -o sqlite.inc /usr/include/sqlite3.h
```

```asm
.include "sqlite.inc"     ; then: ncall "sql.sqlite3_open" ("db", ...) -> (...)
```

Only the header's own declarations are written, not those of the headers it
includes (`--all` writes everything). Structs it uses that are defined elsewhere
are written too, when they are needed. How C types become FFI types:

| C | FFI | |
|---|---|---|
| integers, `_Bool`, enums | `i8` … `u64`, `u8`, `i32` | at this machine's sizes (`long` is 8 bytes on LP64) |
| `float`, `double` | `f32`, `f64` | `long double` cannot be passed: skipped |
| `const char *` | `cstr` | a string in; as a result, copied into a string |
| `char *` parameter | `buf` | C may write into it (an output buffer) |
| `void *`, `T *` (numbers) parameter | `buf` | memory; change to `out T` for a single value, or `ptr` for a handle |
| `struct S *` | `struct* S` (parameter) or `ptr<S>` (result) | `ptr<S>` when `S` cannot be described (opaque handles) |
| `struct S **` parameter | `out ptr<S>` | C hands back a handle |
| `struct S` | `struct S` | by value |
| function pointer parameter | `callback NAME` | named after its typedef, or `FUNCTION_PARAM` |
| `T (*)(...)` result, other pointers | `ptr` | |

What cannot be expressed is written as a comment that gives the reason:
functions using `long double`, unions passed by value, or more than 8
parameters, and structs with unions, bitfields or arrays of structs (they are
then passed only as `ptr<S>`). `static` and `inline` functions are left out,
since they have no symbol to call. When the library can be opened, each
function is looked up in it, and missing ones are left out with a comment:
compiler intrinsics such as `alloca`, or functions this platform lacks
(`--no-check` keeps them). A function declared with an asm label calls the
label's symbol (glibc's `scanf` is `__isoc99_scanf`). A variadic function gets
`...` with nothing after it: add the types to pass, one declaration per
combination.

C declarations do not say who owns a returned pointer, whether an `int *` is an
array or an out parameter, or how long C keeps a pointer it is given. Review
the output, and adjust it (`.ffi_free` for handles the program must free) before
use. `make check-bindgen` generates bindings for `tests/_bindgen/libc.h`,
compares them with `libc.expected`, and calls them.

## Differential fuzzing: `mlwsvm fuzz`

`mlwsvm fuzz` writes random programs that always end, runs each one in the
interpreter and in every other mode, and reports each program whose output or
exit status differs from the interpreter's, or that crashes or hangs:

```
$ mlwsvm fuzz -n 50000 --seed 100000
mlwsvm fuzz: seeds from 100000, size 40, 18 jobs; against the interpreter: jit,hot,first,slice,opt,jit-opt,stress,jit-stress,verify,jit-verify
mlwsvm fuzz: 50000 programs (seeds 100000-149999), 550000 runs, 0 skipped, 0 failed, 200.7 s
```

| Mode | Options | What it checks |
|---|---|---|
| `interp` | | the reference |
| `jit` | `--jit all` | every function compiled as it loads, entered at every block |
| `hot` | `--jit on --jit-hot 2` | functions compiled when hot, in the middle of a run |
| `first` | `--jit on --jit-hot 1` | functions compiled at their first call |
| `slice` | `--jit all --slice 7` | machine code left and entered again at time slices |
| `opt` | `--opt all` | every optimizer pass |
| `jit-opt` | `--jit all --opt all` | the JIT on optimized code |
| `stress` | `--gc-stress` | a collection at every allocation |
| `jit-stress` | `--jit all --gc-stress` | the same, in machine code |
| `verify` | `--gc-verify` | each minor collection checked against a walk of the whole heap |
| `jit-verify` | `--jit all --gc-verify` | the write barriers of machine code |

A program has a few functions and a `main` that calls each of them three
times and prints what they return, then everything they can have written:
lists, a map, a chain of structs, strings, globals. The functions do integer
(at every width), float and bitwise arithmetic, conversions, compares and
selects; read and write list and array elements, struct fields, globals, raw
memory, bytes of strings and byte buffers, map entries and the data stack;
build strings, lists and structs (so collections run); branch, loop (with
breaks, continues and early returns), jump through tables, throw and catch;
call each other directly, through closures with captures, in tail position and
inside coroutines that yield from anywhere. Each calls only the functions
after it and loops a constant number of times, and a budget counted through
loops and calls bounds the instructions and allocations of each call, so a
program runs in a few milliseconds. An instruction that can trap (a divide by
a register, an index not masked into range, a missing map key) is mostly put
inside a `.try` region, so the handler's view of the registers is compared
as well. The program for a seed is the same on every host: `mlwsvm fuzz
--emit SEED` prints it.

* `-n N` programs, or `--time SECONDS` (default: 100 programs), from `--seed
  S` (default: from the clock); `-j N` at once (default: one per core).
* `--size N`: statements per function, about (default 40). One function in
  ten is eight times as long, for a lot of machine code in one function.
* `--modes LIST`: which modes to compare with the interpreter (`--list`
  shows them).
* `--vm PATH`: run another build, such as `mlwsvm-asan`; by default the one
  running `fuzz`. Each run is `mlwsvm run --no-cache --threads 1` with the
  mode's options, and `fuzz` clears every `MLWSVM_` variable first, so a mode
  is exactly what it says.
* `--timeout SECONDS` (default 60): a run that takes longer is a hang (and
  a program that slow in the interpreter is skipped).
* `--out DIR` (default `fuzz-out`): each failing program goes there as
  `fuzz-SEED.masm`, with a header saying which modes failed and how, the
  first line that differs, and the command to reproduce it, and each run's
  output as `fuzz-SEED.MODE.out`.

`mlwsvm fuzz --reduce FILE [--modes LIST]` shrinks a failing program: it
removes lines (halves, then quarters, and so on, then each run of up to four
lines) while the program still assembles, exits as before in the interpreter,
and fails the same way (differs, crashes or hangs) in a mode that failed at
first. A candidate that loops is cut off at four times the original's time.
It writes the result beside the file as `FILE.reduced.masm`, often a few
dozen lines after a few hundred runs (a fault that needs a large function
keeps more).

`make check-fuzz` runs seeds 1 to 300 (a few seconds), in CI too, and `make
check-asan` 100 under AddressSanitizer.

## Before a release: `make release-check`

`sh tests/release-check.sh [-c CC] [-C COMPILERS_DIR] [-f FUZZ_SECONDS]` runs
every check a release needs, on the machine it runs on:

* a clean build, which must have no warnings;
* `make test`, also on one worker thread, and `check-gc`, `check-opt`,
  `check-jit`, `check-embed`, `check-tools`, `check-bindgen`, `check-asan`
  (also with `MLWSVM_OPT=3`, and with the JIT and `MLWSVM_OPT=3`),
  `check-tsan`, `check-switch`, `check-loop`, `check-poison`, `check-regex`, a
  build without libffi, and the benchmarks' output checks;
* the fuzzer for `FUZZ_SECONDS` (default 300), and a tenth of that under
  AddressSanitizer;
* if `../mlws-compilers` (or `COMPILERS_DIR`) is there, the Brainfuck, Lox,
  Lua and Erlang compilers' suites in ten modes with this build: the JIT
  off, on, on compiling every function at its first call, and `all`; GC
  stress and `MLWSVM_OPT=3`, each also with `all`; `MLWSVM_GC_VERIFY` with the
  JIT off and `all`; and three with the AddressSanitizer build: `all`, `all`
  at `MLWSVM_OPT=3`, and on compiling functions once hot (`MLWSVM_JIT_HOT=2`).

It goes on after a failure, prints one line per step, keeps each step's
output in `build/release-check/STEP.log`, and exits with 1 if any step
failed. It takes about an hour. Run it on every platform a release supports.

## Testing the tools

* `make test` runs the debugger tests in `tests/tools/*.masm`, which use
  `debug-script`.
* `make check-tools` smoke-tests what an in-process test can't reach: the profiler
  and flame output, the tracer, bundles run from an empty directory, whole sessions
  of `mlwsvm dap`, two VMs talking through `std.node`, and the monitor, by querying
  a live VM with `mlwsvm top --once` and, if available, `curl`.
