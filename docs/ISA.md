# MLWSVM Instruction Set Reference

> Generated from `src/core/opcodes.def` by `mlwsvm doc isa`. Do not edit by hand;
> change the opcode table and regenerate with `make docs`.

Every instruction is written `mnemonic[.type[.type2]] operands`. See
[ASSEMBLY.md](ASSEMBLY.md) for syntax, [ADDRESSING.md](ADDRESSING.md) for operand forms and
[ABI.md](ABI.md) for calls and the stack.

**Operand names.** `dst` is a writable location (a register or memory). `src` and `val`
are any value operand (register, literal, string or memory). `int` is read as `i64`,
`ptr` is read as a pointer (`p`), and `obj` as a reference. `bool_dst` is written as `b`,
`int_dst` as `i64`, `float_dst` as `f64` and `obj_dst` as a reference. `mem` must be a
memory operand, `Struct` a struct type's name, `n` a non-negative integer constant, and
`[x]` marks an optional operand.

**Types column.** This lists the type suffixes the instruction accepts. `int` covers
`b c i8 i16 i32 i64 u8 u16 u32 u64` and `float` covers `f32 f64`. Without a suffix the
type is inferred from the first register operand: I gives `i64`, F `f64`, R `r`, P/SP/FP `p`.
If there is no register operand, the first literal decides, and `i64` is the fallback.
`mov`, `swap`, `push`, `pop` and `ret` without a suffix use each operand's natural type,
which preserves the type tag of a stack or global word.

**Two-operand shorthand.** For arithmetic and bitwise instructions, `op dst, src` means
`dst = dst op src`. With three or more operands, `dst = s1 op s2 op ... op sN`, so `add`, `mul`,
`min`, `max`, `and`, `or` and `xor` take up to 31 sources.

## Types

Type codes, as `tagof` reports them: 0 for a slot never written, 1 `b`, 2 `c`, 3 `i8`, 4 `i16`, 5 `i32`, 6 `i64`, 7 `u8`, 8 `u16`, 9 `u32`, 10 `u64`, 11 `f32`, 12 `f64`, 13 `r`, 14 `p`, 15 `big`, 16 `dec`, and 255 for raw bytes (written partly, unaligned, or by `memcpy`).


## Control

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0000` | `nop` | `-` | - | Do nothing. |
| `0x0001` | `halt` | `[int]` | - | Stop the current process with the given exit code (default 0). |
| `0x0002` | `break` | `-` | - | Debugger breakpoint. A no-op when no debugger is attached. |
| `0x0003` | `assert` | `int, ["msg"]` | - | Trap with E_ASSERT if the first operand is zero. The optional second operand is a message string. |

## Data movement

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0010` | `mov` | `dst, src` | int float r p | dst = src. Untyped memory-to-memory moves copy the source word together with its type tag. |
| `0x0011` | `swap` | `dst, dst` | int float r p | Exchange the contents of two locations. |
| `0x0012` | `lea` | `ptr_dst, mem` | - | dst = the address of a memory operand, as a pointer. `lea P0, SP[2]` makes P0 point 2 words above SP. |
| `0x0013` | `push` | `val, ...` | int float r p | Push each operand onto the data stack, left to right: for each, SP -= 1 word, then [SP] = value. Untyped pushes use each operand's natural type (I=i64, F=f64, R=r, P=p, int literal=i64, float literal=f64, string=r, memory=its tag). |
| `0x0014` | `pop` | `dst, ...` | int float r p | Pop into each operand, left to right: for each, dst = [SP], then SP += 1 word. `push a, b` is undone by `pop b, a`. |
| `0x0015` | `drop` | `[n]` | - | Discard n words (default 1) from the top of the data stack: SP += n words. |
| `0x0016` | `enter` | `n` | - | Reserve n zeroed words of locals: SP -= n words. Locals are then addressed as LOC[0]..LOC[n-1] (FP[-1]..FP[-n]). |
| `0x0017` | `cvt` | `dst, src` | int float (to.from) | Value conversion `cvt.<to>.<from> dst, src`. int->int wraps or extends, int->float rounds, float->int truncates toward zero and saturates (NaN becomes 0), to-bool tests for non-zero. |
| `0x0018` | `bitcast` | `dst, src` | int float (to.from) | Reinterpret the bits of src as dst's type: `bitcast.<to>.<from>`. Both types must have the same size, e.g. bitcast.i64.f64 or bitcast.f32.u32. |
| `0x0019` | `ldv` | `int_dst, float_dst, val` | - | Load a number into the destination its type tag picks: `ldv I0, F0, R1[I2]` sets I0 (as i64) if the slot holds an integer (b, c, i8 ... u64) and F0 (as f64) if it holds a float, and leaves the other unchanged. A slot never written reads as the integer 0; any other type traps E_TYPE. This reads the number half of a dynamically typed value without a branch on its type. |

## Arithmetic

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0020` | `add` | `dst, src, ...` | int float p | Sum. `add dst, s` means dst += s; `add dst, s1, s2, ...` means dst = s1 + s2 + .... For add.p the first value is a pointer and the remaining values are i64 byte offsets. |
| `0x0021` | `sub` | `dst, src, ...` | int float p | Difference: dst = s1 - s2 - ... (two-operand form: dst -= s). sub.p subtracts i64 byte offsets from a pointer. |
| `0x0022` | `mul` | `dst, src, ...` | int float | Product: dst = s1 * s2 * ... (two-operand form: dst *= s). Integer multiply wraps. |
| `0x0023` | `div` | `dst, src, ...` | int float | Quotient, left to right. Integer division truncates toward zero and traps E_DIV_ZERO on a zero divisor. Float division follows IEEE 754. |
| `0x0024` | `rem` | `dst, src, ...` | int float | Remainder with the sign of the dividend (C semantics; fmod for floats). Integer remainder traps E_DIV_ZERO on a zero divisor. |
| `0x0025` | `min` | `dst, src, ...` | int float | Smallest of the sources (two-operand form: dst = min(dst, s)). |
| `0x0026` | `max` | `dst, src, ...` | int float | Largest of the sources (two-operand form: dst = max(dst, s)). |
| `0x0027` | `neg` | `dst, [src]` | int float | dst = -src (one-operand form negates in place). |
| `0x0028` | `abs` | `dst, [src]` | int float | dst = \|src\| (one-operand form works in place). abs of the minimum signed value wraps to itself. |
| `0x0029` | `inc` | `dst` | int float | dst += 1. |
| `0x002A` | `dec` | `dst` | int float | dst -= 1. |

## Bitwise

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0040` | `and` | `dst, src, ...` | int | Bitwise AND of the sources (two-operand form: dst &= s). |
| `0x0041` | `or` | `dst, src, ...` | int | Bitwise OR of the sources (two-operand form: dst \|= s). |
| `0x0042` | `xor` | `dst, src, ...` | int | Bitwise XOR of the sources (two-operand form: dst ^= s). |
| `0x0043` | `not` | `dst, [src]` | int | Bitwise complement (logical NOT for .b). The one-operand form works in place. |
| `0x0044` | `shl` | `dst, src, [src]` | int | Shift left: `shl dst, n` or `shl dst, src, n`. The count is taken modulo the type's bit width. |
| `0x0045` | `shr` | `dst, src, [src]` | int | Logical shift right within the type's width (zero fill). |
| `0x0046` | `sar` | `dst, src, [src]` | int | Arithmetic shift right within the type's width (sign fill). |
| `0x0047` | `rol` | `dst, src, [src]` | int | Rotate left within the type's width. |
| `0x0048` | `ror` | `dst, src, [src]` | int | Rotate right within the type's width. |
| `0x0049` | `popcnt` | `dst, [src]` | int | Number of set bits within the type's width. |
| `0x004A` | `clz` | `dst, [src]` | int | Count leading zero bits within the type's width (the width itself for 0). |
| `0x004B` | `ctz` | `dst, [src]` | int | Count trailing zero bits (the type's width for 0). |
| `0x004C` | `bswap` | `dst, [src]` | int | Reverse the byte order within the type's width. |

## Floating point

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0050` | `sqrt` | `dst, [src]` | float | Square root. |
| `0x0051` | `floor` | `dst, [src]` | float | Round toward negative infinity. |
| `0x0052` | `ceil` | `dst, [src]` | float | Round toward positive infinity. |
| `0x0053` | `round` | `dst, [src]` | float | Round to nearest, with ties away from zero. |
| `0x0054` | `trunc` | `dst, [src]` | float | Round toward zero. |
| `0x0055` | `fma` | `dst, src, src, src` | float | Fused multiply-add: dst = a * b + c with a single rounding. |

## Comparison

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0060` | `seq` | `bool_dst, src, src` | int float r p | dst = (a == b) as bool. References compare by identity. |
| `0x0061` | `sne` | `bool_dst, src, src` | int float r p | dst = (a != b) as bool. |
| `0x0062` | `slt` | `bool_dst, src, src` | int float p | dst = (a < b) as bool, using the instruction type's signedness. Any comparison involving NaN is false. |
| `0x0063` | `sle` | `bool_dst, src, src` | int float p | dst = (a <= b) as bool. |
| `0x0064` | `sgt` | `bool_dst, src, src` | int float p | dst = (a > b) as bool. |
| `0x0065` | `sge` | `bool_dst, src, src` | int float p | dst = (a >= b) as bool. |
| `0x0066` | `sel` | `dst, int, src, src` | int float r p | Select: dst = cond != 0 ? a : b. Only the chosen source is read. |

## Branches

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0070` | `jmp` | `label` | - | Jump unconditionally. |
| `0x0071` | `bz` | `src, label` | int r p | Branch if the value is zero, false or null. |
| `0x0072` | `bnz` | `src, label` | int r p | Branch if the value is non-zero, true or non-null. |
| `0x0073` | `beq` | `src, src, label` | int float r p | Branch if a == b. |
| `0x0074` | `bne` | `src, src, label` | int float r p | Branch if a != b. |
| `0x0075` | `blt` | `src, src, label` | int float p | Branch if a < b, using the instruction type's signedness. |
| `0x0076` | `ble` | `src, src, label` | int float p | Branch if a <= b. |
| `0x0077` | `bgt` | `src, src, label` | int float p | Branch if a > b. |
| `0x0078` | `bge` | `src, src, label` | int float p | Branch if a >= b. |
| `0x0079` | `jtab` | `int, label, ...` | - | Jump table: `jtab idx, L0, L1, ..., Ln` jumps to L<idx>. If idx is out of range, execution falls through to the next instruction. |
| `0x007A` | `bisa` | `obj, Struct, label` | - | Branch if the reference is a struct of the given type: `bisa R1, Point, L`. One instruction for `isa` and `bnz`. |
| `0x007B` | `bnisa` | `obj, Struct, label` | - | Branch unless the reference is a struct of the given type (null included): `bnisa R1, Point, L`. One instruction for `isa` and `bz`, the guard before a struct's fields are read. |

## Calls

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0080` | `call` | `func (args...) -> (results...)` | - | Call a function: `call f (a1, a2, ...) -> (d1, d2, ...)`. Each argument is read with the callee's declared parameter type and bound to the callee's next free register of that bank (int-class args to I0, I1, ...; floats to F0, ...; refs to R0, ...; pointers to P0, ...). Return values are written to d1, d2, .... The callee gets fresh registers, and FP = SP at the call. |
| `0x0081` | `tcall` | `func (args...)` | - | Tail call: like call, but replaces the current frame. The callee's return values go to the current function's caller. A tail call cannot have result operands. |
| `0x0082` | `ret` | `[val, ...]` | - | Return from the current function with zero or more values. Operands are read with the function's declared return types (or their natural types if none are declared). SP is restored to its value at the call. |
| `0x0083` | `ncall` | `"native" (args...) -> (results...)` | - | Call a native (host) function by name: `ncall "io.print_i" (I0)`. See docs/NATIVES.md for the built-in natives, and docs/EXTENDING.md for natives from extensions (`.extension`) and C functions (`.extern`). Errors in foreign code are raised once it returns; a blocking one runs on the I/O thread pool while this process waits. |

## Raw memory

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0090` | `alloc` | `ptr_dst, int` | - | Allocate a zeroed raw memory block of n bytes. dst = pointer to it. Raw blocks are bounds checked and owned by the process, and are released when it exits. |
| `0x0091` | `free` | `ptr` | - | Free a raw block. Freeing null is a no-op. Freeing anything other than the start of a live block traps E_BAD_FREE. Any later use of the pointer traps E_UAF. |
| `0x0092` | `realloc` | `ptr_dst, ptr, int` | - | Resize a raw block: `realloc dst, ptr, n`. The contents are preserved up to the smaller size and new bytes are zeroed. Old pointers into the block become invalid (E_UAF). |
| `0x0093` | `memcpy` | `ptr, ptr, int` | - | Copy n bytes: `memcpy dst_ptr, src_ptr, n` (the regions may overlap). Copied bytes are untyped: they cannot later be read as references. |
| `0x0094` | `memset` | `ptr, int, int` | - | Fill n bytes with a byte value: `memset ptr, byte, n`. |

## Objects

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x00A0` | `new` | `obj_dst, Struct` | - | Allocate a struct with every field zero (0, 0.0 or null): `new R0, Point`. Fields are then accessed as R0->Point.x. |
| `0x00A1` | `anew` | `obj_dst, int` | int float r p | Allocate an array of n zeroed elements of the instruction type: `anew.f64 R0, 100`. Elements are accessed as R0[I1] or R0[5] and are bounds checked. |
| `0x00A2` | `len` | `int_dst, obj` | - | Length: bytes of a string or byte buffer, elements of an array or list, entries of a map. Traps E_TYPE for a struct and E_NULL for null. |
| `0x00A3` | `typeof` | `int_dst, obj` | - | Kind of a reference: 0 null, 1 string, 2 bytes, 3 array, 4 struct, 5 list, 6 map, 7 closure, 8 BigInt, 9 decimal, 10 foreign handle, 11 coroutine. |
| `0x00A4` | `isa` | `bool_dst, obj, Struct` | - | dst = true if the reference is a struct of the given type: `isa I0, R1, Point`. |
| `0x00A5` | `resize` | `obj, int` | - | Set the length of a byte buffer or list. New bytes are 0. New list elements are untyped zeros, which read as 0, 0.0 or null. |
| `0x00A6` | `tagof` | `int_dst, val` | - | dst = the type a slot holds, as a type code (0 for a slot never written, 255 for raw bytes; see Types in docs/ISA.md): a list element or closure capture its own tag, an array element or struct field its declared type, a stack or global word its tag, a byte of a string or buffer u8, a register or constant its natural type. With this, one tagged slot can hold a dynamically typed value: `tagof I0, R1[I2]`, then branch and read it with its type. |

## Strings

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x00B0` | `strcat` | `obj_dst, obj, ...` | - | Concatenate strings and/or byte buffers into a new string: `strcat R0, R1, "-", R2` (up to 31 parts). `strcat R0, buf` copies a byte buffer into a string. |
| `0x00B1` | `substr` | `obj_dst, obj, int, int` | - | `substr dst, s, start, n`: new string of the n bytes starting at byte offset start. Traps E_BOUNDS if the range is outside s. |
| `0x00B2` | `strcmp` | `int_dst, obj, obj` | - | Compare two strings or byte buffers bytewise: dst = -1, 0 or 1. |
| `0x00B3` | `strfind` | `int_dst, obj, obj, [int]` | - | `strfind dst, s, needle [, start]`: byte offset of the first occurrence of needle at or after start (default 0), or -1. |
| `0x00B4` | `tostr` | `obj_dst, val` | int float r p | Convert a value to a new string, using the instruction type or the value's natural type: integers in decimal, floats in shortest round-trip form, bools as true/false, chars as UTF-8. A string converts to itself, a byte buffer to a copy, and other objects to a description such as <list len=3>. |
| `0x00B5` | `parse` | `dst, obj` | int float | Parse a number from the whole string: `parse.i64 I0, s` (decimal, or 0x/0b/0o prefixed), `parse.f64 F0, s`. Anything else traps E_PARSE. |
| `0x00B6` | `bufnew` | `obj_dst, int` | - | New mutable byte buffer of n zero bytes. Its bytes are accessed as R0[I1] (u8). |
| `0x00B7` | `bufcat` | `obj, obj, ...` | - | Append strings and/or byte buffers to a byte buffer: `bufcat buf, s1, s2, ...`. |
| `0x00B8` | `bufpush` | `obj, int, ...` | - | Append byte values (the low 8 bits of each operand) to a byte buffer. |

## Lists and maps

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x00C0` | `lnew` | `obj_dst, [int]` | - | New empty list, with an optional initial capacity. |
| `0x00C1` | `lpush` | `obj, val, ...` | int float r p | Append values to a list. Each element remembers its type: untyped operands use their natural type, typed ones (lpush.i32) the instruction type. |
| `0x00C2` | `lpop` | `dst, obj` | int float r p | Remove the last element of a list into dst: `lpop dst, list`. Traps E_BOUNDS on an empty list. |
| `0x00C3` | `lins` | `obj, int, val` | int float r p | Insert a value before index i (0..len): `lins list, i, value`. |
| `0x00C4` | `ldel` | `obj, int` | - | Delete the element at index i, shifting later elements down. |
| `0x00C5` | `mnew` | `obj_dst` | - | New empty map. Maps iterate in insertion order. |
| `0x00C6` | `mset` | `obj, val, val` | int float r p | `mset map, key, value`. Keys compare by value: integers of any width by value, floats by value, strings by content, other references by identity, pointers by value. A type suffix applies to the value. |
| `0x00C7` | `mget` | `dst, obj, val, [val]` | int float r p | `mget dst, map, key [, default]`. A missing key yields the default if given, and otherwise traps E_KEY. A type suffix applies to the value and the default. |
| `0x00C8` | `mhas` | `bool_dst, obj, val` | - | dst = true if the map contains the key. |
| `0x00C9` | `mdel` | `obj, val` | - | Remove a key from a map (no effect if it is absent). |
| `0x00CA` | `mkeys` | `obj_dst, obj` | - | New list of a map's keys, in insertion order. |

## Handles

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x00D0` | `hnew` | `int_dst, obj` | - | Make a handle: an i64 that keeps the object alive and can be stored anywhere, including raw memory. Freed handle numbers may be reused. |
| `0x00D1` | `hget` | `obj_dst, int` | - | Get a handle's object. Traps E_BAD_HANDLE for an unknown or freed handle. |
| `0x00D2` | `hfree` | `int` | - | Release a handle. The object becomes collectable if nothing else refers to it. |

## Exceptions

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x00E0` | `throw` | `obj` | - | Raise an exception: any non-null reference, often an Error struct (`new R0, Error`). Control passes to the handler of the innermost enclosing .try region in this function or a caller, and frames without a handler are unwound. An uncaught exception ends the process. To rethrow, throw the caught value again. |
| `0x00E1` | `catch` | `obj_dst` | - | At the start of a handler: dst = the exception being handled. Runtime faults arrive as Error structs whose code field holds the error code (e.g. "E_DIV_ZERO"). |

## Processes

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0100` | `spawn` | `func (args...) -> (results...)` | - | Start a new process running a function: `spawn worker (a, b) -> (I0)` puts the new process id in I0. Arguments are copied into the new process; references are deep-copied, and pointers cannot be passed. The new process inherits this process's quotas and may start running at once, on another thread. To watch or link to it, use spawnm or spawnl: with `spawn` followed by `monitor`, a quick child can finish first. |
| `0x0101` | `send` | `int, val` | int float r p | `send pid, value`: append a copy of value to the mailbox of process pid. References are deep-copied, preserving sharing and cycles. Sending to a finished process does nothing. Traps E_QUOTA if the receiver's mailbox is full and E_TYPE for a pointer. |
| `0x0102` | `recv` | `dst` | int float r p | Take the oldest message from this process's mailbox, waiting until one arrives. The message keeps its type (as list elements do). Traps E_DEADLOCK if every process is waiting and none can ever send. |
| `0x0103` | `recvt` | `dst, int, label` | int float r p | `recvt dst, ms, Ltimeout`: like recv, but if no message arrives within ms milliseconds, jump to Ltimeout instead. ms = 0 polls. |
| `0x0104` | `yield` | `-` | - | Give the rest of this time slice to other processes. |
| `0x0105` | `sleep` | `int` | - | Suspend this process for at least the given number of milliseconds. |
| `0x0106` | `kill` | `int` | - | Terminate a process with error E_KILLED (as a failure, so links and monitors are notified). Killing a finished or unknown process does nothing; killing yourself ends this process. |
| `0x0107` | `link` | `int` | - | Link this process and another. If either fails (an uncaught exception, kill or quota), the other is killed with E_LINKED. Normal exits do not propagate. |
| `0x0108` | `monitor` | `int` | - | Watch a process. When it ends, this process receives a Down struct message with fields pid, exit (exit code), and code/message (null for a normal exit). Monitoring a finished or unknown process delivers a Down message immediately, with code E_NOPROC. |
| `0x0109` | `alive` | `bool_dst, int` | - | dst = true if the process exists and has not finished. |
| `0x010A` | `quota` | `n, int` | - | `quota kind, limit` lowers one of this process's limits (it can never raise them): 0 heap bytes, 1 raw-memory bytes, 2 mailbox messages, 3 instructions executed, 4 foreign calls (natives from extensions and .extern; 0 forbids native code), 5 OS calls (fs.* and os.*; 0 forbids files, commands and the environment). Processes it spawns inherit its limits (and count their own use). Exceeding a limit traps E_QUOTA. |
| `0x010B` | `mlen` | `int_dst` | - | dst = the number of messages waiting in this process's mailbox. |
| `0x010C` | `spawnl` | `func (args...) -> (results...)` | - | Like spawn, and the new process is linked to this one before it starts (see link), so it cannot fail unnoticed in between. |
| `0x010D` | `spawnm` | `func (args...) -> (results...)` | - | Like spawn, and this process monitors the new one from the start (see monitor): a Down message reports how it ends, however quickly. |

## Closures

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0110` | `closure` | `func (args...) -> (results...)` | - | Make a function value: `closure f (v1, v2, ...) -> (R0)`. The values are copied into the closure, and f reads and writes them as UP[0], UP[1], .... Each keeps its own type, as a list element does. Captures are copies: to share a mutable variable, capture a struct or list. |
| `0x0111` | `calli` | `closure (args...) -> (results...)` | - | Call through a closure: `calli R0 (args) -> (results)`. Arguments are read with the function's parameter types; the count and register banks are checked at run time (E_TYPE). |
| `0x0112` | `tcalli` | `closure (args...)` | - | Tail call through a closure (like tcall). |
| `0x0113` | `spawni` | `closure (args...) -> (results...)` | - | Start a new process running a closure: `spawni R0 (args) -> (pid)`. The closure and its captures are deep-copied into the new process. |
| `0x0114` | `spawnil` | `closure (args...) -> (results...)` | - | spawni, linked to the new process from the start (as spawnl). |
| `0x0115` | `spawnim` | `closure (args...) -> (results...)` | - | spawni, monitoring the new process from the start (as spawnm). |

## Coroutines

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0120` | `conew` | `obj_dst, obj, [int]` | - | Make a coroutine that will run a closure: `conew R0, R1 [, words]`. It starts suspended; the first coresume calls the closure. It has its own registers, call frames and data stack (words 8-byte words, default 1024), and shares the heap, globals and raw memory with the rest of the process. Its stack is charged to the heap quota. |
| `0x0121` | `coresume` | `coroutine (args...) -> (results...)` | - | Run a suspended coroutine until it yields, returns or throws: `coresume R0 (args) -> (results)`. The first resume passes the arguments to the closure as a calli would; later ones deliver them to the results of the coyield it is suspended at. The results are its coyield values, or its return values once it finishes (it is then dead). An exception it does not catch kills it and is thrown again here. Resuming a coroutine that is running, normal (it resumed another) or dead traps E_CORO; resuming inside a callback from C traps E_CALLBACK. |
| `0x0122` | `coyield` | `(values...) -> (results...)` | - | Suspend the running coroutine: `coyield (values) -> (results)`. Its resumer's coresume receives the values; the next coresume's arguments arrive in results. Values keep their natural types (as an untyped ret's), and a destination of the wrong bank traps E_TYPE, too few values E_RET_COUNT. Outside a coroutine it traps E_CORO, inside a callback from C E_CALLBACK. |
| `0x0123` | `costatus` | `int_dst, obj` | - | dst = a coroutine's state: 0 suspended (or not yet started), 1 running, 2 normal (it resumed another coroutine), 3 dead (it returned, threw or was closed). |
| `0x0124` | `cocur` | `obj_dst` | - | dst = the running coroutine, or null outside one. |
| `0x0125` | `coclose` | `obj` | - | Kill a suspended coroutine and free its stack at once (its .try handlers do not run). Closing a dead one does nothing; closing a running or normal one traps E_CORO. |
