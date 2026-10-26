/* types.c - see types.h */
#include "types.h"

static const char *const TYPE_NAMES[T__COUNT] = {
    "none", "b", "c", "i8", "i16", "i32", "i64", "u8", "u16",
    "u32", "u64", "f32", "f64", "r", "p", "big", "dec",
};

const char *type_name(MlwsType t) { return t < T__COUNT ? TYPE_NAMES[t] : "?"; }

bool type_from_name(const char *s, size_t n, MlwsType *out) {
    static const struct {
        const char *name;
        MlwsType t;
    } aliases[] = {
        {"bool", T_B}, {"char", T_C}, {"ref", T_R}, {"ptr", T_P},
    };
    for (int t = 1; t < T__COUNT; t++) {
        if (strlen(TYPE_NAMES[t]) == n && memcmp(TYPE_NAMES[t], s, n) == 0) {
            *out = (MlwsType)t;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        if (strlen(aliases[i].name) == n && memcmp(aliases[i].name, s, n) == 0) {
            *out = aliases[i].t;
            return true;
        }
    }
    return false;
}

int type_size(MlwsType t) {
    switch (t) {
    case T_B: case T_I8: case T_U8: return 1;
    case T_I16: case T_U16: return 2;
    case T_C: case T_I32: case T_U32: case T_F32: return 4;
    default: return 8;
    }
}

int type_bits(MlwsType t) { return type_size(t) * 8; }

Bank type_bank(MlwsType t) {
    if (type_is_int(t)) return BANK_I;
    if (type_is_float(t)) return BANK_F;
    if (t == T_P) return BANK_P;
    return BANK_R;
}

bool type_is_int(MlwsType t) { return t >= T_B && t <= T_U64; }
bool type_is_float(MlwsType t) { return t == T_F32 || t == T_F64; }
bool type_is_signed(MlwsType t) { return t >= T_I8 && t <= T_I64; }

MlwsType bank_natural_type(Bank b) {
    switch (b) {
    case BANK_I: return T_I64;
    case BANK_F: return T_F64;
    case BANK_R: return T_R;
    case BANK_P: return T_P;
    default: return T_NONE;
    }
}

const char *bank_prefix(Bank b) {
    static const char *const p[] = {"I", "F", "R", "P", "S"};
    return b <= BANK_S ? p[b] : "?";
}

int64_t canon_int(MlwsType t, int64_t v) {
    switch (t) {
    case T_B: return v != 0;
    case T_C: return (int64_t)(uint32_t)v;
    case T_I8: return (int8_t)v;
    case T_I16: return (int16_t)v;
    case T_I32: return (int32_t)v;
    case T_U8: return (uint8_t)v;
    case T_U16: return (uint16_t)v;
    case T_U32: return (int64_t)(uint32_t)v;
    default: return v;
    }
}

double canon_float(MlwsType t, double v) { return t == T_F32 ? (double)(float)v : v; }

const char *reg_name(uint8_t reg, char buf[8]) {
    int bank = REG_BANK(reg), idx = REG_IDX(reg);
    if (bank == BANK_S) {
        static const char *const s[] = {"SP", "FP", "PID"};
        return idx < SREG__COUNT ? s[idx] : "S?";
    }
    snprintf(buf, 8, "%s%d", bank_prefix((Bank)bank), idx);
    return buf;
}

MlwsType reg_natural_type(uint8_t reg) {
    int bank = REG_BANK(reg);
    if (bank == BANK_S) return REG_IDX(reg) == SREG_PID ? T_I64 : T_P;
    return bank_natural_type((Bank)bank);
}
