/* unicode.h - Unicode character properties, case mapping, normalization,
 * segmentation and display width (the tables: unicode_tables.inc, generated
 * by build-tools/gen_unicode.py from the Unicode Character Database). Text is
 * UTF-8; an invalid byte reads as one U+FFFD, as everywhere in MLWSVM. */
#ifndef MLWSVM_UNICODE_H
#define MLWSVM_UNICODE_H

#include "common.h"

typedef struct {
    uint8_t gc, ccc, gcb, wb, incb, width;
    int8_t digit;
    uint16_t flags;
    int32_t d_upper, d_lower, d_title, d_fold; /* simple case mappings, as deltas */
} UniRecord;

/* Properties (UNI_P_*: the flags a record carries). */
enum {
    UNI_P_ALPHA = 1, UNI_P_WHITE = 2, UNI_P_UPPER = 4, UNI_P_LOWER = 8, UNI_P_XID_START = 16,
    UNI_P_XID_CONTINUE = 32, UNI_P_EXTPICT = 64, UNI_P_CASED = 128, UNI_P_CASE_IGNORABLE = 256,
    UNI_P_DEFAULT_IGNORABLE = 512, UNI_P_EMOJI_PRESENTATION = 1024
};

/* Case mappings. */
enum { UNI_UPPER, UNI_LOWER, UNI_TITLE, UNI_FOLD };

/* Normalization forms. */
enum { UNI_NFC, UNI_NFD, UNI_NFKC, UNI_NFKD };

const char *uni_version(void);
const UniRecord *uni_record(uint32_t cp); /* any value: beyond U+10FFFF, an unassigned one's */
const char *uni_category(uint32_t cp);    /* "Lu", "Nd", ... */
bool uni_has(uint32_t cp, unsigned prop);
bool uni_is_letter(uint32_t cp);  /* general category L* */
bool uni_is_number(uint32_t cp);  /* N* */
bool uni_is_punct(uint32_t cp);   /* P* */
int uni_digit(uint32_t cp);       /* a decimal digit's value (Nd), or -1 */
int uni_char_width(uint32_t cp);  /* columns on a terminal: 0, 1 or 2 */

/* UTF-8: the code point at *i (advancing *i), and appending one. */
uint32_t uni_utf8_next(const unsigned char *s, size_t n, size_t *i);
void uni_utf8_put(StrBuf *b, uint32_t cp);

/* Whole strings, appended to out. */
void uni_case(const char *s, size_t n, int kind, StrBuf *out); /* full mappings; title case by word */
void uni_normalize(const char *s, size_t n, int form, StrBuf *out);
bool uni_is_normalized(const char *s, size_t n, int form);
/* Canonical caseless match: NFD(fold(NFD(a))) == NFD(fold(NFD(b))). */
bool uni_equal_fold(const char *a, size_t an, const char *b, size_t bn);

/* Segmentation (UAX #29). The end of the grapheme cluster starting at byte
 * offset off (which must start one); n when it is the last. */
size_t uni_next_grapheme(const char *s, size_t n, size_t off);
/* Every word boundary, as byte offsets (0 and n included), in a malloc'd array. */
size_t *uni_word_bounds(const char *s, size_t n, size_t *count);
/* Whether the segment [a, b) is a word: it holds a letter, a digit or an ideograph. */
bool uni_is_word(const char *s, size_t a, size_t b);

/* Columns on a terminal: per grapheme cluster, its first character's width,
 * or 2 for an emoji shown as one (with U+FE0F, or Emoji_Presentation). */
int64_t uni_width(const char *s, size_t n);

#endif
