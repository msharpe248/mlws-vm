# Writing Programs for MLWSVM

A tour for someone who wants to write programs, from the first one to one
that serves the web, is tested, debugged and shipped. Each part shows a
little and points to a complete example in [`examples/`](../examples/README.md)
(every one of them runs in `make test`, so they work) and to the reference
for the rest.

The other documents: [ASSEMBLY.md](ASSEMBLY.md) is the language's reference,
[STDLIB.md](STDLIB.md) the library's, [TOOLING.md](TOOLING.md) the tools',
[`docs/learn/`](learn/README.md) explains how the VM works inside, and
[TARGETING.md](TARGETING.md) is for writing a compiler that targets it.

## 1. Getting it, and running a program

Download a release's archive for your system and unpack it (the
[README](../README.md#download) says how), or build it: `make` needs only a
C11 compiler.

```sh
./mlwsvm run examples/hello.masm
./mlwsvm run examples/cat.masm examples/data/poem.txt
echo hello | ./mlwsvm run examples/cat.masm
```

What follows the program's file is its arguments (`os.args`). A program is a
`.masm` file of MLWSVM assembly, which the VM assembles as it loads it.
(`mlwsvm asm` makes a `.mlwso` file ahead of time, and the module cache
remembers what it assembled, so a program starts in a few milliseconds.)

## 2. The first programs

```asm
; hello.masm
.import std.io as io

.func main
    call io.println ("Hello, MLWSVM!")
    ret 0
.endf
```

Every program has a `main`; what it returns is the exit code. A line is an
instruction, a directive (`.func`, `.import`) or a label; `;` starts a
comment. Here is one that reads its input:

```asm
; number.masm - standard input, each line with its number
.include "std/io.inc"

.func main
    .alias in, R0
    .alias line, R1
    .alias n, I0
    ncall "fs.stdin" () -> (in)
    mov n, 0
next:
    ncall "fs.read_line" (in) -> (line)   ; null at the end of the input
    bz.r line, done
    inc n
    tostr R2, n
    strcat R3, R2, ": ", line
    PRINTLN_S R3
    jmp next
done:
    ret 0
.endf
```

```
$ printf 'first\nsecond\n' | mlwsvm run number.masm
1: first
2: second
```

* **Registers** hold everything: `I0`–`I31` integers, `F0`–`F31` floats,
  `R0`–`R31` references (strings, lists, maps, structs, closures), `P0`–`P31`
  raw pointers. Each call gets registers of its own, so nothing needs saving
  around a call. `.alias` gives one a name.
* **Instructions** read their operands left to right, the result first:
  `strcat R3, R2, ": ", line` makes R3. Most take a type suffix when it
  matters (`bz.r`: is this reference null?).
* **Labels** name places to jump to; `1:` with `1f` (forward) and `1b`
  (back) are for short loops.
* `ncall "name" (args) -> (results)` calls the standard library's built-in
  functions; `PRINTLN_S` is a macro from `std/io.inc` that does the same.

[`examples/cat.masm`](../examples/cat.masm), [`grep.masm`](../examples/grep.masm)
and [`wc.masm`](../examples/wc.masm) are the Unix tools of the same name,
with files, options and errors.

## 3. Functions

```asm
; area(w, h) -> w * h, and whether it is a square
.func area(i64 w, i64 h) -> (i64, i64)
    mul I2, w, h            ; w is I0, h is I1: parameters are registers
    seq I3, w, h
    ret I2, I3
.endf

    call area (3, 4) -> (I0, I1)    ; 12, 0
```

Parameters are given registers bank by bank, in order: `(ref name, i64 n,
ref list)` is R0, I0 and R1. A function may return several values. The
verifier checks every function before it runs: a register read as the wrong
type, a call with the wrong arguments, a path that runs off the end, each is
an error when the program loads, not when it runs.

## 4. Values

```asm
.struct Point
    x: i64
    y: i64
.ends

    lnew R0                     ; a list
    lpush R0, 10, 20, 30
    len I0, R0                  ; 3
    mov I1, R0[1]               ; 20
    mnew R1                     ; a map
    mset R1, "apples", 3
    mget I2, R1, "apples"       ; 3
    mget I3, R1, "pears", 0     ; 0: the default, for a key it does not have
    new R2, Point               ; a struct
    mov R2->Point.x, 5
    strcat R3, "list of ", "three"
```

Lists, maps (which keep their keys in the order they were added), structs,
strings and byte buffers are on the process's heap, and collected when
nothing refers to them. Each element keeps its type: reading one as another
raises `E_TYPE` rather than giving nonsense. Integers wrap around as in C;
BigInts (`123n`) grow, and decimals (`19.99d`) are exact, for money.

* [`wordcount.masm`](../examples/wordcount.masm): strings, lists, maps and a
  struct together.
* [`sales.masm`](../examples/sales.masm) reads a CSV file, sums it in a map
  and sorts with a comparator; [`tasks.masm`](../examples/tasks.masm) reads
  and writes JSON.
* [`numbers.masm`](../examples/numbers.masm): each kind of number, and money
  rounded to cents.
* [`generators.masm`](../examples/generators.masm): closures and coroutines.

## 5. Errors

```asm
.try failed
    ncall "fs.open" (name, "r") -> (R2)
    call copy (R2, out)
.endtry
    ...
failed:
    catch R3                           ; an Error: its code and message
    strcat R5, "cat: ", R3->Error.message, "\n"
```

Anything raised inside `.try ... .endtry`, there or in a function it calls,
goes to the label, where `catch` takes it. The VM's own errors are `Error`
structs with a `code` (`E_DIV_ZERO`, `E_NOT_FOUND`; [ERRORS.md](ERRORS.md)
lists them) and a `message`; `throw` raises one of the program's own, or any
other value. An error nothing catches ends its process, and says where.
[`errors.masm`](../examples/errors.masm) shows each case.

## 6. The standard library

Three kinds of thing, all in [STDLIB.md](STDLIB.md):

* **Built-in functions**, called with `ncall`: files (`fs.*`), names of files
  (`path.*`), commands (`os.*`), networks (`net.*`, `tls.*`), text, JSON,
  regular expressions, time, hashes, encodings ([NATIVES.md](NATIVES.md)
  lists each with its arguments).
* **Modules in MLWSVM assembly**, imported: `.import std.http as http`, then
  `call http.get (url, 5000) -> (R0)`. `std.server`, `std.supervisor`,
  `std.pool`, `std.http`, `std.node`.
* **Macros**, included: `std/io.inc` for printing, `std/test.inc` for
  assertions.

A program's own modules are imported the same way, found beside it or on
the library path (`-L dir`); `mlwsvm lib` makes versioned archives of them.

## 7. Processes

A process is a function running by itself, with its own heap, in a VM that
runs millions of them on a few threads. They share nothing: they send each
other messages, which are copied.

```asm
    spawn total (PID) -> (I0)          ; start one: its pid
    send I0, 42                        ; a message to it
    recv R0                            ; wait for one
    recvt R0, 50, 1f                   ; or for 50 ms at most
```

* [`pipeline.masm`](../examples/pipeline.masm): processes in a chain, each a
  stage.
* [`timers.masm`](../examples/timers.masm): waiting with a time limit,
  sleeping, a ticker.
* [`workers.masm`](../examples/workers.masm): work split across processes,
  each monitored, a failure seen and handled.

A process's failure is its own: the others go on. `monitor pid` sends a
`Down` message when it ends, saying how; `link` ties two processes' fates.
Waiting for a message, a timer or I/O holds no thread.

## 8. Servers, supervisors and pools

The patterns of Erlang/OTP, as library modules:

* **`std.server`** keeps a state and answers requests (`proc.call`) one at a
  time, so nothing races: [`kvstore.masm`](../examples/kvstore.masm) is a
  key-value store used by several clients at once.
* **`std.supervisor`** restarts children that fail, within a limit:
  [`http_server.masm`](../examples/http_server.masm) keeps its acceptor
  running this way.
* **`std.pool`** runs one function on many jobs with a bounded number of
  workers: [`primes.masm`](../examples/primes.masm).
* **Names**: `proc.register` and `proc.whereis` find a process by name;
  `std.node` reaches processes on other VMs, on this machine or another.

## 9. The web

```asm
.import std.http as http

; handle(request) -> response: a map, or a string for plain text
.func handle(ref req) -> ref
    mget R1, req, "path"
    ...
.endf

    closure handle -> (R1)
    call http.start ("", 8080, R1) -> (I2, I3)
```

`std.http` serves each connection in a process of its own and calls the
handler with each request, as a map; its client fetches with `http.get` and
`http.post`, over TLS for `https`. [`web.masm`](../examples/web.masm) is a
page, a JSON API and a form; [STDLIB.md](STDLIB.md#http-stdhttp) has the rest.

## 10. Testing

`mlwsvm test DIR` runs every `.masm` file under a directory and checks what
its comments expect:

```asm
; args: -n -i ^(the|on)\s
; stdin: The fog comes
; stdin: on little cat feet.
; expect-stdout: 1:The fog comes
; expect-stdout: 2:on little cat feet.
```

Each test is a program: its output is compared line by line, its exit code
with `; expect-exit:` (0 if not given), or an error it must raise with `;
expect-error: E_CODE`. `; stdin:` lines are its input, `; args:` its
arguments. `std/test.inc` has `ASSERT_EQ` and its kind for checks inside.
[ASSEMBLY.md](ASSEMBLY.md#11-tools) lists every directive. Every example in
`examples/` is a test this way.

## 11. Watching and debugging

* **`--monitor PORT`** opens a web dashboard on a running program: its
  processes (sort them, find one), what each is doing, its log, its
  messages, links, environments. `mlwsvm top HOST:PORT` shows it in a
  terminal. A program writes to the log with `monitor.log`.
* **Debug**, beside a process on the dashboard, pauses that one process and
  opens a debugger for it in a tab of its own, while the rest of the program
  runs: its source, breakpoints (with conditions, hit counts, or messages
  logged without stopping), steps, the call stack, the variables, its
  mailbox. `mlwsvm serve --programs DIR` starts programs from the dashboard,
  and its **Debug** starts one paused before its first instruction.
* **`mlwsvm debug prog.masm`** is a debugger in the terminal, and **`mlwsvm
  dap`** one for editors: `editors/vscode` gives VS Code highlighting and
  debugging of `.masm` files.

[TOOLING.md](TOOLING.md) describes each.

## 12. Making it fast

```sh
mlwsvm run --profile prog.masm        # where the time goes, by function and line
mlwsvm run --jit on prog.masm         # hot functions as machine code
mlwsvm run --opt 2 prog.masm          # the optimizer, as each module loads
```

The JIT (x86-64 and arm64) compiles a function once it is hot and runs the
rest interpreted; the optimizer removes work before either sees it.
[PERFORMANCE.md](PERFORMANCE.md) has the measurements, [JIT.md](JIT.md) and
[OPTIMIZER.md](OPTIMIZER.md) the details.

## 13. Shipping it

```sh
mlwsvm bundle -o app --jit on app.masm          # one executable: the VM, the program, what it imports
mlwsvm run --cap read=/data,connect=api.example.org:443 app.masm   # only what it needs
```

A bundle runs where it was built for, with nothing else installed.
Capabilities (`--cap`) and quotas (`--quota heap=256M`) limit what a program,
or one of its processes, may reach and use ([STDLIB.md](STDLIB.md#sandboxing)).
A program can run inside a C or C++ one as well ([EMBEDDING.md](EMBEDDING.md)),
and call C libraries directly ([EXTENDING.md](EXTENDING.md)).
