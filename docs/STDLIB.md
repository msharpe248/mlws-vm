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
| `net`, `tls` | TCP, UDP, DNS, TLS |
| `text`, `json`, `regex` | formatting, string utilities, UTF-8, JSON, regular expressions |
| `proc`, `std.supervisor`, `std.server` | the process count, a name registry, request/reply calls, supervisors, generic servers |
| `std.node` | processes on other VMs |
| `weak` | weak references and weak maps (and `gc.finalize`) |
| `cap` | limiting what a process may reach |
| `module` | loading modules while the program runs ([ABI.md §9](ABI.md#9-loading-modules-at-run-time)) |
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
  Each process's output goes out a whole line at a time, so lines of processes
  running at once never mix ([ABI.md §1](ABI.md#1-processes) says when an
  unfinished line goes out). Standard output as a whole is buffered: it is
  written out when the VM is idle, at exit, before anything is written to
  `fs.stderr` (so the two appear in the order the program wrote them), and
  when the program calls `io.flush`, which also writes out the calling
  process's unfinished line.
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
* Ports are 1 to 65535 for `net.connect` and `net.send_to`, and 0 to 65535 for
  `net.listen` and `net.udp`, where 0 picks a free one; any other is `E_RANGE`.
* The side that closes a connection first keeps its port for a while after
  (TCP's TIME_WAIT, a minute or more). A client that opens and closes many
  connections quickly can run out of local ports, and `net.connect` then fails
  with `E_RESOURCES` (Windows has about 16,000 such ports, Linux about 28,000
  for each destination). Where the protocol allows it, let the server close
  first: its waiting connections hold only the port it listens on.
* UDP: `net.udp(host, port)` makes a bound socket, `net.send_to` sends a
  datagram, and `net.recv_from` waits for one and returns its sender.
* DNS: `net.resolve(name)` returns the addresses. Names given to `listen`,
  `connect`, `udp` or `send_to` are resolved on the I/O pool; addresses are not.
* Thousands of connections are cheap: each costs a descriptor, a small process
  and a buffer, never a thread. `bench/connections.masm` holds 10,000 open at
  once (20,000 descriptors, since both ends are in the same VM). At startup
  MLWSVM raises its descriptor limit as far as the system allows.
* `net.split(conn)` gives a second handle on a connection, so that one process
  can read it while another writes. `net.read_frame(conn)` reads a message of a
  32-bit little-endian length and that many bytes, up to 64 MiB, for protocols
  framed that way.
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

### TLS

```asm
    ncall "net.connect" ("example.org", 443, 5000) -> (R0)
    ncall "tls.connect" (R0, "example.org", "")      ; "": the system's authorities
    ncall "fs.write" (R0, "GET / HTTP/1.0\r\nHost: example.org\r\n\r\n")
    ncall "fs.read_all" (R0) -> (R1)
```

`tls.connect(conn, host, authorities)` and `tls.accept(conn, certificate, key)`
put TLS on a connection, which is then read and written as before, with
`fs.read`, `fs.read_line` and `fs.write`, its process waiting in the poller, so
TLS costs no threads either. They can come after plain traffic, as STARTTLS does.
`net.close_write` sends TLS's end of input first, and `fs.close` too. A connection
keeps its TLS when it is given to another process (`fs.give`), and `net.split`
shares it between the two handles, a call at a time.

* **A client always checks the certificate:** it must match `host`, and come
  from an authority in the PEM file named, or the system's for `""`. There is no
  way to turn this off. For a private server, name its certificate as the
  authority.
* **A server** gives PEM files of a certificate chain and its key.
* **Versions:** TLS 1.2 and later. `tls.version` says which a connection got.
* Failures are `E_TLS`, and a failed handshake leaves the connection to be
  closed.

MLWSVM has no cryptography of its own. It loads the system's OpenSSL or LibreSSL
when first asked, so it needs neither to build or to run: `tls.available()` says
whether one is there, and without one `tls.connect` raises `E_TLS`. On macOS it
looks only for Homebrew's OpenSSL, since Apple's `libssl` refuses to be loaded.

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
as one U+FFFD. `text.valid` checks a whole string. `text.split` with an empty
separator splits on whitespace; `text.join` accepts lists of any values. `text.replace` replaces every occurrence of a string;
`text.replace_n` replaces the first n and also returns how many it
replaced, as Lua's `gsub` of a plain pattern does, where an empty string
occurs before each byte and at the end. A negative n replaces every
occurrence (Lua's `gsub` replaces none).

### Interned strings

Every module's literal of a text is one object in the whole VM, so `beq.r R0,
"ok"` tests identity, not text, and a string made at run time is never that
object. `text.intern(s)` returns the one string with `s`'s text: the literal
itself if a module has one, or else a string made now, which every module
loaded later shares as its literal of that text. That is how a language's atoms
or symbols, some written in its source and some made by the program (Erlang's
`list_to_atom`), compare by identity across modules:

```asm
    ncall "text.intern" (R0) -> (R0)
    beq.r R0, "ok", matched
```

Interned strings last as long as the VM, as literals do: intern what is known to
be few, not every string a program reads. A byte buffer, or text with a NUL in
it, is `E_TYPE`.

### Unicode

The text natives follow Unicode 17.0 (`text.unicode_version`), from tables
generated from the Unicode Character Database (`build-tools/gen_unicode.py`,
`make unicode UCD=dir`), and pass the Unicode Consortium's own tests for
normalization and for grapheme and word boundaries
(`tests/text/unicode_normalization.masm`, `unicode_breaks.masm`).

| What | Natives |
|---|---|
| Case | `text.upper`, `text.lower`: the full mappings (`ß` to `SS`, a final `Σ` to `ς`). `text.title`: in each word, the first cased character to title case and the rest to lower case. `text.fold`: full case folding. `text.ascii_upper` and `ascii_lower` change ASCII letters only, byte for byte, as C's `toupper` does in the `C` locale |
| Comparison | `text.equal_fold`: equal ignoring case and canonical equivalence (Unicode's canonical caseless match) |
| Normalization | `text.normalize(s, form)` and `text.is_normalized(s, form)`, for `"NFC"`, `"NFD"`, `"NFKC"` and `"NFKD"` |
| Segmentation | `text.graphemes` and `text.next_grapheme`: user-perceived characters (extended grapheme clusters, UAX #29). `text.words`: the words; `text.word_segments`: every segment between word boundaries, spaces and punctuation included |
| Width | `text.width`: columns on a terminal (UAX #11): 2 for wide characters and emoji, 0 for marks and controls, per grapheme cluster; `text.char_width` for one character |
| Properties | `text.category` (`"Lu"`, `"Nd"`, ...); `text.is_letter`, `is_alpha`, `is_digit`, `is_number`, `is_space`, `is_upper`, `is_lower`, `is_punct`, `is_emoji`; `text.is_id_start` and `is_id_continue` (XID_Start and XID_Continue, UAX #31, for a lexer's identifiers); `text.digit_value` |
| Whitespace | `text.trim`, `trim_start`, `trim_end` and `text.split` with an empty separator use White_Space, so `U+3000` and `U+00A0` count |

What is not provided: collation (sorting by language), bidirectional text,
line breaking, and mappings that depend on a language (Turkish and
Lithuanian casing): these need locale data, and belong to a library above.
A character outside the code space reads as an unassigned one.

### Regular expressions

```asm
    ncall "regex.compile" ("(?P<key>\\w+)=(\\d+)", "") -> (R0)
    ncall "regex.groups" (R0, "size=42", 0) -> (R1)        ; ["size=42", "size", "42"]
    ncall "regex.replace" (R0, "a=1 b=2", "$2:${key}", -1) -> (R2, I0)  ; "1:a 2:b", 2
```

`regex.compile(pattern, flags)` makes a regex; `flags` holds any of `i` (case
ignored), `m` (`^` and `$` at lines) and `s` (`.` matches a newline). It is a
handle of the process, and cannot be sent. A bad pattern is `E_REGEX`.

| Native | Result |
|---|---|
| `regex.test(re, text)` | whether it matches anywhere |
| `regex.find(re, text, from)` | the first match at or after a byte offset: byte offsets, the match's start and end, then each group's (-1: it took no part), or null |
| `regex.groups(re, text, from)` | the same, as strings (null: it took no part) |
| `regex.find_all(re, text)` | the text of every match |
| `regex.split(re, text)` | the pieces between matches |
| `regex.replace(re, text, with, n)` | up to n matches replaced (negative: all), and how many |
| `regex.group(re, name)` | a named group's number, or -1 |
| `regex.names(re)` | every group's name, in order: a list with one entry per group, null for one without a name |

**Syntax.** `.`, `[abc]`, `[^a-z]`, `*`, `+`, `?`, `{m}`, `{m,}`, `{m,n}` (up to
1000), lazy forms `*?`, `+?`, `??`, `{m,n}?`, `a|b`, `(...)`, `(?:...)`,
`(?P<name>...)`, `(?<name>...)`, `^`, `$`, `\b`, `\B`, `\A`, `\z`, flags `(?i)` and
`(?i:...)`, the escapes `\n \t \r \f \v \a \e \0 \xHH \x{H...} \uHHHH`, and classes
`\d \w \s \D \W \S`, `\p{L}`, `\p{Lu}`, `\p{Ll}`, `\p{N}`, `\p{Nd}`, `\p{P}` and their
`\P{...}` opposites, which are Unicode's. In a replacement, `$1`, `${1}` and
`${name}` are groups, and `$$` is a dollar sign.

**Matching** works on characters, not bytes: an invalid byte reads as one
U+FFFD. Of the matches starting at the leftmost place, the one the pattern
prefers wins, as in Perl: `a|ab` matches `a` in `ab`. `$` without `m` is the very
end of the text. After an empty match `regex.find_all` moves on a character, and
an empty match next to the previous match counts.

**Linear time.** The engine is a Pike VM: it follows every way of matching at
once, so its work is proportional to the text's length times the pattern's, for
any pattern and text. `(a|a)*b` cannot be made slow. That is why there are no
backreferences or lookaround, which raise `E_REGEX`. A match's work counts against
the process's instruction quota, so a sandboxed program cannot hide in one.

`make check-regex` compares the engine with Python's `re` on 12,000 random patterns
and texts: the same whole match every time, and the same groups but where a loop
over something that can match nothing leaves its capture, which follows RE2 and Go.

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

## Weak references and finalizers

`weak.ref(o)` makes a reference that does not keep `o` alive, and
`weak.get(w)` gives `o`, or null once a collection has found that nothing else
holds it.

`weak.map(m, mode)` makes a map weak: 1 for weak keys, 2 for weak values, 3 for
both, 0 for neither. It stays an ordinary map to every instruction, and a
collection removes an entry when a weak key or value is dead. Weak keys are
*ephemerons*: a value is held only while its key lives, so a cache from
objects to data that refers back to them does not keep them. A weak map sent
to another process is copied, and is weak there too.

`gc.finalize(o, f)` calls the closure `f` with `o`, once, after a collection
finds nothing else holds `o`. `f` runs in the same process, before the
process's next instruction, as a callback does: it cannot wait, and what it
throws is dropped. `f` must not hold `o`, or `o` never dies. `o` is whole
during the call, with everything it holds; if two such objects hold each other,
both are finalized. `f` may keep `o` (then `o` lives on) or register it again.
When a process ends, the finalizers not yet called are not called.

`gc.hold_finalizers(1)` makes the process's finalizers wait, once their
objects are found dead, until it calls `gc.run_finalizers`, which runs those
waiting (and any that their own collections find) and returns how many ran.
A language's runtime uses it so that a finalizer written in that language
never interrupts the runtime halfway through changing a table: it runs them
where its own state is whole, as at a call or a loop's back edge. A waiting
object is kept, with all it holds, so a process that holds its finalizers
and never runs them keeps those objects to its end. Inside a finalizer,
`gc.run_finalizers` runs none and returns 0. `gc.hold_finalizers(0)` lets them
run between instructions again, and runs those waiting before it returns. A
process starts with them not held, and the setting is its own: a process it
spawns does not inherit it.

An object is found dead by the next collection that looks at it: a minor
collection looks only at the young objects, so to be sure, call `gc.collect`.
A register keeps its object until it is written, whether or not the program
reads it again.

Strings, BigInts and decimals are values: they cannot be told from a copy, so
weak references and weak maps hold them like any other value, and
`gc.finalize` raises `E_TYPE`. So do weak references sent to another process.

For a dead object, the order is: weak references and weak values are cleared,
its finalizer runs, and its entry under a weak key goes at the collection
after that, so a finalizer can still look the object up in a weak-keyed map.

## Integer maths

`math.powmod(b, e, m)` computes b^e mod m without overflow, and the result is in
[0, m). It needs e >= 0 and m > 0 (`E_RANGE` otherwise). `math.gcd` and `math.lcm` work on the absolute values, and `math.lcm`
raises `E_OVERFLOW` when the result does not fit in an `i64`. `std.math`
(`lib/std/math.masm`) has more, written in assembly: `ipow`, `isqrt`,
`fact` and `clamp`.

## The monitor's log

`monitor.log(level, text)` and `monitor.event(name, value)` write to the log that
`--monitor` shows and serves (`GET /api/log`), and `monitor.on()` says whether a
monitor is there; without one they keep nothing ([TOOLING.md](TOOLING.md#the-programs-log)).

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

`proc.alive(pid, timeout_ms)` says whether a process runs, as the `alive`
instruction does, for a pid of this VM or of another node, which it asks,
waiting for the answer in the reply slot, as `proc.call` does (`E_TIMEOUT`
after `timeout_ms`; negative waits for ever). A node that is not connected,
or goes while asked, has none running. The `alive` instruction itself never
waits, so it answers for this VM's pids only.

### Other VMs (`std.node`)

```asm
.import std.node as node
    call node.start (1, "cookie", 4370) -> (I0)             ; this VM is node 1
    call node.connect ("db.local", 4370, "cookie") -> (I1)  ; returns its number
    call node.whereis (I1, "logger") -> (I2)                ; a registered process there
    send I2, "hello"                                        ; send, as to any process
    call node.spawn (I1, "worker", "serve", R0) -> (I3)     ; serve(R0), over there
    monitor I3                                              ; a Down when it ends
    ncall "proc.call" (I2, R1, 1000) -> (R2)                ; request and reply, there too
```

A pid may name a node: its top 16 bits are the node's number, and `send` to it
goes there. A node may also have a name: each node it connects to learns it, and
keeps it after the connection goes, so that the node of any pid can be named. A
connection from a node whose name or number this VM, or another connected node,
has is refused (`E_NODE`), since a pid's number must mean one node. Nodes that
have names and no numbers of their own choosing take one made from the name
(`start_named` with 0): two names may give one number, and then the second to
connect is refused. Messages between two processes keep their order, as on one VM. A
plain pid (`PID`) means this VM, so give others `node.self(PID)`, the same pid with
this node's number in it, which works here too, wherever a plain one does. `node.of(pid)` says which node a pid names.

| Function | Does |
|---|---|
| `start(number, cookie, port)` | number this VM (1 to 65535, unique in the cluster) and accept connections on a port (0: any); returns it |
| `start_tls(number, cookie, port, cert, key)` | the same, taking only TLS, with PEM files of a certificate chain and its key |
| `connect(host, port, cookie)` | connect to a node, and return its number; `E_NODE`, saying why, if the connection or the cookie fails |
| `connect_tls(host, port, cookie, ca)` | the same over TLS: the node's certificate must match `host` and come from an authority in `ca` (`""`: the system's) |
| `connected(number)` | whether a node is connected |
| `nodes()` | the numbers of the nodes connected now, as a list, in order |
| `set_name(name)` | name this VM (as Erlang's `name@host`): once, before it connects, at most 255 bytes |
| `name(number)` | a node's name, as it gave it when it last connected (`""`: none known); this VM's for 0 |
| `number(name)` | the number of the node with that name: this VM's, a connected node's, or the last that had it; 0 if none |
| `start_named(name, number, cookie, port)` | name this VM, number it (0: a number made from the name, `node.number_for`), start it as `start` does, and register the name with this machine's name service (`mlwsvm names`); returns the port. `E_NODE` without a service, or for a name registered already |
| `start_tls_named(name, number, cookie, port, cert, key)` | the same, taking only TLS |
| `connect_named(full, cookie)` | connect to the node named `name@host`, its port found from `host`'s name service; its number. `E_NODE` if it is not registered there |
| `connect_tls_named(full, cookie, ca)` | the same over TLS |
| `names(host)` | what a host's name service knows: a list of `[name, port]` |
| `watch()`, `unwatch()` | from now on, a `NodeEvent` comes to this process for each connection that comes or goes; or no more |
| `whereis(number, name)` | the pid registered under a name there (`proc.register`), or 0 |
| `ncall "node.name_pid" (number, name)` | a pid that stands for whichever process has the name there when a message arrives (below) |
| `spawn(number, module, function, arg)` | run an exported function of one reference there; its pid, or 0 |
| `self(pid)` | a pid as other nodes can use it |

* **What crosses** is what a message can be: numbers, strings, byte buffers,
  arrays, lists, maps, BigInts, decimals, structs, whose module the other end
  must have, and closures, sharing and cycles kept as a send on one VM keeps
  them: an object in two places of a message is one object where it arrives,
  and one that holds itself still does. A value may be as deep as a frame
  holds (a list of a million cells, say), and a message as large, up to 64 MiB
  encoded (`E_RANGE` beyond, at the `send`). A module a message needs there is the one loaded
  under that name, or else is loaded for it, as Erlang loads code, from a
  running program's directory or the library path. A literal where it is sent
  (a string, BigInt or decimal literal, or a string `text.intern` gave) is that
  node's literal of the same value where it arrives, the same object, so that
  identity tests (`beq.r`) of atoms, symbols and other constants hold across
  nodes; any other value is a new one. Coroutines, handles of `handle` and pointers
  cannot (`E_TYPE`, at the `send`).
* **A closure crosses as its code's identity and its captured values:** its
  module's name, the hash of that module's version, its function (exported or
  not) and that function's signature. Where that very version is loaded, as the
  current one or one a reload replaced, it is the same closure there. Where it
  is not, nor in a file it can load (as above) that is that version, it
  arrives as one that raises `E_LINK_CODE` when called, as Erlang's fun of
  code a node does not have (`badfun`), and that is still the closure it stands for
  when it is sent on, back or to a node that has the code. Two nodes have the
  same version when the module was built from the same text with the same
  options (the hash leaves out where its files are). A `module.handle` crosses
  as its module and function only, and calls that node's current version
  (`E_LINK_UNDEF` where that node does not export the function).
* **A message that cannot be read where it arrives** (a struct of a module
  that node does not have), or that comes for a process whose mailbox is full
  there, is dropped there, with a warning in its monitor's log, and the
  connection goes on.
* **A message to a name.** `node.name_pid(number, name)` gives a pid for
  whichever process has `name` registered on that node (0: this VM) when a
  message arrives there, as Erlang's `{Name, Node} ! Msg`. `send` and
  `proc.call` to it go in one direction, as to any pid: the name is looked up
  where the message arrives, and a message for a name nobody has is dropped (a
  `proc.call`'s is answered `E_NOPROC`). The pid is the same for the same name,
  so it can be kept while the name passes from process to process. Names are 1
  to 255 bytes. `link`, `monitor`, `kill` and `alive` take a process's own pid
  (`whereis`), and see no process in a name's. Both nodes must be 0.24 or
  later: an earlier one ends the connection at such a message.
* **A sender waits for a busy connection.** When a connection has more than
  1 MiB of messages it has not yet written (a slow network, or a node that
  does not read), a process that sends to that node waits after its `send`
  until it has written all but 512 KiB, as Erlang suspends a sender on a busy
  distribution port. So a fast sender cannot fill this VM's memory with what
  it has not sent. Only the `send` instruction waits; a reply, and a send
  from a callback from C, go on at once.
* **A message to a node that is not connected is dropped,** as one to a finished
  process is. So is one that does not arrive because the connection fails. Use
  `monitor`, and replies, for what must be known.
* **Security.** Two nodes connect once each proves it knows the cookie, with an
  HMAC over the other's random nonce, marked with which end it is. After that, a
  node may message any process and run any exported function: the cookie is the
  whole of the boundary, as in Erlang. **Plain connections are not encrypted**:
  use `start_tls` and `connect_tls` unless the network is one you trust. Then the
  connection is TLS before anything else, the connecting node checks the other's
  certificate, and the cookie is proved over the encrypted link. A TLS node takes
  no plain connections. A restricted process
  (`cap.restrict`) can take no part in routing, though it can still `send`.
* **`link`, `monitor` and `proc.call` take a pid of another node** as one of
  this VM. A failure on either side of a link fails the other with `E_LINKED`,
  and a normal end only unlinks. A `Down` comes as on one VM. A `Call` goes to
  the process there, and its `proc.reply` comes back to the caller's reply slot.
  For a process that is not there, `monitor` gives a `Down` with `E_NOPROC`, a
  linked process fails with `E_LINKED`, and `proc.call` raises `E_NOPROC`, all
  as soon as that node says so.
* **When a connection goes,** this VM acts as if every process over there had
  failed with `E_NODE_DOWN`: each monitor of one gets a `Down` with that code,
  each process linked to one fails with `E_LINKED`, and each `proc.call`
  waiting for an answer from there raises `E_NODE_DOWN`. To a node that is not
  connected, `monitor` gives that `Down` at once, and `link` and `proc.call`
  raise `E_NODE_DOWN`.
* **Watching nodes.** After `watch()`, a process gets a built-in `NodeEvent`
  struct, `{node, name, up}`, each time a connection to another node comes
  (`up` true) or goes. One that goes is told after the `Down`s and failures it
  causes. A watcher that wants to hear of one node only (Erlang's
  `monitor_node`) looks at `node`, and the nodes connected before it watched
  are in `nodes()`.
* **`kill` takes a pid of another node** too: that node kills the process with
  `E_KILLED`, as `kill` here would, so its watchers and links hear of it as of
  any kill. To a node that is not connected, or a process not there, it does
  nothing, as `kill` of a finished process does here. `proc.alive` asks the
  node of a pid whether it runs; the `alive` instruction answers for this VM's
  pids only, false for another node's.

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

### Servers (`std.server`)

```asm
.import std.server as server
    ncall "module.function" ("counter", "init") -> (R0)  ; the callback module, by a closure of its
    call server.start (R0, arg) -> (I0)                  ; init(arg): the first state
    ncall "proc.call" (I0, request, 5000) -> (R1)         ; handle_call's reply
```

A generic server, as Erlang/OTP's `gen_server`: a process that keeps a state
and answers with a callback module's functions:

| Callback | Does |
|---|---|
| `init(ref arg) -> ref` | the first state |
| `handle_call(ref request, ref state) -> (ref, ref)` | a `Call`'s reply, and the next state |
| `handle_info(ref message, ref state) -> ref` | any other message (optional: without it, they are dropped) |
| `code_change(ref state) -> ref` | the state for a new version of the module (optional) |

It runs the version of the module it started with until a reload
([ABI.md](ABI.md#reloading-a-module)) brings in a new one. Then, in the
order of its mailbox, it calls the new version's `code_change`, if there is
one, with its state, and the new version's functions from then on. So the old
version handles the messages that came before the reload, with the state it
made, and the new one sees only a state its `code_change` gave it, as
Erlang's `sys:change_code` makes sure of by suspending the process. Two
reloads met at once go to the newest version, through its `code_change`
alone. It learns of a reload by `module.watch`, whose `Reloaded` comes before
any message the new version's code sends. Messages are references (a number
sent to it fails it with `E_TYPE`), and a callback that raises ends it with
that error, as a failure, for its links and monitors. It is MLWSVM assembly
(`lib/std/server.masm`).

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
| `E_RESOURCES` | out of descriptors, memory or disk space, or (`net.connect`) of local ports |
| `E_IO` | any other OS error |
| `E_TYPE` | a wrong argument (a handle of another kind, a bad mode or format spec) |
| `E_JSON` | malformed JSON (with line and column), or a value with no JSON form |
| `E_TIMEOUT`, `E_NOPROC`, `E_DEADLOCK` | `proc.call`: no reply in time, no such process, or no reply possible; `proc.alive`: no answer in time |
| `E_RESTARTS` | a supervisor gave up: its children failed too often |
| `E_PARSE` | malformed text for `time.parse`, `hex.decode`, `base64.decode`, `url.decode` or `csv.parse` |
| `E_RANGE` | an argument out of range (an offset beyond a day, a time or `Date` outside the years 1678 to 2261, `random.int` with lo > hi, `random.bytes` of more than 16 MiB, a message to another node of more than 64 MiB, a SipHash key that is not 16 bytes, `math.powmod` with e < 0 or m <= 0, a port outside 0 to 65535, or 0 for `net.connect` and `net.send_to`) |
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

For something between all and nothing, a process can be limited by name:

```asm
    mnew R0
    lnew R1
    lpush R1, "/srv/data"
    mset R0, "read", R1                ; may read under /srv/data
    lnew R2
    lpush R2, "api.example.org:443"
    mset R0, "connect", R2             ; may connect there
    ncall "cap.restrict" (R0)          ; and reach nothing else
```

`cap.restrict(grants)` takes a map from kinds to lists of what each allows. **What
is not granted is denied**, so a kind left out allows nothing. From then on that
process, and those it spawns, get `E_PERMISSION` for the rest. `--cap
read=/srv/data,connect=api.example.org:443` does the same for a whole program
(`--cap none`: nothing at all), and `mlwsvm bundle --cap ...` builds it into an
executable.

| Kind | What it limits | An entry |
|---|---|---|
| `read` | reading files and directories, `fs.stat`, `fs.exists`, TLS certificates | a path: it and what is under it |
| `write` | creating, changing, renaming and removing | a path, the same |
| `connect` | `net.connect`, `net.resolve`, `net.send_to` | `host:port`, `host:*`, `*:port`, or `*` |
| `listen` | `net.listen`, `net.udp` | the same |
| `run` | `os.run`, `os.spawn` | a command, as given, or a prefix ending in `*` |
| `env` | `os.env`; `os.environ` shows only these | a name, or a prefix ending in `*` |

* **A process can only narrow what it has.** A grant it does not hold is dropped,
  so restricting again never widens.
* **Paths are judged as they resolve:** links are followed and `..` removed before
  the check, so neither gets out of a directory. A file that does not exist yet is
  judged by the directory it would go in.
* **Foreign code is denied** too, since it could reach anything: restricting sets
  quota 4 to 0. So are `module.load` and `module.reload`, since code from a file
  can include what the process may not read.
* **Handles are capabilities already.** A handle given with `fs.give` works in a
  restricted process whatever its path: that is how a supervisor hands a sandbox
  exactly one file or connection.
* `cap.allowed(kind, what)` asks in advance, and `cap.limited()` whether a limit
  is on.
* A check happens when a path is used, so a link changed between the check and
  the call is not caught. This limits a program the VM runs; it is not a
  boundary against another user on the machine.

## Platforms

macOS, Linux and the BSDs (kqueue or epoll), and Windows (an I/O completion
port). On
Windows:

* `os.run` and `os.spawn` find a command as `CreateProcess` does (the
  program's directory, the system's, then `PATH`, adding `.exe`), and
  quote its arguments as the C runtime parses them. `os.kill` ends the
  command, with the exit code a POSIX shell reports for the signal: 143
  (128 + 15), or 137 (128 + 9) for a kill.
* `fs.stat`'s `mode` is what the permission bits would be: 0755 for a
  directory, 0644 for a file, 0444 for a read-only one.
* An error's message is Windows' own; its code is the same `E_*` code as
  elsewhere.
