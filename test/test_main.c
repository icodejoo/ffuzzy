/*
 * test_main.c – C unit tests for the ffuzzy library.
 *
 * Build with test/CMakeLists.txt (links against the ffuzzy static lib).
 * Exit code 0 = all tests passed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../src/ffuzzy.h"
#include "../src/bitmap.h"
#include "../src/scorer.h"
#include "../src/corpus.h"

/* ------------------------------------------------------------------ */
/* Minimal test framework                                               */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do {                                            \
    if (expr) {                                                     \
        printf("  PASS: %s\n", #expr);                             \
        g_pass++;                                                   \
    } else {                                                        \
        printf("  FAIL: %s  (line %d)\n", #expr, __LINE__);        \
        g_fail++;                                                   \
    }                                                               \
} while (0)

/* ------------------------------------------------------------------ */
/* Bitmap tests                                                         */
/* ------------------------------------------------------------------ */

static void test_bitmap(void)
{
    printf("\n[bitmap]\n");

    uint64_t bm_hello = bitmap_from_utf8("hello");
    uint64_t bm_world = bitmap_from_utf8("world");
    uint64_t bm_he    = bitmap_from_utf8("he");
    uint64_t bm_xyz   = bitmap_from_utf8("xyz");

    /* "hello" contains 'h' and 'e' */
    CHECK(bitmap_could_match(bm_hello, bm_he));
    /* "hello" does not contain 'w' */
    CHECK(!bitmap_could_match(bm_hello, bm_world));
    /* "world" contains all of "world" query */
    CHECK(bitmap_could_match(bm_world, bm_world));
    /* digits */
    uint64_t bm_num = bitmap_from_utf8("test123");
    uint64_t bm_1   = bitmap_from_utf8("1");
    CHECK(bitmap_could_match(bm_num, bm_1));
    /* empty query always matches */
    uint64_t bm_empty = bitmap_from_utf8("");
    CHECK(bitmap_could_match(bm_hello, bm_empty));
    /* xyz not in hello */
    CHECK(!bitmap_could_match(bm_hello, bm_xyz));
}

/* ------------------------------------------------------------------ */
/* UTF-8 decoder tests                                                  */
/* ------------------------------------------------------------------ */

static void test_utf8(void)
{
    printf("\n[utf8]\n");

    /* ASCII round-trip */
    uint32_t *out = NULL; int len = 0;
    CHECK(utf8_to_utf32("hello", &out, &len) == 0);
    CHECK(len == 5);
    CHECK(out && out[0] == 'h' && out[4] == 'o');
    free(out); out = NULL; len = 0;

    /* 2-byte sequence: U+00E9 (é) */
    const char *e_acute = "\xC3\xA9";
    CHECK(utf8_to_utf32(e_acute, &out, &len) == 0);
    CHECK(len == 1);
    CHECK(out && out[0] == 0x00E9u);
    free(out); out = NULL; len = 0;

    /* 3-byte: U+4E2D (中) */
    const char *zhong = "\xE4\xB8\xAD";
    CHECK(utf8_to_utf32(zhong, &out, &len) == 0);
    CHECK(len == 1);
    CHECK(out && out[0] == 0x4E2Du);
    free(out); out = NULL; len = 0;

    /* Empty string */
    CHECK(utf8_to_utf32("", &out, &len) == 0);
    CHECK(len == 0);
    free(out);
}

/* ------------------------------------------------------------------ */
/* Case-fold tests                                                      */
/* ------------------------------------------------------------------ */

static void test_case_fold(void)
{
    printf("\n[case_fold]\n");

    CHECK(utf32_fold_lower('A') == 'a');
    CHECK(utf32_fold_lower('Z') == 'z');
    CHECK(utf32_fold_lower('a') == 'a');
    CHECK(utf32_fold_lower('0') == '0');
    CHECK(utf32_fold_lower(0x00C9) == 0x00E9); /* É -> é */
    CHECK(utf32_fold_lower(0x0141) == 0x0142); /* Ł -> ł */
}

/* ------------------------------------------------------------------ */
/* Scorer tests                                                         */
/* ------------------------------------------------------------------ */

static void u32_from_ascii(const char *s, uint32_t *buf, int *len)
{
    *len = 0;
    while (s[*len]) { buf[*len] = (uint32_t)(unsigned char)s[*len]; (*len)++; }
}

static void test_scorer(void)
{
    printf("\n[scorer]\n");

    uint32_t pat[64], str[256];
    int plen, slen;
    int8_t bonus[256];

    /* Exact match */
    u32_from_ascii("hello", pat, &plen);
    u32_from_ascii("hello world", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    int32_t score1 = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score1 > 0);

    /* Subsequence but not contiguous */
    u32_from_ascii("hw", pat, &plen);
    score1 = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score1 > 0);

    /* Not a subsequence */
    u32_from_ascii("xyz", pat, &plen);
    score1 = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score1 == -1);

    /* Case-insensitive */
    u32_from_ascii("HELLO", pat, &plen);
    score1 = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score1 > 0);

    /* Case-sensitive should fail when mismatched */
    score1 = scorer_score(pat, plen, str, slen, bonus, 0);
    CHECK(score1 == -1);

    /* Empty pattern */
    u32_from_ascii("", pat, &plen);
    score1 = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score1 == 0);

    /* Boundary bonus: "he" at start of word gets higher score than "ll" */
    u32_from_ascii("he", pat, &plen);
    u32_from_ascii("hello", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    int32_t score_he = scorer_score(pat, plen, str, slen, bonus, 1);

    u32_from_ascii("ll", pat, &plen);
    int32_t score_ll = scorer_score(pat, plen, str, slen, bonus, 1);
    CHECK(score_he >= score_ll); /* first-char bonus makes "he" score >= "ll" */

    /* Positions */
    u32_from_ascii("fw", pat, &plen);
    u32_from_ascii("fuzzy world", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    uint32_t positions[2];
    int32_t sc = scorer_score_positions(pat, plen, str, slen, bonus, 1, positions);
    CHECK(sc > 0);
    CHECK(positions[0] == 0);   /* 'f' at index 0 */
    /* 'w' is at index 6 in "fuzzy world" */
    CHECK(positions[1] == 6);
}

/* ------------------------------------------------------------------ */
/* Corpus tests                                                         */
/* ------------------------------------------------------------------ */

static void test_corpus(void)
{
    printf("\n[corpus]\n");

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    CHECK(ffuzzy_corpus_len(c) == 0);

    const char *items[] = { "hello world", "fuzzy search", "Smith Waterman" };
    ffuzzy_corpus_add(c, items, 3);
    CHECK(ffuzzy_corpus_len(c) == 3);

    /* Add more */
    const char *more[] = { "bitmap prefilter", "thread pool" };
    ffuzzy_corpus_add(c, more, 2);
    CHECK(ffuzzy_corpus_len(c) == 5);

    ffuzzy_corpus_free(c);
    CHECK(1); /* must not crash */
}

/* ------------------------------------------------------------------ */
/* End-to-end filter tests                                              */
/* ------------------------------------------------------------------ */

static void test_filter(void)
{
    printf("\n[filter]\n");

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    const char *items[] = {
        "fuzzy string matching",
        "Smith-Waterman algorithm",
        "dynamic programming",
        "flutter plugin",
        "bitmap prefilter",
        "thread pool",
        "camelCase variable",
        "snake_case_var",
        "UTF-8 encoding",
        "Hello World",
    };
    ffuzzy_corpus_add(c, items, 10);

    /* Basic search */
    ffuzzy_results_t *r = ffuzzy_filter(c, "fuzz", 1, 0);
    CHECK(r != NULL);
    CHECK(r->len >= 1);
    /* "fuzzy string matching" should appear */
    int found = 0;
    for (uint32_t i = 0; i < r->len; i++)
        if (r->hits[i].index == 0) { found = 1; break; }
    CHECK(found);
    ffuzzy_results_free(r);

    /* Case-insensitive: query "hw" should match "Hello World" */
    r = ffuzzy_filter(c, "hw", 1, 0);
    CHECK(r != NULL);
    found = 0;
    for (uint32_t i = 0; i < r->len; i++)
        if (r->hits[i].index == 9) { found = 1; break; }
    CHECK(found);
    ffuzzy_results_free(r);

    /* Limit */
    r = ffuzzy_filter(c, "a", 1, 3);
    CHECK(r != NULL);
    CHECK(r->len <= 3);
    ffuzzy_results_free(r);

    /* No match */
    r = ffuzzy_filter(c, "zzzzzzzzzzzzz", 1, 0);
    CHECK(r != NULL);
    CHECK(r->len == 0);
    ffuzzy_results_free(r);

    /* Results sorted descending by score */
    r = ffuzzy_filter(c, "fuzzy", 1, 0);
    CHECK(r != NULL);
    for (uint32_t i = 1; i < r->len; i++)
        CHECK(r->hits[i-1].score >= r->hits[i].score);
    ffuzzy_results_free(r);

    /* Indices are populated */
    r = ffuzzy_filter(c, "fl", 1, 0);
    CHECK(r != NULL);
    if (r->len > 0) {
        CHECK(r->hits[0].indices != NULL);
        CHECK(r->hits[0].indices_len == 2);
    }
    ffuzzy_results_free(r);

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* Unicode filter test                                                  */
/* ------------------------------------------------------------------ */

static void test_unicode(void)
{
    printf("\n[unicode]\n");

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    /* items with non-ASCII */
    const char *items[] = {
        "caf\xC3\xA9",           /* café */
        "\xC3\x89l\xC3\xA8ve",   /* Élève */
        "na\xC3\xAFve",          /* naïve */
    };
    ffuzzy_corpus_add(c, items, 3);
    CHECK(ffuzzy_corpus_len(c) == 3);

    ffuzzy_results_t *r = ffuzzy_filter(c, "cafe", 1, 0);
    CHECK(r != NULL);
    /* "café" should match "cafe" with case fold */
    int found = 0;
    for (uint32_t i = 0; i < r->len; i++)
        if (r->hits[i].index == 0) { found = 1; break; }
    CHECK(found);
    ffuzzy_results_free(r);

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== ffuzzy C unit tests ===\n");

    test_bitmap();
    test_utf8();
    test_case_fold();
    test_scorer();
    test_corpus();
    test_filter();
    test_unicode();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
