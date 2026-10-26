#!/usr/bin/env python3
"""compare.py - run bench/fib.masm and bench/loop.masm against the same programs
in Lua, on the JVM (interpreter only, and with its JIT) and in CPython, and count
the instructions each VM executes. The results are in docs/REGISTER_VS_STACK.md.

Usage: python3 bench/compare/compare.py [runs]       (default: best of 5)

Needs lua and a built ./mlwsvm; java/javac (JAVA_HOME, PATH, or Homebrew's
openjdk) is optional, and the JVM rows are skipped without it. Times are wall-clock milliseconds, minus the same VM's start-up time
(a run with n = 1, or an empty MLWSVM program). The JVM's counts are not
measured: a normal JVM build cannot count bytecodes, so they are counted along
the paths of `javap -c` output in the document instead.
"""
import os, re, shutil, subprocess, sys, tempfile, time

sys.dont_write_bytecode = True  # importing fib.py and loop.py must not leave __pycache__ behind

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 5


def find_java():
    for d in (os.path.join(os.environ.get("JAVA_HOME", ""), "bin"), "/opt/homebrew/opt/openjdk/bin"):
        if os.path.exists(os.path.join(d, "javac")):
            return d
    javac = shutil.which("javac")
    return os.path.dirname(os.path.realpath(javac)) if javac else None


def best(cmd, cwd=HERE):
    b, out = 1e9, ""
    for _ in range(RUNS):
        t = time.perf_counter()
        out = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()
        b = min(b, time.perf_counter() - t)
    return b * 1000, out


def main():
    mlw = os.path.join(ROOT, "mlwsvm")
    lua = os.environ.get("LUA", "lua")
    java = find_java()
    classes = tempfile.mkdtemp()
    if java:
        subprocess.run([os.path.join(java, "javac"), "-d", classes, "Fib.java", "Loop.java"], cwd=HERE, check=True)
    empty = os.path.join(classes, "empty.masm")
    with open(empty, "w") as f:
        f.write(".func main\n    ret 0\n.endf\n")
    jv = [os.path.join(java or "", "java"), "-cp", classes]
    vms = [  # name, fib command, loop command, start-up command
        ("MLWSVM", [mlw, "run", "--threads", "1", f"{ROOT}/bench/fib.masm"],
         [mlw, "run", "--threads", "1", f"{ROOT}/bench/loop.masm"], [mlw, "run", "--threads", "1", empty]),
        ("Lua", [lua, "fib.lua"], [lua, "loop.lua"], [lua, "fib.lua", "1"]),
        ("JVM -Xint", jv[:1] + ["-Xint"] + jv[1:] + ["Fib"], jv[:1] + ["-Xint"] + jv[1:] + ["Loop"],
         jv[:1] + ["-Xint"] + jv[1:] + ["Fib", "1"]),
        ("JVM (JIT)", jv + ["Fib"], jv + ["Loop"], jv + ["Fib", "1"]),
        ("CPython", [sys.executable, "fib.py"], [sys.executable, "loop.py"], [sys.executable, "fib.py", "1"]),
    ]
    if not java:
        vms = [v for v in vms if not v[0].startswith("JVM")]
    print(f"Wall time in ms, best of {RUNS}, start-up subtracted")
    for name, fib, loop, start in vms:
        s, _ = best(start)
        tf, of = best(fib)
        tl, ol = best(loop)
        assert of == "2178309" and ol == "60000255", (name, of, ol)
        print(f"  {name:10} fib {tf - s:7.0f}   loop {tl - s:7.0f}   (start-up {s:.0f})")

    print("Instructions executed: per fib call, per loop iteration")
    calls = 7049155  # fib(32)
    st = subprocess.run([mlw, "run", "--stats", f"{ROOT}/bench/fib.masm"], capture_output=True, text=True).stderr
    fib_n = int(re.search(r"all instructions: *(\d+)", st).group(1))
    st = subprocess.run([mlw, "run", "--stats", f"{ROOT}/bench/loop.masm"], capture_output=True, text=True).stderr
    loop_n = int(re.search(r"all instructions: *(\d+)", st).group(1))
    print(f"  MLWSVM     fib {fib_n / calls:.2f}   loop {loop_n / 30e6:.2f}")
    out = subprocess.run([lua, "count.lua"], cwd=HERE, capture_output=True, text=True).stdout.split()
    print(f"  Lua        fib {float(out[1]):.2f}   loop {float(out[3]):.2f}")
    print(f"  CPython    fib {cpython_count('fib', 20, 21891):.2f}   loop {cpython_count('loop', 100000, 100000):.2f}")
    shutil.rmtree(classes)


def cpython_count(name, arg, units):
    """Count the bytecodes CPython executes, with sys.monitoring INSTRUCTION events."""
    sys.path.insert(0, HERE)
    fn = getattr(__import__(name), name)
    M = sys.monitoring
    n = 0

    def on_insn(code, offset):
        nonlocal n
        n += 1

    M.use_tool_id(0, "count")
    M.register_callback(0, M.events.INSTRUCTION, on_insn)
    M.set_local_events(0, fn.__code__, M.events.INSTRUCTION)
    fn(arg)
    M.set_local_events(0, fn.__code__, 0)
    M.free_tool_id(0)
    return n / units


if __name__ == "__main__":
    main()
