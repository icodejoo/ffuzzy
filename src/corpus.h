#ifndef FFUZZY_CORPUS_H
#define FFUZZY_CORPUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ffuzzy_corpus – stores the pre-processed search corpus.
 *
 * For each item we keep:
 *   items[]    – original UTF-8 string (owned copy)
 *   u32items[] – UTF-32 decoded version
 *   u32lens[]  – length in code points
 *   bonuses[]  – precomputed Smith-Waterman position bonuses (int8_t[u32len])
 *   bitmaps[]  – 64-bit character-presence bitmap (prefilter)
 */
struct ffuzzy_corpus {
    char     **items;      /* original UTF-8 strings     */
    uint32_t **u32items;   /* UTF-32 decoded strings     */
    int       *u32lens;    /* code-point lengths         */
    int8_t   **bonuses;    /* per-position bonus arrays  */
    uint64_t  *bitmaps;    /* bitmap prefilter values    */
    uint32_t   len;        /* current number of items    */
    uint32_t   cap;        /* allocated capacity         */
};

typedef struct ffuzzy_corpus ffuzzy_corpus_t;

ffuzzy_corpus_t *ffuzzy_corpus_new(void);
void             ffuzzy_corpus_add(ffuzzy_corpus_t *corpus,
                                   const char     **items,
                                   uint32_t         count);
uint32_t         ffuzzy_corpus_len(const ffuzzy_corpus_t *corpus);
void             ffuzzy_corpus_free(ffuzzy_corpus_t *corpus);

/* UTF-8 helpers (also used by ffuzzy.c) */
uint32_t utf8_next_codepoint(const char **p);
int      utf8_to_utf32(const char *src, uint32_t **out, int *out_len);

#ifdef __cplusplus
}
#endif

#endif /* FFUZZY_CORPUS_H */
