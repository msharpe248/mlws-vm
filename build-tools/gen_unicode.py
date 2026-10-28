#!/usr/bin/env python3
"""gen_unicode.py UCD_DIR - generate MLWSVM's Unicode tables and conformance tests.

Reads the Unicode Character Database (the unpacked UCD.zip of one version) and
writes:

  src/core/unicode_tables.inc        the tables src/core/unicode.c looks up
  tests/text/_unicode/norm_data.inc  NormalizationTest.txt cases, for tests/text/unicode_normalization.masm
  tests/text/_unicode/break_data.inc GraphemeBreakTest.txt and WordBreakTest.txt cases, for unicode_breaks.masm

Run it from the repository root (`make unicode UCD=dir`). Only the Python
standard library is used. The outputs are committed, so building MLWSVM needs
neither this script nor the UCD.

Every code point maps, through a two-stage table (blocks of 128), to one of a
few thousand distinct property records: general category, combining class,
grapheme and word break classes, Indic conjunct break, display width, decimal
digit value, property flags and the simple case mappings as deltas. The full
(multi-character) case mappings, the decompositions and the canonical
compositions are sorted tables, searched by code point.
"""
import os
import re
import sys

UCD = sys.argv[1] if len(sys.argv) > 1 else None
if not UCD or not os.path.isfile(os.path.join(UCD, "UnicodeData.txt")):
    sys.exit("usage: gen_unicode.py UCD_DIR (the directory holding UnicodeData.txt)")

MAXCP = 0x110000


def path(*p):
    return os.path.join(UCD, *p)


def lines(name):
    with open(path(name), encoding="utf-8") as f:
        for ln in f:
            ln = ln.split("#", 1)[0].strip()
            if ln:
                yield [x.strip() for x in ln.split(";")]


def cprange(s):
    if ".." in s:
        a, b = s.split("..")
        return range(int(a, 16), int(b, 16) + 1)
    return range(int(s, 16), int(s, 16) + 1)


def version():
    with open(path("ReadMe.txt"), encoding="utf-8") as f:
        m = re.search(r"Version (\d+\.\d+\.\d+)", f.read())
    return m.group(1)


# ---- UnicodeData.txt -------------------------------------------------------------

GC_NAMES = ["Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe",
            "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co"]
gc = [0] * MAXCP
ccc = [0] * MAXCP
digit = [-1] * MAXCP
supper = list(range(MAXCP))
slower = list(range(MAXCP))
stitle = list(range(MAXCP))
decomp = {}  # cp -> (compat, [cps])
first = None
for f in lines("UnicodeData.txt"):
    cp = int(f[0], 16)
    if f[1].endswith(", First>"):
        first = cp
        continue
    rng = range(first, cp + 1) if f[1].endswith(", Last>") else range(cp, cp + 1)
    first = None
    for c in rng:
        gc[c] = GC_NAMES.index(f[2])
        ccc[c] = int(f[3])
        if f[2] == "Nd":
            digit[c] = int(f[6])
        if f[12]:
            supper[c] = int(f[12], 16)
        if f[13]:
            slower[c] = int(f[13], 16)
        stitle[c] = int(f[14], 16) if f[14] else supper[c]
    if f[5]:
        d = f[5].split()
        compat = d[0].startswith("<")
        if compat:
            d = d[1:]
        decomp[cp] = (compat, [int(x, 16) for x in d])


# ---- binary properties ----------------------------------------------------------

F_ALPHA, F_WHITE, F_UPPER, F_LOWER, F_XIDS, F_XIDC, F_EXTPICT, F_CASED, F_CASEIGN, F_DEFIGN, F_EMOJIPRES, \
    F_SPECIAL = [1 << i for i in range(12)]
flags = [0] * MAXCP


def setflag(fname, prop, bit):
    for f in lines(fname):
        if len(f) >= 2 and f[1] == prop:
            for c in cprange(f[0]):
                flags[c] |= bit


setflag("DerivedCoreProperties.txt", "Alphabetic", F_ALPHA)
setflag("PropList.txt", "White_Space", F_WHITE)
setflag("DerivedCoreProperties.txt", "Uppercase", F_UPPER)
setflag("DerivedCoreProperties.txt", "Lowercase", F_LOWER)
setflag("DerivedCoreProperties.txt", "XID_Start", F_XIDS)
setflag("DerivedCoreProperties.txt", "XID_Continue", F_XIDC)
setflag(os.path.join("emoji", "emoji-data.txt"), "Extended_Pictographic", F_EXTPICT)
setflag(os.path.join("emoji", "emoji-data.txt"), "Emoji_Presentation", F_EMOJIPRES)
setflag("DerivedCoreProperties.txt", "Cased", F_CASED)
setflag("DerivedCoreProperties.txt", "Case_Ignorable", F_CASEIGN)
setflag("DerivedCoreProperties.txt", "Default_Ignorable_Code_Point", F_DEFIGN)

INCB_NAMES = ["None", "Linker", "Consonant", "Extend"]
incb = [0] * MAXCP
for f in lines("DerivedCoreProperties.txt"):
    if len(f) >= 3 and f[1] == "InCB":
        for c in cprange(f[0]):
            incb[c] = INCB_NAMES.index(f[2])

GCB_NAMES = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend", "SpacingMark", "L",
             "V", "T", "LV", "LVT"]
gcb = [0] * MAXCP
for f in lines(os.path.join("auxiliary", "GraphemeBreakProperty.txt")):
    for c in cprange(f[0]):
        gcb[c] = GCB_NAMES.index(f[1])

WB_NAMES = ["Other", "CR", "LF", "Newline", "Extend", "ZWJ", "Regional_Indicator", "Format", "Katakana",
            "Hebrew_Letter", "ALetter", "Single_Quote", "Double_Quote", "MidNumLet", "MidLetter", "MidNum", "Numeric",
            "ExtendNumLet", "WSegSpace"]
wb = [0] * MAXCP
for f in lines(os.path.join("auxiliary", "WordBreakProperty.txt")):
    for c in cprange(f[0]):
        wb[c] = WB_NAMES.index(f[1])

# Display width (UAX #11 East_Asian_Width, as terminals use it): 2 for Wide and
# Fullwidth, 0 for marks, format characters, default ignorables and the Hangul
# medial vowels and final consonants (they join the syllable before them), 1
# otherwise. Unassigned code points in the wide blocks are listed as W too.
eaw = {}
for f in lines("EastAsianWidth.txt"):
    for c in cprange(f[0]):
        eaw[c] = f[1]
width = [1] * MAXCP
for c in range(MAXCP):
    if eaw.get(c) in ("W", "F"):
        width[c] = 2
    g = GC_NAMES[gc[c]]
    if g in ("Mn", "Me", "Cf") or (flags[c] & F_DEFIGN) or 0x1160 <= c <= 0x11FF or 0xD7B0 <= c <= 0xD7FF:
        width[c] = 0
    if g in ("Cc", "Cs", "Zl", "Zp"):
        width[c] = 0
width[0x00AD] = 1  # soft hyphen: shown where it breaks a line, and counted by most terminals

# ---- case: special casing and folding ---------------------------------------

full_upper, full_lower, full_title, full_fold = {}, {}, {}, {}
for f in lines("SpecialCasing.txt"):
    if len(f) >= 5 and f[4]:  # conditional (language or context): Final_Sigma is coded by hand
        continue
    cp = int(f[0], 16)
    lo, ti, up = ([int(x, 16) for x in f[i].split()] for i in (1, 2, 3))
    if lo != [cp] and lo != [slower[cp]]:
        full_lower[cp] = lo
    if ti != [cp] and ti != [stitle[cp]]:
        full_title[cp] = ti
    if up != [cp] and up != [supper[cp]]:
        full_upper[cp] = up
sfold = list(range(MAXCP))
for f in lines("CaseFolding.txt"):
    cp, status, m = int(f[0], 16), f[1], [int(x, 16) for x in f[2].split()]
    if status in ("C", "S"):
        sfold[cp] = m[0]
    elif status == "F":
        full_fold[cp] = m
for t in (full_upper, full_lower, full_title, full_fold):
    for cp in t:
        flags[cp] |= F_SPECIAL

# ---- normalization ----------------------------------------------------------

excl = set()
for f in lines("DerivedNormalizationProps.txt"):
    if len(f) >= 2 and f[1] == "Full_Composition_Exclusion":
        for c in cprange(f[0]):
            excl.add(c)


def full_decomp(cp, compat):
    d = decomp.get(cp)
    if not d or (d[0] and not compat):
        return [cp]
    out = []
    for c in d[1]:
        out.extend(full_decomp(c, compat))
    return out


canon = {cp: full_decomp(cp, False) for cp in decomp if not decomp[cp][0]}
compatd = {}
for cp in decomp:
    fd = full_decomp(cp, True)
    if fd != canon.get(cp, [cp]):
        compatd[cp] = fd
compose = {}
for cp, (compat, d) in decomp.items():
    if not compat and len(d) == 2 and cp not in excl:
        compose[(d[0], d[1])] = cp

# ---- records and the two-stage table -------------------------------------------

BLOCK = 128


def delta(a, b):
    return b - a


records, recidx = [], {}
cp_rec = [0] * MAXCP
for c in range(MAXCP):
    r = (gc[c], ccc[c], gcb[c], wb[c], incb[c], width[c], digit[c], flags[c], delta(c, supper[c]),
         delta(c, slower[c]), delta(c, stitle[c]), delta(c, sfold[c]))
    i = recidx.get(r)
    if i is None:
        i = recidx[r] = len(records)
        records.append(r)
    cp_rec[c] = i
blocks, blockidx, stage1 = [], {}, []
for b in range(0, MAXCP, BLOCK):
    blk = tuple(cp_rec[b:b + BLOCK])
    i = blockidx.get(blk)
    if i is None:
        i = blockidx[blk] = len(blocks)
        blocks.append(blk)
    stage1.append(i)
assert len(records) < 65536 and len(blocks) < 65536


def seq_table(name, table):
    """A sorted {cp, offset, length} index into one pool of code points."""
    pool, idx = [], []
    for cp in sorted(table):
        idx.append((cp, len(pool), len(table[cp])))
        pool.extend(table[cp])
    return idx, pool


def c_array(out, ctype, name, values, per=16):
    out.append(f"static const {ctype} {name}[{len(values)}] = {{")
    for i in range(0, len(values), per):
        out.append("    " + ", ".join(str(v) for v in values[i:i + per]) + ",")
    out.append("};")


out = ["/* unicode_tables.inc - GENERATED by build-tools/gen_unicode.py from the Unicode",
       f" * Character Database {version()}. Do not edit; run `make unicode UCD=dir`. */",
       f'#define UNI_VERSION "{version()}"',
       f"#define UNI_BLOCK {BLOCK}",
       ""]
for i, n in enumerate(GC_NAMES):
    out.append(f"#define UNI_GC_{n} {i}")
for i, n in enumerate(GCB_NAMES):
    out.append(f"#define UNI_GCB_{n.upper()} {i}")
for i, n in enumerate(WB_NAMES):
    out.append(f"#define UNI_WB_{n.upper()} {i}")
for i, n in enumerate(INCB_NAMES):
    out.append(f"#define UNI_INCB_{n.upper()} {i}")
for n, v in [("ALPHA", F_ALPHA), ("WHITE", F_WHITE), ("UPPER", F_UPPER), ("LOWER", F_LOWER), ("XIDS", F_XIDS),
             ("XIDC", F_XIDC), ("EXTPICT", F_EXTPICT), ("CASED", F_CASED), ("CASEIGN", F_CASEIGN),
             ("DEFIGN", F_DEFIGN), ("EMOJIPRES", F_EMOJIPRES), ("SPECIAL", F_SPECIAL)]:
    out.append(f"#define UNI_F_{n} {v}")
out.append("")
out.append("static const char *const UNI_GC_NAMES[] = {" + ", ".join(f'"{n}"' for n in GC_NAMES) + "};")
out.append("")
out.append("static const UniRecord UNI_RECORDS[] = {")
for r in records:
    out.append("    {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}," % r)
out.append("};")
c_array(out, "uint16_t", "UNI_STAGE1", stage1)
flat = [v for blk in blocks for v in blk]
c_array(out, "uint16_t", "UNI_STAGE2", flat)
for name, table in (("UPPER", full_upper), ("LOWER", full_lower), ("TITLE", full_title), ("FOLD", full_fold),
                    ("CANON", canon), ("COMPAT", compatd)):
    idx, pool = seq_table(name, table)
    out.append(f"static const UniSeq UNI_{name}_IDX[] = {{")
    for cp, off, n in idx:
        out.append(f"    {{0x{cp:X}, {off}, {n}}},")
    out.append("};")
    c_array(out, "uint32_t", f"UNI_{name}_POOL", pool or [0])
out.append("static const UniPair UNI_COMPOSE[] = {")
for (a, b), c in sorted(compose.items()):
    out.append(f"    {{0x{a:X}, 0x{b:X}, 0x{c:X}}},")
out.append("};")
with open("src/core/unicode_tables.inc", "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")

# ---- conformance test data ----------------------------------------------------
#
# Each case is a line of a string constant the tests split: fields separated by
# ';', code points as hex separated by spaces.


def masm_string_lines(cases, name, comment):
    out = [f"; {comment}", f"; GENERATED by build-tools/gen_unicode.py from Unicode {version()}; do not edit.",
           f".macro {name} dst", f"    lnew \\dst"]
    for c in cases:
        out.append(f'    lpush \\dst, "{c}"')
    out.append(".endm")
    return out


norm = []
part = None
for ln in open(path("NormalizationTest.txt"), encoding="utf-8"):
    ln = ln.split("#", 1)[0].strip()
    if not ln:
        continue
    if ln.startswith("@Part"):
        part = int(ln[5:])
        n_in_part = 0
        continue
    n_in_part += 1
    # Part 1 tests every character on its own (17,000 lines): one in 25 is kept
    if part == 1 and n_in_part % 25:
        continue
    f = [x.strip() for x in ln.split(";")][:5]
    norm.append(";".join(f))
os.makedirs("tests/text/_unicode", exist_ok=True)
with open("tests/text/_unicode/norm_data.inc", "w", encoding="utf-8") as f:
    f.write("\n".join(masm_string_lines(
        norm, "NORM_CASES", "NormalizationTest.txt: source;NFC;NFD;NFKC;NFKD, code points in hex")) + "\n")


def break_cases(fname):
    cases = []
    for ln in open(path("auxiliary", fname), encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        toks = ln.split()
        cps, bounds, k = [], [], 0
        for t in toks:
            if t == "÷":
                bounds.append(k)
            elif t == "×":
                pass
            else:
                cps.append(t)
                k += 1
        # the boundaries inside the string (not 0 and the end), as code point indices
        inner = [str(b) for b in bounds if 0 < b < k]
        cases.append(" ".join(cps) + ";" + " ".join(inner))
    return cases


with open("tests/text/_unicode/break_data.inc", "w", encoding="utf-8") as f:
    f.write("\n".join(masm_string_lines(break_cases("GraphemeBreakTest.txt"), "GRAPHEME_CASES",
                                        "GraphemeBreakTest.txt: code points;boundaries (code point indices)")) + "\n")
    f.write("\n".join(masm_string_lines(break_cases("WordBreakTest.txt"), "WORD_CASES",
                                        "WordBreakTest.txt: code points;boundaries (code point indices)")) + "\n")

print(f"Unicode {version()}: {len(records)} records, {len(blocks)} blocks of {BLOCK}, "
      f"{len(canon)} canonical and {len(compatd)} compatibility decompositions, {len(compose)} compositions, "
      f"{len(norm)} normalization cases")
