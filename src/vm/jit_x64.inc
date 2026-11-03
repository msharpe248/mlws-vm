/* jit_x64.inc - the JIT's x86-64 encoder (jit.inc): its machine registers,
 * and the operations jit.inc writes the forms in (jm_*), as x86-64 code
 * (System V calls). jit_a64.inc is the same for arm64. */

/* Kept across C calls (callee-saved; jit_enter saves them): the Ctx, the I
 * window (the F and R windows follow it, at the function's roff), the slice's
 * instructions left (32 bits) and the current Frame. The Process and the code
 * memory's start are loaded where needed. r11 is the encoder's own. */
enum { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { JR_CTX = RBX, JR_WIN = R12, JR_LEFT = R13, JR_FR = R14, JR_TMP = R11 };
/* Scratch, for a form's own use (a shift's count goes through rcx, a
 * division through rax and rdx); C's argument and result registers. */
enum { JR_S0 = RAX, JR_S1 = RCX, JR_S2 = RDX, JR_S3 = R10, JR_S4 = R8 };
enum { JR_A0 = RDI, JR_A1 = RSI, JR_A2 = RDX, JR_A3 = RCX, JR_R0 = RAX, JR_R1 = RDX };
enum { JF_S0 = 0, JF_S1 = 1, JF_TMP = 15 };
#define JIT_ARCH "x86-64"
/* The register cache's machine registers (none of the above). */
#define JC_NI 5
#define JC_NF 13
#define JM_RCACHE 0 /* R registers in the cache's free slots (jc_rslot) */
/* The holes a loop may have and keep its registers (jit_loops): none, since
 * with 5 slots a hole cost more than the loop saved. */
#define JM_LOOP_HOLES 0
static const uint8_t jc_ireg[JC_NI] = {RSI, RDI, R9, RBP, R15};
static const uint8_t jc_freg[JC_NF] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

static void jb8(JBuf *b, unsigned v) {
    jb_need(b, 1);
    b->v[b->n++] = (uint8_t)v;
}
static void jb32(JBuf *b, uint32_t v) {
    jb_need(b, 4);
    memcpy(b->v + b->n, &v, 4);
    b->n += 4;
}
static void jb64(JBuf *b, uint64_t v) {
    jb_need(b, 8);
    memcpy(b->v + b->n, &v, 8);
    b->n += 8;
}

/* ---- encodings ----
 * An instruction: an optional prefix (66, F2, F3), REX when it needs one (W, a
 * register past 7, or a byte register past bl), the opcode bytes, then ModRM
 * with the reg field and a register (x_rr) or a memory operand, [base + disp]
 * or [base + idx*scale + disp] (x_mem). */
static void x_rex(JBuf *b, bool w, int r, int x, int bs, bool force) {
    unsigned v = 0x40u | (unsigned)w << 3 | (unsigned)(r >> 3 & 1) << 2 | (unsigned)(x >> 3 & 1) << 1 | (unsigned)(bs >> 3 & 1);
    if (v != 0x40u || force) jb8(b, v);
}
static void x_ops(JBuf *b, uint32_t op) { /* 1 to 3 bytes, most significant first */
    if (op > 0xFFFF) jb8(b, op >> 16);
    if (op > 0xFF) jb8(b, op >> 8 & 0xFF);
    jb8(b, op & 0xFF);
}
static void x_rr(JBuf *b, unsigned pfx, bool w, uint32_t op, int reg, int rm, bool bytes) {
    if (pfx) jb8(b, pfx);
    x_rex(b, w, reg, 0, rm, bytes && (reg >= 4 || rm >= 4));
    x_ops(b, op);
    jb8(b, 0xC0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7));
}
static void x_memx(JBuf *b, unsigned pfx, bool w, uint32_t op, int reg, int base, int idx, unsigned scale, int64_t disp64,
                   bool bytes) {
    int32_t disp = (int32_t)disp64; /* (offsets are small: a field, a window's register) */
    if (pfx) jb8(b, pfx);
    x_rex(b, w, reg, idx < 0 ? 0 : idx, base, bytes && reg >= 4);
    x_ops(b, op);
    unsigned mod = disp == 0 && (base & 7) != 5 ? 0 : disp >= -128 && disp <= 127 ? 1 : 2;
    if (idx < 0 && (base & 7) != 4) {
        jb8(b, mod << 6 | (unsigned)(reg & 7) << 3 | (unsigned)(base & 7));
    } else {
        jb8(b, mod << 6 | (unsigned)(reg & 7) << 3 | 4);
        jb8(b, (idx < 0 ? 0 : scale) << 6 | (unsigned)(idx < 0 ? 4 : idx & 7) << 3 | (unsigned)(base & 7));
    }
    if (mod == 1) jb8(b, (uint8_t)disp);
    else if (mod == 2) jb32(b, (uint32_t)disp);
}
static void x_mem(JBuf *b, unsigned pfx, bool w, uint32_t op, int reg, int base, int64_t disp, bool bytes) {
    x_memx(b, pfx, w, op, reg, base, -1, 0, disp, bytes);
}

/* ---- the operations ---- */
static void jm_ld(JBuf *b, int d, int n, int64_t off) { x_mem(b, 0, true, 0x8B, d, n, off, false); }
static void jm_st(JBuf *b, int s, int n, int64_t off) { x_mem(b, 0, true, 0x89, s, n, off, false); }
static void jm_stz(JBuf *b, int n, int64_t off) { /* mov qword [n+off], 0 */
    x_mem(b, 0, true, 0xC7, 0, n, off, false);
    jb32(b, 0);
}
static void jm_ld32(JBuf *b, int d, int n, int64_t off) { x_mem(b, 0, false, 0x8B, d, n, off, false); }
static void jm_st32(JBuf *b, int s, int n, int64_t off) { x_mem(b, 0, false, 0x89, s, n, off, false); }
static void jm_ld8(JBuf *b, int d, int n, int64_t off) { x_mem(b, 0, false, 0x0FB6, d, n, off, false); }
static void jm_st8(JBuf *b, int s, int n, int64_t off) { x_mem(b, 0, false, 0x88, s, n, off, true); }
static void jm_fld(JBuf *b, int d, int n, int64_t off) { x_mem(b, 0xF2, false, 0x0F10, d, n, off, false); }
static void jm_fst(JBuf *b, int s, int n, int64_t off) { x_mem(b, 0xF2, false, 0x0F11, s, n, off, false); }
static void jm_ldx(JBuf *b, int d, int n, int m) { x_memx(b, 0, true, 0x8B, d, n, m, 3, 0, false); }
static void jm_stx(JBuf *b, int s, int n, int m) { x_memx(b, 0, true, 0x89, s, n, m, 3, 0, false); }
static void jm_fldx(JBuf *b, int d, int n, int m) { x_memx(b, 0xF2, false, 0x0F10, d, n, m, 3, 0, false); }
static void jm_fstx(JBuf *b, int s, int n, int m) { x_memx(b, 0xF2, false, 0x0F11, s, n, m, 3, 0, false); }
static void jm_ld8x(JBuf *b, int d, int n, int m) { x_memx(b, 0, false, 0x0FB6, d, n, m, 0, 0, false); }
static void jm_st8x(JBuf *b, int s, int n, int m) { x_memx(b, 0, false, 0x88, s, n, m, 0, 0, true); }

static void jm_mov(JBuf *b, int d, int s) {
    if (d != s) x_rr(b, 0, true, 0x89, s, d, false);
}
static void jm_mov32(JBuf *b, int d, int s) { x_rr(b, 0, false, 0x89, s, d, false); }
static void jm_movi(JBuf *b, int d, uint64_t v) {
    if (v <= 0xFFFFFFFFu) { /* mov r32, imm32 (zero-extended) */
        x_rex(b, false, 0, 0, d, false);
        jb8(b, 0xB8u + (unsigned)(d & 7));
        jb32(b, (uint32_t)v);
    } else if ((int64_t)v >= INT32_MIN && (int64_t)v < 0) { /* mov r64, simm32 */
        x_rr(b, 0, true, 0xC7, 0, d, false);
        jb32(b, (uint32_t)v);
    } else { /* movabs */
        x_rex(b, true, 0, 0, d, false);
        jb8(b, 0xB8u + (unsigned)(d & 7));
        jb64(b, v);
    }
}

enum { JA_ADD, JA_SUB, JA_AND, JA_OR, JA_XOR, JA_MUL, JA_SHL, JA_SHR, JA_SAR };
static const uint32_t x_aluop[] = {0x03, 0x2B, 0x23, 0x0B, 0x33, 0x0FAF}; /* op r64, r/m64 */
static void jm_alu(JBuf *b, int op, int d, int a, int c) {
    if (op >= JA_SHL) { /* the count in cl, modulo 64 as shiftop */
        static const int ext[] = {4, 5, 7};
        jm_mov(b, JR_TMP, a);
        jm_mov(b, RCX, c);
        x_rr(b, 0, true, 0xD3, ext[op - JA_SHL], JR_TMP, false);
        jm_mov(b, d, JR_TMP);
        return;
    }
    bool comm = op != JA_SUB;
    if (d == a) {
        x_rr(b, 0, true, x_aluop[op], d, c, false);
    } else if (d == c && comm) {
        x_rr(b, 0, true, x_aluop[op], d, a, false);
    } else if (d == c) {
        jm_mov(b, JR_TMP, c);
        jm_mov(b, d, a);
        x_rr(b, 0, true, x_aluop[op], d, JR_TMP, false);
    } else {
        jm_mov(b, d, a);
        x_rr(b, 0, true, x_aluop[op], d, c, false);
    }
}
static void jm_alui(JBuf *b, int op, int d, int a, int64_t v) {
    if ((op == JA_ADD || op == JA_SUB) && v >= INT32_MIN && v <= INT32_MAX) {
        if (d != a && op == JA_ADD) { /* lea d, [a + v] */
            x_mem(b, 0, true, 0x8D, d, a, v, false);
            return;
        }
        jm_mov(b, d, a);
        bool small = v >= -128 && v <= 127;
        x_rr(b, 0, true, small ? 0x83 : 0x81, op == JA_ADD ? 0 : 5, d, false);
        if (small) jb8(b, (uint8_t)v);
        else jb32(b, (uint32_t)v);
        return;
    }
    jm_movi(b, JR_TMP, (uint64_t)v);
    jm_alu(b, op, d, a, JR_TMP);
}
static void x_alu32i(JBuf *b, unsigned ext, int d, uint32_t v) {
    bool small = v <= 127;
    x_rr(b, 0, false, small ? 0x83 : 0x81, (int)ext, d, false);
    if (small) jb8(b, v);
    else jb32(b, v);
}
static void jm_add32i(JBuf *b, int d, int a, uint32_t v) {
    jm_mov32(b, d, a);
    x_alu32i(b, 0, d, v);
}
static void jm_sub32i(JBuf *b, int d, int a, uint32_t v) {
    jm_mov32(b, d, a);
    x_alu32i(b, 5, d, v);
}
static void jm_subs32i(JBuf *b, int d, int a, uint32_t v) { jm_sub32i(b, d, a, v); } /* (sub sets the flags) */
static void jm_sub32(JBuf *b, int d, int a, int c) {
    if (d == c) {
        jm_mov32(b, JR_TMP, c);
        c = JR_TMP;
    }
    jm_mov32(b, d, a);
    x_rr(b, 0, false, 0x2B, d, c, false);
}
static void jm_addsh3(JBuf *b, int d, int n, int m) { x_memx(b, 0, true, 0x8D, d, n, m, 3, 0, false); }
/* d = a / k or a % k (k a constant, neither 0 nor -1: the quickener's DIV_IK
 * and REM_IK, so idiv cannot fault) */
static void jm_divk(JBuf *b, int d, int a, int64_t k, bool rem) {
    jm_mov(b, RAX, a);
    jm_movi(b, RCX, (uint64_t)k);
    jb8(b, 0x48), jb8(b, 0x99); /* cqo */
    x_rr(b, 0, true, 0xF7, 7, RCX, false);
    jm_mov(b, d, rem ? RDX : RAX);
}

enum { JC_EQ, JC_NE, JC_LT, JC_LE, JC_GT, JC_GE, JC_LO, JC_LS, JC_HI, JC_HS, JC_AL };
static const uint8_t x_cc[] = {4, 5, 0xC, 0xE, 0xF, 0xD, 2, 6, 7, 3, 0};
enum { JC_FEQ, JC_FNE, JC_FLT, JC_FLE, JC_FGT, JC_FGE };

static void jm_cmp(JBuf *b, int a, int c) { x_rr(b, 0, true, 0x3B, a, c, false); }
static void jm_cmpi(JBuf *b, int a, int64_t v) {
    if (v >= -128 && v <= 127) {
        x_rr(b, 0, true, 0x83, 7, a, false);
        jb8(b, (uint8_t)v);
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
        x_rr(b, 0, true, 0x81, 7, a, false);
        jb32(b, (uint32_t)v);
    } else {
        jm_movi(b, JR_TMP, (uint64_t)v);
        jm_cmp(b, a, JR_TMP);
    }
}
static void jm_cmp32(JBuf *b, int a, int c) { x_rr(b, 0, false, 0x3B, a, c, false); }
static void jm_cmpi32(JBuf *b, int a, uint32_t v) { x_alu32i(b, 7, a, v); }
static void x_setcc(JBuf *b, unsigned cc, int d) { x_rr(b, 0, false, 0x0F90u + cc, 0, d, true); }
static void x_movzx8(JBuf *b, int d, int s) { x_rr(b, 0, false, 0x0FB6, d, s, true); }
static void jm_cset(JBuf *b, int d, int cond) {
    x_setcc(b, x_cc[cond], d);
    x_movzx8(b, d, d);
}
static void jm_cselz(JBuf *b, int d, int a, int cond) { /* d = cond ? a : 0, the flags kept */
    if (d == a) {
        jm_mov(b, JR_TMP, a);
        a = JR_TMP;
    }
    jm_movi(b, d, 0); /* (mov, not xor: the flags stay) */
    x_rr(b, 0, true, 0x0F40u + x_cc[cond], d, a, false);
}

enum { JF_ADD, JF_SUB, JF_MUL, JF_DIV };
static void jm_fmov(JBuf *b, int d, int s) {
    if (d != s) x_rr(b, 0, false, 0x0F28, d, s, false); /* movaps */
}
static void jm_fop(JBuf *b, int op, int d, int a, int c) {
    static const uint32_t ops[] = {0x0F58, 0x0F5C, 0x0F59, 0x0F5E};
    bool comm = op == JF_ADD || op == JF_MUL;
    if (d == c && d != a && !comm) {
        jm_fmov(b, JF_TMP, c);
        c = JF_TMP;
    } else if (d == c && d != a) {
        c = a;
        a = d;
    }
    jm_fmov(b, d, a);
    x_rr(b, 0xF2, false, ops[op], d, c, false);
}
static void jm_fmov_from_x(JBuf *b, int d, int x) { x_rr(b, 0x66, true, 0x0F6E, d, x, false); }
static void jm_fmov_to_x(JBuf *b, int x, int d) { x_rr(b, 0x66, true, 0x0F7E, d, x, false); }
enum { JF_SQRT, JF_NEG, JF_ABS };
static void jm_fun(JBuf *b, int op, int d, int a) {
    if (op == JF_SQRT) {
        x_rr(b, 0xF2, false, 0x0F51, d, a, false);
        return;
    }
    jm_fmov_to_x(b, JR_TMP, a); /* the sign bit: btc (neg) or btr (abs) */
    x_rr(b, 0, true, 0x0FBA, op == JF_NEG ? 7 : 6, JR_TMP, false);
    jb8(b, 63);
    jm_fmov_from_x(b, d, JR_TMP);
}
static void jm_scvtf(JBuf *b, int d, int x) {
    x_rr(b, 0, false, 0x0F57, d, d, false); /* xorps: no dependence on d's old value */
    x_rr(b, 0xF2, true, 0x0F2A, d, x, false);
}
static void jm_fconst(JBuf *b, int d, double f) {
    uint64_t bits;
    memcpy(&bits, &f, 8);
    if (!bits) {
        x_rr(b, 0, false, 0x0F57, d, d, false);
        return;
    }
    jm_movi(b, JR_TMP, bits);
    jm_fmov_from_x(b, d, JR_TMP);
}
static void jm_ucomi(JBuf *b, int a, int c) { x_rr(b, 0x66, false, 0x0F2E, a, c, false); }
static void jm_fcset(JBuf *b, int d, int fcond, int a, int c) {
    switch (fcond) {
    case JC_FEQ: case JC_FNE:
        jm_ucomi(b, a, c);
        x_setcc(b, fcond == JC_FEQ ? 4 : 5, d);   /* sete / setne */
        x_setcc(b, fcond == JC_FEQ ? 0xB : 0xA, JR_TMP); /* setnp / setp */
        x_rr(b, 0, false, fcond == JC_FEQ ? 0x20 : 0x08, JR_TMP, d, true); /* and / or d8, r11b */
        break;
    case JC_FLT: case JC_FLE: jm_ucomi(b, c, a), x_setcc(b, fcond == JC_FLT ? 7 : 3, d); break;
    default: jm_ucomi(b, a, c), x_setcc(b, fcond == JC_FGT ? 7 : 3, d); break;
    }
    x_movzx8(b, d, d);
}

static void jm_ccall(JBuf *b, const void *fn) {
    jm_movi(b, RAX, (uint64_t)(uintptr_t)fn);
    x_rr(b, 0, false, 0xFF, 2, RAX, false); /* call rax */
}
static void jm_jmpr(JBuf *b, int r) { x_rr(b, 0, false, 0xFF, 4, r, false); }
static void jm_brk(JBuf *b) { jb8(b, 0xCC); }

/* ---- branches ----
 * Every branch has a 32-bit displacement; a branch's place is that field's. */
enum { FX_B, FX_BL, FX_BCOND, FX_CB };
static uint32_t jm_emit_branch(JBuf *b, int kind, int cond, int reg, bool nz) {
    switch (kind) {
    case FX_B: jb8(b, 0xE9); break;
    case FX_BL: jb8(b, 0xE8); break;
    case FX_BCOND: jb8(b, 0x0F), jb8(b, 0x80u + x_cc[cond]); break;
    default:
        x_rr(b, 0, true, 0x85, reg, reg, false); /* test reg, reg */
        jb8(b, 0x0F), jb8(b, nz ? 0x85 : 0x84);
        break;
    }
    uint32_t at = (uint32_t)b->n;
    jb32(b, 0);
    return at;
}
static void jm_patch_at(uint8_t *p, uint32_t at, int64_t dest) {
    int32_t rel = (int32_t)(dest - ((int64_t)at + 4));
    memcpy(p + at, &rel, 4);
}
static uint32_t jm_fwd(JBuf *b, int cond) { return jm_emit_branch(b, cond == JC_AL ? FX_B : FX_BCOND, cond, 0, false); }
static uint32_t jm_fwd_cbz(JBuf *b, int reg, bool nz) { return jm_emit_branch(b, FX_CB, 0, reg, nz); }
static void jm_bind(JBuf *b, uint32_t at) { jm_patch_at(b->v, at, (int64_t)b->n); }
/* A float compare and branch: one or two places for the caller to fix up (an
 * equality's NaN case is the parity flag's). */
static int jm_fbranch(JBuf *b, int fcond, int a, int c, uint32_t at[2]) {
    switch (fcond) {
    case JC_FEQ: {
        jm_ucomi(b, a, c);
        jb8(b, 0x7A), jb8(b, 6); /* jp: past the je (6 bytes) */
        jb8(b, 0x0F), jb8(b, 0x84);
        at[0] = (uint32_t)b->n;
        jb32(b, 0);
        return 1;
    }
    case JC_FNE:
        jm_ucomi(b, a, c);
        jb8(b, 0x0F), jb8(b, 0x8A); /* jp */
        at[0] = (uint32_t)b->n;
        jb32(b, 0);
        jb8(b, 0x0F), jb8(b, 0x85); /* jne */
        at[1] = (uint32_t)b->n;
        jb32(b, 0);
        return 2;
    case JC_FLT: case JC_FLE:
        jm_ucomi(b, c, a);
        jb8(b, 0x0F), jb8(b, fcond == JC_FLT ? 0x87 : 0x83); /* ja / jae */
        break;
    default:
        jm_ucomi(b, a, c);
        jb8(b, 0x0F), jb8(b, fcond == JC_FGT ? 0x87 : 0x83);
        break;
    }
    at[0] = (uint32_t)b->n;
    jb32(b, 0);
    return 1;
}

/* ---- calls and returns between machine code ----
 * A call is a `call` (after 8 bytes of padding, so the stack stays aligned
 * for C), and its continuation follows it: `add rsp, 8` drops the padding.
 * A return compares the word on top of the stack with its frame's
 * continuation (the code memory's start plus the call's Insn.target): equal,
 * it was pushed by the call from machine code that made this frame, and `ret`
 * goes there, as the CPU's return predictor expects; otherwise (a frame
 * another way made, or the stack since reset) it jumps to the continuation
 * past the `add`. The stack holds only words calls pushed and jit_enter's
 * zero, so the check is exact. A call stops for the interpreter when the
 * stack is near its limit (deep recursion). */
#define JM_CONT_SKIP 4
static void x_pad(JBuf *b) { /* sub rsp, 8 */
    jb8(b, 0x48), jb8(b, 0x83), jb8(b, 0xEC), jb8(b, 8);
}
static void jm_ncall_reg(JBuf *b, int r) {
    x_pad(b);
    x_rr(b, 0, false, 0xFF, 2, r, false); /* call r */
}
static uint32_t jm_ncall_self(JBuf *b) {
    x_pad(b);
    return jm_emit_branch(b, FX_BL, 0, 0, false);
}
static void jm_cont(JBuf *b) { jb8(b, 0x48), jb8(b, 0x83), jb8(b, 0xC4), jb8(b, 8); } /* add rsp, 8 */
static void jm_nret(JBuf *b, int off, const uint8_t *mem) {
    jm_movi(b, JR_TMP, (uint64_t)(uintptr_t)mem);
    x_rr(b, 0, true, 0x03, JR_TMP, off, false); /* add r11, off */
    x_mem(b, 0, true, 0x3B, JR_TMP, RSP, 0, false); /* cmp r11, [rsp] */
    jb8(b, 0x75), jb8(b, 1); /* jne: past the ret */
    jb8(b, 0xC3);
    jm_alui(b, JA_ADD, JR_TMP, JR_TMP, JM_CONT_SKIP);
    jm_jmpr(b, JR_TMP);
}

/* jtab: a table of 8-byte entries, each a `jmp` (5 bytes and 3 of padding). */
static void jm_jtab(JBuf *b, int idx, uint32_t n, uint32_t *at, uint32_t *oob) {
    jm_cmpi(b, idx, n);
    *oob = jm_emit_branch(b, FX_BCOND, JC_HS, 0, false);
    jb8(b, 0x48), jb8(b, 0x8D), jb8(b, 0x05); /* lea rax, [rip + disp32] */
    size_t disp_at = b->n;
    jb32(b, 0);
    x_memx(b, 0, true, 0x8D, RAX, RAX, idx, 3, 0, false); /* lea rax, [rax + idx*8] */
    jm_jmpr(b, RAX);
    while (b->n % 8) jb8(b, 0x90);
    int32_t rel = (int32_t)(b->n - (disp_at + 4));
    memcpy(b->v + disp_at, &rel, 4);
    for (uint32_t k = 0; k < n; k++) {
        at[k] = jm_emit_branch(b, FX_B, 0, 0, false);
        jb8(b, 0x90), jb8(b, 0x90), jb8(b, 0x90);
    }
}

/* ---- the entry and the exit ----
 * jit_enter(c, left, target): the callee-saved registers, then the Ctx's
 * jbase and jlimit (kept for a nested entry), then a zero word: the stack's
 * base in machine code, which no continuation matches. The exit (rax: the
 * instruction to stop at) resets the stack to the base, from any depth of
 * calls, and returns {rax, left} in rax:rdx. */
#define JIT_STACK_MAX (256u << 10) /* bytes of calls between machine code, before the interpreter takes them */
static void x_push(JBuf *b, int r) {
    if (r >= 8) jb8(b, 0x41);
    jb8(b, 0x50u + (unsigned)(r & 7));
}
static void x_pop(JBuf *b, int r) {
    if (r >= 8) jb8(b, 0x41);
    jb8(b, 0x58u + (unsigned)(r & 7));
}
static void jm_tramp(JBuf *b, size_t *exit_at, const uint8_t *mem) {
    (void)mem;
    static const int saved[] = {RBP, RBX, R12, R13, R14, R15};
    for (int k = 0; k < 6; k++) x_push(b, saved[k]);
    jm_mov(b, JR_CTX, RDI);
    x_mem(b, 0, false, 0xFF, 6, JR_CTX, offsetof(Ctx, jbase), false); /* push [rbx + jbase] */
    x_mem(b, 0, false, 0xFF, 6, JR_CTX, offsetof(Ctx, jlimit), false);
    jb8(b, 0x6A), jb8(b, 0); /* push 0 */
    x_mem(b, 0, true, 0x89, RSP, JR_CTX, offsetof(Ctx, jbase), false);
    x_mem(b, 0, true, 0x8D, RAX, RSP, -(int64_t)JIT_STACK_MAX, false);
    jm_st(b, RAX, JR_CTX, offsetof(Ctx, jlimit));
    jm_ld(b, JR_WIN, JR_CTX, offsetof(Ctx, I));
    jm_ld(b, JR_FR, JR_CTX, offsetof(Ctx, fr));
    jm_mov32(b, JR_LEFT, RSI);
    jm_jmpr(b, RDX);
    *exit_at = b->n;
    jm_ld(b, RSP, JR_CTX, offsetof(Ctx, jbase));
    jm_alui(b, JA_ADD, RSP, RSP, 8);
    x_mem(b, 0, false, 0x8F, 0, JR_CTX, offsetof(Ctx, jlimit), false); /* pop [rbx + jlimit] */
    x_mem(b, 0, false, 0x8F, 0, JR_CTX, offsetof(Ctx, jbase), false);
    jm_mov32(b, RDX, JR_LEFT);
    for (int k = 5; k >= 0; k--) x_pop(b, saved[k]);
    jb8(b, 0xC3);
}
static int jm_proc(JBuf *b, int tmp) {
    jm_ld(b, tmp, JR_CTX, offsetof(Ctx, p));
    return tmp;
}
static void jm_reload(JBuf *b) {
    jm_ld(b, JR_FR, JR_CTX, offsetof(Ctx, fr));
    jm_ld(b, JR_WIN, JR_CTX, offsetof(Ctx, I));
}
/* A stepped instruction that popped frames (a return, a throw) stops: the
 * words their calls pushed are still on the stack. */
#define JM_STEP_POP_STOPS 1
