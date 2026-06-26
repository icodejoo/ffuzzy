#include "scorer.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Unicode case folding                                                  */
/* ------------------------------------------------------------------ */

/*
 * Basic Latin Extended case-fold table for U+00C0 – U+017E.
 * Each entry maps an upper-case (or title-case) code point to its
 * lower-case equivalent.  Code points not listed are returned unchanged.
 *
 * Source: Unicode 15 SpecialCasing / UnicodeData (selected range).
 */
static const struct { uint32_t upper; uint32_t lower; } LATIN_EXT_TABLE[] = {
    {0x00C0,0x00E0},{0x00C1,0x00E1},{0x00C2,0x00E2},{0x00C3,0x00E3},
    {0x00C4,0x00E4},{0x00C5,0x00E5},{0x00C6,0x00E6},{0x00C7,0x00E7},
    {0x00C8,0x00E8},{0x00C9,0x00E9},{0x00CA,0x00EA},{0x00CB,0x00EB},
    {0x00CC,0x00EC},{0x00CD,0x00ED},{0x00CE,0x00EE},{0x00CF,0x00EF},
    {0x00D0,0x00F0},{0x00D1,0x00F1},{0x00D2,0x00F2},{0x00D3,0x00F3},
    {0x00D4,0x00F4},{0x00D5,0x00F5},{0x00D6,0x00F6},{0x00D8,0x00F8},
    {0x00D9,0x00F9},{0x00DA,0x00FA},{0x00DB,0x00FB},{0x00DC,0x00FC},
    {0x00DD,0x00FD},{0x00DE,0x00FE},
    {0x0100,0x0101},{0x0102,0x0103},{0x0104,0x0105},{0x0106,0x0107},
    {0x0108,0x0109},{0x010A,0x010B},{0x010C,0x010D},{0x010E,0x010F},
    {0x0110,0x0111},{0x0112,0x0113},{0x0114,0x0115},{0x0116,0x0117},
    {0x0118,0x0119},{0x011A,0x011B},{0x011C,0x011D},{0x011E,0x011F},
    {0x0120,0x0121},{0x0122,0x0123},{0x0124,0x0125},{0x0126,0x0127},
    {0x0128,0x0129},{0x012A,0x012B},{0x012C,0x012D},{0x012E,0x012F},
    {0x0130,0x0069},{0x0132,0x0133},{0x0134,0x0135},{0x0136,0x0137},
    {0x0139,0x013A},{0x013B,0x013C},{0x013D,0x013E},{0x013F,0x0140},
    {0x0141,0x0142},{0x0143,0x0144},{0x0145,0x0146},{0x0147,0x0148},
    {0x014A,0x014B},{0x014C,0x014D},{0x014E,0x014F},{0x0150,0x0151},
    {0x0152,0x0153},{0x0154,0x0155},{0x0156,0x0157},{0x0158,0x0159},
    {0x015A,0x015B},{0x015C,0x015D},{0x015E,0x015F},{0x0160,0x0161},
    {0x0162,0x0163},{0x0164,0x0165},{0x0166,0x0167},{0x0168,0x0169},
    {0x016A,0x016B},{0x016C,0x016D},{0x016E,0x016F},{0x0170,0x0171},
    {0x0172,0x0173},{0x0174,0x0175},{0x0176,0x0177},{0x0178,0x00FF},
    {0x0179,0x017A},{0x017B,0x017C},{0x017D,0x017E},
};
#define LATIN_EXT_COUNT (int)(sizeof(LATIN_EXT_TABLE)/sizeof(LATIN_EXT_TABLE[0]))

uint32_t utf32_fold_lower(uint32_t cp)
{
    /* ASCII fast path */
    if (cp >= 'A' && cp <= 'Z') return cp + 32u;
    /* Latin Extended block */
    if (cp >= 0x00C0 && cp <= 0x017E) {
        for (int i = 0; i < LATIN_EXT_COUNT; i++) {
            if (LATIN_EXT_TABLE[i].upper == cp)
                return LATIN_EXT_TABLE[i].lower;
        }
    }
    return cp;
}

/* ------------------------------------------------------------------ */
/* Char-class helpers                                                    */
/* ------------------------------------------------------------------ */

static char_class_t char_class_of(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') return CC_LOWER;
    if (cp >= 'A' && cp <= 'Z') return CC_UPPER;
    if (cp >= '0' && cp <= '9') return CC_DIGIT;
    /* Also treat lower Unicode letters as LOWER – simplified */
    if (cp > 0x7F) return CC_LOWER;
    return CC_NON_WORD;
}

static int is_boundary_prev(uint32_t cp)
{
    return (cp == ' ' || cp == '_' || cp == '-' || cp == '/');
}

/* ------------------------------------------------------------------ */
/* scorer_compute_bonuses                                                */
/* ------------------------------------------------------------------ */

void scorer_compute_bonuses(const uint32_t *str, int slen, int8_t *bonus)
{
    char_class_t prev_class = CC_NON_WORD;
    for (int j = 0; j < slen; j++) {
        uint32_t     cp  = str[j];
        char_class_t cur = char_class_of(cp);

        int8_t b = 0;
        if (j == 0) {
            /* First character always gets boundary bonus */
            b = BONUS_BOUNDARY;
        } else if (is_boundary_prev(str[j-1])) {
            b = BONUS_BOUNDARY;
        } else if (prev_class == CC_LOWER && cur == CC_UPPER) {
            b = BONUS_CAMEL;     /* camelCase transition */
        } else if (prev_class == CC_NON_WORD && cur != CC_NON_WORD) {
            b = BONUS_BOUNDARY;
        } else {
            b = 0;
        }
        bonus[j]   = b;
        prev_class = cur;
    }
}

/* ------------------------------------------------------------------ */
/* Internal DP core                                                      */
/* ------------------------------------------------------------------ */

/*
 * Run the Smith-Waterman DP and return the best score.
 * If track != NULL it receives the best-score column per row (used for
 * backtracking in scorer_score_positions).
 */
static int32_t sw_dp(const uint32_t *pat, int plen,
                     const uint32_t *str, int slen,
                     const int8_t   *bonus,
                     int             ignore_case,
                     int32_t        *H_out,   /* slen elements, final row */
                     int32_t        *C_out)   /* slen elements, final row */
{
    /* Allocate working arrays on the heap to avoid large stack frames */
    int32_t *H_prev = (int32_t *)calloc(slen, sizeof(int32_t));
    int32_t *H_curr = (int32_t *)calloc(slen, sizeof(int32_t));
    int32_t *C_prev = (int32_t *)calloc(slen, sizeof(int32_t));
    int32_t *C_curr = (int32_t *)calloc(slen, sizeof(int32_t));

    if (!H_prev || !H_curr || !C_prev || !C_curr) {
        free(H_prev); free(H_curr); free(C_prev); free(C_curr);
        return -1;
    }

    int32_t best = -1;

    for (int i = 0; i < plen; i++) {
        uint32_t pc = ignore_case ? utf32_fold_lower(pat[i]) : pat[i];

        memset(H_curr, 0, (size_t)slen * sizeof(int32_t));
        memset(C_curr, 0, (size_t)slen * sizeof(int32_t));

        for (int j = 0; j < slen; j++) {
            uint32_t sc = ignore_case ? utf32_fold_lower(str[j]) : str[j];

            if (sc == pc) {
                /* Consecutive bonus */
                int32_t c_score;
                if (j > 0 && C_prev[j-1] > 0) {
                    c_score = C_prev[j-1] + BONUS_CONSECUTIVE;
                } else {
                    c_score = bonus[j];
                }
                /* Ensure boundary/camel bonus is always used if larger */
                if (bonus[j] > c_score) c_score = bonus[j];

                if (i == 0) {
                    H_curr[j] = SCORE_MATCH + (int32_t)bonus[j] * BONUS_FIRST_CHAR_MULT;
                    C_curr[j] = bonus[j];
                } else {
                    int32_t prev_h = (j > 0) ? H_prev[j-1] : 0;
                    H_curr[j] = prev_h + SCORE_MATCH + c_score;
                    C_curr[j] = c_score;
                }

                if (i == plen - 1 && H_curr[j] > best)
                    best = H_curr[j];
            }
            /* else H_curr[j] = 0, C_curr[j] = 0 (already zeroed) */
        }

        /* Swap prev/curr */
        int32_t *tmp;
        tmp = H_prev; H_prev = H_curr; H_curr = tmp;
        tmp = C_prev; C_prev = C_curr; C_curr = tmp;
    }

    /* After loop H_prev holds the last row */
    if (H_out) memcpy(H_out, H_prev, (size_t)slen * sizeof(int32_t));
    if (C_out) memcpy(C_out, C_prev, (size_t)slen * sizeof(int32_t));

    free(H_prev); free(H_curr); free(C_prev); free(C_curr);
    return best;
}

/* ------------------------------------------------------------------ */
/* Public: scorer_score                                                  */
/* ------------------------------------------------------------------ */

int32_t scorer_score(const uint32_t *pat, int plen,
                     const uint32_t *str, int slen,
                     const int8_t   *bonus,
                     int             ignore_case)
{
    if (plen == 0) return 0;
    if (slen == 0 || plen > slen) return -1;

    /* Quick subsequence check */
    int pi = 0;
    for (int j = 0; j < slen && pi < plen; j++) {
        uint32_t sc = ignore_case ? utf32_fold_lower(str[j]) : str[j];
        uint32_t pc = ignore_case ? utf32_fold_lower(pat[pi]) : pat[pi];
        if (sc == pc) pi++;
    }
    if (pi < plen) return -1;

    return sw_dp(pat, plen, str, slen, bonus, ignore_case, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Public: scorer_score_positions                                        */
/* ------------------------------------------------------------------ */

/*
 * We run the full DP storing *all* H rows (plen × slen matrix), then
 * backtrack greedily from the best cell in the last row.
 */
int32_t scorer_score_positions(const uint32_t *pat, int plen,
                               const uint32_t *str, int slen,
                               const int8_t   *bonus,
                               int             ignore_case,
                               uint32_t       *positions)
{
    if (plen == 0) return 0;
    if (slen == 0 || plen > slen) return -1;

    /* Quick subsequence check */
    int pi = 0;
    for (int j = 0; j < slen && pi < plen; j++) {
        uint32_t sc = ignore_case ? utf32_fold_lower(str[j]) : str[j];
        uint32_t pc = ignore_case ? utf32_fold_lower(pat[pi]) : pat[pi];
        if (sc == pc) pi++;
    }
    if (pi < plen) return -1;

    /* Guard against integer overflow on 32-bit platforms before the multiply */
    if (plen > 65536 || slen > 65536) return -1;
    size_t cells = (size_t)(unsigned)plen * (size_t)(unsigned)slen;

    /* Allocate full matrix H[plen][slen] and C[plen][slen] */
    int32_t *H = (int32_t *)calloc(cells, sizeof(int32_t));
    int32_t *C = (int32_t *)calloc(cells, sizeof(int32_t));
    if (!H || !C) { free(H); free(C); return -1; }

#define H_AT(i,j) H[(i)*slen+(j)]
#define C_AT(i,j) C[(i)*slen+(j)]

    int32_t best  = -1;
    int     best_j = -1;

    for (int i = 0; i < plen; i++) {
        uint32_t pc = ignore_case ? utf32_fold_lower(pat[i]) : pat[i];
        for (int j = 0; j < slen; j++) {
            uint32_t sc = ignore_case ? utf32_fold_lower(str[j]) : str[j];
            if (sc == pc) {
                int32_t c_score;
                if (i > 0 && j > 0 && C_AT(i-1, j-1) > 0) {
                    c_score = C_AT(i-1, j-1) + BONUS_CONSECUTIVE;
                } else {
                    c_score = bonus[j];
                }
                if (bonus[j] > c_score) c_score = bonus[j];

                if (i == 0) {
                    H_AT(i,j) = SCORE_MATCH + (int32_t)bonus[j] * BONUS_FIRST_CHAR_MULT;
                    C_AT(i,j) = bonus[j];
                } else {
                    int32_t prev_h = (j > 0) ? H_AT(i-1, j-1) : 0;
                    H_AT(i,j) = prev_h + SCORE_MATCH + c_score;
                    C_AT(i,j) = c_score;
                }

                if (i == plen - 1 && H_AT(i,j) > best) {
                    best   = H_AT(i,j);
                    best_j = j;
                }
            }
        }
    }

    if (best < 0) { free(H); free(C); return -1; }

    /* Backtrack: walk from (plen-1, best_j) back to row 0, ensuring each
     * chosen position actually matched pat[i] in the scoring pass.
     *
     * Strategy: prefer the diagonal predecessor (i-1, j-1).  If that cell
     * is zero or negative, scan leftward in row i-1 for the highest-scoring
     * cell that corresponds to an actual match of pat[i-1] (H_AT(i-1,j2)>0).
     * Fall back to scanning for the first valid match position. */
    {
        uint32_t pc_ignore;
        (void)pc_ignore;
    }
    int cur_j = best_j;
    for (int i = plen - 1; i >= 0; i--) {
        positions[i] = (uint32_t)cur_j;
        if (i > 0) {
            /* Preferred: exact diagonal predecessor */
            int diag_j = cur_j - 1;
            if (diag_j >= 0 && H_AT(i-1, diag_j) > 0) {
                cur_j = diag_j;
            } else {
                /* Scan leftward for any non-zero cell in the predecessor row */
                int found_j = -1;
                for (int j2 = cur_j - 1; j2 >= 0; j2--) {
                    if (H_AT(i-1, j2) > 0) {
                        found_j = j2;
                        break;
                    }
                }
                if (found_j >= 0) {
                    cur_j = found_j;
                } else {
                    /* No valid predecessor found; step back one position */
                    cur_j = (cur_j > 0) ? cur_j - 1 : 0;
                }
            }
        }
    }

#undef H_AT
#undef C_AT

    free(H);
    free(C);
    return best;
}
