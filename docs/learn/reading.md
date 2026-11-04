# Further reading

Books and papers for going further than these chapters, in the chapters'
order. Each entry says what it gives you and where MLWSVM uses the idea.
Books link to their publisher's or author's page, and are free online where
the entry says so. Papers link to their DOI, and to a free copy where the
authors keep one. [PRIOR_ART.md](../PRIOR_ART.md) lists every influence by
milestone.

If you read only three: [*Crafting Interpreters*](#start-here) for building
a VM, [*The Garbage Collection Handbook*](#memory-and-garbage-collection-chapter-5)
for memory, and [Aycock's history of JITs](#jits-chapters-811) for what
came before chapter 8.

## Start here

- **Robert Nystrom, [*Crafting Interpreters*](https://craftinginterpreters.com/)**,
  2021. Free online. Builds a tree-walking interpreter, then a bytecode VM
  in C for the same language, Lox. Its second half covers chapters 1–6 for
  a stack machine, and the chapters link to it where they overlap. MLWSVM's
  Lox compiler compiles the book's Lox.
- **Thorsten Ball, [*Writing an Interpreter in Go*](https://interpreterbook.com/)**,
  2016, and **[*Writing a Compiler in Go*](https://compilerbook.com/)**, 2018.
  The second builds a stack VM and the compiler for it, test by test. It is
  shorter than *Crafting Interpreters*, and good on how a compiler and a VM
  meet.
- **Harold Abelson and Gerald Jay Sussman, [*Structure and Interpretation of
  Computer Programs*](https://mitp-content-server.mit.edu/books/content/sectbyfn/books_pres_0/6515/sicp.zip/index.html)**,
  2nd edition, 1996. Free online. Chapter 4 writes an interpreter. Chapter 5
  designs register machines, simulates them, and compiles Scheme to them:
  a register VM at its smallest, with its stack, its calls, and a garbage
  collector.
- **Niklaus Wirth, [*Compiler Construction*](https://people.inf.ethz.ch/wirth/CompilerConstruction/)**.
  Free from ETH. A complete compiler for a small language to a small RISC
  in one short book. The quickest way to see every part of a compiler at
  once.

## Compilers

MLWSVM is a target for compilers, and its JIT is one. These are the books
for the compiler side.

- **Alfred Aho, Monica Lam, Ravi Sethi and Jeffrey Ullman, [*Compilers:
  Principles, Techniques, and Tools*](https://suif.stanford.edu/dragonbook/)**,
  2nd edition, 2006 (the Dragon book). Worth rereading three chapters with
  this VM in mind: chapter 7, run-time environments (frames, the heap and
  garbage collection: chapters 5 and 6 here); chapter 8, code generation
  (chapter 10's register cache is a small instance of its register
  descriptors); and chapter 9's natural loops, which are how the JIT finds
  the loops it keeps registers across ([JIT.md](../JIT.md)).
- **Keith Cooper and Linda Torczon, [*Engineering a Compiler*](https://shop.elsevier.com/books/engineering-a-compiler/cooper/978-0-12-815412-0)**,
  3rd edition, 2022. The modern companion to the Dragon book, stronger on
  the back end: instruction selection, scheduling, and register allocation.
- **Andrew Appel, [*Modern Compiler Implementation in C*](https://www.cs.princeton.edu/~appel/modern/c/)**,
  1998 (also in ML and Java). One compiler built through the book. Its
  chapters on activation records and garbage collection are a good second
  view of chapters 5 and 6.
- **Randal Bryant and David O'Hallaron, [*Computer Systems: A Programmer's
  Perspective*](https://csapp.cs.cmu.edu/)**, 3rd edition, 2015. Chapter 3,
  machine-level programs, is the background for reading chapter 8's
  machine code; chapter 7 is linking; chapter 9 is virtual memory, which
  is how the JIT gets memory it can run.
- **John Levine, [*Linkers and Loaders*](https://www.iecc.com/linker/)**,
  1999. Object files, symbols, relocation and loading, for chapter 2's
  modules and `.mlwso` files.

## Virtual machines

- **James Smith and Ravi Nair, [*Virtual Machines: Versatile Platforms for
  Systems and Processes*](https://shop.elsevier.com/books/virtual-machines/smith/978-1-55860-910-5)**,
  2005. The survey of the whole field: emulation and binary translation,
  process VMs like this one, language VMs like the JVM, and system VMs.
- **Adele Goldberg and David Robson, *Smalltalk-80: The Language and its
  Implementation***, 1983 (the Blue Book). [Its implementation
  part](http://www.mirandabanda.org/bluebook/) is free online: one of the
  first complete specifications of a bytecode VM, its object memory and its
  interpreter, written so that you could build one from it.
- **Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes,
  [*The Implementation of Lua 5.0*](https://www.lua.org/doc/jucs05.pdf)**,
  2005. MLWSVM's closest relative: a register VM for a dynamic language,
  and why Lua left its stack machine for one. Short and clear.
- **Yunhe Shi, Kevin Casey, M. Anton Ertl and David Gregg, [*Virtual Machine
  Showdown: Stack Versus Registers*](https://doi.org/10.1145/1328195.1328197)**,
  ACM TACO, 2008. The measurements behind chapter 1's trade, which
  [REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md) repeats on this VM.
- **[*The Java Virtual Machine Specification*](https://docs.oracle.com/javase/specs/jvms/se21/html/index.html)**.
  Chapter 4 is the class file format and its verifier, chapter 2's ideas
  at full size; chapter 6 is the instruction set, a stack machine's.
- **[*WebAssembly Core Specification*](https://webassembly.github.io/spec/core/)**.
  A modern VM specified exactly: validation, which is chapter 2's verifier
  done as a type system, and a compact binary format.
- **Erik Stenman, [*The BEAM Book*](https://blog.stenmans.org/theBeamBook/)**.
  Free online. Erlang's VM from the inside: processes, their heaps, the
  scheduler, and the BEAM's own register machine.
- **Mike Pall, [*LuaJIT 2.0 intellectual property disclosure and research
  opportunities*](http://lua-users.org/lists/lua-l/2009-11/msg00089.html)**,
  2009. One mailing-list post listing the techniques behind LuaJIT, among
  the fastest dynamic-language VMs, from its interpreter written in
  assembly to its trace compiler. Each line is a research topic.

## Interpreters (chapters 3–4)

- **James Bell, [*Threaded Code*](https://doi.org/10.1145/362248.362270)**,
  CACM, 1973. The paper that named threaded code: dispatch by jumping from
  handler to handler.
- **M. Anton Ertl and David Gregg, [*The Structure and Performance of
  Efficient Interpreters*](https://www.jilp.org/vol5/v5paper12.pdf)**,
  JILP, 2003. Why dispatch is slow (branch prediction), and what threading
  and superinstructions do about it. The background for chapter 3.
- **Eli Bendersky, [*Computed goto for efficient dispatch
  tables*](https://eli.thegreenplace.net/2012/07/12/computed-goto-for-efficient-dispatch-tables)**,
  2012. The `switch` against computed `goto`, measured, in a page.
- **Josh Haberman, [*Parsing Protobuf at 2+GB/s: How I Learned To Love Tail
  Calls in C*](https://blog.reverberate.org/2021/04/21/musttail-efficient-interpreters.html)**,
  2021. The case for dispatch by tail calls, which is how MLWSVM's
  quickened forms dispatch (chapter 3).
- **Todd Proebsting, [*Optimizing an ANSI C Interpreter with
  Superoperators*](https://doi.org/10.1145/199448.199526)**, POPL 1995.
  Superinstructions chosen by analysing programs. Chapter 4's are put in
  the instruction set instead, for compilers to emit.
- **Stefan Brunthaler, [*Inline Caching Meets
  Quickening*](https://doi.org/10.1007/978-3-642-14107-2_21)**, ECOOP 2010,
  and **Mark Shannon, [PEP 659, *Specializing Adaptive
  Interpreter*](https://peps.python.org/pep-0659/)**, 2021. Rewriting
  instructions into faster forms as a program runs. MLWSVM does it once,
  at load time (chapter 4); CPython 3.11 does it while it runs.

## Memory and garbage collection (chapter 5)

- **Richard Jones, Antony Hosking and Eliot Moss, [*The Garbage Collection
  Handbook*](https://gchandbook.org/)**, 2nd edition, 2023. The reference:
  every kind of collector, with the trade-offs measured. Read its chapters
  on mark-sweep, mark-region and generational collection with chapter 5.
- **Paul Wilson, [*Uniprocessor Garbage Collection
  Techniques*](https://www.cs.cmu.edu/~fp/courses/15411-f14/misc/wilson94-gc.pdf)**,
  1992. Free. The classic survey, and a shorter way in than the Handbook.
- **David Bacon, Perry Cheng and V. T. Rajan, [*A Unified Theory of Garbage
  Collection*](https://doi.org/10.1145/1028976.1028982)**, OOPSLA 2004.
  Tracing and reference counting turn out to be two ends of one design.
  It changes how you see both.
- **David Ungar, [*Generation Scavenging*](https://doi.org/10.1145/800020.808261)**,
  1984, and **Alan Demers and others, [*Combining Generational and
  Conservative Garbage Collection*](https://doi.org/10.1145/96709.96735)**,
  POPL 1990. Generations, and the second one's sticky mark bits: a
  generational collector that moves nothing, which is MLWSVM's.
- **Stephen Blackburn and Kathryn McKinley, [*Immix: A Mark-Region Garbage
  Collector*](https://doi.org/10.1145/1375581.1375586)**, PLDI 2008
  ([free copy](https://www.steveblackburn.org/pubs/papers/immix-pldi-2008.pdf)).
  Lines, holes and bump allocation into them: chapter 5's heap, which is
  Immix without its evacuation.
- **Andy Wingo, [wingolog](https://wingolog.org/)**. A blog by a maintainer
  of Guile's VM, whose posts on Whippet, an Immix-family collector in C,
  walk through the same decisions chapter 5 makes, one post at a time.

## Calls, frames and coroutines (chapter 6)

- The Dragon book's chapter 7 and Appel's chapter on activation records,
  above.
- **Ana Lúcia de Moura and Roberto Ierusalimschy, [*Revisiting
  Coroutines*](https://doi.org/10.1145/1462166.1462167)**, ACM TOPLAS, 2009.
  What a coroutine is, and why asymmetric ones like Lua's, and MLWSVM's,
  are as powerful as any.

## Processes and the scheduler (chapter 7)

- **Joe Armstrong, [*Making Reliable Distributed Systems in the Presence of
  Software Errors*](https://erlang.org/download/armstrong_thesis_2003.pdf)**,
  PhD thesis, 2003. Free. Why Erlang's processes share nothing, and how
  links and supervisors build reliable systems from them. The idea behind
  chapter 7.
- **Robert Blumofe and Charles Leiserson, [*Scheduling Multithreaded
  Computations by Work Stealing*](https://doi.org/10.1145/324133.324234)**,
  JACM, 1999. Why an idle worker stealing from a busy one balances the
  load, proved.
- **Dmitry Vyukov, [*Scalable Go Scheduler Design
  Doc*](https://docs.google.com/document/d/1TTj4T2JO42uD5ID9e89oa0sLKhJYD0Y_kqxDv3I3XMw/edit)**,
  2012. A real scheduler's design in a few pages: per-worker queues, work
  stealing, and waking idle workers, all of which chapter 7's scheduler
  does.

## JITs (chapters 8–11)

- **John Aycock, [*A Brief History of Just-In-Time*](https://doi.org/10.1145/857076.857077)**,
  ACM Computing Surveys, 2003. Forty years of JITs from Lisp onwards, in
  one survey. The best first paper on the subject.
- **Eli Bendersky, [*How to JIT: an
  introduction*](https://eli.thegreenplace.net/2013/11/05/how-to-jit-an-introduction)**,
  2013. Write machine code into memory and call it, in C, in a page:
  chapter 8's first step on its own.
- **L. Peter Deutsch and Allan Schiffman, [*Efficient Implementation of the
  Smalltalk-80 System*](https://doi.org/10.1145/800017.800542)**, POPL 1984.
  The first JIT for a dynamic language, and the first inline caches.
- **Urs Hölzle, Craig Chambers and David Ungar, [*Optimizing
  Dynamically-Typed Object-Oriented Languages with Polymorphic Inline
  Caches*](https://doi.org/10.1007/BFb0057013)**, ECOOP 1991
  ([free copy](https://bibliography.selflanguage.org/_static/pics.pdf)).
  Inline caches that remember more than one type. Chapter 10's closure
  call compiled for its callee is a one-entry inline cache.
- **Urs Hölzle, Craig Chambers and David Ungar, [*Debugging Optimized Code
  with Dynamic Deoptimization*](https://doi.org/10.1145/143095.143114)**,
  PLDI 1992. How an optimizing JIT goes back to the interpreter when a guess
  is wrong. Chapter 9's side exits are the simple case: the machine code
  writes its registers back before it leaves, so the interpreter carries on
  with nothing to rebuild.
- **Vasanth Bala, Evelyn Duesterwald and Sanjeev Banerjia, [*Dynamo: A
  Transparent Dynamic Optimization System*](https://doi.org/10.1145/349299.349303)**,
  PLDI 2000, and **Andreas Gal and others, [*Trace-based Just-in-Time Type
  Specialization for Dynamic Languages*](https://doi.org/10.1145/1542476.1542528)**,
  PLDI 2009. Trace compilation: compile the path a program takes, not the
  function. LuaJIT is the best-known trace JIT; chapter 10's blocks reached
  by falling through take a little of the idea.
- **Massimiliano Poletto and Vivek Sarkar, [*Linear Scan Register
  Allocation*](https://doi.org/10.1145/330249.330250)**, ACM TOPLAS, 1999.
  The register allocator many JITs use. Chapter 10's register cache is
  simpler, and chapter 11 measures what keeping registers longer would
  gain.
- **Haoran Xu and Fredrik Kjolstad, [*Copy-and-Patch
  Compilation*](https://doi.org/10.1145/3485513)**, OOPSLA 2021
  ([free copy](https://fredrikbk.com/publications/copy-and-patch.pdf)), and
  **Brandt Bucher, [PEP 744, *JIT Compilation*](https://peps.python.org/pep-0744/)**,
  2024. Another way to build a first JIT: let the C compiler make each
  instruction's machine code, and paste the pieces together. CPython 3.13's
  JIT works this way.
- **Leszek Swirski, [*Sparkplug — a non-optimizing
  JavaScript compiler*](https://v8.dev/blog/sparkplug)**, V8 blog, 2021, and
  **John Högberg, [*A first look at the JIT*](https://www.erlang.org/blog/a-first-look-at-the-jit/)**,
  Erlang/OTP blog, 2020. Two baseline JITs, V8's and Erlang's, that
  translate one instruction at a time as MLWSVM's does. Neither keeps
  values in registers between instructions, which chapter 10's cache does.
- **Agner Fog, [*Software optimization resources*](https://www.agner.org/optimize/)**.
  Free. Instruction timings and how each x86 processor runs them, and the
  reason the same machine code runs at different speeds on chapter 11's
  three machines.
- **The [*Arm Architecture Reference Manual*](https://developer.arm.com/documentation/ddi0487/latest)**
  and **the [*Intel 64 and IA-32 Architectures Software Developer's
  Manuals*](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)**.
  Where every instruction's encoding in chapter 8 comes from.
- **The [Valgrind callgrind manual](https://valgrind.org/docs/manual/cl-manual.html)**,
  for chapter 11's instruction counts.

## Testing (chapters 11–12)

- **Xuejun Yang, Yang Chen, Eric Eide and John Regehr, [*Finding and
  Understanding Bugs in C Compilers*](https://doi.org/10.1145/1993498.1993532)**,
  PLDI 2011 ([free copy](https://www.cs.utah.edu/~regehr/papers/pldi11-preprint.pdf)).
  Csmith: random programs, compiled several ways, results compared. The
  idea behind `mlwsvm fuzz`, which runs each program in several modes.
- **Vsevolod Livinskii, Dmitry Babokin and John Regehr, [*Random Testing for
  C and C++ Compilers with YARPGen*](https://doi.org/10.1145/3428264)**,
  OOPSLA 2020. Generating random programs that reach a compiler's
  optimizations, not only its front end.
- **Andreas Zeller and Ralf Hildebrandt, [*Simplifying and Isolating
  Failure-Inducing Input*](https://doi.org/10.1109/32.988498)**, IEEE TSE,
  2002, and **John Regehr and others, [*Test-Case Reduction for C Compiler
  Bugs*](https://doi.org/10.1145/2254064.2254104)**, PLDI 2012. Shrinking a
  failing program to the few lines that matter, as the fuzzer's reducer
  does.
- **Samuel Groß, [Fuzzilli](https://github.com/googleprojectzero/fuzzilli)**.
  A fuzzer for JavaScript engines that builds its programs valid, as
  `mlwsvm fuzz` does, so none is wasted on a syntax error.
- **Yue Jia and Mark Harman, [*An Analysis and Survey of the Development of
  Mutation Testing*](https://doi.org/10.1109/TSE.2010.62)**, IEEE TSE, 2011.
  Testing the tests by breaking the code on purpose, as chapter 12 does.

Back to [the chapters](README.md).
