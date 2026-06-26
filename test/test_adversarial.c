/*
 * test_adversarial.c - Adversarial / stress tests for the ffuzzy library.
 *
 * Build: added to test/CMakeLists.txt as target ffuzzy_adversarial.
 * Exit code 0 = all tests passed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

#include "../src/ffuzzy.h"
#include "../src/scorer.h"
#include "../src/corpus.h"

/* ------------------------------------------------------------------ */
/* Minimal test framework                                               */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current_test = "";

#define CHECK(expr) do {                                                      \
    if (expr) {                                                               \
        printf("  PASS: %s\n", #expr);                                        \
        g_pass++;                                                              \
    } else {                                                                  \
        printf("  FAIL: %s  (line %d in %s)\n", #expr, __LINE__,             \
               g_current_test);                                               \
        g_fail++;                                                              \
    }                                                                         \
} while (0)

#define BEGIN_TEST(name) do {                                                 \
    g_current_test = (name);                                                  \
    printf("\n[%s]\n", name);                                                 \
} while (0)

/* ------------------------------------------------------------------ */
/* Portable wall-clock milliseconds                                     */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
#endif
}

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

/* ================================================================== */
/* BOUNDARY TESTS                                                       */
/* ================================================================== */

/* 1. Empty corpus, filter -> empty results, no crash */
static void test_boundary_empty_corpus(void)
{
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;

    BEGIN_TEST("boundary_empty_corpus");

    c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    CHECK(ffuzzy_corpus_len(c) == 0);

    r = ffuzzy_filter(c, "hello", 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits = %u  (expected 0)\n", r->len);
        CHECK(r->len == 0);
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 2. Pattern longer than every corpus item -> 0 hits */
static void test_boundary_pattern_longer_than_items(void)
{
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[] = { "ab", "cd", "ef" };

    BEGIN_TEST("boundary_pattern_longer_than_items");

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 3);

    /* Query length 10 > each item length 2 */
    r = ffuzzy_filter(c, "abcdefghij", 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits = %u  (expected 0)\n", r->len);
        CHECK(r->len == 0);
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 3. Single char query "a" -> matches all items containing "a" */
static void test_boundary_single_char_query(void)
{
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    /* apple=yes(0), banana=yes(1), cherry=no(2), date=yes(3), elderberry=no(4) */
    const char *items[] = { "apple", "banana", "cherry", "date", "elderberry" };

    BEGIN_TEST("boundary_single_char_query");

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 5);

    r = ffuzzy_filter(c, "a", 0, 0);
    CHECK(r != NULL);
    if (r) {
        uint32_t i;
        int found_apple = 0, found_banana = 0, found_date = 0;

        printf("  hits for 'a' = %u  (expected 3: apple,banana,date)\n", r->len);
        CHECK(r->len == 3);

        for (i = 0; i < r->len; i++) {
            if (r->hits[i].index == 0) found_apple  = 1;
            if (r->hits[i].index == 1) found_banana = 1;
            if (r->hits[i].index == 3) found_date   = 1;
        }
        CHECK(found_apple);
        CHECK(found_banana);
        CHECK(found_date);

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 4. Query equals item exactly -> that item scores highest */
static void test_boundary_exact_match_top_score(void)
{
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[] = { "xdragonx", "dragon", "dragonfly" };

    BEGIN_TEST("boundary_exact_match_top_score");

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 3);

    r = ffuzzy_filter(c, "dragon", 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits = %u\n", r->len);
        CHECK(r->len >= 1);

        if (r->len >= 1) {
            /* Results are sorted descending by score; top hit must be
               the exact match (index 1 = "dragon") */
            printf("  top hit index = %u  (expected 1), score = %d\n",
                   r->hits[0].index, r->hits[0].score);
            CHECK(r->hits[0].index == 1);
        }

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 5. All items identical -> all returned, same score */
static void test_boundary_all_identical(void)
{
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[] = { "hello", "hello", "hello", "hello", "hello" };

    BEGIN_TEST("boundary_all_identical");

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 5);

    r = ffuzzy_filter(c, "hel", 0, 0);
    CHECK(r != NULL);
    if (r) {
        uint32_t i;
        int all_same = 1;

        printf("  hits = %u  (expected 5)\n", r->len);
        CHECK(r->len == 5);

        for (i = 1; i < r->len; i++) {
            if (r->hits[i].score != r->hits[0].score) { all_same = 0; break; }
        }
        printf("  all scores equal: %d  (expected 1)\n", all_same);
        CHECK(all_same);

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 6. Item with 10000 chars -> no stack overflow (heap DP arrays) */
static void test_boundary_long_item_no_stack_overflow(void)
{
    const int ITEM_LEN = 10000;
    char *big_item;
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[1];
    int i;

    BEGIN_TEST("boundary_long_item_no_stack_overflow");

    big_item = (char *)malloc((size_t)ITEM_LEN + 1);
    CHECK(big_item != NULL);
    if (!big_item) return;

    /* Fill with repeating alphabet so query chars exist */
    for (i = 0; i < ITEM_LEN; i++)
        big_item[i] = (char)('a' + (i % 26));
    big_item[ITEM_LEN] = '\0';

    c = ffuzzy_corpus_new();
    items[0] = big_item;
    ffuzzy_corpus_add(c, items, 1);

    printf("  corpus len = %u  (expected 1)\n", ffuzzy_corpus_len(c));
    CHECK(ffuzzy_corpus_len(c) == 1);

    /* Query: a short substring that definitely exists */
    r = ffuzzy_filter(c, "abc", 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits = %u  (expected 1)\n", r->len);
        CHECK(r->len == 1);
        CHECK(r->hits[0].score > 0);
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
    free(big_item);
}

/* 7. Pattern with 100 chars -> correct behavior */
static void test_boundary_long_pattern(void)
{
    char pattern[101];
    char big_item[201];
    char small_item[51];
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[2];

    BEGIN_TEST("boundary_long_pattern");

    memset(pattern,   'a', 100); pattern[100]   = '\0';
    memset(big_item,  'a', 200); big_item[200]  = '\0';
    memset(small_item,'a',  50); small_item[50] = '\0';

    c = ffuzzy_corpus_new();
    items[0] = big_item;
    items[1] = small_item;
    ffuzzy_corpus_add(c, items, 2);

    r = ffuzzy_filter(c, pattern, 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits for 100-char pattern = %u  (expected 1)\n", r->len);
        CHECK(r->len == 1);
        if (r->len >= 1) {
            printf("  matched index = %u  (expected 0=big_item)\n",
                   r->hits[0].index);
            CHECK(r->hits[0].index == 0);
        }
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ================================================================== */
/* MEMORY TESTS                                                         */
/* ================================================================== */

/* 8. Create+fill+free corpus 1000 times -> no crash, no leak growth */
static void test_memory_repeated_create_fill_free(void)
{
    const int ITERS = 1000;
    const char *items[] = {
        "apple", "banana", "cherry", "date", "elderberry",
        "fig", "grape", "honeydew", "kiwi", "lemon"
    };
    int iter;

    BEGIN_TEST("memory_repeated_create_fill_free");

    for (iter = 0; iter < ITERS; iter++) {
        ffuzzy_corpus_t  *c = ffuzzy_corpus_new();
        ffuzzy_results_t *r;
        if (!c) continue;
        ffuzzy_corpus_add(c, items, 10);

        r = ffuzzy_filter(c, "an", 0, 0);
        ffuzzy_results_free(r);

        ffuzzy_corpus_free(c);
    }

    printf("  completed %d create/fill/filter/free cycles\n", ITERS);
    CHECK(1);
}

/* 9. ffuzzy_corpus_free(NULL) -> no crash */
static void test_memory_free_null(void)
{
    BEGIN_TEST("memory_free_null");

    ffuzzy_corpus_free(NULL);
    CHECK(1); /* reached without crash */

    ffuzzy_results_free(NULL);
    CHECK(1); /* reached without crash */
}

/* 10. Corpus with 100000 items, filter with limit=1 -> returns in <2 seconds */
static void test_memory_large_corpus_limit(void)
{
#define LARGE_N 100000
    char **strs;
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    double t0, elapsed;
    int i;
    const char *templates[] = {
        "dragon_%d", "dragonfly_%d", "xdragonx_%d",
        "apple_%d",  "banana_%d",    "cherry_%d"
    };
    int ntpl = (int)(sizeof(templates) / sizeof(templates[0]));

    BEGIN_TEST("memory_large_corpus_limit");

    strs = (char **)malloc((size_t)LARGE_N * sizeof(char *));
    CHECK(strs != NULL);
    if (!strs) return;

    for (i = 0; i < LARGE_N; i++) {
        strs[i] = (char *)malloc(64);
        if (!strs[i]) break;
        snprintf(strs[i], 64, templates[i % ntpl], i);
    }

    c = ffuzzy_corpus_new();
    CHECK(c != NULL);

    /* Add in batches of 1000 */
    for (i = 0; i < LARGE_N; i += 1000) {
        int batch = ((i + 1000) <= LARGE_N) ? 1000 : (LARGE_N - i);
        ffuzzy_corpus_add(c, (const char **)&strs[i], (uint32_t)batch);
    }

    printf("  corpus len = %u  (expected %d)\n",
           ffuzzy_corpus_len(c), LARGE_N);
    CHECK(ffuzzy_corpus_len(c) == (uint32_t)LARGE_N);

    t0 = now_ms();
    r = ffuzzy_filter(c, "dragon", 1 /*ignore_case*/, 1 /*limit=1*/);
    elapsed = now_ms() - t0;
    printf("  filter(limit=1) returned in %.1f ms  (must be < 2000 ms)\n",
           elapsed);

    CHECK(r != NULL);
    if (r) {
        CHECK(r->len == 1);
        ffuzzy_results_free(r);
    }
    CHECK(elapsed < 2000.0);

    ffuzzy_corpus_free(c);
    for (i = 0; i < LARGE_N; i++) free(strs[i]);
    free(strs);
#undef LARGE_N
}

/* ================================================================== */
/* SCORING CORRECTNESS TESTS                                            */
/* ================================================================== */

/* 11. "abc" consecutive scores higher than "abc" scattered */
static void test_scoring_consecutive_beats_scattered(void)
{
    uint32_t pat[64], str_consec[64], str_scattered[64];
    int plen, slen_consec, slen_scattered;
    int8_t bonus_consec[64], bonus_scattered[64];
    int32_t score_consec, score_scattered;

    BEGIN_TEST("scoring_consecutive_beats_scattered");

    u32_from_ascii("abc", pat, &plen);

    u32_from_ascii("abcdef", str_consec, &slen_consec);
    scorer_compute_bonuses(str_consec, slen_consec, bonus_consec);
    score_consec = scorer_score(pat, plen,
                                str_consec, slen_consec,
                                bonus_consec, 0);

    u32_from_ascii("axbxcx", str_scattered, &slen_scattered);
    scorer_compute_bonuses(str_scattered, slen_scattered, bonus_scattered);
    score_scattered = scorer_score(pat, plen,
                                   str_scattered, slen_scattered,
                                   bonus_scattered, 0);

    printf("  score(\"abc\",\"abcdef\")  = %d\n", score_consec);
    printf("  score(\"abc\",\"axbxcx\") = %d\n", score_scattered);
    CHECK(score_consec > 0);
    CHECK(score_scattered > 0);
    CHECK(score_consec > score_scattered);
}

/* 12. Prefix match ("drag" in "dragon") scores higher than mid match
        ("drag" in "xdragx")                                           */
static void test_scoring_prefix_beats_mid(void)
{
    uint32_t pat[64], str_prefix[64], str_mid[64];
    int plen, slen_prefix, slen_mid;
    int8_t bonus_prefix[64], bonus_mid[64];
    int32_t score_prefix, score_mid;

    BEGIN_TEST("scoring_prefix_beats_mid");

    u32_from_ascii("drag", pat, &plen);

    u32_from_ascii("dragon", str_prefix, &slen_prefix);
    scorer_compute_bonuses(str_prefix, slen_prefix, bonus_prefix);
    score_prefix = scorer_score(pat, plen,
                                str_prefix, slen_prefix,
                                bonus_prefix, 0);

    u32_from_ascii("xdragx", str_mid, &slen_mid);
    scorer_compute_bonuses(str_mid, slen_mid, bonus_mid);
    score_mid = scorer_score(pat, plen,
                             str_mid, slen_mid,
                             bonus_mid, 0);

    printf("  score(\"drag\",\"dragon\") = %d\n", score_prefix);
    printf("  score(\"drag\",\"xdragx\") = %d\n", score_mid);
    CHECK(score_prefix > 0);
    CHECK(score_mid > 0);
    CHECK(score_prefix > score_mid);
}

/* 13. Case insensitive: score("DRAGON","dragon") == score("dragon","dragon") */
static void test_scoring_case_insensitive_equal(void)
{
    uint32_t pat_upper[64], pat_lower[64], str[64];
    int plen_upper, plen_lower, slen;
    int8_t bonus[64];
    int32_t score_upper, score_lower;

    BEGIN_TEST("scoring_case_insensitive_equal");

    u32_from_ascii("DRAGON", pat_upper, &plen_upper);
    u32_from_ascii("dragon", pat_lower, &plen_lower);
    u32_from_ascii("dragon", str, &slen);
    scorer_compute_bonuses(str, slen, bonus);

    score_upper = scorer_score(pat_upper, plen_upper,
                               str, slen, bonus, 1 /*ignore_case*/);
    score_lower = scorer_score(pat_lower, plen_lower,
                               str, slen, bonus, 1 /*ignore_case*/);

    printf("  score(\"DRAGON\",\"dragon\",ic=1) = %d\n", score_upper);
    printf("  score(\"dragon\",\"dragon\",ic=1) = %d\n", score_lower);
    CHECK(score_upper > 0);
    CHECK(score_lower > 0);
    CHECK(score_upper == score_lower);
}

/* ================================================================== */
/* UNICODE TESTS                                                        */
/* ================================================================== */

/* 14. UTF-8 multi-byte char in corpus item -> no crash, correct char count */
static void test_unicode_multibyte_no_crash(void)
{
    /* "cafe" with e-acute: U+00E9 = 0xC3 0xA9 in UTF-8 -> 4 code points */
    const char *cafe_utf8 = "caf\xC3\xA9";  /* cafe + e-acute */
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r;
    const char *items[3];

    BEGIN_TEST("unicode_multibyte_no_crash");

    items[0] = cafe_utf8;
    items[1] = "plain";
    items[2] = "caf";

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 3);

    printf("  corpus len = %u  (expected 3)\n", ffuzzy_corpus_len(c));
    CHECK(ffuzzy_corpus_len(c) == 3);

    /* "cafe+e-acute" is 5 bytes but 4 code points */
    printf("  u32len(cafe+eacute) = %d  (expected 4)\n", c->u32lens[0]);
    CHECK(c->u32lens[0] == 4);

    /* "caf" should match "cafe+eacute" and "caf" but not "plain" */
    r = ffuzzy_filter(c, "caf", 0, 0);
    CHECK(r != NULL);
    if (r) {
        printf("  hits for \"caf\" = %u  (expected 2)\n", r->len);
        CHECK(r->len == 2);
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* 15. Chinese characters in query and corpus -> no crash */
static void test_unicode_chinese_no_crash(void)
{
    /*
     * U+4F60 U+597D = two CJK code points, each 3 bytes in UTF-8.
     * We use raw hex escapes to avoid MSVC codepage issues with source files.
     *   U+4F60: 0xE4 0xBD 0xA0
     *   U+597D: 0xE5 0xA5 0xBD
     *   U+6211: 0xE6 0x88 0x91
     */
    const char *nihao = "\xE4\xBD\xA0\xE5\xA5\xBD"; /* 2 CJK chars */
    const char *wo    = "\xE6\x88\x91";              /* 1 CJK char  */
    const char *mixed = "hello\xE4\xBD\xA0world";    /* 11 code pts */
    ffuzzy_corpus_t  *c;
    ffuzzy_results_t *r1;
    ffuzzy_results_t *r2;
    const char *items[4];

    BEGIN_TEST("unicode_chinese_no_crash");

    items[0] = nihao;
    items[1] = wo;
    items[2] = mixed;
    items[3] = "hello";

    c = ffuzzy_corpus_new();
    ffuzzy_corpus_add(c, items, 4);

    printf("  corpus len = %u  (expected 4)\n", ffuzzy_corpus_len(c));
    CHECK(ffuzzy_corpus_len(c) == 4);

    /* Verify UTF-32 lengths */
    printf("  u32len(nihao) = %d  (expected 2)\n", c->u32lens[0]);
    CHECK(c->u32lens[0] == 2);
    printf("  u32len(wo)    = %d  (expected 1)\n", c->u32lens[1]);
    CHECK(c->u32lens[1] == 1);
    printf("  u32len(mixed) = %d  (expected 11)\n", c->u32lens[2]);
    CHECK(c->u32lens[2] == 11);

    /* Query using CJK bytes - must not crash */
    r1 = ffuzzy_filter(c, nihao, 0, 0);
    CHECK(r1 != NULL);
    if (r1) {
        printf("  hits for nihao query = %u  (expected >=1)\n", r1->len);
        CHECK(r1->len >= 1);
        ffuzzy_results_free(r1);
    }

    /* ASCII query should match items 2 and 3 (both contain "hello") */
    r2 = ffuzzy_filter(c, "hello", 0, 0);
    CHECK(r2 != NULL);
    if (r2) {
        printf("  hits for \"hello\" = %u  (expected 2)\n", r2->len);
        CHECK(r2->len == 2);
        ffuzzy_results_free(r2);
    }

    ffuzzy_corpus_free(c);
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */

int main(void)
{
    printf("=== ffuzzy adversarial tests ===\n");

    /* Boundary */
    test_boundary_empty_corpus();
    test_boundary_pattern_longer_than_items();
    test_boundary_single_char_query();
    test_boundary_exact_match_top_score();
    test_boundary_all_identical();
    test_boundary_long_item_no_stack_overflow();
    test_boundary_long_pattern();

    /* Memory */
    test_memory_repeated_create_fill_free();
    test_memory_free_null();
    test_memory_large_corpus_limit();

    /* Scoring correctness */
    test_scoring_consecutive_beats_scattered();
    test_scoring_prefix_beats_mid();
    test_scoring_case_insensitive_equal();

    /* Unicode */
    test_unicode_multibyte_no_crash();
    test_unicode_chinese_no_crash();

    printf("\n=== Adversarial Results: %d passed, %d failed ===\n",
           g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
