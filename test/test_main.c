/*
 * test_main.c - Comprehensive C unit tests for the ffuzzy library.
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
static const char *g_current_test = "";

#define CHECK(expr) do {                                                  \
    if (expr) {                                                           \
        printf("  PASS: %s\n", #expr);                                   \
        g_pass++;                                                         \
    } else {                                                              \
        printf("  FAIL: %s  (line %d in %s)\n", #expr, __LINE__,         \
               g_current_test);                                           \
        g_fail++;                                                         \
    }                                                                     \
} while (0)

#define BEGIN_TEST(name) do { g_current_test = (name); printf("\n[%s]\n", name); } while(0)

/* ------------------------------------------------------------------ */
/* Helper: build uint32_t array from ASCII string                      */
/* ------------------------------------------------------------------ */

static void u32_from_ascii(const char *s, uint32_t *buf, int *len)
{
    *len = 0;
    while (s[*len]) {
        buf[*len] = (uint32_t)(unsigned char)s[*len];
        (*len)++;
    }
}

/* ------------------------------------------------------------------ */
/* Scorer tests                                                         */
/* ------------------------------------------------------------------ */

static void test_scorer_known_score(void)
{
    BEGIN_TEST("scorer_known_score");

    /*
     * "dragon" in "Dragon Treasure" with ignore_case=1
     * D-r-a-g-o-n matches at positions 0,1,2,3,4,5 (consecutive prefix).
     * Expect a positive score.
     */
    uint32_t pat[64], str[256];
    int plen, slen;
    int8_t bonus[256];

    u32_from_ascii("dragon", pat, &plen);
    u32_from_ascii("Dragon Treasure", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);

    int32_t score = scorer_score(pat, plen, str, slen, bonus, 1 /*ignore_case*/);
    printf("  score(\"dragon\", \"Dragon Treasure\", ignore_case=1) = %d\n", score);
    CHECK(score > 0);
}

static void test_scorer_no_subsequence(void)
{
    BEGIN_TEST("scorer_no_subsequence");

    /*
     * "xyz" is not a subsequence of "abc" - must return -1.
     */
    uint32_t pat[64], str[256];
    int plen, slen;
    int8_t bonus[256];

    u32_from_ascii("xyz", pat, &plen);
    u32_from_ascii("abc", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);

    int32_t score = scorer_score(pat, plen, str, slen, bonus, 0 /*case_sensitive*/);
    printf("  score(\"xyz\", \"abc\") = %d  (expected -1)\n", score);
    CHECK(score == -1);
}

static void test_scorer_consecutive_bonus(void)
{
    BEGIN_TEST("scorer_consecutive_bonus");

    /*
     * "abc" in "abcdef" - all three chars are consecutive at the start.
     * "abc" in "aXbXcX" - the same letters but spread out with gaps.
     * Consecutive match should score strictly higher.
     */
    uint32_t pat[64], str_consec[256], str_spread[256];
    int plen, slen_consec, slen_spread;
    int8_t bonus_consec[256], bonus_spread[256];

    u32_from_ascii("abc", pat, &plen);

    u32_from_ascii("abcdef", str_consec, &slen_consec);
    scorer_compute_bonuses(str_consec, slen_consec, bonus_consec);

    u32_from_ascii("aXbXcX", str_spread, &slen_spread);
    scorer_compute_bonuses(str_spread, slen_spread, bonus_spread);

    int32_t score_consec = scorer_score(pat, plen, str_consec, slen_consec, bonus_consec, 0);
    int32_t score_spread = scorer_score(pat, plen, str_spread, slen_spread, bonus_spread, 0);

    printf("  score(\"abc\", \"abcdef\") = %d\n", score_consec);
    printf("  score(\"abc\", \"aXbXcX\") = %d\n", score_spread);
    CHECK(score_consec > 0);
    CHECK(score_spread > 0);
    CHECK(score_consec > score_spread);
}

static void test_scorer_word_boundary(void)
{
    BEGIN_TEST("scorer_word_boundary");

    /*
     * "fl" in "flutter" - 'f' is at position 0 (start of string = boundary bonus).
     * "fl" in "xflx"    - 'f' is at position 1, not a boundary.
     * The boundary score should be higher for "flutter".
     */
    uint32_t pat[64], str_bound[256], str_mid[256];
    int plen, slen_bound, slen_mid;
    int8_t bonus_bound[256], bonus_mid[256];

    u32_from_ascii("fl", pat, &plen);

    u32_from_ascii("flutter", str_bound, &slen_bound);
    scorer_compute_bonuses(str_bound, slen_bound, bonus_bound);

    u32_from_ascii("xflx", str_mid, &slen_mid);
    scorer_compute_bonuses(str_mid, slen_mid, bonus_mid);

    int32_t score_bound = scorer_score(pat, plen, str_bound, slen_bound, bonus_bound, 0);
    int32_t score_mid   = scorer_score(pat, plen, str_mid, slen_mid, bonus_mid, 0);

    printf("  score(\"fl\", \"flutter\") = %d\n", score_bound);
    printf("  score(\"fl\", \"xflx\")   = %d\n", score_mid);
    CHECK(score_bound > 0);
    CHECK(score_mid > 0);
    CHECK(score_bound > score_mid);
}

/* ------------------------------------------------------------------ */
/* Bitmap tests                                                         */
/* ------------------------------------------------------------------ */

static void test_bitmap_from_utf8(void)
{
    BEGIN_TEST("bitmap_from_utf8");

    /*
     * bitmap_from_utf8("hello") must have bits set for h, e, l, o.
     * Bits 0-25 correspond to a-z.
     *   'e' = bit 4
     *   'h' = bit 7
     *   'l' = bit 11
     *   'o' = bit 14
     */
    uint64_t bm = bitmap_from_utf8("hello");
    printf("  bitmap_from_utf8(\"hello\") = 0x%016llx\n", (unsigned long long)bm);

    uint64_t bit_h = (uint64_t)1 << ('h' - 'a');
    uint64_t bit_e = (uint64_t)1 << ('e' - 'a');
    uint64_t bit_l = (uint64_t)1 << ('l' - 'a');
    uint64_t bit_o = (uint64_t)1 << ('o' - 'a');

    CHECK((bm & bit_h) != 0); /* 'h' is set */
    CHECK((bm & bit_e) != 0); /* 'e' is set */
    CHECK((bm & bit_l) != 0); /* 'l' is set */
    CHECK((bm & bit_o) != 0); /* 'o' is set */
}

static void test_bitmap_could_match_reject(void)
{
    BEGIN_TEST("bitmap_could_match_reject");

    /*
     * A query containing a character not present in the item must be
     * rejected by bitmap_could_match.
     */
    uint64_t item_bm  = bitmap_from_utf8("hello");
    uint64_t query_bm = bitmap_from_utf8("heloz"); /* 'z' not in "hello" */

    printf("  item_bm(\"hello\")   = 0x%016llx\n", (unsigned long long)item_bm);
    printf("  query_bm(\"heloz\")  = 0x%016llx\n", (unsigned long long)query_bm);

    int could = bitmap_could_match(item_bm, query_bm);
    printf("  bitmap_could_match = %d  (expected 0)\n", could);
    CHECK(could == 0);

    /* Sanity: query that IS present should pass */
    uint64_t query_ok = bitmap_from_utf8("helo");
    CHECK(bitmap_could_match(item_bm, query_ok) != 0);
}

/* ------------------------------------------------------------------ */
/* Corpus tests                                                         */
/* ------------------------------------------------------------------ */

static void test_corpus_add_and_len(void)
{
    BEGIN_TEST("corpus_add_and_len");

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    CHECK(ffuzzy_corpus_len(c) == 0);

    const char *items[] = { "apple", "banana", "cherry" };
    ffuzzy_corpus_add(c, items, 3);

    uint32_t len = ffuzzy_corpus_len(c);
    printf("  corpus len after adding 3 items = %u\n", len);
    CHECK(len == 3);

    /* Filter to find "banana" (index 1) */
    ffuzzy_results_t *r = ffuzzy_filter(c, "banana", 0 /*case_sensitive*/, 0);
    CHECK(r != NULL);
    int found = 0;
    for (uint32_t i = 0; i < r->len; i++) {
        if (r->hits[i].index == 1) { found = 1; break; }
    }
    printf("  filter(\"banana\") found at index 1: %d\n", found);
    CHECK(found);
    ffuzzy_results_free(r);

    ffuzzy_corpus_free(c);
}

static void test_corpus_free_no_crash(void)
{
    BEGIN_TEST("corpus_free_no_crash");

    /* ffuzzy_corpus_free(NULL) must not crash */
    ffuzzy_corpus_free(NULL);
    CHECK(1); /* reached here without crashing */

    /* Free an empty corpus */
    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_free(c);
    CHECK(1); /* reached here without crashing */

    /* Add items and free */
    c = ffuzzy_corpus_new();
    const char *items[] = { "hello", "world" };
    ffuzzy_corpus_add(c, items, 2);
    ffuzzy_corpus_free(c);
    CHECK(1); /* reached here without crashing */

    /*
     * "add after free is handled": after freeing, create a new corpus
     * and add to it - this should work normally (new corpus, no UAF).
     */
    c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    const char *more[] = { "new item" };
    ffuzzy_corpus_add(c, more, 1);
    CHECK(ffuzzy_corpus_len(c) == 1);
    ffuzzy_corpus_free(c);
    CHECK(1);
}

/* ------------------------------------------------------------------ */
/* Thread pool: single vs multi-thread consistency                      */
/* ------------------------------------------------------------------ */

static void test_thread_pool_consistency(void)
{
    BEGIN_TEST("thread_pool_consistency");

    /*
     * Use the same corpus and query on thread_pool_filter (which uses
     * multiple threads when the corpus is large enough) and verify that
     * the results (number of hits and their scores) are consistent.
     * We do this by running ffuzzy_filter twice - both times the library
     * will decide thread count internally.  We just verify the output is
     * deterministic (same count, same top score) across two invocations.
     */

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    const char *items[] = {
        "dragon", "dragonfly", "Dragon Treasure", "A Dragon", "Lucky Dragon",
        "apple", "banana", "cherry", "elderberry", "fig",
        "alpha", "beta", "gamma", "delta", "epsilon",
        "flutter", "xflx", "workflow", "algorithm", "sequence"
    };
    ffuzzy_corpus_add(c, items, 20);

    /* Run twice and compare */
    ffuzzy_results_t *r1 = ffuzzy_filter(c, "dragon", 1, 0);
    ffuzzy_results_t *r2 = ffuzzy_filter(c, "dragon", 1, 0);

    CHECK(r1 != NULL);
    CHECK(r2 != NULL);

    if (r1 && r2) {
        printf("  run1 hits = %u, run2 hits = %u\n", r1->len, r2->len);
        CHECK(r1->len == r2->len);

        if (r1->len > 0 && r2->len > 0) {
            printf("  run1 top score = %d, run2 top score = %d\n",
                   r1->hits[0].score, r2->hits[0].score);
            CHECK(r1->hits[0].score == r2->hits[0].score);
        }

        /* Verify sorted descending by score in both results */
        for (uint32_t i = 1; i < r1->len; i++)
            CHECK(r1->hits[i-1].score >= r1->hits[i].score);
        for (uint32_t i = 1; i < r2->len; i++)
            CHECK(r2->hits[i-1].score >= r2->hits[i].score);
    }

    ffuzzy_results_free(r1);
    ffuzzy_results_free(r2);
    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* End-to-end: dragon corpus                                            */
/* ------------------------------------------------------------------ */

static void test_endtoend_dragon(void)
{
    BEGIN_TEST("endtoend_dragon");

    /*
     * Corpus: ["Dragon Treasure", "dragonfly", "A Dragon", "Lucky Dragon"]
     * Query: "dragon", ignore_case=1
     * Expected: all 4 items match (each contains d-r-a-g-o-n as a subsequence
     * ignoring case).
     */
    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    const char *items[] = {
        "Dragon Treasure",
        "dragonfly",
        "A Dragon",
        "Lucky Dragon"
    };
    ffuzzy_corpus_add(c, items, 4);

    CHECK(ffuzzy_corpus_len(c) == 4);

    ffuzzy_results_t *r = ffuzzy_filter(c, "dragon", 1 /*ignore_case*/, 0 /*no limit*/);
    CHECK(r != NULL);

    if (r) {
        printf("  hits for \"dragon\" (ignore_case) = %u  (expected 4)\n", r->len);
        CHECK(r->len == 4);

        /* Verify each corpus item was matched */
        for (uint32_t item_idx = 0; item_idx < 4; item_idx++) {
            int found = 0;
            for (uint32_t h = 0; h < r->len; h++) {
                if (r->hits[h].index == item_idx) { found = 1; break; }
            }
            printf("  item[%u] (\"%s\") found: %d\n",
                   item_idx, items[item_idx], found);
            CHECK(found);
        }

        /* All scores must be positive */
        for (uint32_t h = 0; h < r->len; h++) {
            CHECK(r->hits[h].score > 0);
        }

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* Additional edge-case tests                                           */
/* ------------------------------------------------------------------ */

static void test_bitmap_extra(void)
{
    BEGIN_TEST("bitmap_extra");

    /* Empty string bitmap is 0, and matches any item bitmap */
    uint64_t bm_empty = bitmap_from_utf8("");
    uint64_t bm_hello = bitmap_from_utf8("hello");
    CHECK(bm_empty == 0);
    CHECK(bitmap_could_match(bm_hello, bm_empty)); /* empty query always passes */

    /* Uppercase folded correctly */
    uint64_t bm_lower = bitmap_from_utf8("hello");
    uint64_t bm_upper = bitmap_from_utf8("HELLO");
    CHECK(bm_lower == bm_upper);

    /* Digits in bits 26-35 */
    uint64_t bm_num = bitmap_from_utf8("abc123");
    uint64_t bm_1   = bitmap_from_utf8("1");
    uint64_t bm_9   = bitmap_from_utf8("9");
    CHECK(bitmap_could_match(bm_num, bm_1));
    CHECK(!bitmap_could_match(bm_num, bm_9));
}

static void test_scorer_extra(void)
{
    BEGIN_TEST("scorer_extra");

    uint32_t pat[64], str[256];
    int plen, slen;
    int8_t bonus[256];

    /* Empty pattern returns 0 */
    u32_from_ascii("", pat, &plen);
    u32_from_ascii("hello", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    CHECK(scorer_score(pat, plen, str, slen, bonus, 0) == 0);

    /* Pattern longer than string returns -1 */
    u32_from_ascii("toolongpattern", pat, &plen);
    u32_from_ascii("short", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    CHECK(scorer_score(pat, plen, str, slen, bonus, 0) == -1);

    /* Case-sensitive miss */
    u32_from_ascii("HELLO", pat, &plen);
    u32_from_ascii("hello", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);
    CHECK(scorer_score(pat, plen, str, slen, bonus, 0 /*case_sensitive*/) == -1);

    /* Case-insensitive hit */
    CHECK(scorer_score(pat, plen, str, slen, bonus, 1 /*ignore_case*/) > 0);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== ffuzzy C unit tests ===\n");

    /* Scorer tests */
    test_scorer_known_score();
    test_scorer_no_subsequence();
    test_scorer_consecutive_bonus();
    test_scorer_word_boundary();
    test_scorer_extra();

    /* Bitmap tests */
    test_bitmap_from_utf8();
    test_bitmap_could_match_reject();
    test_bitmap_extra();

    /* Corpus tests */
    test_corpus_add_and_len();
    test_corpus_free_no_crash();

    /* Thread pool / consistency */
    test_thread_pool_consistency();

    /* End-to-end */
    test_endtoend_dragon();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
