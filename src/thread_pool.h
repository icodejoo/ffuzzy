#ifndef FFUZZY_THREAD_POOL_H
#define FFUZZY_THREAD_POOL_H

#include "ffuzzy.h"
#include "corpus.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parallel search: split the corpus into N chunks (N = logical CPU count),
 * run each chunk in its own thread, merge results and sort by score desc.
 *
 * Returns a freshly allocated ffuzzy_results_t or NULL on OOM.
 * The caller owns the result and must free it with ffuzzy_results_free().
 */
ffuzzy_results_t *thread_pool_filter(ffuzzy_corpus_t *corpus,
                                     const char      *query,
                                     int              ignore_case,
                                     uint32_t         limit);

#ifdef __cplusplus
}
#endif

#endif /* FFUZZY_THREAD_POOL_H */
