/* regex.h - regular expressions that run in time linear in the text, whatever
 * the pattern: a Pike VM, which runs every way of matching at once, so a
 * pattern cannot be made to take exponential time. There are therefore no
 * backreferences or lookaround. Text is UTF-8; an invalid byte reads as one
 * U+FFFD. The syntax is in docs/STDLIB.md. */
#ifndef MLWSVM_REGEX_H
#define MLWSVM_REGEX_H

#include "common.h"

typedef struct Regex Regex;

enum { REGEX_ICASE = 1, REGEX_MULTILINE = 2, REGEX_DOTALL = 4 };

/* NULL: why, in err. */
Regex *regex_compile(const char *pattern, size_t n, unsigned flags, char *err, size_t errlen);
void regex_free(Regex *re);
int regex_groups(const Regex *re);                 /* the capturing groups, not counting the whole match */
int regex_group_named(const Regex *re, const char *name); /* its number, or -1 */
const char *regex_group_name(const Regex *re, int group); /* a group's name (1: the first group), or NULL */

enum { REGEX_NONE = 0, REGEX_MATCH = 1, REGEX_OUT_OF_STEPS = -1 };

/* The leftmost match at or after byte `start`, the first of its alternatives
 * by the pattern's preference. caps gets 2 * (groups + 1) byte offsets, a
 * group's start and end, -1 for one that took no part. *steps is how much work
 * it may do, and is reduced by what it did. */
int regex_search(const Regex *re, const char *s, size_t n, size_t start, int64_t *caps, uint64_t *steps);

#endif
