# The MLWSVM Standard Library

The standard library is a set of built-in natives, called with
`ncall "name" (args) -> (results)` ([NATIVES.md](NATIVES.md) lists every
signature), plus the assembly wrappers in `lib/std`. This document explains
how they behave: how they wait, what errors they raise, and how a sandbox
restricts them.

| Module | Contents |
|---|---|
| `io`, `str`, `gc`, `test` | printing, string bytes, the collector, test failures |
| `fs` | files, standard streams, directories, metadata |
| `os` | arguments, the environment, exit, running commands |
| `net` | TCP, UDP, DNS |
| `text`, `json` | formatting, string utilities, UTF-8, JSON |
| `proc`, `std.supervisor` | the process count, a name registry, request/reply calls, supervisors |
| `time` | clocks, dates and ISO 8601 |
| `hash`, `random` | CRC-32, SHA-256, SipHash; random numbers |
| `hex`, `base64`, `url`, `csv` | encodings |
| `list`, `map` | sorting and searching, set operations |
| `math` | floating-point and integer maths |

## How I/O waits

I/O never holds up a worker thread, so many processes can each do I/O at once.
There are three ways a call waits:

* **Pipes and sockets the VM created** (pipes from `os.spawn`, and the sockets
  of `std.net`) are non-blocking. When one is not ready, the process waits in the
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
  Standard output is buffered: it is written out when the VM is idle, at exit,
  before anything is written to `fs.stderr` (so the two appear in the order the
  program wrote them), and when the program calls `io.flush`.
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

## Time and dates

A time is an `i64` of **nanoseconds since 1970-01-01T00:00:00Z**, which covers
the years 1678 to 2261. An **offset** is minutes east of UTC (+60 for Central
European winter time, −300 for New York). Calendar arithmetic is done by the
VM, in the proleptic Gregorian calendar, so results are the same on every
platform and do not depend on the time-zone database. Only
`time.local_offset` asks the operating system.

```asm
    ncall "time.now" -> (I0)                        ; the wall clock
    ncall "time.local_offset" (I0) -> (I1)          ; this machine's offset now
    ncall "time.format" (I0, I1) -> (R0)            ; "2026-10-26T14:05:09.25+01:00"
    ncall "time.parse" ("2026-10-26T14:05:09Z") -> (I2)
    ncall "time.date" (I2, 0) -> (R1)               ; R1->Date.year, .month, .weekday, ...
    add R1->Date.month, 1                           ; a month later
    ncall "time.from_date" (R1) -> (I3)
```

* `time.now` is the wall clock, which can jump when the system clock is set.
  `time.monotonic` never goes backwards: use it to measure durations.
  `time.now_ns` is the same clock as `time.monotonic`.
* `time.format(t, offset)` writes RFC 3339 / ISO 8601, with as many fraction
  digits as the time needs (none, 3, 6 or 9), and `Z` for offset 0.
* `time.parse` accepts a date (`2026-10-26`, midnight UTC), or a date and time
  separated by `T` or a space, with optional seconds, a fraction (`.` or `,`,
  up to 9 digits) and an offset (`Z`, `+01:00`, `-0500`). With no offset, the
  time is UTC. Anything else, or an impossible date, is `E_PARSE`.
* `time.date(t, offset)` returns a built-in `Date` struct: `year`, `month`
  (1–12), `day`, `hour`, `minute`, `second`, `nano`, `weekday` (ISO: 1 Monday
  … 7 Sunday), `yday` (1–366) and `offset`. `time.from_date` turns one back
  into a time. Fields out of range carry over, as with C's `mktime`: month 13
  is January of the next year, and day 0 is the last day of the month before.
  `weekday` and `yday` are ignored.

## Hashes

| Native | Result | Use |
|---|---|---|
| `hash.crc32(data)` | `u32` | Checksums: the CRC-32 of zlib, PNG and Ethernet |
| `hash.sha256(data)` | 32 bytes | Content addressing and integrity (`hex.encode` for text) |
| `hash.siphash(key, data)` | `u64` | Hash tables keyed with a secret 16-byte key (`random.bytes(16)`), so that input chosen by an attacker cannot make them slow |

`data` is a string or a byte buffer.

## Random numbers

Each process has its own generator (xoshiro256\*\*). It is seeded from the
OS's entropy source when first used, so two processes never share a sequence.
`random.seed(n)` makes a process's sequence repeatable. `random.int(lo, hi)`
is uniform over the inclusive range, without modulo bias, and
`random.float()` is uniform in [0, 1) with 53 random bits. `random.shuffle`
shuffles a list in place.

The generator is fast but predictable from its output: use
`random.bytes(n)`, which reads the OS's entropy source directly, for keys,
tokens, nonces and passwords. It returns up to 16 MiB at a time (`E_RANGE`
beyond that).

## Encodings

| Natives | Format |
|---|---|
| `hex.encode`, `hex.decode` | Lower-case hex out; either case in |
| `base64.encode(data, url)`, `base64.decode` | RFC 4648. `url` true: the URL-safe alphabet (`-` `_`) without padding. Decoding accepts both alphabets, with or without padding, and ignores whitespace |
| `url.encode`, `url.decode(text, plus)` | Percent-encoding (RFC 3986): everything except `A-Z a-z 0-9 - . _ ~` is escaped. `plus` true decodes `+` as a space (HTML form data) |
| `csv.parse`, `csv.format` | RFC 4180: a list of rows, each a list of strings. Quoted fields may hold commas, line breaks and `""` for a quote. Rows end in `\n` or `\r\n` when read, and in `\n` when written. `csv.format` quotes only the fields that need it, writes other values as `tostr` does, and null as an empty field |

Malformed input is `E_PARSE`, with the offset of the problem.

## Sorting, searching and sets

```asm
.func by_length(ref a, ref b) -> i64
    len I0, a
    len I1, b
    sub I2, I0, I1
    ret I2
.endf
    closure by_length () -> (R1)
    ncall "list.sort" (R0, R1)          ; stable, in place
    ncall "list.sort" (R0, null)        ; numbers or strings in natural order
```

* `list.sort(list, cmp)` is a stable merge sort. `cmp(a, b)` returns negative,
  0 or positive. Its parameter types must match the elements (`E_TYPE`
  otherwise). With `null`, numbers sort by value (NaN last) and strings and
  byte buffers bytewise. A mix of the two is `E_TYPE`.
* `list.bsearch(list, probe)` searches a sorted list. `probe(x)` returns
  negative if `x` comes before what is sought, 0 if `x` is it, and positive if
  after. The result is the index, or `-(insertion point) - 1`. A probe
  usually captures what it looks for: `closure probe (key) -> (R1)`.
* The closures run during the native, through the same re-entrant path as C
  callbacks ([EXTENDING.md](EXTENDING.md#callbacks)). So they cannot wait
  (`E_CALLBACK`), and an exception they do not catch comes out of the native
  exactly as it was thrown. The list may be changed by the comparator, but if
  its length changes, `list.sort` fails with `E_BOUNDS`.
* `map.union(a, b)`, `map.intersect(a, b)` and `map.difference(a, b)` return
  new maps. Union has the entries of both (b's value wins). Intersection keeps
  a's entries whose keys b has, and difference keeps those it does not have.
  Keys keep their order.

## Integer maths

`math.powmod(b, e, m)` computes b^e mod m without overflow, and the result is in
[0, m). It needs e >= 0 and m > 0 (`E_RANGE` otherwise). `math.gcd` and `math.lcm` work on the absolute values, and `math.lcm`
raises `E_OVERFLOW` when the result does not fit in an `i64`. `std.math`
(`lib/std/math.masm`) has more, written in assembly: `ipow`, `isqrt`,
`fact` and `clamp`.

## Processes: names, calls and supervisors

### The registry

`proc.register(name, pid)` names a process, `proc.whereis(name)` looks one up
(0 if none), and `proc.unregister(name)` removes a name. A name belongs to one
live process at a time (`E_EXISTS` otherwise), and is released automatically
when its process ends, so a restarted service simply registers again.

### Request and reply

```asm
    ncall "proc.call" (server, request, 1000) -> (R0)   ; wait up to 1 s for the answer

; in the server:
    recv R1
    isa I0, R1, Call                    ; Call{from, id, body}
    ...
    ncall "proc.reply" (R1, answer)
```

`proc.call` sends a built-in `Call` struct (`from`, `id`, `body`) to the server's
mailbox and waits, like `recv`, for the answer. The answer goes to the caller's
**reply slot**, not its mailbox. So the caller never has to pick the reply out
from other messages (MLWSVM has no selective receive), and a reply that arrives
after its call timed out (`E_TIMEOUT`) is dropped, never mistaken for the
answer to a later call. A call to a process that does not exist raises
`E_NOPROC`. A call that can never be answered (every process waiting) raises
`E_DEADLOCK`, as `recv` does, and so does a process calling itself. Requests and replies are references (wrap
numbers in a list or struct), deep-copied like any message.

### Supervisors (`std.supervisor`)

```asm
.import std.supervisor as sup
    closure worker (config) -> (R0)            ; each child: a closure with no parameters
    lnew R1
    lpush R1, R0
    call sup.start (R1, 0, 5, 10000) -> (I0)   ; one-for-one; at most 5 restarts in 10 s
```

A supervisor starts each child monitored (`spawnim`). When one fails, it
restarts that child (strategy 0, one-for-one) or all of them (1, one-for-all). A
child that exits normally is left alone; when all have, the supervisor exits
normally. More than `max_restarts` failures within `period_ms` means restarting
is not helping: the supervisor kills its children and fails with `E_RESTARTS`,
so whatever supervises *it* can react. This is the restart intensity of
Erlang/OTP. A supervisor answers the call `"children"` with the current child
pids, and stops on the message `"stop"`. It is written in MLWSVM assembly
(`lib/std/supervisor.masm`), on top of the instructions and natives in this
document.

### Example: `examples/http_server.masm`

A small HTTP/1.1 server (keep-alive, `/`, `/json`, and two routes that fail on
purpose) that puts the pieces together:

* one process per connection, each with its own 8 MB heap quota. `GET /crash`
  (an uncaught exception) and `GET /hog` (the heap quota) end only that
  handler; the VM reports it on stderr, and other connections never notice.
* the acceptor runs under a supervisor; the logger is found by name.
* `mlwsvm run examples/http_server.masm 8080` serves for real; with no
  arguments it tests itself (25 concurrent clients, the acceptor killed
  halfway, every request still answered), which is what `make test` runs.

On an Apple M5 Pro laptop, ApacheBench (`ab -n 20000 -c 50`) measures about
36,000 requests per second (ab itself is single-threaded, so this is a lower
bound). Across two Linux desktops, with `examples/http_load.masm` as the
client, it serves about 170,000 requests per second over 200 or 1,000
connections, and keeps doing so while handlers are made to fail
([PERFORMANCE.md](PERFORMANCE.md#http)).

`examples/http_load.masm HOST PORT CONNECTIONS REQUESTS [PATH]` is the load
generator: a process per connection, each making keep-alive requests and
timing them.

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
| `E_TIMEOUT`, `E_NOPROC`, `E_DEADLOCK` | `proc.call`: no reply in time, no such process, or no reply possible |
| `E_RESTARTS` | a supervisor gave up: its children failed too often |
| `E_PARSE` | malformed text for `time.parse`, `hex.decode`, `base64.decode`, `url.decode` or `csv.parse` |
| `E_RANGE` | an argument out of range (an offset beyond a day, a time or `Date` outside the years 1678 to 2261, `random.int` with lo > hi, `random.bytes` of more than 16 MiB, a SipHash key that is not 16 bytes, `math.powmod` with e < 0 or m <= 0) |
| `E_OVERFLOW` | a result that does not fit (`math.lcm`) |
| `E_CALLBACK` | a comparator, probe or C callback tried to wait |

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

Quota kind 5 counts a process's OS calls: every `fs.*`, `os.*` and `net.*`
call. A sandbox lowers it to 0 (`quota 5, 0`, or `--quota os=0` for the whole
program) to deny the code it runs any access to files, commands, the network or
the environment.
Children inherit the limit. Kind 4 does the same for native extensions and the
FFI ([EXTENDING.md](EXTENDING.md)).

## Platforms

POSIX systems with kqueue or epoll: macOS, Linux and the BSDs. On Windows the
`fs` and `os` natives are not yet implemented (an IOCP-based poller is on the
roadmap); they fail with `E_IO`.
