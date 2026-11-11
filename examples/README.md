# Examples

Small, complete programs, each commented from the top: what it does, how to
run it, and what it shows. Every one is also a test: `make test` runs them
all and checks their output (the `; expect-stdout:` lines at the top), so they
stay working.

Run one from the repository's top directory:

```sh
./mlwsvm run examples/hello.masm
```

## Start here

| Example | Shows |
|---|---|
| [hello.masm](hello.masm) | The smallest program: a module, `main`, a library call. |
| [wordcount.masm](wordcount.masm) | Strings, lists, maps and a struct together. |
| [fib.masm](fib.masm) | Calls and recursion; a quick benchmark (`--stats`, `--profile`, `--jit on`). |
| [errors.masm](errors.masm) | Raising and catching errors, errors of the program's own, cleaning up and passing one on, and a process's error seen by another. |

## Reading input and files: Unix tools

| Example | Shows |
|---|---|
| [cat.masm](cat.masm) | Files or standard input to standard output, a piece at a time; errors on standard error and in the exit code. `echo hi \| ./mlwsvm run examples/cat.masm` |
| [grep.masm](grep.masm) | Lines matching a regular expression, with `-i`, `-n`, `-v`; reading line by line; options from `os.args`. |
| [wc.masm](wc.masm) | Counting the lines, words and bytes of files or standard input, a byte at a time. |
| [walk.masm](walk.masm) | Directories, file metadata and running commands, a process per directory. |

## Data

| Example | Shows |
|---|---|
| [sales.masm](sales.masm) | A CSV file summed by a column, sorted with a comparator closure, printed as a table and as JSON. |
| [tasks.masm](tasks.masm) | A JSON document read, walked with each value's type, and summarized as JSON. |
| [numbers.masm](numbers.masm) | i64 that wraps, BigInts that grow, decimals that are exact (money, rounded to cents), and floats. |
| [records.masm](records.masm) | Records laid out by hand in raw memory, as C would. |
| [sieve.masm](sieve.masm) | A sieve of Eratosthenes over a raw memory block. |

## Functions, closures and coroutines

| Example | Shows |
|---|---|
| [generators.masm](generators.masm) | Coroutines as generators: an endless Fibonacci, a tree walked in order from inside its recursion, and a generator that is also fed values. |
| [translation.masm](translation.masm) | How a scripting language's program maps onto MLWSVM assembly ([TARGETING.md](../docs/TARGETING.md)). |

## Processes

| Example | Shows |
|---|---|
| [pipeline.masm](pipeline.masm) | Processes in a chain, each a stage that receives, works and sends on. |
| [timers.masm](timers.masm) | Waiting for a message with a time limit, sleeping, and a ticker process. |
| [primes.masm](primes.masm) | A pool of four workers (`std.pool`) counting primes in twenty ranges, never more than four at once. |
| [workers.masm](workers.masm) | A job split across processes, each monitored, its failure seen and handled. |
| [kvstore.masm](kvstore.masm) | A key-value store as a server process (`std.server`), found by name, called by several clients at once. |

## Networks

| Example | Shows |
|---|---|
| [http_server.masm](http_server.masm) | An HTTP/1.1 server under a supervisor, a process per connection. `./mlwsvm run examples/http_server.masm 8080` |
| [http_load.masm](http_load.masm) | An HTTP load generator, a process per connection. |
| [serve/echo.masm](serve/echo.masm) | An echo server to start and send messages to from the monitor's dashboard (`./mlwsvm serve --programs examples/serve`). |

## Embedding

| Example | Shows |
|---|---|
| [embed/host.c](embed/host.c) | A C program that runs MLWSVM inside it (`make lib check-embed`; [EMBEDDING.md](../docs/EMBEDDING.md)). |

The files in [data/](data) are the examples' inputs.
