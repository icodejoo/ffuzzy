/*
 * ffuzzy.c – main entry point.
 *
 * The corpus lifecycle functions (ffuzzy_corpus_new / _add / _len / _free)
 * are implemented in corpus.c, which exports them under the exact names
 * declared in ffuzzy.h.  This file only provides the search entry points.
 */

#include "ffuzzy.h"
#include "thread_pool.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Search                                                               */
/* ------------------------------------------------------------------ */

ffuzzy_results_t *ffuzzy_filter(ffuzzy_corpus_t *corpus,
                                const char      *query,
                                int              ignore_case,
                                uint32_t         limit)
{
    return thread_pool_filter(corpus, query, ignore_case, limit);
}

/* ------------------------------------------------------------------ */
/* Result cleanup                                                        */
/* ------------------------------------------------------------------ */

void ffuzzy_results_free(ffuzzy_results_t *results)
{
    if (!results) return;
    if (results->hits) {
        for (uint32_t i = 0; i < results->len; i++)
            free(results->hits[i].indices);
        free(results->hits);
    }
    free(results);
}
