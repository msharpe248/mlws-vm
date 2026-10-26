/* assembler.c - parses preprocessed lines into functions, data and
 * instructions, resolves symbols and encodes a Module.
 *
 * Pipeline: preprocessor (preproc.c) -> per-line parse (labels, directives,
 * instructions with operands) -> .endf resolves labels and register counts ->
 * finish() resolves functions/globals across the module and encodes code. */
#include "asm.h"

#include <ctype.h>
#include <math.h>

#include "../core/check.h"
#include "../core/insn.h"
#include "preproc.h"

/* ---- data structures ------------------------------------------------------ */

typedef struct AsmOp {
    Operand o;
    int col;
    char *sym;           /* unresolved label / function / global name */
    bool localref;       /* numeric local label reference (1b / 1f) */
    int64_t lnum;
    char ldir;
    struct AsmOp *inner; /* OM_MIND* */
} AsmOp;

typedef VEC(AsmOp) AsmOpVec;

typedef struct {
    uint16_t op;
    uint8_t type, type2;
    int nops;
    AsmOp *ops;
    const PLine *pl;
    uint32_t seq;
} AsmInsn;

typedef struct {
    int64_t num;
    uint32_t seq, idx;
} NumLabel;

typedef struct AsmFunc {
    char *name;
    const PLine *pl;
    MFunc mf;
    StrMap labels;  /* name -> idx+1 */
    VEC(NumLabel) numlabels;
    StrMap aliases; /* name -> reg+1 */
    VEC(AsmInsn) insns;
    int regs_min[NBANKS];
    int nparam_bank[NBANKS];
} AsmFunc;

typedef struct {
    PP pp;
    MlwsError *err;
    char *modname;
    const PLine *module_pl;
    StrMap import_alias; /* alias or full name -> import idx+1 */
    VEC(char *) imports;
    VEC(AsmFunc *) funcs;
    StrMap funcmap; /* local function name -> index+1 */
    AsmFunc *cur;
    StrMap mod_aliases;
    ByteBuf gdata;
    VEC(uint8_t) gtags;
    VEC(MSym) syms;
    StrMap symmap; /* data symbol -> idx+1 */
    VEC(char *) pending;
    VEC(const PLine *) pending_pl;
    VEC(char *) exports;
    VEC(const PLine *) export_pl;
    VEC(MConst) consts;
    StrMap constmap;
    VEC(char *) natives;
    StrMap nativemap;
    VEC(MGref) grefs;
    StrMap grefmap;
    VEC(MFunc) ifuncs; /* imported function entries, appended after locals */
    VEC(MStruct) structs;  /* local definitions and import entries */
    StrMap structmap;      /* local name or "I:imp:name" -> idx+1 */
    MStruct *cur_struct;   /* open .struct, if any */
    const PLine *struct_pl;
    VEC(MFieldRef) frefs;
    StrMap frefmap;        /* "sidx:field" -> idx+1 */
    StrMap ifuncmap;
    uint32_t seq;
} Asm;

typedef struct {
    Token *t;
    size_t n, pos;
    const PLine *pl;
} Cur;

static void *IV(size_t i) { return (void *)(uintptr_t)(i + 1); }
static size_t VI(void *p) { return (size_t)((uintptr_t)p - 1); }

static Token *cur(Cur *c) { return &c->t[c->pos < c->n ? c->pos : c->n - 1]; }
static Token *peekn(Cur *c, size_t k) { return &c->t[c->pos + k < c->n ? c->pos + k : c->n - 1]; }
static char *tokdup(const Token *t) { return xstrndup(t->start, (size_t)t->len); }

static bool aerr(Asm *A, const PLine *pl, int col, const char *code, const char *fmt, ...) MLWS_PRINTF(5, 6);
static bool aerr(Asm *A, const PLine *pl, int col, const char *code, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return pp_line_error(A->err, pl, col, code, "%s", buf);
}

static bool expect_eol(Asm *A, Cur *c) {
    if (cur(c)->kind == TK_EOF) return true;
    return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "unexpected '%.*s'", cur(c)->len, cur(c)->start);
}

static bool expect_punct(Asm *A, Cur *c, const char *p) {
    if (tok_is(cur(c), p)) {
        c->pos++;
        return true;
    }
    return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected '%s'", p);
}

/* ---- registers & expressions ----------------------------------------------- */

static int parse_reg_name(const char *s, size_t n) {
    if (n == 2 && !memcmp(s, "SP", 2)) return REG_MAKE(BANK_S, SREG_SP);
    if (n == 2 && !memcmp(s, "FP", 2)) return REG_MAKE(BANK_S, SREG_FP);
    if (n == 3 && !memcmp(s, "PID", 3)) return REG_MAKE(BANK_S, SREG_PID);
    if (n < 2 || n > 3) return -1;
    int bank;
    switch (s[0]) {
    case 'I': bank = BANK_I; break;
    case 'F': bank = BANK_F; break;
    case 'R': bank = BANK_R; break;
    case 'P': bank = BANK_P; break;
    default: return -1;
    }
    if (!isdigit((unsigned char)s[1]) || (n == 3 && (!isdigit((unsigned char)s[2]) || s[1] == '0'))) return -1;
    int idx = s[1] - '0';
    if (n == 3) idx = idx * 10 + (s[2] - '0');
    if (idx >= NREGS) return -1;
    return REG_MAKE(bank, idx);
}

static int resolve_reg(Asm *A, const Token *t) {
    if (t->kind != TK_IDENT) return -1;
    int r = parse_reg_name(t->start, (size_t)t->len);
    if (r >= 0) return r;
    char buf[256];
    if ((size_t)t->len >= sizeof buf) return -1;
    memcpy(buf, t->start, (size_t)t->len);
    buf[t->len] = 0;
    void *v;
    if (A->cur && smap_get(&A->cur->aliases, buf, &v)) return (int)VI(v);
    if (smap_get(&A->mod_aliases, buf, &v)) return (int)VI(v);
    return -1;
}

static bool is_reserved_name(const char *s) {
    return parse_reg_name(s, strlen(s)) >= 0 || !strcmp(s, "ARG") || !strcmp(s, "LOC") || !strcmp(s, "null") ||
           !strcmp(s, "true") || !strcmp(s, "false");
}

static bool eval_expr(Asm *A, Cur *c, ExprVal *v) {
    ExprParser ep = {c->t, c->n, c->pos, pp_const_lookup, pp_defined, &A->pp, {0}, 0};
    if (!expr_eval(&ep, v)) return aerr(A, c->pl, ep.errcol, "E_ASM_EXPR", "%s", ep.err);
    c->pos = ep.pos;
    return true;
}

static bool eval_int(Asm *A, Cur *c, int64_t *out) {
    int col = cur(c)->col;
    ExprVal v;
    if (!eval_expr(A, c, &v)) return false;
    if (v.is_float) return aerr(A, c->pl, col, "E_ASM_EXPR", "expected an integer expression");
    *out = v.i;
    return true;
}

/* Is the identifier a named constant (so an operand starting with it is an expression)? */
static bool ident_is_const(Asm *A, const Token *t) {
    ExprVal v;
    return tok_ident(t, "true") || tok_ident(t, "false") || tok_ident(t, "inf") || tok_ident(t, "nan") ||
           tok_ident(t, "sizeof") || tok_ident(t, "defined") ||
           pp_const_lookup(&A->pp, t->start, (size_t)t->len, &v);
}

/* ---- module tables ----------------------------------------------------------- */

static uint32_t add_const_string(Asm *A, const char *s, size_t n) {
    StrBuf key = {0};
    sb_put_quoted(&key, s, n);
    void *v;
    uint32_t idx;
    if (smap_get(&A->constmap, key.s, &v)) {
        idx = (uint32_t)VI(v);
    } else {
        MConst mc = {CONST_STRING, (uint32_t)n, xmalloc(n + 1)};
        memcpy(mc.data, s, n);
        mc.data[n] = 0;
        idx = (uint32_t)A->consts.n;
        VEC_PUSH(A->consts, mc);
        smap_put(&A->constmap, key.s, IV(idx));
    }
    sb_free(&key);
    return idx;
}

static uint32_t add_native(Asm *A, const char *name) {
    void *v;
    if (smap_get(&A->nativemap, name, &v)) return (uint32_t)VI(v);
    uint32_t idx = (uint32_t)A->natives.n;
    VEC_PUSH(A->natives, xstrdup(name));
    smap_put(&A->nativemap, name, IV(idx));
    return idx;
}

/* Split "mod.path.sym" into an import index and symbol using the longest
 * import name/alias prefix. Returns false if no import matches. */
static bool split_import(Asm *A, const char *qname, uint32_t *imp, const char **sym) {
    size_t n = strlen(qname);
    for (size_t i = n; i-- > 0;) {
        if (qname[i] != '.') continue;
        char *prefix = xstrndup(qname, i);
        void *v;
        bool found = smap_get(&A->import_alias, prefix, &v);
        free(prefix);
        if (found && qname[i + 1]) {
            *imp = (uint32_t)VI(v);
            *sym = qname + i + 1;
            return true;
        }
    }
    return false;
}

/* ---- operands ---------------------------------------------------------------- */

static void op_init_mode(AsmOp *o, uint8_t mode, int col) {
    memset(o, 0, sizeof *o);
    o->o.mode = mode;
    o->col = col;
}

static bool check_base(Asm *A, Cur *c, int reg, int col) {
    int bank = REG_BANK(reg);
    if (bank == BANK_P || (bank == BANK_S && REG_IDX(reg) != SREG_PID)) return true;
    if (bank == BANK_R)
        return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND",
                    "objects are indexed as R0[I1] or R0[3] (element units), not with { } or address forms");
    return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND",
                "a memory base must be a pointer: a P register, SP or FP (use cvt/lea to make pointers)");
}

/* Parse "[index]" or "{index}" after a base. On entry cur(c) is '[' or '{'.
 * Produces disp-only or index forms. *is_idx tells which. */
static bool parse_index(Asm *A, Cur *c, bool *bytes, bool *is_idx, uint8_t *ireg, uint64_t *scale, int64_t *disp) {
    *bytes = tok_is(cur(c), "{");
    const char *close = *bytes ? "}" : "]";
    c->pos++;
    *is_idx = false;
    *scale = 1;
    *disp = 0;
    int r = resolve_reg(A, cur(c));
    if (r >= 0) {
        if (REG_BANK(r) != BANK_I)
            return aerr(A, c->pl, cur(c)->col, "E_ASM_BAD_OPERAND", "an index register must be an I register");
        *is_idx = true;
        *ireg = (uint8_t)r;
        c->pos++;
        if (tok_is(cur(c), "*")) {
            c->pos++;
            int col = cur(c)->col;
            ExprParser ep = {c->t, c->n, c->pos, pp_const_lookup, pp_defined, &A->pp, {0}, 0};
            ExprVal v;
            if (!expr_eval_term(&ep, &v)) return aerr(A, c->pl, ep.errcol, "E_ASM_EXPR", "%s", ep.err);
            c->pos = ep.pos;
            if (v.is_float || v.i < 0)
                return aerr(A, c->pl, col, "E_ASM_RANGE", "index scale must be a non-negative integer");
            *scale = (uint64_t)v.i;
        }
        /* "+ d" / "- d": let the expression parser see the sign as unary so
         * that I1-1+2 means a displacement of +1. */
        if (tok_is(cur(c), "+") || tok_is(cur(c), "-")) {
            if (!eval_int(A, c, disp)) return false;
        }
    } else {
        if (!eval_int(A, c, disp)) return false;
    }
    return expect_punct(A, c, close);
}

static bool parse_operand(Asm *A, Cur *c, char pat, AsmOp *out);

/* Location operand (not a literal). Handles [..], {..}, -[..], @global, reg[..]. */
static bool parse_location(Asm *A, Cur *c, AsmOp *out, bool *handled) {
    Token *t = cur(c);
    int col = t->col;
    *handled = true;
    /* -[reg] / -{reg}: pre-decrement */
    if (tok_is(t, "-") && (tok_is(peekn(c, 1), "[") || tok_is(peekn(c, 1), "{"))) {
        bool bytes = tok_is(peekn(c, 1), "{");
        c->pos += 2;
        int r = resolve_reg(A, cur(c));
        if (r < 0) return aerr(A, c->pl, cur(c)->col, "E_ASM_BAD_OPERAND", "expected a register in pre-decrement operand");
        if (!check_base(A, c, r, cur(c)->col)) return false;
        c->pos++;
        if (!expect_punct(A, c, bytes ? "}" : "]")) return false;
        op_init_mode(out, bytes ? OM_PREDECB : OM_PREDEC, col);
        out->o.reg = (uint8_t)r;
        return true;
    }
    if (tok_is(t, "{")) { /* {reg} or {reg}+ */
        c->pos++;
        int r = resolve_reg(A, cur(c));
        if (r < 0) return aerr(A, c->pl, cur(c)->col, "E_ASM_BAD_OPERAND", "expected a register inside { }");
        if (!check_base(A, c, r, cur(c)->col)) return false;
        c->pos++;
        if (!expect_punct(A, c, "}")) return false;
        if (tok_is(cur(c), "+")) {
            c->pos++;
            op_init_mode(out, OM_POSTINCB, col);
        } else {
            op_init_mode(out, OM_IND, col);
        }
        out->o.reg = (uint8_t)r;
        return true;
    }
    if (tok_is(t, "[")) {
        int r = resolve_reg(A, peekn(c, 1));
        if (r >= 0 && tok_is(peekn(c, 2), "]")) {
            if (!check_base(A, c, r, peekn(c, 1)->col)) return false;
            c->pos += 3;
            if (tok_is(cur(c), "+")) {
                c->pos++;
                op_init_mode(out, OM_POSTINC, col);
                out->o.reg = (uint8_t)r;
                return true;
            }
            if (tok_is(cur(c), "[") || tok_is(cur(c), "{")) {
                bool bytes, is_idx;
                uint8_t ireg = 0;
                uint64_t scale;
                int64_t disp;
                if (!parse_index(A, c, &bytes, &is_idx, &ireg, &scale, &disp)) return false;
                op_init_mode(out, is_idx ? (bytes ? OM_IDXB : OM_IDX) : (bytes ? OM_DISPB : OM_DISP), col);
                out->o.reg = (uint8_t)r;
                out->o.ireg = ireg;
                out->o.scale = scale;
                out->o.imm = disp;
                return true;
            }
            op_init_mode(out, OM_IND, col);
            out->o.reg = (uint8_t)r;
            return true;
        }
        /* [mem] -> memory-indirect */
        c->pos++;
        AsmOp *inner = xcalloc(1, sizeof *inner);
        if (!parse_operand(A, c, 'S', inner)) {
            free(inner);
            return false;
        }
        if (!mode_is_memory(inner->o.mode) || inner->o.mode == OM_MIND || inner->o.mode == OM_MINDB) {
            int icol = inner->col;
            free(inner->sym);
            free(inner);
            return aerr(A, c->pl, icol, "E_ASM_BAD_OPERAND",
                        "inside [ ] expected a register or a plain memory operand (e.g. [FP[2]])");
        }
        if (!expect_punct(A, c, "]")) {
            free(inner->sym);
            free(inner);
            return false;
        }
        bool bytes = false;
        int64_t disp = 0;
        if (tok_is(cur(c), "[") || tok_is(cur(c), "{")) {
            bytes = tok_is(cur(c), "{");
            c->pos++;
            if (!eval_int(A, c, &disp) || !expect_punct(A, c, bytes ? "}" : "]")) {
                free(inner->sym);
                free(inner);
                return false;
            }
        }
        op_init_mode(out, bytes ? OM_MINDB : OM_MIND, col);
        out->inner = inner;
        out->o.inner = &inner->o;
        out->o.imm = disp;
        return true;
    }
    if (tok_is(t, "@")) {
        c->pos++;
        Token *nt = cur(c);
        if (nt->kind != TK_IDENT) return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected a data symbol after '@'");
        char *name = tokdup(nt);
        c->pos++;
        uint8_t mode = OM_GLOBAL;
        uint8_t ireg = 0;
        uint64_t scale = 1;
        int64_t disp = 0;
        if (tok_is(cur(c), "[") || tok_is(cur(c), "{")) {
            bool bytes, is_idx;
            if (!parse_index(A, c, &bytes, &is_idx, &ireg, &scale, &disp)) {
                free(name);
                return false;
            }
            mode = is_idx ? (bytes ? OM_GLOBALIDXB : OM_GLOBALIDX) : (bytes ? OM_GLOBALB : OM_GLOBAL);
        }
        op_init_mode(out, mode, col);
        out->sym = name;
        out->o.ireg = ireg;
        out->o.scale = scale;
        out->o.imm = disp;
        return true;
    }
    if (t->kind == TK_IDENT && (tok_ident(t, "ARG") || tok_ident(t, "LOC")) && tok_is(peekn(c, 1), "[")) {
        bool arg = tok_ident(t, "ARG");
        c->pos += 2;
        int64_t n;
        int ncol = cur(c)->col;
        if (!eval_int(A, c, &n) || !expect_punct(A, c, "]")) return false;
        if (n < 0) return aerr(A, c->pl, ncol, "E_ASM_RANGE", "%s index must be non-negative", arg ? "ARG" : "LOC");
        op_init_mode(out, OM_DISP, col);
        out->o.reg = REG_MAKE(BANK_S, SREG_FP);
        out->o.imm = arg ? n : -1 - n;
        return true;
    }
    int r = resolve_reg(A, t);
    if (r >= 0 && REG_BANK(r) == BANK_R && (tok_is(peekn(c, 1), "[") || tok_is(peekn(c, 1), "->"))) {
        c->pos++;
        if (tok_is(cur(c), "->")) { /* R2->Type.field */
            c->pos++;
            Token *ft = cur(c);
            const char *dot = ft->kind == TK_IDENT ? memchr(ft->start, '.', (size_t)ft->len) : NULL;
            if (!dot || ft->start[ft->len - 1] == '.')
                return aerr(A, c->pl, ft->col, "E_ASM_SYNTAX", "expected Type.field after '->'");
            op_init_mode(out, OM_FIELD, col);
            out->o.reg = (uint8_t)r;
            out->sym = tokdup(ft);
            c->pos++;
            return true;
        }
        c->pos++; /* '[' */
        int ir = resolve_reg(A, cur(c));
        if (ir >= 0) {
            if (REG_BANK(ir) != BANK_I)
                return aerr(A, c->pl, cur(c)->col, "E_ASM_BAD_OPERAND", "an element index must be an I register");
            c->pos++;
            op_init_mode(out, OM_ELEM, col);
            out->o.ireg = (uint8_t)ir;
        } else {
            int icol = cur(c)->col;
            int64_t idx;
            if (!eval_int(A, c, &idx)) return false;
            if (idx < 0) return aerr(A, c->pl, icol, "E_ASM_RANGE", "element index must be non-negative");
            op_init_mode(out, OM_ELEMI, col);
            out->o.index = (uint64_t)idx;
        }
        out->o.reg = (uint8_t)r;
        if (!tok_is(cur(c), "]"))
            return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX",
                        "expected ']' (an element index is an I register or a constant; no scale or displacement)");
        c->pos++;
        return true;
    }
    if (r >= 0) {
        c->pos++;
        if (tok_is(cur(c), "[") || tok_is(cur(c), "{")) {
            if (!check_base(A, c, r, col)) return false;
            bool bytes, is_idx;
            uint8_t ireg = 0;
            uint64_t scale;
            int64_t disp;
            if (!parse_index(A, c, &bytes, &is_idx, &ireg, &scale, &disp)) return false;
            op_init_mode(out, is_idx ? (bytes ? OM_IDXB : OM_IDX) : (bytes ? OM_DISPB : OM_DISP), col);
            out->o.reg = (uint8_t)r;
            out->o.ireg = ireg;
            out->o.scale = scale;
            out->o.imm = disp;
            return true;
        }
        if (tok_is(cur(c), "->"))
            return aerr(A, c->pl, cur(c)->col, "E_ASM_BAD_OPERAND", "field access needs an R register: R0->Type.field");
        op_init_mode(out, OM_REG, col);
        out->o.reg = (uint8_t)r;
        return true;
    }
    *handled = false;
    return true;
}

static bool parse_operand(Asm *A, Cur *c, char pat, AsmOp *out) {
    Token *t = cur(c);
    int col = t->col;
    memset(out, 0, sizeof *out);
    out->col = col;
    if (t->kind == TK_EOF || tok_is(t, ",") || tok_is(t, ")"))
        return aerr(A, c->pl, col, "E_ASM_SYNTAX", "expected an operand");
    switch (pat) {
    case 'L':
        if (t->kind == TK_LOCALREF) {
            op_init_mode(out, OM_LABEL, col);
            out->localref = true;
            out->lnum = t->ival;
            out->ldir = t->p[0];
            c->pos++;
            return true;
        }
        if (t->kind != TK_IDENT || resolve_reg(A, t) >= 0)
            return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND", "expected a label");
        op_init_mode(out, OM_LABEL, col);
        out->sym = tokdup(t);
        c->pos++;
        return true;
    case 'F':
        if (t->kind != TK_IDENT || resolve_reg(A, t) >= 0)
            return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND", "expected a function name");
        op_init_mode(out, OM_FUNC, col);
        out->sym = tokdup(t);
        c->pos++;
        return true;
    case 'T':
        if (t->kind != TK_IDENT || resolve_reg(A, t) >= 0)
            return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND", "expected a struct type name");
        op_init_mode(out, OM_STRUCT, col);
        out->sym = tokdup(t);
        c->pos++;
        return true;
    case 'N':
        if (t->kind != TK_STRING)
            return aerr(A, c->pl, col, "E_ASM_BAD_OPERAND", "expected a native function name string, e.g. \"io.print_i\"");
        op_init_mode(out, OM_NATIVE, col);
        out->o.index = add_native(A, t->sval);
        c->pos++;
        return true;
    default: break;
    }
    bool handled;
    if (!parse_location(A, c, out, &handled)) return false;
    if (handled) return true;
    if (t->kind == TK_STRING) {
        op_init_mode(out, OM_CONST, col);
        out->o.index = add_const_string(A, t->sval, t->slen);
        c->pos++;
        return true;
    }
    if (tok_ident(t, "null")) {
        op_init_mode(out, OM_IMM, col);
        c->pos++;
        return true;
    }
    if (t->kind == TK_IDENT && !ident_is_const(A, t)) {
        return aerr(A, c->pl, col, "E_ASM_UNDEF_SYM",
                    "'%.*s' is not a register, alias or constant (labels and functions are only allowed as branch/call targets)",
                    t->len, t->start);
    }
    if (!expr_can_start(t)) return aerr(A, c->pl, col, "E_ASM_SYNTAX", "expected an operand");
    ExprVal v;
    if (!eval_expr(A, c, &v)) return false;
    if (v.is_float) {
        op_init_mode(out, OM_FIMM, col);
        out->o.f = v.f;
    } else {
        op_init_mode(out, OM_IMM, col);
        out->o.imm = v.i;
    }
    return true;
}

static void asmop_free(AsmOp *o) {
    free(o->sym);
    if (o->inner) {
        asmop_free(o->inner);
        free(o->inner);
    }
}

/* ---- labels ------------------------------------------------------------------ */

static void bind_pending(Asm *A) {
    for (size_t i = 0; i < A->pending.n; i++) {
        MSym s = {A->pending.v[i], (uint32_t)A->gdata.n, 0, 0};
        smap_put(&A->symmap, s.name, IV(A->syms.n));
        VEC_PUSH(A->syms, s);
    }
    A->pending.n = 0;
    A->pending_pl.n = 0;
}

static bool define_label(Asm *A, Cur *c, Token *t) {
    if (t->kind == TK_INT) {
        if (!A->cur) return aerr(A, c->pl, t->col, "E_ASM_SYNTAX", "numeric labels are only allowed inside a .func");
        NumLabel nl = {t->ival, A->seq, (uint32_t)A->cur->insns.n};
        VEC_PUSH(A->cur->numlabels, nl);
        return true;
    }
    char *name = tokdup(t);
    if (is_reserved_name(name)) {
        aerr(A, c->pl, t->col, "E_ASM_SYNTAX", "'%s' is a reserved name and cannot be a label", name);
        free(name);
        return false;
    }
    if (A->cur) {
        if (smap_has(&A->cur->labels, name)) {
            aerr(A, c->pl, t->col, "E_ASM_DUP_SYM", "label '%s' is already defined in function %s", name, A->cur->name);
            free(name);
            return false;
        }
        smap_put(&A->cur->labels, name, IV(A->cur->insns.n));
        free(name);
        return true;
    }
    bool dup = smap_has(&A->symmap, name) || smap_has(&A->funcmap, name) || smap_has(&A->structmap, name);
    for (size_t i = 0; i < A->pending.n; i++) dup = dup || !strcmp(A->pending.v[i], name);
    if (dup) {
        aerr(A, c->pl, t->col, "E_ASM_DUP_SYM", "symbol '%s' is already defined", name);
        free(name);
        return false;
    }
    VEC_PUSH(A->pending, name);
    VEC_PUSH(A->pending_pl, c->pl);
    return true;
}

/* ---- data ------------------------------------------------------------------- */

static void data_align(Asm *A, size_t align) {
    while (A->gdata.n % align) bb_u8(&A->gdata, 0);
}

static void sync_tags(Asm *A) {
    size_t words = (A->gdata.n + 7) / 8;
    while (A->gtags.n < words) VEC_PUSH(A->gtags, (uint8_t)T_NONE);
}

static void data_put(Asm *A, MlwsType t, uint64_t bits) {
    int w = type_size(t);
    size_t off = A->gdata.n;
    for (int i = 0; i < w; i++) bb_u8(&A->gdata, (uint8_t)(bits >> (8 * i)));
    sync_tags(A);
    for (size_t word = off / 8; word * 8 < off + (size_t)w; word++)
        A->gtags.v[word] = (w == 8 && off % 8 == 0) ? (uint8_t)t : (uint8_t)TAG_RAW;
}

static bool data_directive(Asm *A, Cur *c, MlwsType t) {
    if (A->cur) return aerr(A, c->pl, c->t[c->pos - 1].col, "E_ASM_SYNTAX", "data directives are not allowed inside a .func");
    data_align(A, (size_t)type_size(t));
    bind_pending(A);
    if (cur(c)->kind == TK_EOF) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected at least one value");
    for (;;) {
        int col = cur(c)->col;
        ExprVal v;
        if (!eval_expr(A, c, &v)) return false;
        uint64_t bits;
        if (type_is_float(t)) {
            double d = v.is_float ? v.f : (double)v.i;
            if (t == T_F32) {
                float f = (float)d;
                uint32_t u;
                memcpy(&u, &f, 4);
                bits = u;
            } else {
                memcpy(&bits, &d, 8);
            }
        } else {
            if (v.is_float) return aerr(A, c->pl, col, "E_ASM_EXPR", "float value in integer data");
            bits = (uint64_t)canon_int(t, v.i);
        }
        data_put(A, t, bits);
        if (!tok_is(cur(c), ",")) break;
        c->pos++;
    }
    return expect_eol(A, c);
}

/* ---- directives ----------------------------------------------------------- */

static bool parse_type_tok(Asm *A, Cur *c, MlwsType *t) {
    Token *tk = cur(c);
    if (tk->kind != TK_IDENT || !type_from_name(tk->start, (size_t)tk->len, t))
        return aerr(A, c->pl, tk->col, "E_ASM_BAD_TYPE", "expected a type name (i64, f64, ref, ptr, ...)");
    c->pos++;
    return true;
}

static bool dir_func(Asm *A, Cur *c) {
    if (A->cur) return aerr(A, c->pl, c->t[0].col, "E_ASM_SYNTAX", "nested .func (missing .endf for %s?)", A->cur->name);
    Token *nt = cur(c);
    if (nt->kind != TK_IDENT || nt->start[0] == '.')
        return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected a function name");
    char *name = tokdup(nt);
    if (is_reserved_name(name) || smap_has(&A->funcmap, name) || smap_has(&A->symmap, name) ||
        smap_has(&A->structmap, name)) {
        aerr(A, c->pl, nt->col, "E_ASM_DUP_SYM", "'%s' is already defined or reserved", name);
        free(name);
        return false;
    }
    c->pos++;
    bind_pending(A);
    AsmFunc *f = xcalloc(1, sizeof *f);
    f->name = name;
    f->pl = c->pl;
    smap_put(&A->funcmap, name, IV(A->funcs.n));
    VEC_PUSH(A->funcs, f);
    A->cur = f;
    if (tok_is(cur(c), "(")) {
        c->pos++;
        while (!tok_is(cur(c), ")")) {
            if (f->mf.nparams >= MAX_PARAMS)
                return aerr(A, c->pl, cur(c)->col, "E_ASM_RANGE", "too many parameters (max %d)", MAX_PARAMS);
            MlwsType t;
            if (!parse_type_tok(A, c, &t)) return false;
            Bank b = type_bank(t);
            int reg = REG_MAKE(b, f->nparam_bank[b]);
            f->nparam_bank[b]++;
            if (cur(c)->kind == TK_IDENT) {
                char *pn = tokdup(cur(c));
                if (is_reserved_name(pn) || smap_has(&f->aliases, pn)) {
                    aerr(A, c->pl, cur(c)->col, "E_ASM_DUP_SYM", "bad or duplicate parameter name '%s'", pn);
                    free(pn);
                    return false;
                }
                smap_put(&f->aliases, pn, IV((size_t)reg));
                free(pn);
                c->pos++;
            }
            f->mf.params[f->mf.nparams++] = (uint8_t)t;
            if (tok_is(cur(c), ",")) c->pos++;
            else if (!tok_is(cur(c), ")")) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected ',' or ')'");
        }
        c->pos++;
    }
    if (tok_is(cur(c), "->")) {
        c->pos++;
        f->mf.flags |= MF_RETS_DECLARED;
        bool paren = tok_is(cur(c), "(");
        if (paren) c->pos++;
        while (!(paren && tok_is(cur(c), ")"))) {
            if (f->mf.nrets >= MAX_PARAMS) return aerr(A, c->pl, cur(c)->col, "E_ASM_RANGE", "too many results");
            MlwsType t;
            if (!parse_type_tok(A, c, &t)) return false;
            f->mf.rets[f->mf.nrets++] = (uint8_t)t;
            if (!paren) break;
            if (tok_is(cur(c), ",")) c->pos++;
            else if (!tok_is(cur(c), ")")) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected ',' or ')'");
        }
        if (paren) c->pos++;
    }
    return expect_eol(A, c);
}

static bool resolve_local_labels(Asm *A, AsmFunc *f, AsmInsn *ai, AsmOp *o) {
    if (o->o.mode != OM_LABEL) return true;
    if (o->localref) {
        const NumLabel *best = NULL;
        for (size_t i = 0; i < f->numlabels.n; i++) {
            const NumLabel *nl = &f->numlabels.v[i];
            if (nl->num != o->lnum) continue;
            if (o->ldir == 'b' ? nl->seq <= ai->seq && (!best || nl->seq >= best->seq)
                               : nl->seq > ai->seq && (!best || nl->seq < best->seq))
                best = nl;
        }
        if (!best)
            return aerr(A, ai->pl, o->col, "E_ASM_UNDEF_LABEL", "no %s definition of local label %lld",
                        o->ldir == 'b' ? "earlier" : "later", (long long)o->lnum);
        o->o.index = best->idx;
        return true;
    }
    void *v;
    if (!smap_get(&f->labels, o->sym, &v))
        return aerr(A, ai->pl, o->col, "E_ASM_UNDEF_LABEL", "undefined label '%s' in function %s", o->sym, f->name);
    o->o.index = VI(v);
    return true;
}

static void note_reg(int maxr[NBANKS], uint8_t reg) {
    int b = REG_BANK(reg);
    if (b < NBANKS && REG_IDX(reg) + 1 > maxr[b]) maxr[b] = REG_IDX(reg) + 1;
}

static void note_operand_regs(int maxr[NBANKS], const Operand *o) {
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_DISP: case OM_DISPB: case OM_POSTINC: case OM_PREDEC:
    case OM_POSTINCB: case OM_PREDECB:
        note_reg(maxr, o->reg);
        break;
    case OM_IDX: case OM_IDXB:
        note_reg(maxr, o->reg);
        note_reg(maxr, o->ireg);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB: note_reg(maxr, o->ireg); break;
    case OM_ELEM:
        note_reg(maxr, o->reg);
        note_reg(maxr, o->ireg);
        break;
    case OM_ELEMI: case OM_FIELD: note_reg(maxr, o->reg); break;
    case OM_MIND: case OM_MINDB: note_operand_regs(maxr, o->inner); break;
    default: break;
    }
}

static bool dir_endf(Asm *A, Cur *c) {
    AsmFunc *f = A->cur;
    if (!f) return aerr(A, c->pl, c->t[0].col, "E_ASM_SYNTAX", ".endf without .func");
    if (!expect_eol(A, c)) return false;
    if (f->insns.n == 0) return aerr(A, f->pl, 0, "E_ASM_SYNTAX", "function %s has no instructions", f->name);
    for (size_t i = 0; i < f->labels.cap; i++)
        if (f->labels.keys[i] && VI(f->labels.vals[i]) >= f->insns.n)
            return aerr(A, c->pl, 0, "E_ASM_SYNTAX", "label '%s' is at the end of %s with no instruction after it",
                        f->labels.keys[i], f->name);
    for (size_t i = 0; i < f->numlabels.n; i++)
        if (f->numlabels.v[i].idx >= f->insns.n)
            return aerr(A, c->pl, 0, "E_ASM_SYNTAX", "local label %lld is at the end of %s",
                        (long long)f->numlabels.v[i].num, f->name);
    int maxr[NBANKS] = {0};
    for (size_t i = 0; i < f->insns.n; i++) {
        AsmInsn *ai = &f->insns.v[i];
        for (int k = 0; k < ai->nops; k++) {
            if (!resolve_local_labels(A, f, ai, &ai->ops[k])) return false;
            note_operand_regs(maxr, &ai->ops[k].o);
        }
    }
    AsmInsn *last = &f->insns.v[f->insns.n - 1];
    if (!(op_info[last->op].flags & OPF_TERM))
        return aerr(A, last->pl, 0, "E_ASM_SYNTAX",
                    "function %s can run off its end: the last instruction must be ret, jmp, tcall or halt", f->name);
    for (int b = 0; b < NBANKS; b++) {
        int n = maxr[b];
        if (f->nparam_bank[b] > n) n = f->nparam_bank[b];
        if (f->regs_min[b] > n) n = f->regs_min[b];
        f->mf.nregs[b] = (uint16_t)n;
    }
    A->cur = NULL;
    return true;
}

static bool dir_regs(Asm *A, Cur *c) {
    if (!A->cur) return aerr(A, c->pl, c->t[0].col, "E_ASM_SYNTAX", ".regs must be inside a .func");
    while (cur(c)->kind != TK_EOF) {
        Token *bt = cur(c);
        int b = -1;
        if (bt->kind == TK_IDENT && bt->len == 1) b = bt->start[0] == 'I' ? 0 : bt->start[0] == 'F' ? 1 : bt->start[0] == 'R' ? 2 : bt->start[0] == 'P' ? 3 : -1;
        if (b < 0) return aerr(A, c->pl, bt->col, "E_ASM_SYNTAX", "expected a bank name I, F, R or P");
        c->pos++;
        if (!expect_punct(A, c, "=")) return false;
        int64_t n;
        int col = cur(c)->col;
        if (!eval_int(A, c, &n)) return false;
        if (n < 0 || n > NREGS) return aerr(A, c->pl, col, "E_ASM_RANGE", "register count must be 0..%d", NREGS);
        A->cur->regs_min[b] = (int)n;
        if (tok_is(cur(c), ",")) c->pos++;
    }
    return true;
}

static bool dir_alias(Asm *A, Cur *c) {
    Token *nt = cur(c);
    if (nt->kind != TK_IDENT) return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected an alias name");
    char *name = tokdup(nt);
    if (is_reserved_name(name)) {
        aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "'%s' is reserved", name);
        free(name);
        return false;
    }
    c->pos++;
    if (tok_is(cur(c), ",")) c->pos++;
    int r = parse_reg_name(cur(c)->start, (size_t)cur(c)->len);
    if (cur(c)->kind != TK_IDENT || r < 0) {
        free(name);
        return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected a register");
    }
    c->pos++;
    smap_put(A->cur ? &A->cur->aliases : &A->mod_aliases, name, IV((size_t)r));
    free(name);
    return expect_eol(A, c);
}

static bool dir_import(Asm *A, Cur *c) {
    if (A->cur) return aerr(A, c->pl, c->t[0].col, "E_ASM_SYNTAX", ".import must be outside a .func");
    Token *nt = cur(c);
    if (nt->kind != TK_IDENT || nt->start[0] == '.') return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected a module name");
    char *name = tokdup(nt);
    c->pos++;
    void *v;
    size_t idx;
    if (smap_get(&A->import_alias, name, &v) && !strcmp(A->imports.v[VI(v)], name)) {
        idx = VI(v);
        free(name);
    } else {
        idx = A->imports.n;
        VEC_PUSH(A->imports, name);
        smap_put(&A->import_alias, name, IV(idx));
    }
    if (tok_ident(cur(c), "as")) {
        c->pos++;
        Token *at = cur(c);
        if (at->kind != TK_IDENT) return aerr(A, c->pl, at->col, "E_ASM_SYNTAX", "expected an alias after 'as'");
        char *alias = tokdup(at);
        if (smap_get(&A->import_alias, alias, &v) && VI(v) != idx) {
            aerr(A, c->pl, at->col, "E_ASM_DUP_SYM", "import alias '%s' is already used", alias);
            free(alias);
            return false;
        }
        smap_put(&A->import_alias, alias, IV(idx));
        free(alias);
        c->pos++;
    }
    return expect_eol(A, c);
}

static bool dir_struct(Asm *A, Cur *c) {
    if (A->cur || A->cur_struct)
        return aerr(A, c->pl, c->t[0].col, "E_ASM_SYNTAX", ".struct must be at top level");
    Token *nt = cur(c);
    if (nt->kind != TK_IDENT || nt->start[0] == '.' || memchr(nt->start, '.', (size_t)nt->len))
        return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected a struct name (without dots)");
    char *name = tokdup(nt);
    bool pending_dup = false;
    for (size_t i = 0; i < A->pending.n; i++) pending_dup = pending_dup || !strcmp(A->pending.v[i], name);
    if (is_reserved_name(name) || smap_has(&A->structmap, name) || smap_has(&A->funcmap, name) ||
        smap_has(&A->symmap, name) || pending_dup) {
        aerr(A, c->pl, nt->col, "E_ASM_DUP_SYM", "'%s' is already defined or reserved", name);
        free(name);
        return false;
    }
    c->pos++;
    MStruct st;
    memset(&st, 0, sizeof st);
    st.name = name;
    smap_put(&A->structmap, name, IV(A->structs.n));
    VEC_PUSH(A->structs, st);
    A->cur_struct = &A->structs.v[A->structs.n - 1];
    A->struct_pl = c->pl;
    return expect_eol(A, c);
}

/* Inside .struct: `name: type` lines until .ends. */
static bool struct_line(Asm *A, Cur *c) {
    Token *t = cur(c);
    if (t->kind == TK_EOF) return true;
    if (tok_ident(t, ".ends")) {
        c->pos++;
        if (A->cur_struct->nfields == 0)
            return aerr(A, A->struct_pl, 0, "E_ASM_SYNTAX", "struct %s has no fields", A->cur_struct->name);
        A->cur_struct = NULL;
        return expect_eol(A, c);
    }
    if (t->kind != TK_IDENT || t->start[0] == '.' || !tok_is(peekn(c, 1), ":"))
        return aerr(A, c->pl, t->col, "E_ASM_SYNTAX", "expected 'field: type' or .ends inside .struct");
    MStruct *st = A->cur_struct;
    char *fname = tokdup(t);
    for (uint32_t i = 0; i < st->nfields; i++) {
        if (!strcmp(st->fields[i], fname)) {
            aerr(A, c->pl, t->col, "E_ASM_DUP_SYM", "duplicate field '%s' in struct %s", fname, st->name);
            free(fname);
            return false;
        }
    }
    c->pos += 2;
    MlwsType ty;
    if (!parse_type_tok(A, c, &ty)) {
        free(fname);
        return false;
    }
    st->fields = xrealloc(st->fields, (st->nfields + 1) * sizeof(char *));
    st->types = xrealloc(st->types, st->nfields + 1);
    st->fields[st->nfields] = fname;
    st->types[st->nfields] = (uint8_t)ty;
    st->nfields++;
    return expect_eol(A, c);
}

static bool directive(Asm *A, Cur *c) {
    Token *d = cur(c);
    c->pos++;
#define IS(s) tok_ident(d, s)
    static const struct {
        const char *name;
        MlwsType t;
    } data_dirs[] = {
        {".i8", T_I8}, {".i16", T_I16}, {".i32", T_I32}, {".i64", T_I64}, {".u8", T_U8}, {".u16", T_U16},
        {".u32", T_U32}, {".u64", T_U64}, {".f32", T_F32}, {".f64", T_F64}, {".b", T_B}, {".c", T_C},
    };
    for (size_t i = 0; i < sizeof data_dirs / sizeof data_dirs[0]; i++)
        if (IS(data_dirs[i].name)) return data_directive(A, c, data_dirs[i].t);
    if (IS(".func")) return dir_func(A, c);
    if (IS(".struct")) return dir_struct(A, c);
    if (IS(".ends")) return aerr(A, c->pl, d->col, "E_ASM_SYNTAX", ".ends without .struct");
    if (IS(".endf")) return dir_endf(A, c);
    if (IS(".regs")) return dir_regs(A, c);
    if (IS(".alias")) return dir_alias(A, c);
    if (IS(".import")) return dir_import(A, c);
    if (IS(".module")) {
        Token *nt = cur(c);
        if (nt->kind != TK_IDENT) return aerr(A, c->pl, nt->col, "E_ASM_SYNTAX", "expected a module name");
        free(A->modname);
        A->modname = tokdup(nt);
        A->module_pl = c->pl;
        c->pos++;
        return expect_eol(A, c);
    }
    if (IS(".export")) {
        while (cur(c)->kind == TK_IDENT) {
            VEC_PUSH(A->exports, tokdup(cur(c)));
            VEC_PUSH(A->export_pl, c->pl);
            c->pos++;
            if (!tok_is(cur(c), ",")) break;
            c->pos++;
        }
        if (A->exports.n == 0) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected names to export");
        return expect_eol(A, c);
    }
    if (IS(".zero") || IS(".space") || IS(".align")) {
        if (A->cur) return aerr(A, c->pl, d->col, "E_ASM_SYNTAX", "data directives are not allowed inside a .func");
        int64_t n;
        int col = cur(c)->col;
        if (!eval_int(A, c, &n)) return false;
        if (IS(".align")) {
            if (n <= 0 || (n & (n - 1)) || n > 4096)
                return aerr(A, c->pl, col, "E_ASM_RANGE", ".align needs a power of two up to 4096");
            data_align(A, (size_t)n);
        } else {
            if (n < 0 || n > (1 << 30)) return aerr(A, c->pl, col, "E_ASM_RANGE", "bad size");
            bind_pending(A);
            for (int64_t i = 0; i < n; i++) bb_u8(&A->gdata, 0);
        }
        sync_tags(A);
        return expect_eol(A, c);
    }
    if (IS(".ascii") || IS(".asciz")) {
        if (A->cur) return aerr(A, c->pl, d->col, "E_ASM_SYNTAX", "data directives are not allowed inside a .func");
        if (cur(c)->kind != TK_STRING) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected a string");
        bind_pending(A);
        size_t off = A->gdata.n;
        bb_bytes(&A->gdata, cur(c)->sval, cur(c)->slen);
        if (IS(".asciz")) bb_u8(&A->gdata, 0);
        sync_tags(A);
        for (size_t w = off / 8; w * 8 < A->gdata.n; w++) A->gtags.v[w] = TAG_RAW;
        c->pos++;
        return expect_eol(A, c);
    }
#undef IS
    return aerr(A, c->pl, d->col, "E_ASM_SYNTAX", "unknown directive '%.*s'", d->len, d->start);
}

/* ---- instructions --------------------------------------------------------- */

static MlwsType natural_of(const AsmOp *o) {
    switch (o->o.mode) {
    case OM_REG: return reg_natural_type(o->o.reg);
    case OM_IMM: return T_I64;
    case OM_FIMM: return T_F64;
    case OM_CONST: return T_R;
    default: return T_NONE;
    }
}

static MlwsType default_type(uint32_t set) {
    static const MlwsType pref[] = {T_I64, T_F64, T_R, T_P};
    for (size_t i = 0; i < 4; i++)
        if (set & TM(pref[i])) return pref[i];
    for (int t = 1; t < T__COUNT; t++)
        if (set & TM(t)) return (MlwsType)t;
    return T_NONE;
}

/* Declared type of a `Type.field` reference to a struct defined earlier in this
 * module, or T_NONE (imported or not yet known). */
static MlwsType local_field_type(Asm *A, const char *sym) {
    const char *dot = strrchr(sym, '.');
    if (!dot) return T_NONE;
    char *sname = xstrndup(sym, (size_t)(dot - sym));
    void *v;
    MlwsType t = T_NONE;
    if (smap_get(&A->structmap, sname, &v)) {
        const MStruct *st = &A->structs.v[VI(v)];
        for (uint32_t i = 0; i < st->nfields; i++)
            if (!strcmp(st->fields[i], dot + 1)) t = (MlwsType)st->types[i];
    }
    free(sname);
    return t;
}

static MlwsType operand_hint(Asm *A, const AsmOp *o) {
    if (o->o.mode == OM_REG) return reg_natural_type(o->o.reg);
    if (o->o.mode == OM_FIELD) return local_field_type(A, o->sym);
    return T_NONE;
}

static void infer_type(Asm *A, AsmInsn *ai, bool has_type, bool has_type2) {
    const OpInfo *oi = &op_info[ai->op];
    if (!has_type && oi->types && !(oi->flags & OPF_PERVAL)) {
        MlwsType t = T_NONE;
        if (oi->flags & OPF_TYPE2) {
            if (ai->nops > 0) t = operand_hint(A, &ai->ops[0]);
        } else {
            for (int k = 0; k < ai->nops && !t; k++) {
                char p = op_pattern_at(oi, k);
                if (p == 'D' || p == 'S') t = operand_hint(A, &ai->ops[k]);
            }
            if (!t && !(oi->flags & OPF_NATURAL)) {
                for (int k = 0; k < ai->nops && !t; k++)
                    if (op_pattern_at(oi, k) == 'S') t = natural_of(&ai->ops[k]);
                if (!t || !(oi->types & TM(t))) t = default_type(oi->types);
            }
        }
        ai->type = (uint8_t)t;
    }
    if ((oi->flags & OPF_TYPE2) && !has_type2 && ai->nops > 1) {
        MlwsType t2 = ai->ops[1].o.mode == OM_FIELD ? operand_hint(A, &ai->ops[1]) : natural_of(&ai->ops[1]);
        if (t2 != T_R && t2 != T_P) ai->type2 = (uint8_t)t2;
    }
}

static bool parse_call(Asm *A, Cur *c, AsmInsn *ai, AsmOpVec *ops) {
    AsmOp target, argc;
    if (!parse_operand(A, c, ai->op == OP_NCALL ? 'N' : 'F', &target)) return false;
    VEC_PUSH(*ops, target);
    op_init_mode(&argc, OM_IMM, cur(c)->col);
    VEC_PUSH(*ops, argc);
    int nargs = 0;
    if (tok_is(cur(c), "(")) {
        c->pos++;
        while (!tok_is(cur(c), ")")) {
            AsmOp a;
            if (!parse_operand(A, c, 'V', &a)) return false;
            VEC_PUSH(*ops, a);
            nargs++;
            if (tok_is(cur(c), ",")) c->pos++;
            else if (!tok_is(cur(c), ")")) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected ',' or ')'");
        }
        c->pos++;
    }
    ops->v[1].o.imm = nargs;
    if (tok_is(cur(c), "->")) {
        if (ai->op == OP_TCALL)
            return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "tcall cannot have results; they go to the current caller");
        c->pos++;
        bool paren = tok_is(cur(c), "(");
        if (paren) c->pos++;
        while (!(paren && tok_is(cur(c), ")"))) {
            AsmOp r;
            if (!parse_operand(A, c, 'W', &r)) return false;
            VEC_PUSH(*ops, r);
            if (!paren) break;
            if (tok_is(cur(c), ",")) c->pos++;
            else if (!tok_is(cur(c), ")")) return aerr(A, c->pl, cur(c)->col, "E_ASM_SYNTAX", "expected ',' or ')'");
        }
        if (paren) c->pos++;
    }
    return expect_eol(A, c);
}

static bool instruction(Asm *A, Cur *c) {
    Token *m = cur(c);
    const char *dot = memchr(m->start, '.', (size_t)m->len);
    size_t blen = dot ? (size_t)(dot - m->start) : (size_t)m->len;
    int op = op_lookup(m->start, blen);
    if (op < 0)
        return aerr(A, c->pl, m->col, "E_ASM_UNKNOWN_OP", "unknown instruction or macro '%.*s'", m->len, m->start);
    if (!A->cur) return aerr(A, c->pl, m->col, "E_ASM_SYNTAX", "instructions must be inside a .func");
    const OpInfo *oi = &op_info[op];
    AsmInsn ai = {0};
    ai.op = (uint16_t)op;
    ai.pl = c->pl;
    ai.seq = A->seq;
    bool has_type = false, has_type2 = false;
    if (dot) {
        const char *s = dot + 1, *end = m->start + m->len;
        const char *dot2 = memchr(s, '.', (size_t)(end - s));
        MlwsType t;
        if (!type_from_name(s, (size_t)((dot2 ? dot2 : end) - s), &t))
            return aerr(A, c->pl, m->col, "E_ASM_BAD_TYPE", "unknown type suffix in '%.*s'", m->len, m->start);
        ai.type = (uint8_t)t;
        has_type = true;
        if (dot2) {
            if (!(oi->flags & OPF_TYPE2))
                return aerr(A, c->pl, m->col, "E_ASM_BAD_TYPE", "%s takes only one type suffix", oi->mnem);
            if (!type_from_name(dot2 + 1, (size_t)(end - dot2 - 1), &t))
                return aerr(A, c->pl, m->col, "E_ASM_BAD_TYPE", "unknown type suffix in '%.*s'", m->len, m->start);
            ai.type2 = (uint8_t)t;
            has_type2 = true;
        }
    }
    c->pos++;
    AsmOpVec ops = {0};
    bool ok = true;
    if (oi->flags & OPF_CALL) {
        ok = parse_call(A, c, &ai, &ops);
    } else if (cur(c)->kind != TK_EOF) {
        for (int k = 0;; k++) {
            char pat = op_pattern_at(oi, k);
            if (!pat) {
                ok = aerr(A, c->pl, cur(c)->col, "E_ASM_OPERAND_COUNT", "too many operands for %s (max %d)", oi->mnem,
                          oi->maxops);
                break;
            }
            AsmOp o;
            if (!parse_operand(A, c, pat, &o)) {
                ok = false;
                break;
            }
            VEC_PUSH(ops, o);
            if (!tok_is(cur(c), ",")) break;
            c->pos++;
        }
        if (ok) ok = expect_eol(A, c);
    }
    if (ok && ops.n > MAX_OPERANDS)
        ok = aerr(A, c->pl, m->col, "E_ASM_OPERAND_COUNT", "an instruction can have at most %d operands", MAX_OPERANDS);
    if (!ok) {
        for (size_t i = 0; i < ops.n; i++) asmop_free(&ops.v[i]);
        VEC_FREE(ops);
        return false;
    }
    ai.ops = ops.v;
    ai.nops = (int)ops.n;
    infer_type(A, &ai, has_type, has_type2);

    Operand tmp[MAX_OPERANDS];
    Insn in = {ai.op, ai.type, ai.type2, (uint8_t)ai.nops, tmp};
    for (int k = 0; k < ai.nops; k++) tmp[k] = ai.ops[k].o;
    CheckErr ce;
    if (!check_insn(&in, &ce)) {
        int col = ce.operand >= 0 && ce.operand < ai.nops ? ai.ops[ce.operand].col : m->col;
        const char *code = strstr(ce.msg, "operand(s)") || strstr(ce.msg, "operands, got") ? "E_ASM_OPERAND_COUNT"
                           : strstr(ce.msg, "type") && ce.operand < 0                     ? "E_ASM_BAD_TYPE"
                                                                                          : "E_ASM_BAD_OPERAND";
        for (int k = 0; k < ai.nops; k++) asmop_free(&ai.ops[k]);
        free(ai.ops);
        return aerr(A, c->pl, col, code, "%s", ce.msg);
    }
    VEC_PUSH(A->cur->insns, ai);
    return true;
}

static bool statement(Asm *A, const PLine *pl) {
    TokVec tv = {0};
    char msg[256];
    int col = 0;
    if (!lex_line(pl->text, &tv, msg, sizeof msg, &col)) {
        tokvec_free(&tv);
        return aerr(A, pl, col, "E_ASM_SYNTAX", "%s", msg);
    }
    Cur c = {tv.v, tv.n, 0, pl};
    A->seq++;
    bool ok = true;
    if (A->cur_struct) {
        ok = struct_line(A, &c);
        tokvec_free(&tv);
        return ok;
    }
    while (ok && (cur(&c)->kind == TK_IDENT || cur(&c)->kind == TK_INT) && tok_is(peekn(&c, 1), ":")) {
        ok = define_label(A, &c, cur(&c));
        c.pos += 2;
    }
    if (ok && cur(&c)->kind != TK_EOF) {
        Token *t = cur(&c);
        if (t->kind != TK_IDENT) ok = aerr(A, pl, t->col, "E_ASM_SYNTAX", "expected an instruction or directive");
        else if (t->start[0] == '.') ok = directive(A, &c);
        else ok = instruction(A, &c);
    }
    tokvec_free(&tv);
    return ok;
}

/* ---- module-level resolution & encoding ------------------------------------ */

static bool resolve_func_ref(Asm *A, const PLine *pl, AsmOp *o) {
    void *v;
    if (smap_get(&A->funcmap, o->sym, &v)) {
        o->o.index = VI(v);
        return true;
    }
    uint32_t imp;
    const char *sym;
    if (!split_import(A, o->sym, &imp, &sym)) {
        if (strchr(o->sym, '.'))
            return aerr(A, pl, o->col, "E_ASM_UNDEF_SYM", "undefined function '%s' (is its module imported with .import?)", o->sym);
        return aerr(A, pl, o->col, "E_ASM_UNDEF_SYM", "undefined function '%s'", o->sym);
    }
    char key[512];
    snprintf(key, sizeof key, "%u:%s", imp, sym);
    if (smap_get(&A->ifuncmap, key, &v)) {
        o->o.index = A->funcs.n + VI(v);
        return true;
    }
    MFunc mf;
    memset(&mf, 0, sizeof mf);
    mf.name = xstrdup(sym);
    mf.flags = MF_IMPORT;
    mf.import_idx = imp;
    smap_put(&A->ifuncmap, key, IV(A->ifuncs.n));
    o->o.index = A->funcs.n + A->ifuncs.n;
    VEC_PUSH(A->ifuncs, mf);
    return true;
}

static bool resolve_gref(Asm *A, const PLine *pl, AsmOp *o) {
    void *v;
    char key[512];
    MGref g;
    memset(&g, 0, sizeof g);
    if (smap_get(&A->symmap, o->sym, &v)) {
        g.kind = GREF_LOCAL;
        g.offset = A->syms.v[VI(v)].offset;
        snprintf(key, sizeof key, "L:%u", g.offset);
    } else {
        uint32_t imp;
        const char *sym;
        if (!split_import(A, o->sym, &imp, &sym))
            return aerr(A, pl, o->col, "E_ASM_UNDEF_SYM", "undefined data symbol '@%s'", o->sym);
        g.kind = GREF_IMPORT;
        g.import_idx = imp;
        snprintf(key, sizeof key, "I:%u:%s", imp, sym);
        if (!smap_has(&A->grefmap, key)) g.sym = xstrdup(sym);
    }
    if (smap_get(&A->grefmap, key, &v)) {
        o->o.index = VI(v);
        return true;
    }
    o->o.index = A->grefs.n;
    smap_put(&A->grefmap, key, IV(A->grefs.n));
    VEC_PUSH(A->grefs, g);
    return true;
}

/* Struct table index for a local or imported (qualified) struct name. */
static bool resolve_struct_name(Asm *A, const PLine *pl, int col, const char *name, uint32_t *out) {
    void *v;
    if (smap_get(&A->structmap, name, &v)) {
        *out = (uint32_t)VI(v);
        return true;
    }
    uint32_t imp;
    const char *sym;
    if (!split_import(A, name, &imp, &sym))
        return aerr(A, pl, col, "E_ASM_UNDEF_SYM", "undefined struct type '%s'", name);
    char key[512];
    snprintf(key, sizeof key, "I:%u:%s", imp, sym);
    if (smap_get(&A->structmap, key, &v)) {
        *out = (uint32_t)VI(v);
        return true;
    }
    MStruct st;
    memset(&st, 0, sizeof st);
    st.name = xstrdup(sym);
    st.flags = MSF_IMPORT;
    st.import_idx = imp;
    *out = (uint32_t)A->structs.n;
    smap_put(&A->structmap, key, IV(A->structs.n));
    VEC_PUSH(A->structs, st);
    return true;
}

static bool resolve_field_ref(Asm *A, const PLine *pl, AsmOp *o) {
    const char *dot = strrchr(o->sym, '.');
    char *sname = xstrndup(o->sym, (size_t)(dot - o->sym));
    uint32_t si;
    bool ok = resolve_struct_name(A, pl, o->col, sname, &si);
    free(sname);
    if (!ok) return false;
    const char *field = dot + 1;
    const MStruct *st = &A->structs.v[si];
    if (!(st->flags & MSF_IMPORT)) {
        bool found = false;
        for (uint32_t i = 0; i < st->nfields; i++) found = found || !strcmp(st->fields[i], field);
        if (!found) return aerr(A, pl, o->col, "E_ASM_UNDEF_SYM", "struct %s has no field '%s'", st->name, field);
    }
    char key[512];
    snprintf(key, sizeof key, "%u:%s", si, field);
    void *v;
    if (smap_get(&A->frefmap, key, &v)) {
        o->o.index = VI(v);
        return true;
    }
    MFieldRef fr = {si, xstrdup(field)};
    o->o.index = A->frefs.n;
    smap_put(&A->frefmap, key, IV(A->frefs.n));
    VEC_PUSH(A->frefs, fr);
    return true;
}

static bool resolve_module_refs(Asm *A, const PLine *pl, AsmOp *o) {
    if (o->inner && !resolve_module_refs(A, pl, o->inner)) return false;
    if (o->o.mode == OM_FIELD) return resolve_field_ref(A, pl, o);
    if (o->o.mode == OM_STRUCT) {
        uint32_t si;
        if (!resolve_struct_name(A, pl, o->col, o->sym, &si)) return false;
        o->o.index = si;
        return true;
    }
    if (o->o.mode == OM_FUNC) return resolve_func_ref(A, pl, o);
    if (o->o.mode >= OM_GLOBAL && o->o.mode <= OM_GLOBALIDXB) return resolve_gref(A, pl, o);
    return true;
}

static void fill_operand(Operand *dst, AsmOp *o) {
    *dst = o->o;
    dst->inner = o->inner ? &o->inner->o : NULL;
}

static Module *finish(Asm *A) {
    if (A->cur) {
        aerr(A, A->cur->pl, 0, "E_ASM_SYNTAX", "missing .endf for function %s", A->cur->name);
        return NULL;
    }
    if (A->cur_struct) {
        aerr(A, A->struct_pl, 0, "E_ASM_SYNTAX", "missing .ends for struct %s", A->cur_struct->name);
        return NULL;
    }
    bind_pending(A);
    data_align(A, 8);
    sync_tags(A);
    for (size_t i = 0; i < A->syms.n; i++) {
        uint32_t end = i + 1 < A->syms.n ? A->syms.v[i + 1].offset : (uint32_t)A->gdata.n;
        A->syms.v[i].size = end - A->syms.v[i].offset;
    }
    for (size_t i = 0; i < A->exports.n; i++) {
        void *v;
        if (smap_get(&A->funcmap, A->exports.v[i], &v)) A->funcs.v[VI(v)]->mf.flags |= MF_EXPORT;
        else if (smap_get(&A->symmap, A->exports.v[i], &v)) A->syms.v[VI(v)].flags |= MS_EXPORT;
        else if (smap_get(&A->structmap, A->exports.v[i], &v)) A->structs.v[VI(v)].flags |= MSF_EXPORT;
        else {
            aerr(A, A->export_pl.v[i], 0, "E_ASM_UNDEF_SYM", "cannot export undefined symbol '%s'", A->exports.v[i]);
            return NULL;
        }
    }
    for (size_t f = 0; f < A->funcs.n; f++) {
        AsmFunc *af = A->funcs.v[f];
        for (size_t i = 0; i < af->insns.n; i++) {
            AsmInsn *ai = &af->insns.v[i];
            for (int k = 0; k < ai->nops; k++)
                if (!resolve_module_refs(A, ai->pl, &ai->ops[k])) return NULL;
        }
    }

    Module *m = xcalloc(1, sizeof *m);
    m->name = xstrdup(A->modname);
    m->source = xstrdup(A->pp.files.n ? A->pp.files.v[0] : "");
    ByteBuf code = {0};
    m->nfuncs = (uint32_t)(A->funcs.n + A->ifuncs.n);
    m->funcs = xcalloc(m->nfuncs ? m->nfuncs : 1, sizeof(MFunc));
    for (size_t f = 0; f < A->funcs.n; f++) {
        AsmFunc *af = A->funcs.v[f];
        MFunc *mf = &m->funcs[f];
        *mf = af->mf;
        mf->name = xstrdup(af->name);
        mf->code_off = (uint32_t)code.n;
        mf->ninsns = (uint32_t)af->insns.n;
        mf->lines = xcalloc(af->insns.n, sizeof(uint32_t));
        for (size_t i = 0; i < af->insns.n; i++) {
            AsmInsn *ai = &af->insns.v[i];
            Operand tmp[MAX_OPERANDS];
            for (int k = 0; k < ai->nops; k++) fill_operand(&tmp[k], &ai->ops[k]);
            Insn in = {ai->op, ai->type, ai->type2, (uint8_t)ai->nops, tmp};
            encode_insn(&code, &in);
            mf->lines[i] = (uint32_t)ai->pl->origin.line;
        }
        mf->code_len = (uint32_t)code.n - mf->code_off;
    }
    for (size_t i = 0; i < A->ifuncs.n; i++) {
        m->funcs[A->funcs.n + i] = A->ifuncs.v[i];
        A->ifuncs.v[i].name = NULL; /* ownership moved */
    }
    m->code = code.b ? code.b : xmalloc(1);
    m->codelen = (uint32_t)code.n;

    m->nimports = (uint32_t)A->imports.n;
    m->imports = xcalloc(A->imports.n + 1, sizeof(char *));
    for (size_t i = 0; i < A->imports.n; i++) m->imports[i] = xstrdup(A->imports.v[i]);
    m->nconsts = (uint32_t)A->consts.n;
    m->consts = A->consts.v;
    A->consts.v = NULL;
    A->consts.n = 0;
    m->nsyms = (uint32_t)A->syms.n;
    m->syms = xcalloc(A->syms.n + 1, sizeof(MSym));
    for (size_t i = 0; i < A->syms.n; i++) {
        m->syms[i] = A->syms.v[i];
        m->syms[i].name = xstrdup(A->syms.v[i].name);
    }
    m->ngrefs = (uint32_t)A->grefs.n;
    m->grefs = A->grefs.v;
    A->grefs.v = NULL;
    A->grefs.n = 0;
    m->nstructs = (uint32_t)A->structs.n;
    m->structs = A->structs.v ? A->structs.v : xcalloc(1, sizeof(MStruct));
    A->structs.v = NULL;
    A->structs.n = 0;
    m->nfrefs = (uint32_t)A->frefs.n;
    m->frefs = A->frefs.v ? A->frefs.v : xcalloc(1, sizeof(MFieldRef));
    A->frefs.v = NULL;
    A->frefs.n = 0;
    m->nnatives = (uint32_t)A->natives.n;
    m->natives = xcalloc(A->natives.n + 1, sizeof(char *));
    for (size_t i = 0; i < A->natives.n; i++) m->natives[i] = xstrdup(A->natives.v[i]);
    m->gsize = (uint32_t)A->gdata.n;
    m->gdata = xcalloc(m->gsize + 1, 1);
    memcpy(m->gdata, A->gdata.b, m->gsize);
    m->gtags = xcalloc(m->gsize / 8 + 1, 1);
    memcpy(m->gtags, A->gtags.v, m->gsize / 8);
    return m;
}

static void asm_free(Asm *A) {
    for (size_t f = 0; f < A->funcs.n; f++) {
        AsmFunc *af = A->funcs.v[f];
        for (size_t i = 0; i < af->insns.n; i++) {
            for (int k = 0; k < af->insns.v[i].nops; k++) asmop_free(&af->insns.v[i].ops[k]);
            free(af->insns.v[i].ops);
        }
        VEC_FREE(af->insns);
        VEC_FREE(af->numlabels);
        smap_free(&af->labels);
        smap_free(&af->aliases);
        free(af->name);
        free(af);
    }
    VEC_FREE(A->funcs);
    for (size_t i = 0; i < A->imports.n; i++) free(A->imports.v[i]);
    VEC_FREE(A->imports);
    for (size_t i = 0; i < A->syms.n; i++) free(A->syms.v[i].name);
    VEC_FREE(A->syms);
    for (size_t i = 0; i < A->pending.n; i++) free(A->pending.v[i]);
    VEC_FREE(A->pending);
    VEC_FREE(A->pending_pl);
    for (size_t i = 0; i < A->exports.n; i++) free(A->exports.v[i]);
    VEC_FREE(A->exports);
    VEC_FREE(A->export_pl);
    for (size_t i = 0; i < A->consts.n; i++) free(A->consts.v[i].data);
    VEC_FREE(A->consts);
    for (size_t i = 0; i < A->natives.n; i++) free(A->natives.v[i]);
    VEC_FREE(A->natives);
    for (size_t i = 0; i < A->grefs.n; i++) free(A->grefs.v[i].sym);
    VEC_FREE(A->grefs);
    for (size_t i = 0; i < A->ifuncs.n; i++) free(A->ifuncs.v[i].name);
    VEC_FREE(A->ifuncs);
    smap_free(&A->import_alias);
    smap_free(&A->funcmap);
    smap_free(&A->mod_aliases);
    smap_free(&A->symmap);
    smap_free(&A->constmap);
    smap_free(&A->nativemap);
    smap_free(&A->grefmap);
    smap_free(&A->ifuncmap);
    for (size_t i = 0; i < A->structs.n; i++) {
        MStruct *st = &A->structs.v[i];
        free(st->name);
        for (uint32_t k = 0; k < st->nfields; k++) free(st->fields[k]);
        free(st->fields);
        free(st->types);
    }
    VEC_FREE(A->structs);
    smap_free(&A->structmap);
    for (size_t i = 0; i < A->frefs.n; i++) free(A->frefs.v[i].field);
    VEC_FREE(A->frefs);
    smap_free(&A->frefmap);
    bb_free(&A->gdata);
    VEC_FREE(A->gtags);
    free(A->modname);
    pp_free(&A->pp);
}

Module *assemble_file(const char *path, const char *default_name, const char *const *libpaths, size_t nlib,
                      MlwsError *err) {
    Asm A;
    memset(&A, 0, sizeof A);
    A.err = err;
    pp_init(&A.pp, libpaths, nlib, err);
    A.modname = xstrdup(default_name ? default_name : "main");
    Module *m = NULL;
    if (pp_run_file(&A.pp, path)) {
        bool ok = true;
        for (size_t i = 0; ok && i < A.pp.out.n; i++) ok = statement(&A, &A.pp.out.v[i]);
        if (ok) m = finish(&A);
    }
    asm_free(&A);
    return m;
}

bool preprocess_file(const char *path, const char *const *libpaths, size_t nlib, StrBuf *out, MlwsError *err) {
    PP pp;
    pp_init(&pp, libpaths, nlib, err);
    bool ok = pp_run_file(&pp, path);
    if (ok) {
        const char *last_file = NULL;
        for (size_t i = 0; i < pp.out.n; i++) {
            const PLine *l = &pp.out.v[i];
            if (l->origin.file != last_file) {
                sb_printf(out, "; file %s\n", l->origin.file);
                last_file = l->origin.file;
            }
            const char *text = l->text;
            while (*text == ' ' || *text == '\t') text++;
            sb_printf(out, "%s%-56s ; %d%s%s\n", text[0] == '.' && strchr(text, ':') == NULL ? "" : "    ", text,
                      l->origin.line, l->macro ? " via " : "", l->macro ? l->macro : "");
        }
    }
    pp_free(&pp);
    return ok;
}
