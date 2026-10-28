/* library.h - library archives (.mlwsl): a library's manifest and its
 * modules in one file. Layout in docs/BINARY_FORMAT.md ("Library archives");
 * built by `mlwsvm lib build`, read by the loader (src/vm/loader.c). */
#ifndef MLWSVM_LIBRARY_H
#define MLWSVM_LIBRARY_H

#include "../core/bytes.h"
#include "../core/module.h"
#include "../core/semver.h"

#define MLWSL_MAGIC "MLWL"
#define MLWSL_VERSION 1

typedef struct {
    char *name;         /* a module of the library */
    uint32_t off, len;  /* its .mlwso image, within LibArchive.data */
} LibEntry;

typedef struct LibArchive {
    char *path;
    Module *manifest;   /* lib_name, lib_version, requires, publics */
    SemVer version;
    uint8_t *data;
    size_t len;
    LibEntry *mods;
    uint32_t nmods;
} LibArchive;

/* An archive of a manifest and its modules (each already checked). */
void lib_write(const Module *manifest, Module *const *mods, uint32_t nmods, ByteBuf *out);
/* Read and check an archive file (its modules are read when asked for). */
LibArchive *lib_read(const char *path, MlwsError *err);
/* Module i of the archive, or NULL with err set. */
Module *lib_module(const LibArchive *a, uint32_t i, MlwsError *err);
int lib_find(const LibArchive *a, const char *module); /* index, or -1 */
bool lib_is_public(const LibArchive *a, const char *module);
void lib_free(LibArchive *a);
/* Whether a module belongs to a library by its name: `lib` or `lib.…`. */
bool lib_owns(const char *lib, const char *module);

#endif
