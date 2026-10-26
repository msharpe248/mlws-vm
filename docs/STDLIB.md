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
| `net` | M13 (planned) | TCP, UDP, DNS |
| `text`, `json` | M14 (planned) | formatting, UTF-8, JSON |

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
| `E_RESOURCES` | out of descriptors, memory or disk space |
| `E_IO` | any other OS error |
| `E_TYPE` | a wrong argument (a handle of another kind, a bad mode) |

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
