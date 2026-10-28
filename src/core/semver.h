/* semver.h - versions (MAJOR.MINOR.PATCH) and the constraints libraries state
 * on each other (docs/ASSEMBLY.md, "Libraries"). A constraint is one or more
 * terms separated by commas, all of which must hold:
 *
 *   1.2.3, =1.2.3   exactly that version
 *   ^1.2.3          compatible: >=1.2.3 and <2.0.0 (^0.4.1: <0.5.0; ^0.0.3: <0.0.4)
 *   ~1.2.3          the same minor version: >=1.2.3 and <1.3.0
 *   >=, >, <=, <    ordered comparisons
 *   *               any version
 *
 * A version may leave out its minor and patch numbers ("2", "1.4"): they are 0. */
#ifndef MLWSVM_SEMVER_H
#define MLWSVM_SEMVER_H

#include "common.h"

typedef struct {
    uint32_t major, minor, patch;
} SemVer;

bool semver_parse(const char *s, SemVer *v); /* the whole string; false if malformed */
int semver_cmp(SemVer a, SemVer b);           /* <0, 0, >0 */
/* Whether a constraint is well formed (why: what is wrong, if not). */
bool semver_constraint_ok(const char *c, char *why, size_t whylen);
bool semver_satisfies(SemVer v, const char *constraint); /* false for a malformed one */

#endif
