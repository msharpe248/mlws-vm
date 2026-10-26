# MLWSVM Addressing Modes

Every operand of every instruction is written in one of the forms below, and any
operand position that takes a value or a destination accepts **all** of them,
including memory operands as destinations and memory-to-memory operations. The
assembler picks the encoding. Compilers can treat operands uniformly.

**Units.** Square brackets `[...]` count in **words** (8 bytes) and curly braces
`{...}` count in **bytes**. So `SP[2]` and `SP{16}` are the same location.

**Bases.** A memory base must be a pointer: a `P` register, `SP` or `FP`. An `R`
register base means element or field access on a heap object (modes 22–24).

**Index registers** must be `I` registers. Their value is taken as a signed 64-bit
integer.

## Summary

| # | Syntax | Mode byte | Location / value |
|---|---|---|---|
| 1 | `I3` `F0` `R2` `P1` `SP` `FP` `PID` | `0x01` REG | the register |
| 2 | `42` `-7` `'a'` `true` `null` `N*2+1` | `0x02` IMM | integer literal (constant expressions folded) |
| 3 | `3.5` `1e-9` `inf` | `0x03` FIMM | float literal |
| 4 | `"text"`, `123n`, `1.5d` | `0x04` CONST | reference to an immutable string, BigInt or decimal in the module constant pool |
| 5 | `[P1]` | `0x05` IND | memory at P1 |
| 6 | `SP[42]` `FP[-3]` `P1[N+1]` | `0x06` DISP | memory at base + 8·disp |
| 7 | `P1{13}` | `0x07` DISPB | memory at base + disp |
| 8 | `P1[I2]` `P1[I2*3+1]` `P1[I2-1]` | `0x08` IDX | memory at base + 8·(I2·scale + disp) |
| 9 | `P1{I2*4+8}` | `0x09` IDXB | memory at base + I2·scale + disp |
| 10 | `[P1]+` `[SP]+` | `0x0A` POSTINC | memory at base, then base += 8 |
| 11 | `-[P1]` `-[SP]` | `0x0B` PREDEC | base -= 8, then memory at base |
| 12 | `{P1}+` | `0x0C` POSTINCB | memory at base, then base += access width |
| 13 | `-{P1}` | `0x0D` PREDECB | base -= access width, then memory at base |
| 14 | `@x` `@x[3]` | `0x0E` GLOBAL | global `x` + 8·disp |
| 15 | `@x{3}` | `0x0F` GLOBALB | global `x` + disp |
| 16 | `@x[I1]` `@x[I1*2+1]` | `0x10` GLOBALIDX | global `x` + 8·(I1·scale + disp) |
| 17 | `@x{I1*4}` | `0x11` GLOBALIDXB | global `x` + I1·scale + disp |
| 18 | `[SP[0]]` `[@p][2]` `[P1[I2]][3]`¹ | `0x12` MIND | load a pointer from an inner memory operand, then + 8·disp |
| 19 | `[SP[0]]{4}` | `0x13` MINDB | the same, with a byte displacement |
| 20 | `ARG[n]` | DISP on FP | `FP[n]`: the n-th stack-passed argument |
| 21 | `LOC[n]` | DISP on FP | `FP[-1-n]`: the n-th local reserved by `enter` |
| 22 | `R2[I4]` | `0x17` ELEM | element I4 of the string, byte buffer, array or list in R2 |
| 23 | `R2[7]` `R2[N-1]` | `0x18` ELEMI | element 7 (a constant index) |
| 24 | `R2->Point.x` `R2->geo.Point.x` | `0x19` FIELD | field `x` of the `Point` struct in R2 |
| 25 | `UP[2]` | `0x1B` UPVAL | capture 2 of the closure currently running (read/write) |
| — | `Point` after `new`/`isa` | `0x1A` STRUCT | a struct type (`new` and `isa` only) |
| — | `label` | `0x14` LABEL | branch target (branch operands only) |
| — | `func`, `mod.func` | `0x15` FUNC | call target (`call`/`tcall` only) |
| — | `"io.print_i"` after `ncall` | `0x16` NATIVE | native function (`ncall` only) |

¹ Only a constant displacement is allowed after a memory-indirect operand. The inner
operand can use any of modes 5–17.

## Details and examples

### Registers, literals and strings

```asm
mov I0, 0x7fff_ffff          ; integer literal (signed LEB128 in the binary)
mov F1, 2.5e-3               ; float literal (8 bytes)
add F2, F1, 1                ; integer literals are converted in float context
mov R0, "hello"              ; constant-pool string (deduplicated per module)
mov R1, null                 ; null reference: the integer literal 0
mov I1, MAX_ITEMS*2          ; named constants and expressions are folded
```

String constants are immutable, immortal objects. Two identical literals in the
same module are the same object (`beq.r` is true).

### Base + displacement: `SP[n]`, `FP[n]`, `P[n]`, `P{n}`

```asm
push 10, 20, 30              ; SP[0]=30, SP[1]=20, SP[2]=10
mov  SP[1], 99               ; write the second word
mov.u8 P0{3}, 0xFF           ; byte 3 of the block P0 points to
mov  [P0], 1                 ; displacement 0 (IND)
```

`SP[42]` is "42 words above the stack pointer". The data stack grows **down**, so
positive displacements from SP reach values that are already on the stack.

### Indexed: `base[Ireg*scale+disp]`

```asm
mov P0[I1], I2               ; word array: P0[i] = I2
mov.i32 P0{I1*4}, -5         ; i32 array: byte offset i*4
mov I3, P0[I1*2+1]           ; array of 2-word records, second field
```

The index register comes first. `*scale` is optional (default 1) and must be a
single term: a literal, a constant, or a parenthesised expression. `+disp` / `-disp`
is optional and may be any constant expression.

### Auto-increment / auto-decrement

```asm
mov [P1]+, I0                ; store, then P1 += 8
add I2, -[P1]                ; P1 -= 8, then add the word there
mov.u8 {P3}+, 'a'            ; store a byte, then P3 += 1
mov.u16 {P3}+, 0x6463        ; store 2 bytes, then P3 += 2
mov -[SP], 7                 ; same as push 7
mov I6, [SP]+                ; same as pop I6
```

The register is updated as a side effect, at the point the operand is evaluated
(operands are evaluated left to right, destination first). Untyped byte-stepping
accesses step by 8.

### Globals: `@name`

```asm
counter: .i64 0
table:   .i32 1, 2, 3, 4

add @counter, 1
mov.i32 I0, @table{I1*4}     ; table[I1]
lea P0, @table               ; pointer into the module's global segment
mov @std.math.some_table[2], I0  ; exported data from an imported module
```

Global references are resolved by the linker to the module's global segment in the
current process.

### Memory-indirect: `[mem]`, `[mem][d]`, `[mem]{d}`

```asm
push P0                      ; SP[0] holds a pointer
mov I0, [SP[0]][2]           ; I0 = word 2 of the block P0 points to
mov.p @head, P0
add [@head]{8}, 1            ; follow the pointer stored in the global head
```

The inner operand must be a plain memory operand (modes 5–17). It is read as a
pointer (`p`), and the outer displacement is added to it.

### Arguments and locals: `ARG[n]`, `LOC[n]`

```asm
.func sum3                   ; three arguments passed on the data stack
    enter 1                  ; one local: LOC[0]
    add LOC[0], ARG[0], ARG[1], ARG[2]
    mov I0, LOC[0]
    ret I0
.endf
```

These are aliases for `FP[n]` and `FP[-1-n]`; see [ABI.md](ABI.md) for the frame
layout.

### Object elements and fields: `R[i]`, `R->Type.field`

```asm
anew.f64 R1, 8
mov  R1[I2], 2.5             ; array element (bounds checked)
add  R0->Point.x, R1[0]      ; struct field + array element
lpush R3, R0->Point.label    ; fields are ordinary operands
mov.u8 R4[0], 'H'            ; byte buffer byte
mov  I5, R6[I1]              ; byte of a string (read-only)
```

The base must be an `R` register holding a non-null object (`E_NULL`
otherwise). The index is an `I` register or a non-negative constant, with no scale
or displacement, and it counts elements. A field operand checks that the object is
a struct of the named type (`E_TYPE`). Element and field operands are not
addresses: `lea` rejects them, and they can't be the inner operand of a
memory-indirect operand.

## Access width and types

The instruction's type sets how many bytes a memory operand reads or writes (`u8`
1, `i16` 2, `i32`/`u32`/`f32`/`c` 4, everything else 8). Memory is
**little-endian** on every host. Unaligned accesses are allowed for every type
except `r`.

## Safety

Every memory access is checked:

* **Bounds**: the access must lie inside its block (stack, global segment or raw
  allocation). Otherwise the VM traps with `E_BOUNDS`, or `E_STACK_UNDERFLOW` /
  `E_STACK_OVERFLOW` on the stack.
* **Lifetime**: pointers carry a generation, so using a pointer to a freed or
  reallocated block traps with `E_UAF`.
* **Null**: dereferencing a null pointer traps with `E_NULL`.
* **References**: a word read as `r` must have been written as a reference, or
  never written (which reads as null). Anything else traps with `E_TYPE`, and a
  reference word cannot be read as an integer. This keeps the GC precise and makes
  references unforgeable.
* **Objects**: element indices are bounds checked, field operands check the struct
  type, and typed slots check the register bank (see [ASSEMBLY.md §7](ASSEMBLY.md#7-heap-objects-and-structs)).
