// ffz — a small C fuzzy matcher with functional & performance parity to
// nucleo-matcher 0.3.1 (the engine behind the `ffuzzy` Flutter plugin).
//
// Design (see README.md for the full rationale):
//   * Single codepoint (UTF-32) code path — no per-type monomorphization.
//   * Same fzf-derived scoring model as nucleo (word-boundary / camelCase /
//     consecutive bonuses, gap penalties, first-char multiplier).
//   * Match modes: fuzzy (subsequence), substring, prefix, postfix, exact.
//   * Pattern layer parses `! ^ ' $` syntax and splits words into atoms.
//   * Unicode: Latin diacritic folding + full simple case folding; CJK works
//     at codepoint granularity. Functional parity, NOT byte-identical scores.
//
// Threading: a `ffz_matcher` owns reusable scratch buffers and is NOT
// thread-safe; use one matcher per thread.
#ifndef FFZ_H
#define FFZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Character classes (order matters: used in `class > Delimiter` comparisons).
// ---------------------------------------------------------------------------
typedef enum {
    FFZ_CLASS_WHITESPACE = 0,
    FFZ_CLASS_NONWORD = 1,
    FFZ_CLASS_DELIMITER = 2,
    FFZ_CLASS_LOWER = 3,
    FFZ_CLASS_UPPER = 4,
    FFZ_CLASS_LETTER = 5,
    FFZ_CLASS_NUMBER = 6
} ffz_char_class;

// ---------------------------------------------------------------------------
// Match modes (mirror nucleo's AtomKind).
// ---------------------------------------------------------------------------
typedef enum {
    FFZ_FUZZY = 0,    // subsequence match with gaps (default)
    FFZ_SUBSTRING,    // contiguous substring
    FFZ_PREFIX,       // leading match
    FFZ_POSTFIX,      // trailing match
    FFZ_EXACT         // whole-string match
} ffz_mode;

// ---------------------------------------------------------------------------
// Scoring mode — controls the algorithm used in ffz_match / ffz_corpus_filter.
// ---------------------------------------------------------------------------
typedef enum {
    FFZ_SCORE_FAST   = 0,  // default: 2-row rolling DP, simplified bonuses
    FFZ_SCORE_OFF    = 1,  // prefilter only; score=0, original insertion order
    FFZ_SCORE_NUCLEO = 2,  // nucleo-compatible full-matrix DP (legacy behaviour)
} ffz_scoring_mode;

// ---------------------------------------------------------------------------
// Config — controls bonuses and normalization. Use the constructors below.
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *delimiter_chars;  // ASCII delimiters that grant a boundary bonus
    size_t delimiter_len;
    uint16_t bonus_boundary_white;
    uint16_t bonus_boundary_delimiter;
    ffz_char_class initial_char_class;  // class assumed before index 0
    bool normalize;       // strip Latin diacritics before comparing
    bool ignore_case;     // case-insensitive comparison
    bool prefer_prefix;   // small bonus for matches near the start
    // Internal: precomputed class for each ASCII byte (O(1) classification).
    // Filled by the constructors; do not set by hand.
    uint8_t ascii_class[128];
    ffz_scoring_mode scoring_mode;  // algorithm for ffz_match / corpus_filter
} ffz_config;

// Default config: delimiters "/,:;|", boundary bonuses, normalize+ignore_case on.
ffz_config ffz_config_default(void);
// Path-oriented config: delimiters "/" (+ "\\" semantics), tuned boundary bonus.
ffz_config ffz_config_match_paths(void);

// ---------------------------------------------------------------------------
// Strings — dual representation (mirrors nucleo's Utf32Str). All-ASCII text
// stays as raw bytes so the matcher can use SIMD memchr; non-ASCII text is
// decoded to UTF-32 codepoints. Exactly one of {b, u} is non-NULL.
// Build with ffz_str_from_utf8().
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *b;    // ASCII bytes (NULL if Unicode)
    const uint32_t *u;   // UTF-32 codepoints (NULL if ASCII)
    size_t len;          // number of code units (bytes == codepoints here)
} ffz_str;

// An owned codepoint buffer produced from UTF-8 input.
typedef struct {
    uint32_t *cp;
    size_t len;
    size_t cap;
} ffz_str_buf;

// Decode UTF-8 `s` (length `n` bytes) into `buf` (reused/grown). Returns a view.
// Invalid bytes are decoded as U+FFFD. `buf` must be zero-initialized on first use.
ffz_str ffz_str_from_utf8(const char *s, size_t n, ffz_str_buf *buf);
void ffz_str_buf_free(ffz_str_buf *buf);

// ---------------------------------------------------------------------------
// Indices — growable list of matched codepoint positions (for highlighting).
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t *data;
    size_t len;
    size_t cap;
} ffz_indices;

void ffz_indices_clear(ffz_indices *ix);          // len = 0, keeps capacity
void ffz_indices_free(ffz_indices *ix);            // release memory
void ffz_indices_sort_dedup(ffz_indices *ix);      // ascending, unique

// ---------------------------------------------------------------------------
// Matcher — holds reusable scratch memory. Create once, reuse across calls.
// ---------------------------------------------------------------------------
typedef struct ffz_matcher ffz_matcher;

ffz_matcher *ffz_matcher_new(ffz_config cfg);
void ffz_matcher_free(ffz_matcher *m);
ffz_config *ffz_matcher_config(ffz_matcher *m);  // mutate per-query if needed

// Low-level match. `needle` MUST already be normalized (case-folded /
// accent-folded) by the caller to match `cfg` — the pattern layer below does
// this for you. Returns the score (>=0) or -1 on no match. If `out` is
// non-NULL the matched codepoint indices are appended (NOT cleared).
int32_t ffz_match(ffz_matcher *m, ffz_str haystack, ffz_str needle,
                  ffz_mode mode, ffz_indices *out);

// ---------------------------------------------------------------------------
// Pattern / Atom layer — parses query syntax and normalizes the needle.
// ---------------------------------------------------------------------------
typedef enum {
    FFZ_CASE_RESPECT = 0,  // 'a' != 'A'
    FFZ_CASE_IGNORE,       // 'a' == 'A'
    FFZ_CASE_SMART         // ignore case unless the query has an uppercase char
} ffz_case_matching;

typedef enum {
    FFZ_NORM_NEVER = 0,    // never fold accents
    FFZ_NORM_SMART         // fold unless the query itself contains accents
} ffz_normalization;

// A single parsed needle component.
typedef struct ffz_atom ffz_atom;

// Parse a full query into atoms (whitespace-separated words, `! ^ ' $` syntax).
typedef struct ffz_pattern ffz_pattern;

ffz_pattern *ffz_pattern_parse(const char *query, size_t n,
                               ffz_case_matching cm, ffz_normalization nm);
// Like parse but treats the whole word literally (no `! ^ ' $` parsing),
// forcing every atom to `kind`.
ffz_pattern *ffz_pattern_new(const char *query, size_t n, ffz_case_matching cm,
                             ffz_normalization nm, ffz_mode kind);
void ffz_pattern_free(ffz_pattern *p);

// Score `haystack` against the whole pattern (sum of atom scores). Returns the
// total score (>=0) or -1 if any required atom fails / a negative atom hits.
// If `out` is non-NULL, matched indices for positive atoms are appended.
int32_t ffz_pattern_match(ffz_matcher *m, const ffz_pattern *p,
                          ffz_str haystack, ffz_indices *out);

#ifdef __cplusplus
}
#endif
#endif  // FFZ_H
