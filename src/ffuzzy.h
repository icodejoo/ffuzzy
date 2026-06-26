#ifndef FFUZZY_H
#define FFUZZY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- result types ---- */

/*
 * ffuzzy_hit_t – one fuzzy-match result.
 *
 * indices / indices_len contract:
 *   indices != NULL && indices_len == pat_len  : positions were computed normally.
 *   indices == NULL && indices_len == 0        : position tracking failed (OOM).
 *
 * score range: depends on string lengths and scoring constants; higher is
 * better.  A score >= 0 indicates a match; -1 is never stored in a hit.
 *
 * The caller must free the result via ffuzzy_results_free() — do NOT free
 * individual hits or the indices pointer directly.
 *
 * Layout note: int32_t score ensures consistent struct layout across Win32
 * and POSIX (avoids 'int' size ambiguity for FFI consumers such as ffigen).
 */
typedef struct {
    uint32_t  index;        /* index into the corpus */
    int32_t   score;        /* Smith-Waterman score  */
    uint32_t *indices;      /* matched char positions in original string */
    uint32_t  indices_len;
} ffuzzy_hit_t;

typedef struct {
    ffuzzy_hit_t *hits;
    uint32_t      len;
} ffuzzy_results_t;

/* ---- corpus opaque handle ---- */

typedef struct ffuzzy_corpus ffuzzy_corpus_t;

/* ---- corpus API ---- */

ffuzzy_corpus_t *ffuzzy_corpus_new(void);

/*
 * ffuzzy_corpus_add – add 'count' UTF-8 strings to the corpus.
 *
 * The library makes a private copy of each string.  The caller retains full
 * ownership of the items array and each items[i] pointer and may free them
 * immediately after this call returns.
 *
 * Items can only be appended; there is no remove or update operation.
 * Indices returned by ffuzzy_filter() are stable insertion-order positions.
 */
void             ffuzzy_corpus_add(ffuzzy_corpus_t *corpus,
                                   const char     **items,
                                   uint32_t         count);
uint32_t         ffuzzy_corpus_len(const ffuzzy_corpus_t *corpus);
void             ffuzzy_corpus_free(ffuzzy_corpus_t *corpus);

/* ---- search API ---- */

/*
 * ffuzzy_filter – search the corpus for query.
 *
 * ignore_case : 1 = case-insensitive, 0 = case-sensitive
 * limit       : maximum number of results (0 = all matches)
 *
 * Returns a heap-allocated ffuzzy_results_t that the caller must free
 * with ffuzzy_results_free().
 */
ffuzzy_results_t *ffuzzy_filter(ffuzzy_corpus_t *corpus,
                                const char      *query,
                                int              ignore_case,
                                uint32_t         limit);

void ffuzzy_results_free(ffuzzy_results_t *results);

#ifdef __cplusplus
}
#endif

#endif /* FFUZZY_H */
