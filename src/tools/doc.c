/* doc.c - `mlwsvm doc isa|natives`: generate reference documentation from the
 * opcode table and the native registry so the docs never drift from the code. */
#include "tools.h"

static void pattern_str(StrBuf *b, const OpInfo *oi) {
    if (oi->flags & OPF_CALL) {
        int op = (int)(oi - op_info);
        if (op == OP_NCALL) sb_puts(b, "\"native\" (args...) -> (results...)");
        else if (op == OP_TCALL) sb_puts(b, "func (args...)");
        else if (op == OP_TCALLI) sb_puts(b, "closure (args...)");
        else if (op == OP_CORESUME) sb_puts(b, "coroutine (args...) -> (results...)");
        else if (op == OP_COYIELD) sb_puts(b, "(values...) -> (results...)");
        else if (op_calls_closure(op)) sb_puts(b, "closure (args...) -> (results...)");
        else sb_puts(b, "func (args...) -> (results...)");
        return;
    }
    static const struct {
        char c;
        const char *name;
    } names[] = {
        {'D', "dst"}, {'S', "src"}, {'V', "val"}, {'W', "dst"}, {'B', "bool_dst"}, {'Q', "int"}, {'A', "ptr"},
        {'E', "ptr_dst"}, {'M', "mem"}, {'K', "\"msg\""}, {'I', "n"}, {'L', "label"}, {'F', "func"}, {'N', "\"native\""},
    };
    for (int i = 0; i < oi->nelems; i++) {
        const char *n = "?";
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
            if (names[k].c == oi->elems[i]) n = names[k].name;
        if (i) sb_puts(b, ", ");
        bool last = i == oi->nelems - 1;
        if (last && oi->last_mod == '?') sb_printf(b, "[%s]", n);
        else if (last && oi->last_mod == '*') sb_printf(b, "[%s, ...]", n);
        else if (last && oi->last_mod == '+') sb_printf(b, "%s, ...", n);
        else sb_puts(b, n);
    }
    if (oi->nelems == 0) sb_puts(b, "-");
}

static void types_str(StrBuf *b, uint32_t set) {
    if (!set) {
        sb_puts(b, "-");
        return;
    }
    bool first = true;
#define PART(s)                                                                \
    do {                                                                       \
        if (!first) sb_puts(b, " ");                                           \
        sb_puts(b, s);                                                         \
        first = false;                                                         \
    } while (0)
    if ((set & TS_INT) == TS_INT) PART("int");
    else
        for (int t = T_B; t <= T_U64; t++)
            if (set & TM(t)) PART(type_name((MlwsType)t));
    if ((set & TS_FLT) == TS_FLT) PART("float");
    else if (set & TS_FLT) PART(set & TM(T_F32) ? "f32" : "f64");
    if (set & TS_REF) PART("r");
    if (set & TS_PTR) PART("p");
#undef PART
}

static void gen_isa(StrBuf *b) {
    sb_puts(b, "# MLWSVM Instruction Set Reference\n\n"
               "> Generated from `src/core/opcodes.def` by `mlwsvm doc isa`. Do not edit by hand;\n"
               "> change the opcode table and regenerate with `make docs`.\n\n"
               "Every instruction is written `mnemonic[.type[.type2]] operands`. See\n"
               "[ASSEMBLY.md](ASSEMBLY.md) for syntax, [ADDRESSING.md](ADDRESSING.md) for operand forms and\n"
               "[ABI.md](ABI.md) for calls and the stack.\n\n"
               "**Operand names.** `dst` is a writable location (a register or memory). `src` and `val`\n"
               "are any value operand (register, literal, string or memory). `int` is read as `i64`,\n"
               "`ptr` is read as a pointer (`p`), and `bool_dst` is written as `b`. `mem` must be a memory\n"
               "operand, `n` is a non-negative integer constant, and `[x]` marks an optional operand.\n\n"
               "**Types column.** This lists the type suffixes the instruction accepts. `int` covers\n"
               "`b c i8 i16 i32 i64 u8 u16 u32 u64` and `float` covers `f32 f64`. Without a suffix the\n"
               "type is inferred from the first register operand: I gives `i64`, F `f64`, R `r`, P/SP/FP `p`.\n"
               "If there is no register operand, the first literal decides, and `i64` is the fallback.\n"
               "`mov`, `swap`, `push`, `pop` and `ret` without a suffix use each operand's natural type,\n"
               "which preserves the type tag of a stack or global word.\n\n"
               "**Two-operand shorthand.** For arithmetic and bitwise instructions, `op dst, src` means\n"
               "`dst = dst op src`. With three or more operands, `dst = s1 op s2 op ... op sN`, so `add`, `mul`,\n"
               "`min`, `max`, `and`, `or` and `xor` take up to 31 sources.\n\n");
    /* the codes tagof reports (and memory word tags hold) */
    sb_puts(b, "## Types\n\nType codes, as `tagof` reports them: 0 for a slot never written");
    for (int t = 1; t < T__COUNT; t++) sb_printf(b, ", %d `%s`", t, type_name((MlwsType)t));
    sb_printf(b, ", and %d for raw bytes (written partly, unaligned, or by `memcpy`).\n\n", TAG_RAW);
    const char *cat = NULL;
    for (int i = 0; i < OP__COUNT; i++) {
        const OpInfo *oi = &op_info[i];
        if (!cat || strcmp(cat, oi->category) != 0) {
            cat = oi->category;
            sb_printf(b, "\n## %s\n\n| Opcode | Mnemonic | Operands | Types | Description |\n|---|---|---|---|---|\n", cat);
        }
        StrBuf pat = {0}, ty = {0};
        pattern_str(&pat, oi);
        types_str(&ty, oi->types);
        if (oi->flags & OPF_TYPE2) sb_puts(&ty, " (to.from)");
        sb_printf(b, "| `0x%04X` | `%s` | `%s` | %s | ", oi->code, oi->mnem, pat.s, ty.s);
        for (const char *d = oi->doc; *d; d++) {
            if (*d == '|') sb_puts(b, "\\|");
            else sb_putc(b, *d);
        }
        sb_puts(b, " |\n");
        sb_free(&pat);
        sb_free(&ty);
    }
}

static void native_rows(StrBuf *b, const NativeDef *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sb_printf(b, "| `%s` | ", t[i].name);
        for (int k = 0; k < t[i].np; k++) sb_printf(b, "%s%s", k ? ", " : "", type_name((MlwsType)t[i].pt[k]));
        if (!t[i].np) sb_puts(b, "-");
        sb_puts(b, " | ");
        for (int k = 0; k < t[i].nr; k++) sb_printf(b, "%s%s", k ? ", " : "", type_name((MlwsType)t[i].rt[k]));
        if (!t[i].nr) sb_puts(b, "-");
        sb_printf(b, " | %s |\n", t[i].doc);
    }
}

static void gen_natives(StrBuf *b) {
    size_t n, nos, ntext, nproc, nutil;
    const NativeDef *t = natives_builtin_table(&n), *os = natives_os_table(&nos), *text = natives_text_table(&ntext),
                    *procs = natives_proc_table(&nproc), *util = natives_util_table(&nutil);
    sb_puts(b, "# MLWSVM Built-in Natives\n\n"
               "> Generated from `src/native/natives.c` by `mlwsvm doc natives`. Do not edit by hand.\n\n"
               "Natives are host functions called with `ncall \"name\" (args...) -> (results...)`.\n"
               "Arguments are read with the parameter types listed below, and the verifier checks\n"
               "argument counts and register banks at load time. Most programs call them through the\n"
               "wrappers in `lib/std/*.masm` instead of directly.\n\n"
               "| Name | Parameters | Results | Description |\n|---|---|---|---|\n");
    native_rows(b, t, n);
    native_rows(b, procs, nproc);
    sb_puts(b, "\n## Files, the operating system and the network (`fs.*`, `os.*`, `net.*`)\n\n"
               "> Generated from `src/native/osio.c`. See [STDLIB.md](STDLIB.md) for how they wait,\n"
               "> their errors, and the OS-call quota (kind 5).\n\n"
               "| Name | Parameters | Results | Description |\n|---|---|---|---|\n");
    native_rows(b, os, nos);
    sb_puts(b, "\n## Text and JSON (`text.*`, `json.*`)\n\n"
               "> Generated from `src/native/text.c`. See [STDLIB.md](STDLIB.md#text-and-json).\n\n"
               "| Name | Parameters | Results | Description |\n|---|---|---|---|\n");
    native_rows(b, text, ntext);
    sb_puts(b, "\n## Time, hashes, random numbers, encodings and collections\n\n"
               "> Generated from `src/native/util.c`. See [STDLIB.md](STDLIB.md#time-and-dates).\n\n"
               "| Name | Parameters | Results | Description |\n|---|---|---|---|\n");
    native_rows(b, util, nutil);
}

int tool_doc(int argc, char **argv) {
    const char *what = NULL, *out = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else what = argv[i];
    }
    op_init();
    StrBuf b = {0};
    if (what && !strcmp(what, "isa")) gen_isa(&b);
    else if (what && !strcmp(what, "natives")) gen_natives(&b);
    else {
        fprintf(stderr, "usage: mlwsvm doc isa|natives [-o file]\n");
        return 2;
    }
    int rc = 0;
    if (out) {
        if (!write_file(out, b.s, b.n)) {
            fprintf(stderr, "mlwsvm doc: cannot write %s\n", out);
            rc = 1;
        }
    } else {
        fwrite(b.s, 1, b.n, stdout);
    }
    sb_free(&b);
    return rc;
}
