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
| `time.now_ns` | - | i64 | Monotonic clock in nanoseconds. |
| `str.len` | r | i64 | Length of a string or byte buffer in bytes (same as the len instruction). |
| `str.byte` | r, i64 | u8 | Byte at an index of a string (bounds checked). |
| `test.fail` | r | - | Trap with E_ASSERT and the given message. |
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
