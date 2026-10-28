# MLWSVM Binary Module Format (`.mlwso`)

`mlwsvm asm prog.masm` writes `prog.mlwso`. The loader accepts either form, and a
compiler may emit `.mlwso` directly instead of text. Everything is **little-endian**
and byte-aligned. Integers marked *uleb*/*sleb* are unsigned/signed LEB128, and a
*str* is a uleb length followed by that many bytes (UTF-8, not NUL-terminated).

The reader is `src/loader/module_io.c` and the instruction codec is
`src/core/insn.c`. The loader validates every count, index and range, so a corrupt
file produces `E_LOAD_FORMAT` (or `E_VERIFY`) rather than a crash.

## File layout

```
magic        4 bytes  "MLWS"
version      u16      8
flags        u16      0
name         str      dotted module name, e.g. "std.io"
source       str      source file name (for backtraces); may be empty
srcfiles     uleb n, then n × str            files of the translated program that `.loc` names

imports      uleb n, then n × str            module names, in .import order

constants    uleb n, then n × {
               kind  u8    1 = string, 2 = BigInt, 3 = decimal (numbers as decimal text, e.g. "-12.50")
               len   uleb
               bytes len
             }

functions    uleb n, then n × {
               name   str
               flags  uleb   1 = exported, 2 = import, 4 = results declared
               if import:
                 import_index uleb            index into imports; name is the symbol
               else:
                 nparams u8, params u8 × nparams     MlwsType codes
                 nrets   u8, rets   u8 × nrets
                 nregs   uleb × 4                    I, F, R, P window sizes (0..32)
                 code_off uleb, code_len uleb        byte range in the code section
                 ninsns  uleb
                 lines   uleb × ninsns               source line of each instruction
                 nlocs   uleb, then nlocs × {        `.loc` runs, in increasing start order:
                           start uleb,               from this instruction until the next run,
                           file  uleb,               the code came from this file (1-based into
                           line  uleb                srcfiles; 0 = no position) and line
                         }
                 nhandlers uleb, then nhandlers × {start uleb, end uleb, target uleb}
                                                     exception regions [start, end) -> target,
                                                     as instruction indices
             }

data symbols uleb n, then n × { name str, offset uleb, size uleb, flags uleb (1 = exported) }

global refs  uleb n, then n × {
               kind u8   0 = local:  offset uleb
                         1 = import: import_index uleb, symbol str
             }

natives      uleb n, then n × str           native names used by ncall

structs      uleb n, then n × {
               name   str
               flags  uleb   1 = exported, 2 = import, 4 = built-in (provided by the VM:
                             Error, Down, FileInfo, Call, Callback, Date; nothing follows)
               if import:
                 import_index uleb                    the struct is defined there
               else:
                 nfields uleb, then nfields × { name str, type u8 (MlwsType) }
             }

field refs   uleb n, then n × { struct_index uleb, field_name str }
                                              resolved by name at link time

globals      uleb size (multiple of 8)
             bytes × size                     initial image of the global segment
             tags  × size/8                   one MlwsType tag per word (0xFF = raw bytes)

code         uleb size, then bytes × size     all functions' instructions

extensions   uleb n, then n × str           .extension names (docs/EXTENDING.md)

externs      uleb n, then n × {             .extern declarations (the FFI)
               name str, library str, symbol str
               flags uleb                   1 = blocking, 2 = serial
               nfixed u8                    parameters before `...` (= nparams if not variadic)
               nparams u8, then nparams × { kind u8, tag str }
               result kind u8, result tag str
             }
             kinds: 0 void, 1..4 i8..i64, 5..8 u8..u64, 9 f32, 10 f64,
             11 cstr, 12 buf, 13 ptr, 14 struct (by value), 15 struct*,
             16 callback (tag: the .callback type);
             +0x80 = an out parameter (numbers, ptr, or 14: an out struct);
             tag: the ptr<tag> name or the .cstruct name, or "" for none

cstructs     uleb n, then n × {             .cstruct layouts; each has a struct of the same name
               name str
               nfields uleb, then nfields × {
                 name str, kind u8          kinds as above: numbers, 11 cstr, 13 ptr, 14 struct
                 tag str                    ptr<tag> or the nested .cstruct, or ""
                 count uleb                 > 0: an array of that many numbers (at most 65536)
               }
             }

ffi frees    uleb n, then n × { tag str, library str, symbol str }   .ffi_free

callbacks    uleb n, then n × {             .callback function-pointer types
               name str
               flags uleb                   1 = async
               nparams u8, then nparams × { kind u8, tag str }   numbers, 11 cstr, 13 ptr (not async)
               result kind u8, result tag str                    0, a number or 13 ptr; async: 0
             }

requires     uleb n, then n × { library str, constraint str }   .requires
library      name str, version str          .library, in a library's manifest; both empty otherwise
publics      uleb n, then n × str           .public: the library's public modules (a manifest only)

(nothing may follow)
```

Operands refer to these tables by index: `CONST` refers to constants, `FUNC` to
functions (local functions and import stubs share one table), `GLOBAL*` to global
refs, `NATIVE` to natives, `STRUCT` to structs and `FIELD` to field refs.

### Type codes (`MlwsType`)

| Code | Type | Code | Type | Code | Type |
|---|---|---|---|---|---|
| 0 | none | 6 | i64 | 12 | f64 |
| 1 | b | 7 | u8 | 13 | r |
| 2 | c | 8 | u16 | 14 | p |
| 3 | i8 | 9 | u32 | 15 | big |
| 4 | i16 | 10 | u64 | 16 | dec |
| 5 | i32 | 11 | f32 | | |

## Instruction encoding

```
instruction := opcode:u16  type:u8  [type2:u8]  nops:u8  operand × nops
```

* `opcode` is the stable code from [ISA.md](ISA.md) (for example `add` = `0x0020`).
* `type2` is present only for opcodes that take two types (`cvt`, `bitcast`).
* `nops` is 0–32.

```
operand := mode:u8 payload
```

| Mode | Name | Payload |
|---|---|---|
| `0x01` | REG | reg:u8 |
| `0x02` | IMM | value:sleb |
| `0x03` | FIMM | value:f64 (8 bytes) |
| `0x04` | CONST | index:uleb |
| `0x05` | IND | reg:u8 |
| `0x06` | DISP | reg:u8 disp:sleb (words) |
| `0x07` | DISPB | reg:u8 disp:sleb (bytes) |
| `0x08` | IDX | reg:u8 ireg:u8 scale:uleb disp:sleb (words) |
| `0x09` | IDXB | reg:u8 ireg:u8 scale:uleb disp:sleb (bytes) |
| `0x0A` | POSTINC | reg:u8 |
| `0x0B` | PREDEC | reg:u8 |
| `0x0C` | POSTINCB | reg:u8 |
| `0x0D` | PREDECB | reg:u8 |
| `0x0E` | GLOBAL | gref:uleb disp:sleb (words) |
| `0x0F` | GLOBALB | gref:uleb disp:sleb (bytes) |
| `0x10` | GLOBALIDX | gref:uleb ireg:u8 scale:uleb disp:sleb (words) |
| `0x11` | GLOBALIDXB | gref:uleb ireg:u8 scale:uleb disp:sleb (bytes) |
| `0x12` | MIND | inner:operand disp:sleb (words) |
| `0x13` | MINDB | inner:operand disp:sleb (bytes) |
| `0x14` | LABEL | instruction index within the function:uleb |
| `0x15` | FUNC | function table index:uleb |
| `0x16` | NATIVE | native table index:uleb |
| `0x17` | ELEM | reg:u8 ireg:u8 disp:sleb (elements) |
| `0x18` | ELEMI | reg:u8 index:uleb |
| `0x19` | FIELD | reg:u8 field_ref:uleb |
| `0x1A` | STRUCT | struct table index:uleb |
| `0x1B` | UPVAL | capture index:uleb |

**Register byte**: `bank << 5 | index`, where the bank is 0 = I, 1 = F, 2 = R,
3 = P, 4 = special. For the special bank, index 0 = SP, 1 = FP and 2 = PID.

**Call blocks** (`call`, `tcall`, `ncall`, `spawn`, `spawnl`, `spawnm`,
`closure`, and the closure forms `calli`, `tcalli`, `spawni`, `spawnil`,
`spawnim`): operand 0 is the target (FUNC, NATIVE for `ncall`, or a value operand
holding the closure for the closure forms), operand 1 is IMM *argc*, the next
*argc* operands are the arguments (for `closure`, the captured values), and the
remaining operands are result destinations.

Branch targets are instruction indices, not byte offsets, so instructions can be
re-encoded freely. The loader decodes each function once into an internal form
with resolved operands, and the verifier then checks every invariant the
interpreter relies on.

## Versions

The header's version number identifies this layout, and the loader refuses a
file with any other version. Assemble `.mlwso` files with the VM that will run
them, or ship `.masm`.

## Library archives (`.mlwsl`)

`mlwsvm lib build` writes a library's manifest and modules into one file
([ASSEMBLY.md §8.1](ASSEMBLY.md#81-libraries)); the reader is
`src/loader/library_io.c`.

```
magic        4 bytes  "MLWL"
version      u16      1
flags        u16      0
manifest     uleb n, then n bytes        a module image (above) with .library, .requires and .public
modules      uleb n, then n × {
               name  str                 the module, `library` or `library.…`
               image uleb len, then len bytes   its module image
             }

(nothing may follow)
```

The loader reads an archive's manifest and module table when it scans the
library path, and decodes a module only when it is imported.

