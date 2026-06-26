#ifndef FFUZZY_SCORER_H
#define FFUZZY_SCORER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- scoring constants ---- */

#define SCORE_MATCH          16
#define SCORE_GAP_START      (-3)
#define SCORE_GAP_EXTEND     (-1)

#define BONUS_BOUNDARY       8   /* char after space / _ / - / / */
#define BONUS_CAMEL          7   /* camelCase transition           */
#define BONUS_CONSECUTIVE    4   /* consecutive matches            */
#define BONUS_FIRST_CHAR_MULT 2  /* multiplier for first pattern char */

/* ---- character class ---- */

typedef enum {
    CC_NON_WORD = 0,
    CC_LOWER    = 1,
    CC_UPPER    = 2,
    CC_DIGIT    = 3
} char_class_t;

/* ---- precomputed bonus array ---- */

/*
 * scorer_compute_bonuses – fill bonus[0..slen-1] using the char-class
 * transitions of the UTF-32 string str[0..slen-1].
 * Caller must allocate bonus (int8_t[slen]).
 */
void scorer_compute_bonuses(const uint32_t *str, int slen, int8_t *bonus);

/* ---- main scoring entry points ---- */

/*
 * scorer_score – return the best Smith-Waterman score for matching
 * pattern pat[0..plen-1] against string str[0..slen-1].
 * ignore_case: 1 = fold to lower before comparison.
 * Returns -1 if pat is not a subsequence of str.
 */
int32_t scorer_score(const uint32_t *pat, int plen,
                     const uint32_t *str, int slen,
                     const int8_t   *bonus,
                     int             ignore_case);

/*
 * scorer_score_positions – same as scorer_score, but also fills
 * positions[0..plen-1] with the indices (into str) of the matched chars.
 * positions must be pre-allocated with plen elements.
 * Returns -1 if pat is not a subsequence of str.
 */
int32_t scorer_score_positions(const uint32_t *pat, int plen,
                               const uint32_t *str, int slen,
                               const int8_t   *bonus,
                               int             ignore_case,
                               uint32_t       *positions);

/* ---- Unicode case folding helper (used by scorer internally) ---- */

uint32_t utf32_fold_lower(uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif /* FFUZZY_SCORER_H */
