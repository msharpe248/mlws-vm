/* bytes.c - see bytes.h */
#include "bytes.h"

static void bb_reserve(ByteBuf *b, size_t extra) {
    if (b->n + extra > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + extra) nc *= 2;
        b->b = xrealloc(b->b, nc);
        b->cap = nc;
    }
}

void bb_u8(ByteBuf *b, uint8_t v) {
    bb_reserve(b, 1);
    b->b[b->n++] = v;
}

void bb_u16(ByteBuf *b, uint16_t v) {
    bb_u8(b, (uint8_t)v);
    bb_u8(b, (uint8_t)(v >> 8));
}

void bb_u32(ByteBuf *b, uint32_t v) {
    for (int i = 0; i < 4; i++) bb_u8(b, (uint8_t)(v >> (8 * i)));
}

void bb_u64(ByteBuf *b, uint64_t v) {
    for (int i = 0; i < 8; i++) bb_u8(b, (uint8_t)(v >> (8 * i)));
}

void bb_f64(ByteBuf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    bb_u64(b, u);
}

void bb_uleb(ByteBuf *b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        bb_u8(b, byte);
    } while (v);
}

void bb_sleb(ByteBuf *b, int64_t v) {
    bool more = true;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7; /* arithmetic shift on all supported compilers */
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) more = false;
        else byte |= 0x80;
        bb_u8(b, byte);
    }
}

void bb_bytes(ByteBuf *b, const void *p, size_t n) {
    bb_reserve(b, n);
    if (n) memcpy(b->b + b->n, p, n);
    b->n += n;
}

void bb_str(ByteBuf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    bb_uleb(b, n);
    bb_bytes(b, s, n);
}

void bb_free(ByteBuf *b) {
    free(b->b);
    memset(b, 0, sizeof *b);
}

uint8_t br_u8(ByteReader *r) {
    if (r->pos >= r->n) {
        r->err = true;
        return 0;
    }
    return r->b[r->pos++];
}

uint16_t br_u16(ByteReader *r) {
    uint16_t lo = br_u8(r);
    return (uint16_t)(lo | (uint16_t)br_u8(r) << 8);
}

uint32_t br_u32(ByteReader *r) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)br_u8(r) << (8 * i);
    return v;
}

uint64_t br_u64(ByteReader *r) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)br_u8(r) << (8 * i);
    return v;
}

double br_f64(ByteReader *r) {
    uint64_t u = br_u64(r);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

uint64_t br_uleb(ByteReader *r) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t byte = br_u8(r);
        if (r->err) return 0;
        v |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

int64_t br_sleb(ByteReader *r) {
    uint64_t v = 0;
    int shift = 0;
    uint8_t byte;
    do {
        if (shift >= 70) {
            r->err = true;
            return 0;
        }
        byte = br_u8(r);
        if (r->err) return 0;
        if (shift < 64) v |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) v |= ~(uint64_t)0 << shift;
    return (int64_t)v;
}

bool br_bytes(ByteReader *r, void *out, size_t n) {
    if (n > r->n - r->pos || r->pos > r->n) {
        r->err = true;
        return false;
    }
    if (n) memcpy(out, r->b + r->pos, n);
    r->pos += n;
    return true;
}

char *br_str(ByteReader *r) {
    uint64_t n = br_uleb(r);
    if (r->err || n > r->n - r->pos) {
        r->err = true;
        return NULL;
    }
    char *s = xstrndup((const char *)r->b + r->pos, (size_t)n);
    r->pos += (size_t)n;
    return s;
}
