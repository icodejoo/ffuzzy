#ifndef FFUZZY_H
#define FFUZZY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- result types ---- */

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
