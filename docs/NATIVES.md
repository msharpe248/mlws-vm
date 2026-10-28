# MLWSVM Built-in Natives

> Generated from `src/native/natives.c` by `mlwsvm doc natives`. Do not edit by hand.

Natives are host functions called with `ncall "name" (args...) -> (results...)`.
Arguments are read with the parameter types listed below, and the verifier checks
argument counts and register banks at load time. Most programs call them through the
wrappers in `lib/std/*.masm` instead of directly.

| Name | Parameters | Results | Description |
|---|---|---|---|
| `io.print_i` | i64 | - | Print a signed integer. |
| `io.print_u` | u64 | - | Print an unsigned integer. |
| `io.print_hex` | u64 | - | Print an integer in hexadecimal with a 0x prefix. |
| `io.print_f` | f64 | - | Print a float using the shortest representation that round-trips. |
| `io.print_s` | r | - | Print a string or byte buffer (null prints `null`). |
| `io.print_c` | c | - | Print a character (UTF-8 encoded). |
| `io.print_b` | b | - | Print `true` or `false`. |
| `io.print_p` | p | - | Print a pointer for debugging. |
| `io.nl` | - | - | Print a newline. |
| `io.flush` | - | - | Write out the program's buffered standard output now. Output is also flushed when the VM is idle, at exit, and before anything is written to standard error. |
| `time.now_ns` | - | i64 | Monotonic clock in nanoseconds (the same as time.monotonic). |
| `str.len` | r | i64 | Length of a string or byte buffer in bytes (same as the len instruction). |
| `str.byte` | r, i64 | u8 | Byte at an index of a string (bounds checked). |
| `test.fail` | r | - | Trap with E_ASSERT and the given message. |
| `foreign.release` | r | - | Release a foreign handle now: run its finalizer (an extension's, or the tag's .ffi_free function) once; later uses see a released handle. |
| `ffi.async_callback` | r, i64 | r | A C function pointer for an async .callback type (by name) whose calls, from any thread, become Callback{name, args} messages to a process. Keep the handle while C may call it. |
| `gc.collect` | - | - | Request a garbage collection; it runs before the next instruction. |
| `gc.heap_bytes` | - | i64 | Bytes currently allocated on this process's heap (live and not yet collected). |
| `gc.collections` | - | i64 | Number of garbage collections so far in this process. |
| `proc.count` | - | i64 | Number of processes currently alive in the VM (including this one). |
| `math.sin` | f64 | f64 | Sine (radians). |
| `math.cos` | f64 | f64 | Cosine (radians). |
| `math.tan` | f64 | f64 | Tangent (radians). |
| `math.asin` | f64 | f64 | Arc sine. |
| `math.acos` | f64 | f64 | Arc cosine. |
| `math.atan` | f64 | f64 | Arc tangent. |
| `math.atan2` | f64, f64 | f64 | atan2(y, x). |
| `math.exp` | f64 | f64 | e raised to x. |
| `math.log` | f64 | f64 | Natural logarithm. |
| `math.log2` | f64 | f64 | Base-2 logarithm. |
| `math.log10` | f64 | f64 | Base-10 logarithm. |
| `math.pow` | f64, f64 | f64 | x raised to y. |
| `math.hypot` | f64, f64 | f64 | sqrt(x*x + y*y) without undue overflow. |
| `proc.register` | r, i64 | - | Give a process a name (E_EXISTS if a live process has it); the name goes when the process ends. |
| `proc.unregister` | r | - | Remove a name. |
| `proc.whereis` | r | i64 | The pid with a name, or 0. |
| `proc.call` | i64, r, i64 | r | Send Call{from, id, body} and wait for proc.reply's answer, up to a timeout in ms (negative: none; E_TIMEOUT). The answer does not go through the mailbox. |
| `proc.reply` | r, r | - | Answer a Call. |

## Files, the operating system and the network (`fs.*`, `os.*`, `net.*`)

> Generated from `src/native/osio.c`. See [STDLIB.md](STDLIB.md) for how they wait,
> their errors, and the OS-call quota (kind 5).

| Name | Parameters | Results | Description |
|---|---|---|---|
| `fs.open` | r, r | r | Open a file: mode "r", "w" (create or truncate), "a" (append), "r+", "w+", "a+"; add "x" to fail if it exists. |
| `fs.close` | r | - | Close a file now (it is also closed when collected). |
| `fs.read` | r, i64 | r | Read up to n bytes (n <= 0: up to 16 MiB), as a byte buffer; empty at the end of input. |
| `fs.read_line` | r | r | Read a line, without its \n (or \r\n), as a string; null at the end of input. |
| `fs.read_all` | r | r | Read everything up to the end of input, as a byte buffer. |
| `fs.write` | r, r | - | Write all of a string or byte buffer. |
| `fs.seek` | r, i64, i64 | i64 | Move in a file: whence 0 from the start, 1 from here, 2 from the end; returns the new offset. |
| `fs.set_timeout` | r, i64 | - | Give later reads and writes on a pipe or socket a timeout in ms (E_TIMEOUT); negative: none. |
| `fs.stdin` | - | r | The program's standard input. |
| `fs.stdout` | - | r | The program's standard output (the same output as io.print). |
| `fs.stderr` | - | r | The program's standard error. |
| `fs.give` | r | i64 | Hand a file or socket to another process: the handle closes, and the returned ticket (send it) is turned back into a handle by fs.take, once. |
| `fs.take` | i64 | r | Take a handle given with fs.give. |
| `fs.read_file` | r | r | A whole file, as a byte buffer. |
| `fs.write_file` | r, r | - | Create or replace a file with a string or byte buffer. |
| `fs.stat` | r | r | A FileInfo for a path: size, mtime (ns), mode, kind (1 file, 2 directory, 3 link, 4 other). |
| `fs.exists` | r | b | Whether a path exists. |
| `fs.list` | r | r | The names in a directory, sorted, as a list of strings. |
| `fs.mkdir` | r | - | Create a directory. |
| `fs.remove` | r | - | Delete a file. |
| `fs.rmdir` | r | - | Delete an empty directory. |
| `fs.rename` | r, r | - | Rename or move a file or directory. |
| `fs.cwd` | - | r | The current directory. |
| `os.args` | - | r | The program's arguments (after the program file on the command line), as a list of strings. |
| `os.env` | r | r | An environment variable, or null. |
| `os.environ` | - | r | The whole environment, as a map of strings. |
| `os.exit` | i64 | - | End the whole program now, from any process, with this exit code. |
| `os.platform` | - | r | "macos", "linux", "freebsd", ... or "windows". |
| `os.spawn` | r, i64 | r, r, r, r | Start a command (a list: the program, found on PATH, then its arguments). Pipes: 1 stdin, 2 stdout, 4 stderr, 8 stderr into stdout. Returns the process and its piped streams (null if not piped). |
| `os.wait` | r | i64 | Wait for a command to finish; its exit code (128 + N if killed by signal N). |
| `os.kill` | r, b | - | Stop a command: ask it to terminate, or kill it (true). |
| `os.run` | r | i64, r | Run a command to completion: its exit code and its standard output, as a byte buffer. |
| `net.listen` | r, i64 | r | Listen for TCP connections on a host ("" = all addresses, "127.0.0.1", "::", a name) and port (0: any free one). |
| `net.accept` | r | r | Wait for the next connection; read and write it with fs.read, fs.read_line and fs.write. |
| `net.connect` | r, i64, i64 | r | Connect to a host and port, with a timeout in ms (negative: the OS default). |
| `net.port` | r | i64 | A socket's local port (the one chosen for net.listen port 0). |
| `net.peer` | r | r, i64 | The address and port of a connection's other end. |
| `net.close_write` | r | - | Finish sending on a connection (the other end reads end of input) but keep reading. |
| `net.resolve` | r | r | A host name's addresses, as a list of strings. |
| `net.udp` | r, i64 | r | A UDP socket bound to a host ("" = any) and port (0 = any). |
| `net.send_to` | r, r, i64, r | - | Send a datagram (a string or byte buffer) to a host and port. |
| `net.recv_from` | r | r, r, i64 | Wait for a datagram: its bytes, and the sender's address and port. |

## Text and JSON (`text.*`, `json.*`)

> Generated from `src/native/text.c`. See [STDLIB.md](STDLIB.md#text-and-json).

| Name | Parameters | Results | Description |
|---|---|---|---|
| `text.format` | r, r | r | Format with {} placeholders: text.format("{} is {:>5.1f}", [name, x]). Spec: [[fill]align][+][0][width][.precision][type]; types d x X o b f e g s q (quoted). |
| `text.split` | r, r | r | Split on a separator, as a list of strings; an empty separator splits on runs of whitespace (White_Space). |
| `text.join` | r, r | r | Join a list's elements (any values) with a separator. |
| `text.trim` | r | r | Remove whitespace (White_Space) from both ends. |
| `text.trim_start` | r | r | Remove whitespace from the start. |
| `text.trim_end` | r | r | Remove whitespace from the end. |
| `text.find` | r, r, i64 | i64 | The byte offset of the first occurrence at or after an offset, or -1. |
| `text.replace` | r, r, r | r | Replace every occurrence. |
| `text.starts_with` | r, r | b | Whether a string starts with a prefix. |
| `text.ends_with` | r, r | b | Whether a string ends with a suffix. |
| `text.slice` | r, i64, i64 | r | The bytes [start, end) as a string; negative offsets count from the end; clamped to the string. |
| `text.upper` | r | r | Upper case, by Unicode's full mappings (ß becomes SS). |
| `text.lower` | r | r | Lower case, by Unicode's full mappings (a final Σ becomes ς). |
| `text.ascii_upper` | r | r | ASCII letters to upper case; every other byte unchanged. |
| `text.ascii_lower` | r | r | ASCII letters to lower case; every other byte unchanged. |
| `text.title` | r | r | Title case: in each word, the first cased character to title case and the rest to lower case. |
| `text.fold` | r | r | Unicode case folding (full), for caseless comparison. |
| `text.equal_fold` | r, r | b | Whether two strings are equal ignoring case and canonical equivalence (canonical caseless match). |
| `text.normalize` | r, r | r | Unicode normalization: the form is "NFC", "NFD", "NFKC" or "NFKD". |
| `text.is_normalized` | r, r | b | Whether a string is already in a normalization form ("NFC", "NFD", "NFKC", "NFKD"). |
| `text.graphemes` | r | r | The user-perceived characters (extended grapheme clusters), as a list of strings. |
| `text.next_grapheme` | r, i64 | i64 | The end of the grapheme cluster that starts at a byte offset (-1 at the end), for iterating. |
| `text.words` | r | r | The words, as a list of strings: the segments between word boundaries that hold a letter, digit or ideograph. |
| `text.word_segments` | r | r | Every segment between word boundaries (words, spaces, punctuation), as a list of strings. |
| `text.width` | r | i64 | Columns on a terminal: 2 for wide (East Asian) characters and emoji, 0 for marks and controls. |
| `text.category` | c | r | A character's general category: "Lu", "Nd", ... |
| `text.is_letter` | c | b | Whether a character is a letter (category L*). |
| `text.is_alpha` | c | b | Whether a character is Alphabetic. |
| `text.is_digit` | c | b | Whether a character is a decimal digit (category Nd). |
| `text.is_number` | c | b | Whether a character is a number (category N*). |
| `text.is_space` | c | b | Whether a character is White_Space. |
| `text.is_upper` | c | b | Whether a character is Uppercase. |
| `text.is_lower` | c | b | Whether a character is Lowercase. |
| `text.is_punct` | c | b | Whether a character is punctuation (category P*). |
| `text.is_id_start` | c | b | Whether a character can start an identifier (XID_Start, UAX #31). |
| `text.is_id_continue` | c | b | Whether a character can continue an identifier (XID_Continue, UAX #31). |
| `text.is_emoji` | c | b | Whether a character is Extended_Pictographic. |
| `text.digit_value` | c | i64 | A decimal digit's value, or -1. |
| `text.char_width` | c | i64 | A character's columns on a terminal: 0, 1 or 2. |
| `text.unicode_version` | - | r | The version of Unicode the tables come from, e.g. "17.0.0". |
| `text.repeat` | r, i64 | r | A string repeated n times. |
| `text.lines` | r | r | Split into lines (\n or \r\n), as a list of strings. |
| `text.valid` | r | b | Whether a string or byte buffer is valid UTF-8. |
| `text.length` | r | i64 | The number of characters (an invalid byte counts as one). |
| `text.chars` | r | r | The characters, as a list of chars (invalid bytes: U+FFFD). |
| `text.next` | r, i64 | c, i64 | The character at a byte offset and the offset of the next one (-1 at the end), for iterating. |
| `text.from_chars` | r | r | A string from a list of characters (or code points). |
| `json.parse` | r | r | Parse JSON whose top level is an object (a map) or array (a list). Integers are i64 (BigInt if larger), fractions f64, true/false bool, null null. |
| `json.parse_any` | r | r | Parse any JSON value, returned as the only element of a list (so numbers and booleans keep their types). |
| `json.stringify` | r, i64 | r | JSON for a map, list, array, struct (an object of its fields), string, number or null; indent 0 is compact. |

## Time, hashes, random numbers, encodings and collections

> Generated from `src/native/util.c`. See [STDLIB.md](STDLIB.md#time-and-dates).

| Name | Parameters | Results | Description |
|---|---|---|---|
| `time.now` | - | i64 | The wall clock: nanoseconds since 1970-01-01T00:00:00Z. |
| `time.monotonic` | - | i64 | A clock that never goes backwards, in nanoseconds from an arbitrary start: for measuring durations. |
| `time.format` | i64, i64 | r | A time as ISO 8601 (RFC 3339) at an offset in minutes east of UTC: 2026-10-26T14:05:09.25+01:00, or ...Z for 0. |
| `time.parse` | r | i64 | Parse ISO 8601: a date (2026-10-26), or a date and time with optional seconds, fraction and offset (no offset: UTC). |
| `time.date` | i64, i64 | r | A time as a Date at an offset: year, month 1-12, day, hour, minute, second, nano, weekday (1 Monday - 7 Sunday), yday (1-366), offset. |
| `time.from_date` | r | i64 | The time a Date names (weekday and yday are ignored; other fields out of range carry over). |
| `time.local_offset` | i64 | i64 | The local time zone's offset from UTC, in minutes east, at a time. |
| `hash.crc32` | r | u32 | CRC-32 (as zlib, PNG and Ethernet) of a string or byte buffer. |
| `hash.sha256` | r | r | SHA-256 of a string or byte buffer, as 32 bytes. |
| `hash.siphash` | r, r | u64 | SipHash-2-4 of data with a 16-byte key: a keyed hash that resists hash flooding. |
| `random.seed` | i64 | - | Seed this process's generator, for a repeatable sequence (it starts from OS entropy). |
| `random.int` | i64, i64 | i64 | A uniformly random integer in [lo, hi]. |
| `random.float` | - | f64 | A uniformly random float in [0, 1). |
| `random.bytes` | i64 | r | n bytes from the OS's entropy source: for keys, tokens and nonces. |
| `random.shuffle` | r | - | Shuffle a list in place. |
| `hex.encode` | r | r | Lower-case hex of a string or byte buffer. |
| `hex.decode` | r | r | Bytes from hex (either case). |
| `base64.encode` | r, b | r | Base64 (RFC 4648), padded; with url true, the URL-safe alphabet without padding. |
| `base64.decode` | r | r | Bytes from base64 in either alphabet; padding is optional and whitespace is ignored. |
| `url.encode` | r | r | Percent-encode every byte except A-Z a-z 0-9 - . _ ~ (RFC 3986). |
| `url.decode` | r, b | r | Decode %XX escapes; with plus true, + is a space too (HTML form data). |
| `csv.parse` | r | r | CSV (RFC 4180) as a list of rows, each a list of strings. |
| `csv.format` | r | r | A list of rows as CSV: fields quoted when needed, other values as tostr writes them, rows ending in \n. |
| `list.sort` | r, r | - | Sort a list in place, stably: with a comparator closure cmp(a, b) -> negative, 0 or positive, or null for numbers and strings in natural order. |
| `list.bsearch` | r, r | i64 | Binary search of a sorted list with probe(x) -> negative (x is before), 0 (found) or positive: the index, or -(insertion point) - 1. |
| `map.union` | r, r | r | A new map with the entries of both (b's values win). |
| `map.intersect` | r, r | r | A new map of a's entries whose keys b has too. |
| `map.difference` | r, r | r | A new map of a's entries whose keys b does not have. |
| `math.powmod` | i64, i64, i64 | i64 | b^e mod m, without overflow (e >= 0, m > 0). |
| `math.gcd` | i64, i64 | i64 | Greatest common divisor (of the absolute values). |
| `math.lcm` | i64, i64 | i64 | Least common multiple (E_OVERFLOW if it does not fit). |

## Loading modules at run time (`module.*`)

> Generated from `src/native/modload.c`. See [ABI.md](ABI.md#9-loading-modules-at-run-time).

| Name | Parameters | Results | Description |
|---|---|---|---|
| `module.load` | r | - | Load a module (and what it imports) while the program runs: as if the calling module imported it, so found in the program's directory first if the caller is one of the program's own modules, with the same library versions, privacy and verification. Loading a loaded module does nothing. |
| `module.loaded` | r | b | Whether a module is loaded: the module that name means to the caller, as for module.load. |
| `module.function` | r, r | r | A closure for a function a loaded module exports, to call with calli (its signature is checked then). |
| `module.exports` | r | r | The names of the functions a loaded module exports. |
