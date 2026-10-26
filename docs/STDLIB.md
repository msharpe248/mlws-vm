# The MLWSVM Standard Library

The standard library is a set of built-in natives, called with
`ncall "name" (args) -> (results)` ([NATIVES.md](NATIVES.md) lists every
signature), plus the assembly wrappers in `lib/std`. This document explains
how they behave: how they wait, what errors they raise, and how a sandbox
restricts them.

| Module | Status | Contents |
|---|---|---|
| `io`, `math`, `str`, `gc`, `proc`, `time`, `test` | since M2–M8 | printing, maths, string bytes, the collector, process count, the clock |
| `fs` | M12 | files, standard streams, directories, metadata |
| `os` | M12 | arguments, the environment, exit, running commands |
| `net` | M13 | TCP, UDP, DNS |
| `text`, `json` | M14 | formatting, string utilities, UTF-8, JSON |

## How I/O waits

I/O never holds up a worker thread, so many processes can each do I/O at once.
There are three ways a call waits:

* **Pipes and sockets the VM created** (pipes from `os.spawn`, and the sockets
  of M13) are non-blocking. When one is not ready, the process waits in the
  **poller** (kqueue on macOS and the BSDs, epoll on Linux), in the state `io`,
  exactly as a process waits in `recv`. It holds no thread, so 10,000 waiting
  processes cost 10,000 small processes, not 10,000 threads. The monitor shows
  them as `io`.
* **Regular files, and descriptors the VM inherited** (standard input) cannot be
  waited for that way, so those calls run on the **I/O thread pool** while the
  process waits (state `native`).
* **Everything else** (`os.env`, `fs.cwd`, ...) is quick and runs directly.

A wait for I/O is never mistaken for a deadlock: `main` only gets `E_DEADLOCK`
when no process is waiting for I/O, a timer or a blocking call.

**Timeouts.** `fs.set_timeout(handle, ms)` gives later reads and writes on a pipe
or socket a timeout. A timed-out call raises `E_TIMEOUT`, and the handle can
still be used. A timeout covers the whole operation, so `fs.write` of a large
buffer has one deadline no matter how many waits it takes.

**Kills.** A process killed while waiting for I/O stops at once. One in a call
on the I/O pool stops when that call returns.

## Handles

`fs.open`, `fs.stdin`, `fs.stdout`, `fs.stderr` and `os.spawn` return
**handles**: foreign handles (`typeof` 10) that print as `<foreign fs.file>`.
A handle is closed by `fs.close`, or when it is collected or its process ends,
so a forgotten file is closed eventually. Use after closing raises `E_CLOSED`.
Handles belong to their process and cannot be sent to another (`E_TYPE`).

* `fs.write` always writes everything, even to a pipe that is only ready for
  part of it.
* `fs.read_line` and `fs.read_all` read ahead into the handle's buffer. Mixing
  them with `fs.read` on the same handle is fine; mixing handles on one
  descriptor (two `fs.stdin()` handles) gives each its own buffer.
* `fs.stdout` writes go to the same place as `io.print` (and so are captured by
  the test runner); `fs.stderr` goes to standard error. They cannot be read.
* Seeking (`fs.seek`) works on files only.

## Networking

```asm
    ncall "net.listen" ("127.0.0.1", 8080) -> (R0)   ; "" listens on every address; port 0 picks one
1:  ncall "net.accept" (R0) -> (R1)                  ; waits in the poller
    ncall "fs.give" (R1) -> (I0)                     ; hand the connection to its own process
    spawn handle_connection (I0)
    jmp 1b

.func handle_connection(i64 ticket)
    ncall "fs.take" (ticket) -> (R0)
    ncall "fs.read_line" (R0) -> (R1)                ; connections are streams: fs.read, fs.write, ...
```

* A connection is a stream handle: read and write it with `fs.read`,
  `fs.read_line`, `fs.read_all` and `fs.write`, and close it with `fs.close`.
  `net.close_write` sends end-of-input but keeps reading. `fs.set_timeout`
  applies to connections and listeners alike.
* `net.connect(host, port, timeout_ms)` resolves the name and connects on the
  I/O pool, trying each address in turn. Everything after that uses the poller.
  Connections have `TCP_NODELAY` set, which suits request/reply protocols.
* UDP: `net.udp(host, port)` makes a bound socket, `net.send_to` sends a
  datagram, and `net.recv_from` waits for one and returns its sender.
* DNS: `net.resolve(name)` returns the addresses. Names given to `listen`,
  `connect`, `udp` or `send_to` are resolved on the I/O pool; addresses are not.
* Thousands of connections are cheap: each costs a descriptor, a small process
  and a buffer, never a thread. `bench/connections.masm` holds 10,000 open at
  once (20,000 descriptors, since both ends are in the same VM). At startup
  MLWSVM raises its descriptor limit as far as the system allows.
* Writing to a connection the other end has closed raises `E_RESET`. The VM
  ignores `SIGPIPE`, so this never kills the program.

### Handing a handle to another process

Handles belong to their process, so a server that accepts in one process and
serves in another moves the connection explicitly. `fs.give(handle)` detaches
the descriptor (the handle is closed from then on) and returns a **ticket**: a
random 64-bit number that can be sent in a message or passed to `spawn`.
`fs.take(ticket)` turns it into a handle in the receiving process, exactly once.
A ticket cannot usefully be guessed, and one never taken is closed when the VM
ends. This works for files and pipes too. A handle with read-ahead input
buffered, or a write in progress, cannot be given.

## Text and JSON

### `text.format`

```asm
    lnew R0
    lpush R0, name, score, ratio
    ncall "text.format" ("{} scored {:>4} ({:.1f}%)", R0) -> (R1)
```

`{}` takes the next argument from the list, and `{2}` a given one. `{{` and
`}}` are literal braces. A spec follows a colon, `{:[[fill]align][+][0][width][.precision][type]}`,
as in Python and Rust:

| Part | Meaning |
|---|---|
| align `<` `>` `^` | left, right, centre; with an optional fill character first (`{:*^9}`) |
| `+` | show the sign of positive numbers |
| `0` | pad numbers with zeros after the sign |
| width | the minimum width, in characters |
| `.precision` | digits after the point for floats; the maximum characters for text |
| type `d` `x` `X` `o` `b` | integers in decimal, hex, octal or binary |
| type `f` `e` `g` | floats (integers and BigInt/decimal are converted) |
| type `s` | the value's text (the default) |
| type `q` | a string quoted, with escapes |

Without a type, every value has a natural form: integers in decimal, floats
in the shortest form that round-trips, `true`/`false`, characters as
themselves, BigInts and decimals exactly, and other objects as a description.
The arguments are typed list elements, so a mismatch (`{:x}` of a string, a
missing argument) is an error (`E_TYPE`, `E_BOUNDS`), never a crash. This
makes it the safe alternative to C's `printf`.

### Strings and UTF-8

Strings are bytes, by convention UTF-8. `text.find` and `text.slice` use byte
offsets, so they are fast and never split anything unexpectedly. Work on
characters goes through `text.next` (iterate without allocating),
`text.chars`, `text.length` and `text.from_chars`; these read an invalid byte
as one U+FFFD. `text.valid` checks a whole string. `text.upper` and `text.lower`
change ASCII letters only (full Unicode case mapping needs tables, and is
planned). `text.split` with an empty separator splits on whitespace;
`text.join` accepts lists of any values.

### JSON

| JSON | MLWSVM |
|---|---|
| object | map with string keys, in the order written (a repeated key: the last wins) |
| array | list |
| string | string (escapes, including `\u` surrogate pairs, decoded) |
| integer | `i64`, or a BigInt if it does not fit: integers are never rounded |
| other numbers | `f64` |
| `true`, `false` | bool |
| `null` | null |

`json.parse` accepts a document whose top level is an object or array (the
usual case). `json.parse_any` accepts any value and returns it as the only
element of a list, so a top-level number or boolean keeps its type. Errors are
`E_JSON` with the line and column. Nesting deeper than 256 levels is refused,
so hostile input cannot exhaust the stack.

`json.stringify(value, indent)` writes maps (string or integer keys), lists,
typed arrays, structs (as objects of their fields), strings, numbers,
BigInts and decimals (exactly), booleans and null. `indent` 0 gives compact
output, and n > 0 indents by n spaces per level. NaN, infinity, non-UTF-8 text
and cycles are refused with `E_JSON`.

## Errors

Every failure raises an ordinary, catchable `Error`. The message names the call
and the path, and gives the OS's description:

| Code | Meaning |
|---|---|
| `E_NOT_FOUND` | no such file, directory or command |
| `E_EXISTS` | it already exists (including `fs.open` with `x`), or a directory is not empty |
| `E_PERMISSION` | not allowed by the OS |
| `E_IS_DIR`, `E_NOT_DIR` | a directory where a file was expected, or the reverse |
| `E_CLOSED` | the handle has been closed |
| `E_TIMEOUT` | a timeout set with `fs.set_timeout` passed |
| `E_RESET` | the other end of a pipe or connection closed it |
| `E_REFUSED` | nothing is listening at that address |
| `E_ADDR_IN_USE` | `net.listen` on a port already in use |
| `E_RESOURCES` | out of descriptors, memory or disk space |
| `E_IO` | any other OS error |
| `E_TYPE` | a wrong argument (a handle of another kind, a bad mode or format spec) |
| `E_JSON` | malformed JSON (with line and column), or a value with no JSON form |

## Commands

`os.run(argv)` runs a command to completion and returns its exit code and its
standard output. `os.spawn(argv, pipes)` starts one and returns
`(process, stdin, stdout, stderr)`. `pipes` chooses which streams are piped (1
stdin, 2 stdout, 4 stderr, 8 stderr into stdout); the others are inherited and
come back null. The program is found on `PATH`. Arguments are passed as given,
with no shell, so nothing needs quoting. To use a shell, run `sh -c "..."`
explicitly.

`os.wait` returns the exit code, or 128 + N for a command killed by signal N.
`os.kill(process, force)` asks it to terminate, or kills it outright. A command
whose handle is dropped without `os.wait` keeps running; it is reaped by the OS
when MLWSVM exits.

## The program's view

* `os.args()`: the arguments after the program file:
  `mlwsvm run prog.masm a b` gives `["a", "b"]`.
* `os.env(name)` and `os.environ()`: the environment. There is no `setenv`:
  the environment is shared by every process and thread, so it is read-only.
* `os.exit(code)`: ends the whole program from any process, with that exit
  code.
* There is no `chdir`, for the same reason: the current directory is shared.
  Build paths from `fs.cwd()` instead.

## Sandboxing

Quota kind 5 counts a process's OS calls: every `fs.*` and `os.*` call. A
sandbox lowers it to 0 (`quota 5, 0`, or `--quota os=0` for the whole program)
to deny the code it runs any access to files, commands or the environment.
Children inherit the limit. Kind 4 does the same for native extensions and the
FFI ([EXTENDING.md](EXTENDING.md)).

## Platforms

POSIX systems with kqueue or epoll: macOS, Linux and the BSDs. On Windows the
`fs` and `os` natives are not yet implemented (an IOCP-based poller is on the
roadmap); they fail with `E_IO`.
