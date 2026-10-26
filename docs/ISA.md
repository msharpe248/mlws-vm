# MLWSVM Instruction Set Reference

> Generated from `src/core/opcodes.def` by `mlwsvm doc isa`. Do not edit by hand;
> change the opcode table and regenerate with `make docs`.

Every instruction is written `mnemonic[.type[.type2]] operands`. See
[ASSEMBLY.md](ASSEMBLY.md) for syntax, [ADDRESSING.md](ADDRESSING.md) for operand forms and
[ABI.md](ABI.md) for calls and the stack.

**Operand names.** `dst` is a writable location (a register or memory). `src` and `val`
are any value operand (register, literal, string or memory). `int` is read as `i64`,
`ptr` is read as a pointer (`p`), and `bool_dst` is written as `b`. `mem` must be a memory
operand, `n` is a non-negative integer constant, and `[x]` marks an optional operand.

**Types column.** This lists the type suffixes the instruction accepts. `int` covers
`b c i8 i16 i32 i64 u8 u16 u32 u64` and `float` covers `f32 f64`. Without a suffix the
type is inferred from the first register operand: I gives `i64`, F `f64`, R `r`, P/SP/FP `p`.
If there is no register operand, the first literal decides, and `i64` is the fallback.
`mov`, `swap`, `push`, `pop` and `ret` without a suffix use each operand's natural type,
which preserves the type tag of a stack or global word.

**Two-operand shorthand.** For arithmetic and bitwise instructions, `op dst, src` means
`dst = dst op src`. With three or more operands, `dst = s1 op s2 op ... op sN`, so `add`, `mul`,
`min`, `max`, `and`, `or` and `xor` take up to 31 sources.


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

## Calls

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0080` | `call` | `func (args...) -> (results...)` | - | Call a function: `call f (a1, a2, ...) -> (d1, d2, ...)`. Each argument is read with the callee's declared parameter type and bound to the callee's next free register of that bank (int-class args to I0, I1, ...; floats to F0, ...; refs to R0, ...; pointers to P0, ...). Return values are written to d1, d2, .... The callee gets fresh registers, and FP = SP at the call. |
| `0x0081` | `tcall` | `func (args...)` | - | Tail call: like call, but replaces the current frame. The callee's return values go to the current function's caller. A tail call cannot have result operands. |
| `0x0082` | `ret` | `[val, ...]` | - | Return from the current function with zero or more values. Operands are read with the function's declared return types (or their natural types if none are declared). SP is restored to its value at the call. |
| `0x0083` | `ncall` | `"native" (args...) -> (results...)` | - | Call a native (host) function by name: `ncall "io.print_i" (I0)`. See docs/NATIVES.md for the available natives and their signatures. |

## Raw memory

| Opcode | Mnemonic | Operands | Types | Description |
|---|---|---|---|---|
| `0x0090` | `alloc` | `ptr_dst, int` | - | Allocate a zeroed raw memory block of n bytes. dst = pointer to it. Raw blocks are bounds checked and owned by the process, and are released when it exits. |
| `0x0091` | `free` | `ptr` | - | Free a raw block. Freeing null is a no-op. Freeing anything other than the start of a live block traps E_BAD_FREE. Any later use of the pointer traps E_UAF. |
| `0x0092` | `realloc` | `ptr_dst, ptr, int` | - | Resize a raw block: `realloc dst, ptr, n`. The contents are preserved up to the smaller size and new bytes are zeroed. Old pointers into the block become invalid (E_UAF). |
| `0x0093` | `memcpy` | `ptr, ptr, int` | - | Copy n bytes: `memcpy dst_ptr, src_ptr, n` (the regions may overlap). Copied bytes are untyped: they cannot later be read as references. |
| `0x0094` | `memset` | `ptr, int, int` | - | Fill n bytes with a byte value: `memset ptr, byte, n`. |
