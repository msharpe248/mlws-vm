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
