/* sha256.h - SHA-256 (FIPS 180-4), used to pin trusted native extensions. */
#ifndef MLWSVM_SHA256_H
#define MLWSVM_SHA256_H

#include "common.h"

typedef struct {
    uint32_t h[8];
    uint64_t len; /* bytes so far */
    uint8_t buf[64];
    size_t nbuf;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
/* Hash a whole file as 64 lowercase hex digits; false if it cannot be read. */
bool sha256_file_hex(const char *path, char hex[65]);

#endif
