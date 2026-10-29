/* arith.h - the value semantics of the pure arithmetic, bitwise, shift,
 * compare and conversion instructions, on the 64-bit words registers hold.
 * The interpreter executes with these, and the optimizer folds constants with
 * them, so a folded result is exactly what the instruction would compute.
 * Integer division by zero is the caller's to trap (or not to fold). */
#ifndef MLWSVM_ARITH_H
#define MLWSVM_ARITH_H

#include <math.h>

#include "opcodes.h"

static inline uint64_t arith_width_mask(MlwsType t) {
    int bits = type_bits(t);
    return bits == 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
}

static inline Word arith_binop(int op, MlwsType t, Word a, Word b) {
    Word r;
    if (type_is_float(t)) {
        double x = a.f, y = b.f, z;
        switch (op) {
        case OP_ADD: z = x + y; break;
        case OP_SUB: z = x - y; break;
        case OP_MUL: z = x * y; break;
        case OP_DIV: z = x / y; break;
        case OP_REM: z = fmod(x, y); break;
        case OP_MIN: z = (isnan(x) || isnan(y)) ? NAN : (x < y ? x : y); break;
        default: z = (isnan(x) || isnan(y)) ? NAN : (x > y ? x : y); break;
        }
        r.f = canon_float(t, z);
        return r;
    }
    bool u = !type_is_signed(t);
    uint64_t x = a.u, y = b.u;
    switch (op) {
    case OP_ADD: r.u = x + y; break;
    case OP_SUB: r.u = x - y; break;
    case OP_MUL: r.u = x * y; break;
    case OP_DIV:
    case OP_REM:
        if (u) r.u = op == OP_DIV ? x / y : x % y;
        else if (a.i == INT64_MIN && b.i == -1) r.i = op == OP_DIV ? INT64_MIN : 0;
        else r.i = op == OP_DIV ? a.i / b.i : a.i % b.i;
        break;
    case OP_MIN: r = (u ? x < y : a.i < b.i) ? a : b; break;
    case OP_MAX: r = (u ? x > y : a.i > b.i) ? a : b; break;
    case OP_AND: r.u = x & y; break;
    case OP_OR: r.u = x | y; break;
    default: r.u = x ^ y; break;
    }
    r.i = canon_int(t, r.i);
    return r;
}

static inline int arith_popcount64(uint64_t v) {
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

static inline Word arith_unop(int op, MlwsType t, Word a) {
    Word r;
    if (type_is_float(t)) {
        double x = a.f, z;
        switch (op) {
        case OP_NEG: z = -x; break;
        case OP_ABS: z = fabs(x); break;
        case OP_SQRT: z = sqrt(x); break;
        case OP_FLOOR: z = floor(x); break;
        case OP_CEIL: z = ceil(x); break;
        case OP_ROUND: z = round(x); break;
        default: z = trunc(x); break;
        }
        r.f = canon_float(t, z);
        return r;
    }
    int bits = type_bits(t);
    uint64_t v = a.u & arith_width_mask(t);
    switch (op) {
    case OP_NEG: r.u = 0 - a.u; break;
    case OP_ABS: r.u = (type_is_signed(t) && a.i < 0) ? 0 - a.u : a.u; break;
    case OP_NOT: r.u = t == T_B ? (uint64_t)(a.u == 0) : ~a.u; break;
    case OP_POPCNT: r.i = arith_popcount64(v); break;
    case OP_CLZ: {
        int n = 0;
        for (int i = bits - 1; i >= 0 && !((v >> i) & 1); i--) n++;
        r.i = n;
        break;
    }
    case OP_CTZ: {
        int n = 0;
        while (n < bits && !((v >> n) & 1)) n++;
        r.i = n;
        break;
    }
    default: { /* BSWAP */
        uint64_t out = 0;
        int bytes = bits / 8;
        for (int i = 0; i < bytes; i++) out |= ((v >> (8 * i)) & 0xFF) << (8 * (bytes - 1 - i));
        r.u = out;
        break;
    }
    }
    r.i = canon_int(t, r.i);
    return r;
}

static inline Word arith_shift(int op, MlwsType t, Word a, Word n) {
    int bits = type_bits(t);
    uint64_t mask = arith_width_mask(t);
    unsigned s = (unsigned)(n.u % (uint64_t)bits);
    uint64_t v = a.u & mask;
    Word r;
    switch (op) {
    case OP_SHL: r.u = v << s; break;
    case OP_SHR: r.u = v >> s; break;
    case OP_SAR: {
        int64_t sv = (int64_t)(v << (64 - bits)) >> (64 - bits);
        r.i = sv >> s;
        break;
    }
    case OP_ROL: r.u = s ? ((v << s) | (v >> (bits - (int)s))) & mask : v; break;
    default: r.u = s ? ((v >> s) | (v << (bits - (int)s))) & mask : v; break;
    }
    r.i = canon_int(t, r.i);
    return r;
}

static inline bool arith_cmp_eq(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f == b.f;
    if (t == T_R) return a.r == b.r;
    return a.u == b.u;
}

static inline bool arith_cmp_lt(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f < b.f;
    if (type_is_signed(t)) return a.i < b.i;
    return a.u < b.u;
}

static inline bool arith_cmp_le(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f <= b.f;
    if (type_is_signed(t)) return a.i <= b.i;
    return a.u <= b.u;
}

static inline bool arith_compare(int op, MlwsType t, Word a, Word b) {
    switch (op) {
    case OP_SEQ: case OP_BEQ: return arith_cmp_eq(t, a, b);
    case OP_SNE: case OP_BNE: return !arith_cmp_eq(t, a, b);
    case OP_SLT: case OP_BLT: return arith_cmp_lt(t, a, b);
    case OP_SLE: case OP_BLE: return arith_cmp_le(t, a, b);
    case OP_SGT: case OP_BGT: return arith_cmp_lt(t, b, a);
    default: return arith_cmp_le(t, b, a);
    }
}

static inline Word arith_convert(MlwsType to, MlwsType from, Word v) {
    Word r;
    if (type_is_float(from)) {
        double d = v.f;
        if (type_is_float(to)) {
            r.f = canon_float(to, d);
        } else if (to == T_B) {
            r.i = d != 0;
        } else if (isnan(d)) {
            r.i = 0;
        } else {
            d = trunc(d);
            int bits = type_bits(to);
            if (type_is_signed(to)) {
                double hi = ldexp(1.0, bits - 1); /* exclusive */
                if (d >= hi) r.i = (int64_t)((((uint64_t)1) << (bits - 1)) - 1);
                else if (d < -hi) r.i = (int64_t)(0 - (((uint64_t)1) << (bits - 1)));
                else r.i = (int64_t)d;
            } else {
                double hi = ldexp(1.0, bits);
                if (d <= 0) r.u = 0;
                else if (d >= hi) r.u = arith_width_mask(to);
                else r.u = (uint64_t)d;
            }
            r.i = canon_int(to, r.i);
        }
        return r;
    }
    if (type_is_float(to)) r.f = canon_float(to, from == T_U64 ? (double)v.u : (double)v.i);
    else r.i = canon_int(to, v.i);
    return r;
}

static inline Word arith_bitcast(MlwsType to, MlwsType from, Word v) {
    uint64_t bits;
    if (from == T_F32) {
        float f = (float)v.f;
        uint32_t u;
        memcpy(&u, &f, 4);
        bits = u;
    } else if (from == T_F64) {
        memcpy(&bits, &v.f, 8);
    } else {
        bits = v.u & arith_width_mask(from);
    }
    Word r;
    if (to == T_F32) {
        uint32_t u = (uint32_t)bits;
        float f;
        memcpy(&f, &u, 4);
        r.f = f;
    } else if (to == T_F64) {
        memcpy(&r.f, &bits, 8);
    } else {
        r.i = canon_int(to, (int64_t)bits);
    }
    return r;
}

#endif
