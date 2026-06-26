#ifndef FFUZZY_BITMAP_H
#define FFUZZY_BITMAP_H

/*
 * bitmap.h – header-only 64-bit character-presence prefilter.
 *
 * Bits 0-25  : letters a-z (ASCII tolower)
 * Bits 26-35 : digits  0-9
 *
 * This is intentionally a coarse, ASCII-only filter.  A non-zero result
 * from bitmap_could_match() only means that every letter/digit in the
 * query exists somewhere in the item; it is NOT a guarantee of an actual
 * fuzzy match.  The full Smith-Waterman scorer is the authority.
 */

#include <stdint.h>

static inline uint64_t bitmap_from_utf8(const char *s)
{
    uint64_t bm = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p++;
        if (c >= 'A' && c <= 'Z') {
            bm |= (uint64_t)1 << (c - 'A');          /* fold to lower */
        } else if (c >= 'a' && c <= 'z') {
            bm |= (uint64_t)1 << (c - 'a');
        } else if (c >= '0' && c <= '9') {
            bm |= (uint64_t)1 << (26 + (c - '0'));
        }
        /* skip high bytes of multi-byte UTF-8 sequences transparently */
    }
    return bm;
}

/*
 * Returns non-zero if every bit set in query_bm is also set in item_bm,
 * i.e. the item contains at least one occurrence of every ASCII
 * letter/digit present in the query.
 */
static inline int bitmap_could_match(uint64_t item_bm, uint64_t query_bm)
{
    return (item_bm & query_bm) == query_bm;
}

#endif /* FFUZZY_BITMAP_H */
