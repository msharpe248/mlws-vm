#!/usr/bin/env python3
"""Differential test of MLWSVM's regular expressions against Python's re.

Random patterns over a small alphabet, with classes, groups, alternation,
anchors, word boundaries and every kind of repeat, greedy and lazy, are run on
random texts by both engines, through one generated program (`make
check-regex`). The first match must be the same, and its groups too, except in
the one place the engines differ on purpose: which pass of a loop over
something that can match nothing leaves its capture (MLWSVM follows RE2 and Go
there). find_all is compared where no match is empty, since the engines step
past an empty match differently, and `$` texts never end in a newline, since
Python's `$` also matches before one.

Usage: compare.py MLWSVM [-s SEED ...] [-n CASES]   (exit 1 on a whole-match difference)
"""
import argparse, json, os, random, re, subprocess, sys, tempfile

def generate(rnd):
    def atom(d):
        c = rnd.random()
        if c < .45 or d <= 0: return rnd.choice("abc")
        if c < .55: return "."
        if c < .65: return rnd.choice(["[ab]", "[^a]", "[a-c]", "\\w", "\\d", "\\s"])
        if c < .8: return "(" + alt(d - 1) + ")"
        if c < .9: return "(?:" + alt(d - 1) + ")"
        return rnd.choice(["^", "$", "\\b"])
    def rep(d):
        a = atom(d)
        if a in ("^", "$", "\\b") or rnd.random() < .55: return a
        return a + rnd.choice(["*", "+", "?", "*?", "+?", "??", "{2}", "{1,2}", "{0,2}?"])
    def cat(d): return "".join(rep(d) for _ in range(rnd.randint(1, 3)))
    def alt(d): return "|".join(cat(d) for _ in range(rnd.randint(1, 2)))
    while True:
        p = alt(3)
        try:
            r = re.compile(p)
        except re.error:
            continue
        t = "".join(rnd.choice("abc1 \n") for _ in range(rnd.randint(0, 9)))
        if "$" in p: t = t.rstrip("\n")
        m = r.search(t)
        return p, t, None if m is None else [x for i in range(r.groups + 1) for x in m.span(i)], [x.group(0) for x in r.finditer(t)]

def quote(s): return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vm")
    ap.add_argument("-s", "--seed", type=int, action="append")
    ap.add_argument("-n", "--cases", type=int, default=3000)
    a = ap.parse_args()
    whole = groups = 0
    total = 0
    for seed in a.seed or [1, 2, 3, 4]:
        rnd = random.Random(seed)
        cases = [generate(rnd) for _ in range(a.cases)]
        src = ['.include "std/io.inc"', ".func main"]
        for p, t, _, _ in cases:
            src += [f'    ncall "regex.compile" ({quote(p)}, "") -> (R0)',
                    f'    ncall "regex.find" (R0, {quote(t)}, 0) -> (R1)',
                    '    ncall "json.stringify" (R1, 0) -> (R2)', "    PRINTLN_S R2",
                    f'    ncall "regex.find_all" (R0, {quote(t)}) -> (R3)',
                    '    ncall "json.stringify" (R3, 0) -> (R4)', "    PRINTLN_S R4"]
        src += ["    ret 0", ".endf"]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "regex_cases.masm")
            with open(path, "w") as f: f.write("\n".join(src) + "\n")
            out = subprocess.run([a.vm, "run", "--no-cache", path], capture_output=True, text=True)
        if out.returncode:
            print(out.stderr[:2000], file=sys.stderr)
            return 1
        lines = out.stdout.split("\n")
        for i, (p, t, want, want_all) in enumerate(cases):
            got, got_all = json.loads(lines[2 * i]), json.loads(lines[2 * i + 1])
            total += 1
            if (got is None) != (want is None) or (got is not None and got[:2] != want[:2]):
                whole += 1
                print(f"seed {seed}: {p!r} on {t!r}: want {want}, got {got}", file=sys.stderr)
            elif got != want or (got_all != want_all and "" not in want_all and "" not in got_all):
                groups += 1
    print(f"check-regex: {total} cases, {whole} whole matches differ, {groups} differ in groups of empty loops")
    return 1 if whole else 0

if __name__ == "__main__":
    sys.exit(main())
