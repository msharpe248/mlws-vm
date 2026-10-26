# MLWSVM Tooling

Tools for seeing what a program is doing: resource statistics, a profiler, an
instruction tracer, an interactive debugger, and a live monitor with a web
dashboard and a `top`-style terminal view.

| Tool | Command | Overhead when off |
|---|---|---|
| Statistics | `mlwsvm run --stats prog.masm` | none |
| Profiler | `mlwsvm run --profile [--flame FILE] prog.masm` | none |
| Tracer | `mlwsvm run --trace FILE\|- [--trace-limit N] prog.masm` | none |
| Debugger | `mlwsvm debug prog.masm` | — |
| Monitor + dashboard | `mlwsvm run --monitor [HOST:]PORT [--hold] prog.masm` | none |
| Terminal view | `mlwsvm top [HOST:PORT] [--once] [--interval MS]` | — |
| VM options | `mlwsvm run --max-frames N --quota heap=64M ... prog.masm` | — |
| Native code | `mlwsvm ext list\|install\|remove\|allow-ffi\|deny-ffi` ([EXTENDING.md](EXTENDING.md)) | — |

The profiler, tracer and debugger use one per-instruction hook, which is
installed only while one of them is active. A program run under them uses a
single worker thread, so traces and profiles are deterministic and the debugger
can stop the whole program. The monitor needs no hook: the first worker thread
polls its socket about once a millisecond between time slices, and while it waits
when idle, so the monitor needs no thread of its own. To answer a request it
briefly pauses the other worker threads at their next time-slice boundary ("stop
the world"), so the numbers it reports are a consistent snapshot.

---

## VM options

Settings for the VM itself. Each can be given on the command line (`mlwsvm run`
and `mlwsvm test`), as an environment variable (`MLWSVM_` plus the name in
capitals with `_` for `-`, e.g. `MLWSVM_MAX_FRAMES=500000`), or in a test file as
`; option: --max-frames 500000`. The command line overrides the environment, and
a test file's own lines override both.

| Option | Default | Meaning |
|---|---|---|
| `--threads N` | one per core | worker threads (1 to 256) |
| `--stack-words N` | 131072 | each process's data stack, in 8-byte words |
| `--max-frames N` | 100000 | call depth limit (`E_CALL_DEPTH`) |
| `--slice N` | 2000 | time slice, in instructions, before another process gets a turn |
| `--gc-min BYTES` | 1M | the heap size below which no collection runs |
| `--quota LIST` | unlimited | starting limits for every process, e.g. `heap=64M,raw=1G,mailbox=1000,insns=5000000,foreign=0,os=0`; any subset, `unlimited` resets one |
| `--ext-path DIR[:DIR]` | (none) | trusted native-extension directories, searched before `lib/ext` ([EXTENDING.md](EXTENDING.md)) |

Sizes take an optional `K`, `M` or `G` suffix (powers of 1024). A bad value is
an error (exit code 2) on the command line, a failing test in a test file, and a
warning that is otherwise ignored in the environment.

```
$ mlwsvm run --max-frames 1000000 deep_recursion.masm
$ MLWSVM_QUOTA=heap=256M,insns=10000000000 mlwsvm run untrusted.masm
```

A backtrace from a runaway recursion stays short: runs of identical frames are
collapsed (`... repeated 99998 more times`), and a very deep stack shows only its
innermost 40 and outermost 10 frames.

## Statistics: `--stats`

Printed on stderr when the program ends: instructions and speed of `main`, call
depth, stack peak, raw-memory allocations and anything leaked, heap peak and final
size, objects allocated and collected, GC count and time, processes spawned,
the most alive at once, time slices, and instructions across all processes.

## Profiler: `--profile`

```
$ mlwsvm run --profile examples/fib.masm
--- mlwsvm profile: 2860297 instructions ---

functions (instructions executed in the function itself; ...):
       %        insns    entries  function
  100.0%      2860292     635621  fib.fib
...
hot lines:
   22.2%       635621  examples/fib.masm:9 (fib.fib)
...
opcodes:
   22.2%       635622  ret
```

* **functions**: instructions executed in each function itself (exclusive).
  `entries` counts how often the function's first instruction ran: calls, plus any
  loop that jumps back to it.
* **hot lines**: instructions per source line.
* **opcodes**: the instruction mix.
* `--flame FILE` also writes sampled call stacks (one sample every 997
  instructions) in the *collapsed* format, `main;f;g 123`, which
  `flamegraph.pl` and speedscope read directly.

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

| Command | Short | Action |
|---|---|---|
| `run` | `r` | start the program |
| `continue` | `c` | resume until a breakpoint or the end |
| `step` | `s` | execute one instruction, entering calls |
| `next` | `n` | execute one instruction, stepping over calls |
| `finish` | `fin` | run until the current function returns |
| `break SPEC` | `b` | breakpoint at `FUNCTION`, `module.FUNCTION`, `LINE` (main file) or `FILE:LINE` |
| `delete [N]` | `d` | delete breakpoint N, or all |
| `info break` | `i` | list breakpoints with hit counts |
| `print REG` | `p` | a register (`I3`, `F0`, `R1`, `P2`, `SP`, `FP`, `PID`); objects are shown with their contents |
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

## Monitor: `--monitor`

```
$ mlwsvm run --monitor 7070 server.masm
mlwsvm: monitor at http://127.0.0.1:7070/  (or: mlwsvm top 127.0.0.1:7070)
```

`--monitor PORT` binds to 127.0.0.1. `--monitor HOST:PORT` binds elsewhere; use
it with care, because the API has no authentication. It is **read-only**:
nothing it serves can change the program. `--hold` keeps serving after `main`
ends, so you can inspect the final state (Ctrl-C to quit).

### Web dashboard: `GET /`

Summary tiles (live processes, instructions per second, heap, time slices per
second), two-minute charts of instructions per second and heap size, a sortable
process table (click a row for details: quotas, links, monitors and a backtrace),
and the most recent exits with their results. It is a single self-contained page,
compiled into the binary from `web/dashboard.html`, and it polls the JSON API once
a second.

### JSON API

| Endpoint | Returns |
|---|---|
| `GET /api/vm` | `uptime_ns`, `processes` (live), `spawned`, `max_live`, `slices`, `insns` (all processes), `heap_bytes`, `raw_bytes`, `modules`, `main_pid` |
| `GET /api/procs` | an array, per live process: `pid`, `state` (`ready` / `running` / `waiting` / `sleeping`), `entry`, `function`, `line`, `insns`, `cpu_ns`, `heap_bytes`, `heap_peak`, `raw_bytes`, `stack_bytes`, `depth`, `mailbox`, `msgs_sent`, `msgs_received`, `gc_count`, `gc_ns`, `exceptions`, `objects`, `links`, `monitors` |
| `GET /api/proc/PID` | the same for one process, plus `quota` (`heap`, `raw`, `mailbox`, `insns`; `null` = unlimited) and `backtrace` |
| `GET /api/exits` | the last 64 finished processes, newest first: `pid`, `entry`, `failed`, `code`, `exit`, `insns`, `cpu_ns`, `ago_ns` |
| `GET /top` | the plain-text table that `mlwsvm top` shows |

Counters are cumulative; compute rates by differencing two samples, as the
dashboard does. `cpu_ns` is the time spent executing that process's time slices.

## Terminal view: `mlwsvm top`

```
MLWSVM  up 12.0s  processes 4 (max 4, spawned 4)  insns 33.9M  heap 712.0KB

   PID STATE     %CPU    INSNS     HEAP      RAW   STACK  MBOX MSGS IN/OUT   GC  FUNCTION
     2 ready     46.5    16.9M   356.0K        0       0     0         0/0   97  busy.spinner+7 (busy.masm:9)
     1 sleeping   0.0        4        0        0       0     0         0/0    0  busy.main+4 (busy.masm:20)

recent exits:
     7 worker           failed E_BOUNDS        1.2K insns
```

`top` redraws every second (`--interval MS`) and sorts processes by CPU. `--once`
prints one snapshot and exits; there, %CPU is measured over the VM's whole uptime.
It fetches `/top` from the monitor, so it works against any VM started with
`--monitor`.

## Testing the tools

* `make test` runs the debugger tests in `tests/tools/*.masm`, which use
  `debug-script`.
* `make check-tools` smoke-tests what an in-process test can't reach: the profiler
  and flame output, the tracer, and the monitor, by querying a live VM with
  `mlwsvm top --once` and, if available, `curl`.
