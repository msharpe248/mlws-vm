# 5. Memory and garbage collection

A program makes strings, lists, maps and records, uses them for a while, and
stops needing them. Something has to give that memory back. This chapter
explains why a virtual machine usually does that itself, the general ways
of doing it, and how MLWSVM's collector works: the kinds of memory a program
can use, what an object looks like, how an object is allocated, how the
collector finds the objects still in use, how it reclaims the rest without
visiting them, and how generations and a write barrier keep a collection's
cost down to what survives it. It ends with a program you can run to watch
the collector work.

## Why a VM manages memory

In C you call `malloc` and `free`. Getting `free` right is the programmer's
job, and three things go wrong: memory that is never freed (a leak), memory
used after it is freed (a use after free), and memory freed twice. The
languages MLWSVM runs, Lox, Lua and Erlang, have no `free` at all. Their
programs make objects and drop them, and the language promises that dropped
objects are reclaimed and live ones never are.

There are two families of answers:

* **Reference counting.** Each object counts the references to it, and is
  freed when the count reaches zero. CPython does this (with a separate
  collector for cycles, which counting alone never frees). The cost is
  spread over every store of a reference.
* **Tracing.** Every so often, start from the *roots* (the references the
  running program can use directly: registers, stack slots, globals), follow
  every reference to find every object that can still be reached, and treat
  the rest as garbage. The JVM, V8, Lua, BEAM and MLWSVM all trace. Cycles
  are no problem, and a store costs little or nothing; the cost is the
  collections.

A tracing collector also has to know which words are references. A
*conservative* collector (Boehm's, for C) treats anything that looks like an
address as one, so an integer can keep an object alive. A *precise*
collector knows exactly which words are references, which a VM can, since
it defines the types. MLWSVM's collector is precise.

*Crafting Interpreters* builds a simple tracing collector for Lox in
[Garbage Collection](https://craftinginterpreters.com/garbage-collection.html):
mark from the roots, then sweep a list of every object. MLWSVM starts from
the same idea and changes the two places where it is slow.

## The kinds of memory

Not every byte a program touches is collected. MLWSVM has five kinds of
memory, and only the first is garbage-collected
([DESIGN.md §10](../DESIGN.md#10-pointers-segments-and-tags)):

| Kind | Held in | Made by | Freed |
|---|---|---|---|
| Heap objects: strings, byte buffers, arrays, structs, lists, maps, closures, BigInts, decimals, coroutines | `R` registers, and words tagged as references | `new`, `lnew`, `mnew`, `closure`, string operations, ... | by the collector |
| The data stack | reached through `SP`, `FP`, `ARG[n]`, `LOC[n]`, or a `P` pointer | first use | with the process |
| Global segments, one per module | named globals, or a `P` pointer | when the process starts | with the process |
| Raw blocks | `P` registers | `alloc` | by `free`, checked |
| Handles | `I` registers: an integer naming an object | `hnew` | by `hfree` |

A `P` register holds a *checked pointer*, not an address. It is a segment
number, a generation and an offset:

```text
 63            44 43        32 31                       0
+----------------+------------+-------------------------+
|  segment (20)  |  gen (12)  |      offset (32)        |
+----------------+------------+-------------------------+
```

Every dereference looks the segment up in the process's own table, checks
its generation and its bounds, and traps otherwise. Freeing a raw block
bumps its slot's generation, so an old pointer to it no longer matches:

```asm
.func main
    alloc P0, 8
    free P0
    mov I0, [P0]
    ret 0
.endf
```

```text
$ ./mlwsvm run tests/memory/use_after_free.masm
runtime error[E_UAF]: use of a freed or reallocated raw block
  at use_after_free.main (tests/memory/use_after_free.masm:6)
```

The two worlds are kept apart. The data stack and the globals keep a *tag*
byte for every 8-byte word, the type it was last written as. Reading a word
as a reference requires the tag `T_R`, and reading a reference word as
anything else traps, so a reference can be neither forged from an integer
nor read out as a number. Raw blocks have no tags and cannot hold a
reference at all. That is what makes the collector's job possible: it can
find every reference by looking at `R` registers and at `T_R`-tagged words,
and nowhere else. A handle is how raw memory or C code refers to an object:
`hnew` puts the object in the process's handle table, which is a root, and
returns its index as an integer.

## A heap object

Every object starts with a 16-byte header, `struct Obj` in
[src/heap/object.h](../../src/heap/object.h):

```c
typedef struct Obj {
    uint8_t kind;
    uint8_t flags;
    uint16_t slab; /* its size in 16-byte granules, if it is in its process's chunks (gc.c); 0: from malloc */
    uint32_t off;  /* in the chunks: its offset in its chunk */
    struct Obj *next; /* heap list (objects from malloc) */
} Obj;
```

`kind` says how to read the rest (it is also what the `typeof` instruction
returns). `flags` holds the collector's state: `OBJF_MARK`, `OBJF_OLD`,
`OBJF_REMEMBERED`, `OBJF_OWNS` and `OBJF_IMMORTAL`, which the sections below
explain. `slab` and `off` say where a small object lives, so the collector
can find its chunk without a lookup. `next` links objects that came from
`malloc` instead.

After the header comes the object itself. A struct (`ObjStruct`) is a
pointer to its type and then one 8-byte word per field, so a struct with
two `ref` fields is 16 + 8 + 16 = 40 bytes. The type (`StructDef`) records
each field's type, which is how the collector knows which fields are
references. Lists and maps keep a tag byte per value instead, since each
element can hold any type.

## Allocation: bumping into free lines

The simplest allocator for a collected heap is a free list: freed objects
are put on a list, and allocation takes one off. Each allocation costs a
search, and each free costs a visit to the dead object. The fastest
allocator is a *bump pointer*: memory is one free run, and allocation adds
the size to a pointer. Copying collectors (the young generations of HotSpot
and V8) get a free run by moving every survivor out, which needs them to
update every reference to a moved object.

MLWSVM uses *mark-region* allocation, the idea of Blackburn and McKinley's
Immix collector. Small objects (up to 256 bytes) live in the process's
*chunks*. A chunk is divided into *lines* of 128 bytes, each with a mark
byte. A collection marks the lines that surviving objects lie on. The runs
of unmarked lines are *holes*, and the allocator bumps a pointer through
them:

```text
chunk:  | line 0 | line 1 | line 2 | line 3 | line 4 | line 5 | line 6 | line 7 |
marks:      1        1        0        0        0        1        0        0
                         \______ hole ______/            \___ hole ___/
                          bump ──►  bump_lim  hole_end
```

`bump` is the next free byte, `bump_lim` the end of the part already zeroed
(holes are zeroed 2 KiB at a time ahead of the allocator, not object by
object), and `hole_end` the end of the hole. The fast path is inline in
every place that makes an object, `gc_alloc` in
[src/heap/heap.h](../../src/heap/heap.h):

```c
static inline Obj *gc_alloc(Process *p, uint8_t kind, size_t size) {
    size_t n = (size + 15) & ~(size_t)15;
    unsigned char *b = p->bump;
    uint64_t hb = p->heap_bytes + size;
    if (__builtin_expect(size <= GC_SMALL_MAX && !((1u << kind) & GC_OWNER_KINDS) &&
                             (size_t)(p->bump_lim - b) >= n && hb < p->alloc_limit, 1)) {
        p->bump = b + n;
        p->heap_bytes = hb;
        Obj *o = (Obj *)b;
        o->kind = kind;
        o->slab = (uint16_t)(n / 16);
        o->off = (uint32_t)(b - p->bump_base);
        p->young_plain_bytes += size;
        p->objects_allocated++;
        return o;
    }
    return gc_alloc_slow(p, kind, size);
}
```

There is no lock and no `malloc`. Two compares decide the fast path: is
there room in the zeroed part of the hole, and is the heap still below
`alloc_limit`, one number that is both the next collection's threshold and
the heap quota. The 40-byte struct above takes 48 bytes, three 16-byte
granules. The JIT emits the same sequence in machine code for `new`.

When the hole runs out, `bump_refill` in
[src/heap/gc.c](../../src/heap/gc.c) takes the slow path: it zeroes the next
2 KiB of the hole, or finds the next hole in the chunk (`next_hole` scans
the line marks for a run of zeros long enough), or moves to the next chunk
with free lines, or makes a new chunk.

Chunks start at 1 KiB and double up to 64 KiB, so a process that holds a
few objects stays small. Objects larger than 256 bytes come from `malloc`
and go on the list `Process.heap`. An object that owns memory besides
itself (a byte buffer's bytes, a map's entries, a list's separate storage, a
coroutine's stack, a C resource) goes on an *owner list*, for a reason the
section on reclaiming gives.

## Marking from the roots

A collection marks every object reachable from the roots. In MLWSVM the
roots are ([DESIGN.md §11.4](../DESIGN.md#114-marking)):

1. every frame's `R` registers, and the closure it is running;
2. the words of the data stack tagged `T_R`;
3. the words of every global segment tagged `T_R`;
4. the handle table;
5. the exception being thrown, the running coroutine, objects a native call
   in progress is using;
6. the messages in the mailbox.

That list is short because collections run only *between* instructions, at
a safepoint ([DESIGN.md §8.5](../DESIGN.md#85-safepoints)). An allocation
only sets `gc_pending`; the interpreter checks it after the instruction.
While an instruction or a native call runs, C code may hold object pointers
in its own local variables, and nothing will be freed under it. So the
collector never has to find roots in the C stack.

Following references could be written as a recursive function, but a long
list or a deep tree would overflow the C stack. Instead `mark` pushes each
newly marked object on a mark stack, and a loop pops and scans them. Here
is `mark`, slightly trimmed:

```c
static void mark(Process *p, Obj *o) {
    if (!o) return;
    ...
    uint8_t f = o->flags;
    if ((f & (OBJF_OLD | OBJF_MARK)) == (OBJF_OLD | p->mark_bit) || (f & OBJF_IMMORTAL)) return;
    o->flags = (uint8_t)((f & ~OBJF_MARK) | OBJF_OLD | p->mark_bit);
    size_t fp = obj_footprint(o);
    p->gc_live++;
    p->gc_live_bytes += fp;
    if (o->slab) {
        Chunk *c = (Chunk *)((unsigned char *)o - o->off - offsetof(Chunk, data));
        uint32_t l = o->off >> LINE_SHIFT, last = (o->off + o->slab * SLAB_GRAIN - 1u) >> LINE_SHIFT;
        do c->lines[l] = 1;
        while (++l <= last);
        ...
    }
    VEC_PUSH(p->markstack, o);
}
```

Marking an object sets its flags and also the mark bytes of every line it
lies on (an object may cross a line boundary). `off` takes it straight to
its chunk's header. The roots are walked by `mark_roots`; the first of them,
the register windows, is `mark_frames`:

```c
static void mark_frames(Process *p, uint64_t *regs, const Frame *frames, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const Frame *f = &frames[i];
        Obj *const *r = (Obj *const *)frame_reg(regs, f, BANK_R, 0);
        for (uint32_t k = 0; k < f->fn->mf->nregs[BANK_R]; k++) mark(p, r[k]);
        mark(p, f->closure);
    }
}
```

The `R` bank is the only register bank that can hold a reference, so the
collector scans exactly those words and ignores the `I`, `F` and `P`
registers. `scan` then reads each popped object by its kind: a struct's
fields whose declared type is a reference, a list's elements tagged `T_R`,
a map's keys and values tagged `T_R`, a closure's captures. Immortal
objects (a module's constant strings, made once and shared by every
process) are never marked, and so never written.

## Reclaiming: nothing is swept

The classic second phase is the *sweep*: walk every object and free the
unmarked ones. Its cost is proportional to the dead objects, which are most
of what a program allocates. MLWSVM has no sweep of the chunks. A dead
object in a chunk is never touched again: once no survivor's line covers
it, its bytes are part of a hole and the allocator writes over them.

Three things still need a walk, all of them short:

* **The owner lists.** A dead byte buffer must `free` its bytes, a dead
  file handle must close its file. That is why such objects are on lists of
  their own: the collector walks those lists, not the chunks.
* **The `malloc` list**, for the large objects.
* **The chunks' line marks**, to decide which chunks have free lines and go
  on the allocator's list (`recycle_chunks`), most recently used first since
  their memory is likeliest to be in the caches.

When a process ends, its chunks are freed whole, after its foreign objects
are finalized.

## Generations and the write barrier

Most objects die young: a temporary string, the list built to hold one
call's results. Objects that survive a while tend to survive a long time.
This is the *generational hypothesis*, and most production collectors (the
JVM's, V8's) exploit it: collect the young objects often and cheaply, and
the whole heap rarely.

In MLWSVM an object that survives a collection is *old* (`OBJF_OLD`). A
*minor* collection marks only young objects and frees only young ones; a
*major* one does the whole heap. `gc_collect` makes it major when the old
objects have doubled since the last major one, when a heap quota is near, or
when the program asks (`gc.collect`).

Objects do not move between generations; the flag is the generation. These
are *sticky mark bits*: marking an object makes it old, and an old object's
mark bit always equals `Process.mark_bit`. A minor collection therefore
skips every old object at the first test in `mark` above. A major
collection flips `mark_bit`, which makes every old object read as unmarked
at once, with no pass over the heap to clear marks.

There is one catch. A minor collection does not look inside old objects, so
if a program stores a young object into an old one, and nothing else refers
to the young one, the collector would miss it and free it while the old
object still points at it. The fix is a **write barrier**: every store of a
reference into an existing object runs a check after it, `gc_write` in
[src/heap/heap.h](../../src/heap/heap.h):

```c
static inline bool gc_young(const Obj *v) { return v && !(v->flags & (OBJF_OLD | OBJF_IMMORTAL)); }
static inline void gc_write(Process *p, Obj *o, const Obj *v) {
    if (__builtin_expect((o->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD, 0) && gc_young(v)) gc_remember(p, o);
}
```

If the object is old and not yet remembered, and the value is young, the
object goes on the *remembered set* (`Process.remset`). It checks the
object first because most stores are into young objects, for which the
check fails on one load. The next minor collection scans every remembered
object as if it were a root, in `gc_minor`:

```c
static void gc_minor(Process *p) {
    ...
    mark_roots(p);
    uint64_t rescanned = 0;
    for (size_t i = 0; i < p->remset.n; i++) {
        Obj *o = p->remset.v[i];
        o->flags &= (uint8_t)~OBJF_REMEMBERED;
        rescanned += obj_footprint(o);
        scan(p, o);
    }
    p->remset.n = 0;
    drain(p);
    weak_finish(p, false);
    if (p->vm->gc_verify) gc_verify(p);
    ...
```

(`weak_finish` decides what weak references and finalizers leave alive:
[DESIGN.md §11.10](../DESIGN.md#1110-weak-references-and-finalizers).)

A barrier that is missing anywhere is a bug that frees live memory, so the
sites are worth listing. They are:

| Where | What stores |
|---|---|
| `slot_write` in [src/vm/interp.c](../../src/vm/interp.c) | the generic `mov` into a field, element or capture |
| `GC_WRITE_R` in [src/vm/qforms.inc](../../src/vm/qforms.inc) | the quickened field, element and capture stores, which jump to a separate handler (`h_gc_remember_R0`) to remember |
| `list_push`, `list_insert`, `map_set` in [src/heap/objects.c](../../src/heap/objects.c) | lists and maps, and so every native that uses them |
| [src/native/ffi.c](../../src/native/ffi.c) | an FFI struct filled in place (`gc_touch`) |
| `coro_swap` in [src/vm/coro.c](../../src/vm/coro.c) | a coroutine given a new context (`gc_touch`) |
| [src/vm/jit.inc](../../src/vm/jit.inc) | compiled stores, which check and leave for the interpreter to remember |

An object just allocated is young, so filling it in needs no barrier, and
the JIT skips the check for an object made in the same block.

How big should the young generation be? A minor collection runs each time
the process has allocated `Process.nursery` bytes. It starts at 64 KiB
(`--gc-min`), small enough to stay in the caches for many processes, and
doubles, up to 16 MiB, while a minor collection promotes more than an
eighth of it: a program building a structure larger than the nursery would
otherwise have it caught half-built, promoted, and left to die old.

## Why this collector

MLWSVM's collector is non-moving, non-incremental, generational and
mark-region, one per process, with a write barrier and no locks
([DESIGN.md §11.6](../DESIGN.md#116-why-this-collector)). The reasons:

* **Objects do not move**, because C code relies on their addresses. A
  native such as `list.sort` calls back into VM code, which may collect,
  while holding object pointers; maps hash objects by address. The price is
  that a line stays in use while any object on it lives, and a line held by
  an old object that has died waits for the next major collection.
* **Lines** make allocation a pointer bump, and make the dead free. With
  nothing kept, a program that allocates 1.6 GB spends 1.4 ms in 20,833
  collections, against 81 ms when each dead object went back on a free list
  ([PERFORMANCE.md](../PERFORMANCE.md#the-small-object-allocator)).
* **Generations** stop the collector re-marking long-lived data at every
  collection.

Both were measured against what they replaced, with the JIT, on the three
test machines ([PERFORMANCE.md](../PERFORMANCE.md#the-collector)). A few
rows of each table:

| Change | Program | M5 | i7, GCC | Ryzen, Clang |
|---|---|---|---|---|
| Generations, against collecting the whole heap each time | Lua binarytrees | −25% | −39% | −28% |
| | Erlang binarytrees | −12% | −24% | −13% |
| Lines and bump allocation, against free lists and a sweep of the young | Lua binarytrees | −25% | −36% | −34% |
| | Lox instantiation | −28% | −29% | −37% |
| | bench/maps | −12% | −12% | −7% |

## One heap per process

Each MLWSVM process (chapter 7) has its own heap, collected only on its own
thread, between its own instructions. No heap is ever touched by two
threads. Messages between processes are copied into the receiver's heap,
and the only objects processes share are immortal constants, which are
never written, not even by marking. So the collector needs no locks, never
stops another thread, and a process with a small heap has small, fast
collections however big the other processes' heaps are. BEAM, Erlang's VM,
also gives each process its own heap, for much the same reasons.

## Worked example: watching the collector

This program builds one tree of 32,767 nodes and keeps it to the end, then
builds and discards a hundred trees of 2,047 nodes each:

```asm
; trees.masm - one long-lived tree, then many short-lived ones
.struct Node
    left: ref
    right: ref
.ends

.func make(i64 d) -> ref            ; a complete tree of depth d
    new R0, Node
    bz d, 1f
    sub I1, d, 1
    call make (I1) -> (R1)
    call make (I1) -> (R2)
    mov R0->Node.left, R1           ; R0 may be old by now: the barrier checks
    mov R0->Node.right, R2
1:  ret R0
.endf

.func count(ref n) -> i64           ; its nodes
    mov R1, n->Node.left
    bz R1, 1f
    call count (R1) -> (I0)
    mov R2, n->Node.right
    call count (R2) -> (I1)
    add I0, I0, I1, 1
    ret I0
1:  ret 1
.endf

.func main
    call make (14) -> (R0)          ; kept to the end: it becomes old
    mov I1, 0
    mov I2, 0
1:  call make (10) -> (R1)          ; garbage as soon as it is counted
    call count (R1) -> (I3)
    add I2, I3
    inc I1
    blt I1, 100, 1b
    call count (R0) -> (I0)
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    ncall "io.print_i" (I2)
    ncall "io.nl" ()
    ret 0
.endf
```

Run it with `--stats` (this run on an Apple M5 Pro; times vary from run to
run):

```text
$ ./mlwsvm run --stats trees.masm
32767
204700
--- mlwsvm stats ---
...
heap peak:        3848960 bytes
heap at exit:     3685200 bytes
objects:          237467 allocated, 145337 collected
gc:               10 collections (3 of the whole heap), 0.774 ms
heap chunks:      4652032 bytes, 49% of their lines in use at the last collection
...
```

237,467 is 32,767 + 100 × 2,047: every node is one object. The tree kept to
the end is 32,767 × 40 bytes, about 1.3 MB, yet 3.7 MB is in use at exit.
The difference is the young generation working as described above: each
short-lived tree is 82 KB, larger than the 64 KiB nursery, so minor
collections catch trees half-built on the register stack, promote them, and
they die old, waiting for a major collection. Start with a larger nursery
and fewer are caught:

```text
$ ./mlwsvm run --stats --gc-min 1M trees.masm
...
heap at exit:     2948280 bytes
objects:          237467 allocated, 163760 collected
gc:               5 collections (1 of the whole heap), 0.355 ms
```

The ten collections took under a millisecond together, for a program that
allocated about 9.5 MB: each minor collection's cost is what it marks, and
the dead short-lived trees cost nothing.

Two options exist for testing the collector itself
([TOOLING.md](../TOOLING.md#vm-options)), and `make check-gc` runs the whole
test suite under each:

* `--gc-stress` (or `MLWSVM_GC_STRESS=1`) asks for a collection after every
  allocation, at the next safepoint, makes every eighth one major, and fills
  free lines with the byte `0xDB`, so a use after a free reads garbage
  rather than the old value.
* `--gc-verify` (or `MLWSVM_GC_VERIFY=1`) shrinks the nursery to 16 KiB
  (unless `--gc-min` is given) and, after each minor collection's marking,
  walks the whole heap from the roots. A young object that the walk reaches
  but the marking missed means a missing write barrier, and the VM aborts
  naming the kind of object that held it.

```text
$ ./mlwsvm run --stats --gc-stress trees.masm
...
heap at exit:     1392560 bytes
objects:          237467 allocated, 202653 collected
gc:               752 collections (95 of the whole heap), 23.058 ms
```

Under stress the heap at exit is exactly the long-lived tree plus the last
short-lived one, which `R1` still holds (1,310,680 + 81,880 bytes), and the
output is the same, which is the point. There are fewer collections than allocations because a
quickened `new` only marks a collection as due, and it runs at the next
safepoint (after a generic instruction, or at the next slice).

To see the barrier, store young objects into an old list:

```asm
; barrier2.masm - an old list given young objects: the write barrier at work
.struct Box
    v: i64
.ends

.func main
    lnew R0                         ; lives to the end, so it becomes old
    mov I1, 0
1:  new R1, Box                     ; young
    mov R1->Box.v, I1
    lpush R0, R1                    ; young into old: list_push remembers the list
    new R2, Box                     ; garbage at once
    new R2, Box
    inc I1
    blt I1, 20000, 1b
    mov R1, R0[19999]               ; the last box pushed
    ncall "io.print_i" (R1->Box.v)
    ncall "io.nl" ()
    ret 0
.endf
```

```text
$ ./mlwsvm run --gc-verify --stats barrier2.masm
19999
...
objects:          60001 allocated, 39958 collected
gc:               73 collections (2 of the whole heap), 5.800 ms
```

After the first collection the list is old, and each `lpush` of a new box
goes through `list_push`'s barrier. Each minor collection finds the boxes
pushed since the one before from the remembered list, and `--gc-verify`
confirms that none was missed. Without that barrier, those boxes would be
freed while the list still held them, and the verifier would stop the run
at the first such collection.

## Where to look

| What | Where |
|---|---|
| The object header and layouts | `struct Obj`, `ObjStruct`, `ObjList`, `ObjMap`, `ObjClosure` in [src/heap/object.h](../../src/heap/object.h) |
| The allocator's fast path, the write barrier | `gc_alloc`, `gc_write`, `gc_touch` in [src/heap/heap.h](../../src/heap/heap.h) |
| Chunks, lines and holes | `chunk_new`, `next_hole`, `bump_refill`, `gc_alloc_unaccounted` in [src/heap/gc.c](../../src/heap/gc.c) |
| Marking | `mark`, `mark_frames`, `mark_segment`, `mark_roots`, `scan`, `drain` in gc.c |
| Collections | `gc_minor`, `gc_major`, `gc_collect`, `gc_threshold`, `recycle_chunks` in gc.c |
| Checking the barrier | `gc_verify`, `verify_reach` in gc.c |
| Checked pointers and raw blocks | `proc_segment_fast` in [src/vm/vm.h](../../src/vm/vm.h), `mem_ptr` in [src/vm/interp.c](../../src/vm/interp.c), `proc_alloc` in [src/vm/process.c](../../src/vm/process.c) |
| Handles | `handle_new`, `handle_get`, `handle_free` in gc.c |
| Reference | [DESIGN.md §10](../DESIGN.md#10-pointers-segments-and-tags) and [§11](../DESIGN.md#11-the-heap-and-the-collector), [ABI.md §6](../ABI.md#6-the-garbage-collected-heap), [PERFORMANCE.md: Memory](../PERFORMANCE.md#memory), [PRIOR_ART.md](../PRIOR_ART.md) for Immix and sticky mark bits |

Previous: [4. Making an interpreter fast](04-fast-interpreter.md) · Next: [6. Calls, frames and exceptions](06-calls-frames-exceptions.md)
