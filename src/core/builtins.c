/* builtins.c - see builtins.h */
#include "builtins.h"

static const char *const ERROR_FIELDS[] = {"code", "message", "trace"};
static const uint8_t ERROR_TYPES[] = {T_R, T_R, T_R};
static const char *const DOWN_FIELDS[] = {"pid", "exit", "code", "message"};
static const uint8_t DOWN_TYPES[] = {T_I64, T_I64, T_R, T_R};

const BuiltinStruct BUILTIN_STRUCTS[BUILTIN__COUNT] = {
    {"Error", 3, ERROR_FIELDS, ERROR_TYPES},
    {"Down", 4, DOWN_FIELDS, DOWN_TYPES},
};

int builtin_struct_find(const char *name) {
    for (int i = 0; i < BUILTIN__COUNT; i++)
        if (!strcmp(BUILTIN_STRUCTS[i].name, name)) return i;
    return -1;
}
