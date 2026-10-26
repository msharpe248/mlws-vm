/* insn.h - portable instruction encoding.
 *
 *   insn    := opcode:u16le type:u8 [type2:u8 if OPF_TYPE2] nops:u8 operand*
 *   operand := mode:u8 payload
 *
 * Displacements and immediates are signed LEB128 and indices unsigned LEB128,
 * so the encoding is compact and has no alignment requirements. See
 * docs/BINARY_FORMAT.md and docs/ADDRESSING.md. */
#ifndef MLWSVM_INSN_H
#define MLWSVM_INSN_H

#include "bytes.h"
#include "opcodes.h"

/* Addressing modes (on-disk mode byte). "w" = word units (8 bytes), "b" = bytes. */
typedef enum {
    OM_REG = 0x01,        /* reg:u8                                   I3, SP        */
    OM_IMM = 0x02,        /* value:sleb                               42, 'a'       */
    OM_FIMM = 0x03,       /* value:f64le                              3.5           */
    OM_CONST = 0x04,      /* const:uleb                               "text"        */
    OM_IND = 0x05,        /* reg:u8                                   [P1]          */
    OM_DISP = 0x06,       /* reg:u8 disp:sleb (w)                     SP[42]        */
    OM_DISPB = 0x07,      /* reg:u8 disp:sleb (b)                     P1{13}        */
    OM_IDX = 0x08,        /* reg:u8 ireg:u8 scale:uleb disp:sleb (w)  P1[I2*2+1]    */
    OM_IDXB = 0x09,       /* same, byte units                         P1{I2*4+8}    */
    OM_POSTINC = 0x0A,    /* reg:u8 ; step 1 word                     [SP]+         */
    OM_PREDEC = 0x0B,     /* reg:u8 ; step 1 word                     -[SP]         */
    OM_POSTINCB = 0x0C,   /* reg:u8 ; step = access width             {P1}+         */
    OM_PREDECB = 0x0D,    /* reg:u8 ; step = access width             -{P1}         */
    OM_GLOBAL = 0x0E,     /* gref:uleb disp:sleb (w)                  @x, @x[3]     */
    OM_GLOBALB = 0x0F,    /* gref:uleb disp:sleb (b)                  @x{3}         */
    OM_GLOBALIDX = 0x10,  /* gref:uleb ireg:u8 scale:uleb disp:sleb (w) @x[I1]      */
    OM_GLOBALIDXB = 0x11, /* same, byte units                         @x{I1*4}      */
    OM_MIND = 0x12,       /* inner:operand disp:sleb (w)              [FP[2]][3]    */
    OM_MINDB = 0x13,      /* inner:operand disp:sleb (b)              [FP[2]]{3}    */
    OM_LABEL = 0x14,      /* insn:uleb (index within the function)    loop          */
    OM_FUNC = 0x15,       /* func:uleb (module function table)        fib, io.print */
    OM_NATIVE = 0x16,     /* native:uleb (module native table)        "io.print_i"  */
    OM_ELEM = 0x17,       /* reg:u8 ireg:u8                           R2[I4]        */
    OM_ELEMI = 0x18,      /* reg:u8 index:uleb                        R2[7]         */
    OM_FIELD = 0x19,      /* reg:u8 fref:uleb (module field refs)     R2->Point.x   */
    OM_STRUCT = 0x1A,     /* struct:uleb (module struct table)        Point         */
} OperandMode;

typedef struct Operand {
    uint8_t mode;
    uint8_t reg;    /* register / base register */
    uint8_t ireg;   /* index register */
    uint8_t pad;
    int64_t imm;    /* OM_IMM value, or displacement in encoded units */
    uint64_t scale; /* index scale in encoded units */
    uint64_t index; /* const / gref / label / func / native index */
    double f;       /* OM_FIMM */
    struct Operand *inner; /* OM_MIND* */
    /* Filled in by the loader: */
    int64_t bdisp;  /* displacement in bytes */
    int64_t bscale; /* scale in bytes */
    union {
        void *ptr;      /* const object, Function*, NativeDef* */
        uint64_t gbase; /* resolved global segment pointer */
        uint32_t target;
    } rv;
} Operand;

typedef struct {
    uint16_t op; /* dense Opcode */
    uint8_t type, type2;
    uint8_t nops;
    Operand *ops;
} Insn;

bool mode_is_memory(uint8_t mode);
bool mode_is_object(uint8_t mode);   /* element or field of a heap object */
bool mode_is_writable(uint8_t mode); /* register, memory or object slot */

void encode_operand(ByteBuf *b, const Operand *o);
void encode_insn(ByteBuf *b, const Insn *in);
/* Decode one instruction. Operand arrays are malloc'd; free with insn_free. */
bool decode_insn(ByteReader *r, Insn *out, MlwsError *err);
void insn_free(Insn *in);

#endif
