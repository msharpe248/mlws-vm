/* bundle.h - a program in one file: a copy of mlwsvm with the program's
 * modules after it, as `mlwsvm bundle` writes it. Layout in
 * docs/BINARY_FORMAT.md ("Bundles"). At start-up main() looks for one at the
 * end of its own executable; the loader then finds modules in it first. */
#ifndef MLWSVM_BUNDLE_H
#define MLWSVM_BUNDLE_H

#include "library.h"

#define MLWSB_MAGIC "MLWB"
#define MLWSB_VERSION 1
#define MLWSB_TRAILER "MLWSBNDL" /* the file's last 8 bytes, after the payload's length */
#define MLWSB_TRAILER_LEN 16

typedef struct Bundle {
    char *path;
    uint8_t *data;
    size_t len;
    char *root;        /* the module to run */
    LibEntry *mods;
    uint32_t nmods;
    char **opts;       /* VM options: name, value, name, value, ... */
    uint32_t nopts;
} Bundle;

/* The payload for a program: its root, VM options and modules (each already
 * checked). */
void bundle_write(const char *root, char *const *opts, uint32_t nopts, Module *const *mods, uint32_t nmods, ByteBuf *out);
/* The bundle at the end of an executable: NULL if it has none, or (err set)
 * if it is damaged. */
Bundle *bundle_read(const char *exe, MlwsError *err);
int bundle_find(const Bundle *b, const char *module); /* index, or -1 */
Module *bundle_module(const Bundle *b, uint32_t i, MlwsError *err);

#endif
