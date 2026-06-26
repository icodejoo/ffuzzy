#include "corpus.h"
#include "bitmap.h"
#include "scorer.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* UTF-8 decoder                                                         */
/* ------------------------------------------------------------------ */

/*
 * Decode the next UTF-8 code point from *p, advance *p past it.
 * Returns the code point, or U+FFFD on encoding error.
 */
uint32_t utf8_next_codepoint(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;

    if (s[0] < 0x80) {
        /* 1-byte (ASCII) */
        cp  = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        /* 2-byte */
        if ((s[1] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp  = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        /* 3-byte */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp  = ((uint32_t)(s[0] & 0x0F) << 12)
            | ((uint32_t)(s[1] & 0x3F) <<  6)
            |  (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        /* 4-byte */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *p += 1; return 0xFFFD;
        }
        cp  = ((uint32_t)(s[0] & 0x07) << 18)
            | ((uint32_t)(s[1] & 0x3F) << 12)
            | ((uint32_t)(s[2] & 0x3F) <<  6)
            |  (s[3] & 0x3F);
        *p += 4;
    } else {
        /* Invalid lead byte */
        *p += 1;
        return 0xFFFD;
    }
    /* Reject surrogate halves (U+D800–U+DFFF) and out-of-Unicode-range values */
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;
    if (cp > 0x10FFFF) return 0xFFFD;
    return cp;
}

/*
 * Decode an entire UTF-8 string to a freshly allocated UTF-32 array.
 * *out must be freed by the caller.  Returns 0 on success, -1 on OOM.
 */
int utf8_to_utf32(const char *src, uint32_t **out, int *out_len)
{
    /* Count code points first */
    const char *p = src;
    int count = 0;
    while (*p) {
        utf8_next_codepoint(&p);
        count++;
    }

    uint32_t *buf = (uint32_t *)malloc((size_t)(count + 1) * sizeof(uint32_t));
    if (!buf) return -1;

    p = src;
    for (int i = 0; i < count; i++)
        buf[i] = utf8_next_codepoint(&p);
    buf[count] = 0;

    *out     = buf;
    *out_len = count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Corpus lifecycle                                                      */
/* ------------------------------------------------------------------ */

#define INITIAL_CAP 64

ffuzzy_corpus_t *ffuzzy_corpus_new(void)
{
    ffuzzy_corpus_t *c = (ffuzzy_corpus_t *)calloc(1, sizeof(ffuzzy_corpus_t));
    if (!c) return NULL;

    c->items    = (char     **)malloc(INITIAL_CAP * sizeof(char *));
    c->u32items = (uint32_t **)malloc(INITIAL_CAP * sizeof(uint32_t *));
    c->u32lens  = (int       *)malloc(INITIAL_CAP * sizeof(int));
    c->bonuses  = (int8_t   **)malloc(INITIAL_CAP * sizeof(int8_t *));
    c->bitmaps  = (uint64_t  *)malloc(INITIAL_CAP * sizeof(uint64_t));

    if (!c->items || !c->u32items || !c->u32lens || !c->bonuses || !c->bitmaps) {
        ffuzzy_corpus_free(c);
        return NULL;
    }

    c->cap = INITIAL_CAP;
    c->len = 0;
    return c;
}

static int corpus_grow(ffuzzy_corpus_t *c)
{
    /* Guard against uint32_t overflow on doubling */
    if (c->cap > UINT32_MAX / 2) return -1;
    uint32_t new_cap = c->cap * 2;

    /*
     * Realloc each array.  When realloc succeeds, the old pointer is freed
     * internally — we MUST update the struct field immediately so that we
     * never hold a stale (freed) pointer.  If a later realloc fails, the
     * corpus will have some arrays at new_cap and others still at the old cap,
     * which is consistent-enough for continued use at the OLD len (no new
     * items are added after a grow failure).  The already-grown arrays are
     * not rolled back (that would require a second realloc and could also
     * fail), but they remain valid pointers at new_cap size.
     */
    char     **ni = (char     **)realloc(c->items,    new_cap * sizeof(char *));
    if (ni)  c->items    = ni;
    uint32_t **nu = (uint32_t **)realloc(c->u32items, new_cap * sizeof(uint32_t *));
    if (nu)  c->u32items = nu;
    int       *nl = (int       *)realloc(c->u32lens,  new_cap * sizeof(int));
    if (nl)  c->u32lens  = nl;
    int8_t   **nb = (int8_t   **)realloc(c->bonuses,  new_cap * sizeof(int8_t *));
    if (nb)  c->bonuses  = nb;
    uint64_t  *nm = (uint64_t  *)realloc(c->bitmaps,  new_cap * sizeof(uint64_t));
    if (nm)  c->bitmaps  = nm;

    if (!ni || !nu || !nl || !nb || !nm) return -1;

    c->cap = new_cap;
    return 0;
}

void ffuzzy_corpus_add(ffuzzy_corpus_t *corpus,
                       const char     **items,
                       uint32_t         count)
{
    if (!corpus || !items || count == 0) return;

    for (uint32_t k = 0; k < count; k++) {
        if (corpus->len >= corpus->cap) {
            if (corpus_grow(corpus) != 0) return; /* OOM – stop */
        }

        const char *src = items[k] ? items[k] : "";
        uint32_t    idx = corpus->len;

        /* Allocate all resources for this slot into locals first, then
         * commit atomically so the corpus arrays are never partially written. */
        size_t slen = strlen(src);
        char *item_copy = (char *)malloc(slen + 1);
        if (!item_copy) return;
        memcpy(item_copy, src, slen + 1);

        /* UTF-32 conversion */
        uint32_t *u32  = NULL;
        int       ulen = 0;
        if (utf8_to_utf32(src, &u32, &ulen) != 0) {
            free(item_copy);
            return;
        }

        /* Precompute bonuses */
        int8_t *bon = NULL;
        if (ulen > 0) {
            bon = (int8_t *)malloc((size_t)ulen * sizeof(int8_t));
            if (!bon) {
                free(item_copy);
                free(u32);
                return;
            }
            scorer_compute_bonuses(u32, ulen, bon);
        }

        /* All allocations succeeded — commit atomically */
        corpus->items[idx]    = item_copy;
        corpus->u32items[idx] = u32;
        corpus->u32lens[idx]  = ulen;
        corpus->bonuses[idx]  = bon;

        /* Bitmap prefilter */
        corpus->bitmaps[idx] = bitmap_from_utf8(src);

        corpus->len++;
    }
}

uint32_t ffuzzy_corpus_len(const ffuzzy_corpus_t *corpus)
{
    return corpus ? corpus->len : 0;
}

void ffuzzy_corpus_free(ffuzzy_corpus_t *corpus)
{
    if (!corpus) return;

    if (corpus->items) {
        for (uint32_t i = 0; i < corpus->len; i++)
            free(corpus->items[i]);
        free(corpus->items);
    }
    if (corpus->u32items) {
        for (uint32_t i = 0; i < corpus->len; i++)
            free(corpus->u32items[i]);
        free(corpus->u32items);
    }
    free(corpus->u32lens);
    if (corpus->bonuses) {
        for (uint32_t i = 0; i < corpus->len; i++)
            free(corpus->bonuses[i]);
        free(corpus->bonuses);
    }
    free(corpus->bitmaps);
    free(corpus);
}
