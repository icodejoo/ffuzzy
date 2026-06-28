// Internal shared declarations and the fzf-derived scoring constants.
// These mirror nucleo-matcher 0.3.1 `score.rs` exactly.
#ifndef FFZ_INTERNAL_H
#define FFZ_INTERNAL_H

#include <string.h>  // memchr, memcpy (used by ffz_find_ci and SWAR path)
#include "ffz.h"

// --- scoring constants (identical to nucleo) ------------------------------
#define FFZ_SCORE_MATCH 16
#define FFZ_PENALTY_GAP_START 3
#define FFZ_PENALTY_GAP_EXTENSION 1
#define FFZ_BONUS_BOUNDARY (FFZ_SCORE_MATCH / 2)            // 8
#define FFZ_BONUS_CAMEL123 (FFZ_BONUS_BOUNDARY - FFZ_PENALTY_GAP_START)  // 5
#define FFZ_BONUS_NON_WORD FFZ_BONUS_BOUNDARY               // 8
#define FFZ_BONUS_CONSECUTIVE (FFZ_PENALTY_GAP_START + FFZ_PENALTY_GAP_EXTENSION)  // 4
#define FFZ_BONUS_FIRST_CHAR_MULTIPLIER 2
#define FFZ_PREFIX_BONUS_SCALE 2
#define FFZ_MAX_PREFIX_BONUS FFZ_BONUS_BOUNDARY             // 8

// --- size limits for the optimal DP (mirror matrix.rs) --------------------
#define FFZ_MAX_MATRIX_SIZE (100 * 1024)  // cells; beyond this -> greedy fallback
#define FFZ_MAX_NEEDLE_LEN 2048

// Cap atoms-per-query so a pathological space-heavy query (each word = one atom,
// each atom re-scanned against every corpus item) can't drive O(atoms*items)
// into a CPU hang. Extra words past the cap are dropped.
#define FFZ_MAX_ATOMS 64

// --- DP cells (explicit two-track scoring; see ffz_fuzzy.c) ---------------
// M-track: needle[k] matched AT this haystack column.
typedef struct {
    uint16_t score;
    uint8_t consec;    // consecutive-bonus state carried forward
    uint8_t matched;   // reached via consecutive extension (vs after a gap)
    uint8_t valid;     // a real alignment reaches this cell
} ffz_mcell;

// --- matcher internals ----------------------------------------------------
struct ffz_matcher {
    ffz_config cfg;
    // Reusable DP scratch, grown on demand (see ffz_fuzzy.c / ffz_match.c).
    uint32_t *hay;      // normalized haystack window           [cap_hay]
    uint8_t  *bonus;    // precomputed bonus per column          [cap_hay]
    ffz_mcell *mgrid;   // full M grid (needle_len x width)       [cap_grid]
    uint8_t  *pmat;     // full P-origin bits (needle_len x width)[cap_grid]
    uint32_t *roll;     // 2 rolling rows for FAST DP [2 * cap_hay uint32_t]
                        // Row stride is W (current window width), NOT cap_hay.
                        // Rows start at roll[0] and roll[W]; see ffz_fuzzy_rolling.
    size_t cap_hay, cap_grid;
};

// Grow scratch so a (needle_len x width) problem fits. Returns false on OOM.
bool ffz_matcher_reserve(ffz_matcher *m, size_t width, size_t needle_len);

// --- char ops (ffz_chars.c) ----------------------------------------------
ffz_char_class ffz_char_class_of(uint32_t cp, const ffz_config *cfg);
// Returns the class of `cp` (computed on the ORIGINAL codepoint) and writes the
// normalized (folded) codepoint to *out.
ffz_char_class ffz_class_and_normalize(uint32_t cp, const ffz_config *cfg,
                                       uint32_t *out);
uint32_t ffz_normalize_cp(uint32_t cp, const ffz_config *cfg);
uint16_t ffz_bonus_for(const ffz_config *cfg, ffz_char_class prev,
                       ffz_char_class cls);
// Simplified 4-bonus model for FFZ_SCORE_FAST: BOUNDARY=8, CAMEL=7, else 0.
// Ignores whitespace/delimiter distinction (no 10/9/8 tiers).
uint8_t ffz_fast_bonus(ffz_char_class prev, ffz_char_class cls);
// Pattern-layer helpers (case folding independent of config).
uint32_t ffz_cp_to_lower(uint32_t cp);
bool ffz_cp_is_upper(uint32_t cp);
bool ffz_cp_has_normalize(uint32_t cp);

// Codepoint accessor over either representation (well-predicted branch).
static inline uint32_t ffz_at(ffz_str s, size_t i) {
    return s.b ? (uint32_t)s.b[i] : s.u[i];
}

#define FFZ_NF ((size_t)-1)  // sentinel: "not found"

// The SWAR/SIMD byte search (ffz_find_ci) and the all-ASCII scan assume a
// little-endian target (byte i is the i-th least-significant). All supported
// targets (x86, ARM, Android, iOS, Windows) are LE; fail loudly otherwise.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "ffz requires a little-endian target."
#endif

// count-trailing-zeros, portable across GCC/Clang and MSVC. Inputs are nonzero
// at every call site (guarded by a prior mask test).
#if defined(_MSC_VER)
#include <intrin.h>
static inline unsigned ffz_ctz32(uint32_t x) {
    unsigned long i;
    _BitScanForward(&i, (unsigned long)x);
    return (unsigned)i;
}
static inline unsigned ffz_ctz64(uint64_t x) {
    unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanForward64(&i, x);
    return (unsigned)i;
#else
    if ((uint32_t)x) { _BitScanForward(&i, (unsigned long)x); return (unsigned)i; }
    _BitScanForward(&i, (unsigned long)(x >> 32));
    return (unsigned)i + 32;
#endif
}
#else
#define ffz_ctz32(x) ((unsigned)__builtin_ctz(x))
#define ffz_ctz64(x) ((unsigned)__builtin_ctzll(x))
#endif

// count-leading-zeros (mirror of ctz, used for reverse SIMD byte search).
#if defined(_MSC_VER)
static inline unsigned ffz_clz32(uint32_t x) {
    unsigned long i;
    _BitScanReverse(&i, (unsigned long)x);
    return 31u - (unsigned)i;
}
static inline unsigned ffz_clz64(uint64_t x) {
    unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanReverse64(&i, x);
    return 63u - (unsigned)i;
#else
    if ((uint32_t)(x >> 32)) {
        _BitScanReverse(&i, (unsigned long)(x >> 32));
        return 31u - (unsigned)i;
    }
    _BitScanReverse(&i, (unsigned long)x);
    return 63u - (unsigned)i;
#endif
}
#else
#define ffz_clz32(x) ((unsigned)__builtin_clz(x))
#define ffz_clz64(x) ((unsigned)__builtin_clzll(x))
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define FFZ_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define FFZ_NEON 1
#endif

// First index of byte `c` in h[0..n) — case-insensitive (also matches c-32) if
// `ic` and `c` is a lowercase letter; matches nucleo's memchr2. The two-case
// search runs 16 bytes/iter on SSE2/NEON and falls back to an 8-byte SWAR scan
// elsewhere. Case-sensitive search defers to libc memchr. Little-endian.
static inline size_t ffz_find_ci(const uint8_t *h, size_t n, uint8_t c, bool ic) {
    if (ic && c >= 'a' && c <= 'z') {
        uint8_t c2 = (uint8_t)(c - 32);
        size_t i = 0;
#if defined(FFZ_SSE2)
        __m128i v1 = _mm_set1_epi8((char)c), v2 = _mm_set1_epi8((char)c2);
        for (; i + 16 <= n; i += 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(h + i));
            __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, v1), _mm_cmpeq_epi8(x, v2));
            unsigned mask = (unsigned)_mm_movemask_epi8(m);
            if (mask) return i + (size_t)ffz_ctz32(mask);
        }
#elif defined(FFZ_NEON)
        uint8x16_t v1 = vdupq_n_u8(c), v2 = vdupq_n_u8(c2);
        for (; i + 16 <= n; i += 16) {
            uint8x16_t x = vld1q_u8(h + i);
            uint8x16_t m = vorrq_u8(vceqq_u8(x, v1), vceqq_u8(x, v2));
            uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(m), 0);
            uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(m), 1);
            if (lo) return i + ((size_t)ffz_ctz64(lo) >> 3);
            if (hi) return i + 8 + ((size_t)ffz_ctz64(hi) >> 3);
        }
#else
        const uint64_t ONES = 0x0101010101010101ULL, HIGH = 0x8080808080808080ULL;
        uint64_t b1 = ONES * c, b2 = ONES * (uint64_t)c2;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            memcpy(&w, h + i, 8);
            uint64_t x1 = w ^ b1, x2 = w ^ b2;
            uint64_t mm = (((x1 - ONES) & ~x1) | ((x2 - ONES) & ~x2)) & HIGH;
            if (mm) return i + ((size_t)ffz_ctz64(mm) >> 3);
        }
#endif
        for (; i < n; i++)
            if (h[i] == c || h[i] == c2) return i;
        return FFZ_NF;
    }
    const uint8_t *p = (const uint8_t *)memchr(h, c, n);
    return p ? (size_t)(p - h) : FFZ_NF;
}

// Last index of byte c in h[0..n), case-insensitive when ic is true and c is
// a lowercase ASCII letter (also matches c-32). Returns FFZ_NF if not found.
// Runs 16 bytes/iter on SSE2/NEON; falls back to scalar otherwise.
static inline size_t ffz_rfind_ci(const uint8_t *h, size_t n, uint8_t c, bool ic) {
    if (ic && c >= 'a' && c <= 'z') {
        uint8_t c2 = (uint8_t)(c - 32);
        size_t i = n;
#if defined(FFZ_SSE2)
        __m128i v1 = _mm_set1_epi8((char)c), v2 = _mm_set1_epi8((char)c2);
        while (i >= 16) {
            i -= 16;
            __m128i x = _mm_loadu_si128((const __m128i *)(h + i));
            __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, v1), _mm_cmpeq_epi8(x, v2));
            unsigned mask = (unsigned)_mm_movemask_epi8(m);
            if (mask) return i + (size_t)(31u - ffz_clz32(mask));
        }
#elif defined(FFZ_NEON)
        uint8x16_t v1 = vdupq_n_u8(c), v2 = vdupq_n_u8(c2);
        while (i >= 16) {
            i -= 16;
            uint8x16_t x = vld1q_u8(h + i);
            uint8x16_t m = vorrq_u8(vceqq_u8(x, v1), vceqq_u8(x, v2));
            // ARM LE: lane 0 = bytes 0-7 (byte 0 at LSB), lane 1 = bytes 8-15
            uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(m), 0);
            uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(m), 1);
            if (hi) return i + 8u + (size_t)(7u - (ffz_clz64(hi) >> 3));
            if (lo) return i + (size_t)(7u - (ffz_clz64(lo) >> 3));
        }
#endif
        while (i > 0) {
            uint8_t b = h[--i];
            if (b == c || b == c2) return i;
        }
    } else {
        size_t i = n;
#if defined(FFZ_SSE2)
        __m128i v1 = _mm_set1_epi8((char)c);
        while (i >= 16) {
            i -= 16;
            __m128i x = _mm_loadu_si128((const __m128i *)(h + i));
            unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, v1));
            if (mask) return i + (size_t)(31u - ffz_clz32(mask));
        }
#elif defined(FFZ_NEON)
        uint8x16_t v1 = vdupq_n_u8(c);
        while (i >= 16) {
            i -= 16;
            uint8x16_t x = vld1q_u8(h + i);
            uint8x16_t m = vceqq_u8(x, v1);
            uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(m), 0);
            uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(m), 1);
            if (hi) return i + 8u + (size_t)(7u - (ffz_clz64(hi) >> 3));
            if (lo) return i + (size_t)(7u - (ffz_clz64(lo) >> 3));
        }
#endif
        while (i > 0) {
            if (h[--i] == c) return i;
        }
    }
    return FFZ_NF;
}

// --- scoring (ffz_score.c) ------------------------------------------------
// Score a window [start,end) of `hay` against `needle`, assuming needle matches
// as a subsequence inside it. Appends indices if `out` != NULL.
uint16_t ffz_calculate_score(ffz_matcher *m, ffz_str hay, ffz_str needle,
                             size_t start, size_t end, ffz_indices *out);

// --- fuzzy core (ffz_fuzzy.c) --------------------------------------------
int32_t ffz_fuzzy_optimal(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t greedy_end, size_t end,
                          ffz_indices *out);
int32_t ffz_fuzzy_greedy(ffz_matcher *m, ffz_str hay, ffz_str needle,
                         size_t start, size_t end, ffz_indices *out);
// 2-row rolling Smith-Waterman DP (FAST mode, score-only, no backtracking).
// greedy_end is the linear-fallback window end (used when oversized); end is the
// DP window end.
int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t greedy_end, size_t end);

// --- prefilter (ffz_prefilter.c) -----------------------------------------
// Find (start, greedy_end, end) bounds for a subsequence match. Returns false
// if the needle can't be a subsequence. If `only_greedy`, end == greedy_end.
bool ffz_prefilter(const ffz_config *cfg, ffz_str hay, ffz_str needle,
                   bool only_greedy, size_t *start, size_t *greedy_end,
                   size_t *end);

// --- small helpers --------------------------------------------------------
void ffz_indices_push(ffz_indices *ix, uint32_t v);
// Decode one UTF-8 codepoint at s[*pos] (within [0,n)); advances *pos. Invalid/
// truncated/overlong/surrogate -> U+FFFD. Shared by the haystack & pattern
// decoders (single source of truth).
uint32_t ffz_decode_cp(const uint8_t *s, size_t n, size_t *pos);
static inline uint16_t ffz_sat_sub_u16(uint16_t a, uint16_t b) {
    return a > b ? (uint16_t)(a - b) : 0;
}
static inline uint16_t ffz_sat_add_u16(uint16_t a, uint16_t b) {
    uint32_t s = (uint32_t)a + (uint32_t)b;
    return s > 0xFFFFu ? (uint16_t)0xFFFF : (uint16_t)s;
}
static inline uint16_t ffz_u16_max(uint16_t a, uint16_t b) {
    return a > b ? a : b;
}

#endif  // FFZ_INTERNAL_H
