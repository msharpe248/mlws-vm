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
| Monitor + dashboard | `mlwsvm run --monitor [HOST:]PORT [--hold] [--monitor-actions] prog.masm` | none |
| A VM that stays up | `mlwsvm serve --programs DIR [--monitor [HOST:]PORT]` | — |
| Terminal view | `mlwsvm top [HOST:PORT] [--once] [--interval MS]` | — |
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
it with care, because what it shows needs no authentication. It is
**read-only** unless `--monitor-actions` is given (below). `--hold` keeps
serving after `main` ends, so you can inspect the final state (Ctrl-C to quit).

```
$ mlwsvm run --monitor 7070 --monitor-actions server.masm
mlwsvm: monitor at http://127.0.0.1:7070/#token=4f1c...  (actions allowed with that token)
$ mlwsvm run --monitor 0.0.0.0:7443 --monitor-cert cert.pem --monitor-key key.pem server.masm
```

* **Actions** (`--monitor-actions`): kill a process, send it a message, and
  reload or purge a module, from the dashboard or the API. Each needs a token:
  128 random bits, printed at the start, or `MLWSVM_MONITOR_TOKEN`'s value. Open
  the printed address: the token is in its `#` part, which a browser never sends
  to a server, and the page keeps it for that tab only, and only then shows the
  buttons. An action also needs, when the monitor listens on this machine only,
  a `Host` of that address (so a page from another site cannot reach it through
  a name of its own pointing here), and no `Origin` but the monitor's own. Every
  action is written to the program's log (`monitor.kill`, `monitor.send`,
  `monitor.reload`, `monitor.purge`). Reads still need no token.
* **TLS** (`--monitor-cert FILE --monitor-key FILE`, PEM): the dashboard and the
  API over HTTPS, and nothing over plain HTTP. It needs OpenSSL or LibreSSL at
  run time, as `std.tls` does. `mlwsvm top` speaks plain HTTP only.

### Web dashboard: `GET /`

Summary tiles (live processes, instructions per second, heap, time slices per
second), two-minute charts of instructions per second and heap size, a sortable
process table (click a row for details: quotas, links and monitors, here and on
other nodes, a backtrace, and the first messages waiting), the program's log
(below), a graph of who is linked with and who monitors whom, the modules loaded
with each version, the registered names, the nodes connected, and the most recent
exits with their results. It is a single self-contained page, compiled into the
binary from `web/dashboard.html`, and it polls the JSON API once a second (the
modules, names, nodes and graph every five).

### JSON API

| Endpoint | Returns |
|---|---|
| `GET /api/vm` | `vm` (`"mlwsvm"`), `uptime_ns`, `processes` (live), `spawned`, `max_live`, `slices`, `insns` (all processes), `heap_bytes`, `raw_bytes`, `modules`, `main_pid` |
| `GET /api/procs` | an array, per live process: `pid`, `program` and `program_name` (the program it belongs to: its number, from 0, and file, as `--also` or `mlwsvm serve` started it), `state` (`ready` / `running` / `waiting` / `sleeping` / `native`: in a blocking native call / `io`: waiting for a pipe or socket), `entry`, `function`, `line`, `insns`, `cpu_ns`, `heap_bytes`, `heap_peak`, `raw_bytes`, `stack_bytes`, `depth`, `mailbox`, `msgs_sent`, `msgs_received`, `gc_count`, `gc_ns`, `exceptions`, `objects`, `links`, `monitors`, and `far_links`, `far_monitors` and `far_watching`, its ties with processes on other nodes (`node` and `pid`: a whole pid can be larger than JavaScript's numbers hold exactly) |
| `GET /api/proc/PID` | the same for one process, plus `quota` (`heap`, `raw`, `mailbox`, `insns`, `foreign`, `os`; `null` = unlimited), `messages` (the first 20 waiting: `type`, and `size` or a short `preview`) and `backtrace` |
| `GET /api/exits` | the last 64 finished processes, newest first: `pid`, `entry`, `program`, `program_name`, `main` (its program's main), `failed`, `code`, `exit`, `insns`, `cpu_ns`, `ago_ns` |
| `GET /api/log?since=N` | the log's entries after number `N` (0: all it has), oldest first: `seq`, `t_ns` (since the VM started), `pid`, `program` and `program_name` (null for the monitor's own), and `level` and `text`, or `event` and `value` (JSON); with `next`, the number to ask from next time, and `lost`, how many were overwritten before being asked for |
| `GET /api/modules` | every module loaded, each version in the order loaded: `name`, `path`, `version` (1, 2, ...), `current`, `state` (`ready`, `failed`, `purged`), `functions`, `globals_bytes` |
| `GET /api/names` | the registry: `name` and `pid` |
| `GET /api/nodes` | `self`, this VM's node number (0: none), its `name` (`""`: none), and `connected`: `node`, its `name`, and `carrier`, the process carrying that connection |
| `GET /api/graph` | `links` (`[a, b]`, each once), `monitors` (`[watcher, watched]`), and `far`: ties with other nodes, `pid`, `kind` (`link`, `watched_by`, `watches`) and `other` |
| `GET /top` | the plain-text table that `mlwsvm top` shows |

Actions (with `--monitor-actions`; `Authorization: Bearer TOKEN`). Each answers
`{"ok": true}`, or `{"error": ...}` with 400 (a bad body, or a module that will
not reload), 401 (no token, or the wrong one), 403 (actions off, another
`Host`, another site) or 404 (no such process, module or action):

| Endpoint | Does |
|---|---|
| `POST /api/proc/PID/kill` | the process fails with `E_KILLED` (so its links and monitors hear of it) |
| `POST /api/proc/PID/send` | the body, JSON of up to 64 KiB, as a message: an object is a map, an array a list, a string a string, a number an `i64` or `f64` |
| `POST /api/modules/NAME/reload` | a new version of the module from its file (`module.reload`) |
| `POST /api/modules/NAME/purge` | frees the old versions nothing runs (`module.purge`); answers `freed` and `kept` |

`GET /api/vm` also says whether actions are on (`actions`).

Counters are cumulative; compute rates by differencing two samples, as the
dashboard does. `cpu_ns` is the time spent executing that process's time slices.

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
its own, and its monitor with actions on (the token is printed, or
`MLWSVM_MONITOR_TOKEN`). Programs are started from the dashboard's Programs
card, or the API, each with its own `os.args` and, if given, capabilities and
quotas of its own in `--cap`'s and `--quota`'s syntax, within the VM's. A
program is a file in the `--programs` directory: a path that leads out of it,
through `..` or a link, is refused. The same file started again is another main
of the same module, as with `--also`. Programs share the VM's output, as with
`--also`; what one wants to show in the dashboard it writes to the monitor's log
(`monitor.log`). VM options apply to all of them. `--monitor-cert` and
`--monitor-key` serve the monitor over HTTPS, as for `run`; it listens on
127.0.0.1:7070 unless told otherwise.

| Endpoint | Does |
|---|---|
| `GET /api/programs` | the programs started: `id`, `path` (in the programs' directory), `cap` and `quota` (as started, or null), `args`, `pid` (of its main), `state` (`running`, `done` with `exit`, `failed` with `code` and `message`) |
| `GET /api/programs/files` | the programs in the directory, for the dashboard's picker: each `.masm` or `.mlwso` file by its path within it, at most four directories down |
| `POST /api/programs` | start one: `{"path": "app.masm", "args": ["-v"], "cap": "read=/data", "quota": "heap=64M"}` (all but `path` may be left out); answers its `id` and `pid`; 400 for a program that does not load or a bad `cap` or `quota`, 403 for a path not in the directory |
| `POST /api/programs/ID/stop` | its main fails with `E_KILLED`, and its other processes stop, as when a program's main ends |
| `POST /api/programs/ID/rerun` | the same file again, with the same arguments, capabilities and quotas, as a new program; answers its `id` and `pid` |
| `POST /api/shutdown` | every program is stopped, and `mlwsvm serve` exits 0 |

These need the token, as every action does, and each is written to the log
(`monitor.start`, `monitor.rerun`, `monitor.stop`, `monitor.shutdown`). `GET
/api/vm` says `"serve": true`. On the dashboard, a program is chosen from the
directory's, by sub-directory; a program that has ended can be run again from
the Programs card or from Recent exits; every process, exit and log line names
its program; and the log can be narrowed to the process selected, or to its
program.

## Terminal view: `mlwsvm top`

```
MLWSVM  up 12.0s  processes 4 (max 4, spawned 4)  insns 33.9M  heap 712.0KB

   PID STATE     %CPU    INSNS     HEAP      RAW   STACK  MBOX MSGS IN/OUT   GC  FUNCTION
     2 ready     46.5    16.9M   356.0K        0       0     0         0/0   97  busy.spinner+7 (busy.masm:9)
     1 sleeping   0.0        4        0        0       0     0         0/0    0  busy.main+4 (busy.masm:20)

recent exits:
     7 worker           failed E_BOUNDS        1.2K insns
```

`top` connects to `127.0.0.1:7070` unless given another address. It redraws
every second (`--interval MS`) and sorts processes by CPU. `--once`
prints one snapshot and exits; there, %CPU is measured over the VM's whole uptime.
It fetches `/top` from the monitor, so it works against any VM started with
`--monitor`.

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
