/* bytes.h - little-endian byte buffer writer/reader with LEB128 varints.
 * All MLWSVM binary formats are little-endian regardless of the host. */
#ifndef MLWSVM_BYTES_H
#define MLWSVM_BYTES_H

#include "common.h"

typedef struct {
    uint8_t *b;
    size_t n, cap;
} ByteBuf;

void bb_u8(ByteBuf *b, uint8_t v);
void bb_u16(ByteBuf *b, uint16_t v);
void bb_u32(ByteBuf *b, uint32_t v);
void bb_u64(ByteBuf *b, uint64_t v);
void bb_f64(ByteBuf *b, double v);
void bb_uleb(ByteBuf *b, uint64_t v);
void bb_sleb(ByteBuf *b, int64_t v);
void bb_bytes(ByteBuf *b, const void *p, size_t n);
void bb_str(ByteBuf *b, const char *s); /* uleb length + bytes */
void bb_free(ByteBuf *b);

typedef struct {
    const uint8_t *b;
    size_t n, pos;
    bool err; /* set on any out-of-bounds or malformed read */
} ByteReader;

uint8_t br_u8(ByteReader *r);
uint16_t br_u16(ByteReader *r);
uint32_t br_u32(ByteReader *r);
uint64_t br_u64(ByteReader *r);
double br_f64(ByteReader *r);
uint64_t br_uleb(ByteReader *r);
int64_t br_sleb(ByteReader *r);
bool br_bytes(ByteReader *r, void *out, size_t n);
char *br_str(ByteReader *r); /* malloc'd, NUL terminated; NULL on error */

#endif
