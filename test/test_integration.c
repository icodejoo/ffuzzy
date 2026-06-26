/*
 * test_integration.c - Smoke tests and integration tests for the ffuzzy library.
 *
 * Build with test/CMakeLists.txt (links against the ffuzzy source files).
 * Exit code 0 = all tests passed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ffuzzy.h"

/* ------------------------------------------------------------------ */
/* Minimal test framework (mirrors test_main.c style)                  */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current_test = "";

#define CHECK(expr) do {                                                   \
    if (expr) {                                                            \
        printf("  PASS: %s\n", #expr);                                    \
        g_pass++;                                                          \
    } else {                                                               \
        printf("  FAIL: %s  (line %d in %s)\n", #expr, __LINE__,          \
               g_current_test);                                            \
        g_fail++;                                                          \
    }                                                                      \
} while (0)

#define BEGIN_TEST(name) \
    do { g_current_test = (name); printf("\n[%s]\n", name); } while(0)

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Return 1 if any hit in r has r->hits[i].index == idx */
static int results_contain_index(const ffuzzy_results_t *r, uint32_t idx)
{
    if (!r) return 0;
    for (uint32_t i = 0; i < r->len; i++) {
        if (r->hits[i].index == idx) return 1;
    }
    return 0;
}

/* Return 1 if the results are sorted in descending score order */
static int results_sorted_desc(const ffuzzy_results_t *r)
{
    if (!r || r->len <= 1) return 1;
    for (uint32_t i = 1; i < r->len; i++) {
        if (r->hits[i-1].score < r->hits[i].score) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* SMOKE TEST 1                                                         */
/* Corpus: ["Dragon Treasure","dragonfly","A Dragon","Golden Fortune",  */
/*          "Super Gems 1000","Lucky Dragon"]                           */
/* Query "dragon" ignoreCase=true -> at least 3 hits,                  */
/* Dragon items in top results                                          */
/* ------------------------------------------------------------------ */

static void smoke_test_dragon_search(void)
{
    BEGIN_TEST("smoke_dragon_search");

    const char *items[] = {
        "Dragon Treasure",   /* 0 - contains dragon */
        "dragonfly",         /* 1 - contains dragon */
        "A Dragon",          /* 2 - contains dragon */
        "Golden Fortune",    /* 3 - no dragon */
        "Super Gems 1000",   /* 4 - no dragon */
        "Lucky Dragon"       /* 5 - contains dragon */
    };

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_add(c, items, 6);
    CHECK(ffuzzy_corpus_len(c) == 6);

    ffuzzy_results_t *r = ffuzzy_filter(c, "dragon", 1 /*ignore_case*/, 0 /*no limit*/);
    CHECK(r != NULL);

    if (r) {
        printf("  hits for \"dragon\" (ignore_case=1) = %u  (expected >= 3)\n", r->len);
        CHECK(r->len >= 3);

        /* Dragon items (indices 0, 1, 2, 5) must all be present */
        int found0 = results_contain_index(r, 0);
        int found1 = results_contain_index(r, 1);
        int found2 = results_contain_index(r, 2);
        int found5 = results_contain_index(r, 5);

        printf("  item[0] \"Dragon Treasure\" found: %d\n", found0);
        printf("  item[1] \"dragonfly\"        found: %d\n", found1);
        printf("  item[2] \"A Dragon\"          found: %d\n", found2);
        printf("  item[5] \"Lucky Dragon\"      found: %d\n", found5);

        CHECK(found0);
        CHECK(found1);
        CHECK(found2);
        CHECK(found5);

        /* Results must be sorted descending by score */
        CHECK(results_sorted_desc(r));

        /* Scores for dragon items must be positive */
        for (uint32_t i = 0; i < r->len; i++) {
            CHECK(r->hits[i].score > 0);
        }

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* SMOKE TEST 2                                                         */
/* Query "gem" -> hits contain index of "Super Gems 1000"              */
/* ------------------------------------------------------------------ */

static void smoke_test_gem_search(void)
{
    BEGIN_TEST("smoke_gem_search");

    const char *items[] = {
        "Dragon Treasure",   /* 0 */
        "dragonfly",         /* 1 */
        "A Dragon",          /* 2 */
        "Golden Fortune",    /* 3 */
        "Super Gems 1000",   /* 4 */
        "Lucky Dragon"       /* 5 */
    };

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_add(c, items, 6);

    ffuzzy_results_t *r = ffuzzy_filter(c, "gem", 1 /*ignore_case*/, 0);
    CHECK(r != NULL);

    if (r) {
        printf("  hits for \"gem\" = %u\n", r->len);
        CHECK(r->len >= 1);

        /* "Super Gems 1000" is at index 4 */
        int found4 = results_contain_index(r, 4);
        printf("  item[4] \"Super Gems 1000\" found: %d\n", found4);
        CHECK(found4);

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* SMOKE TEST 3                                                         */
/* Query "zzznomatch" -> 0 hits                                         */
/* ------------------------------------------------------------------ */

static void smoke_test_no_match(void)
{
    BEGIN_TEST("smoke_no_match");

    const char *items[] = {
        "Dragon Treasure",
        "dragonfly",
        "A Dragon",
        "Golden Fortune",
        "Super Gems 1000",
        "Lucky Dragon"
    };

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_add(c, items, 6);

    ffuzzy_results_t *r = ffuzzy_filter(c, "zzznomatch", 0 /*case_sensitive*/, 0);
    CHECK(r != NULL);

    if (r) {
        printf("  hits for \"zzznomatch\" = %u  (expected 0)\n", r->len);
        CHECK(r->len == 0);
        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* SMOKE TEST 4                                                         */
/* Empty query -> all 6 items returned with score 0                    */
/* ------------------------------------------------------------------ */

static void smoke_test_empty_query(void)
{
    BEGIN_TEST("smoke_empty_query");

    const char *items[] = {
        "Dragon Treasure",
        "dragonfly",
        "A Dragon",
        "Golden Fortune",
        "Super Gems 1000",
        "Lucky Dragon"
    };

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_add(c, items, 6);

    ffuzzy_results_t *r = ffuzzy_filter(c, "", 0, 0);
    CHECK(r != NULL);

    if (r) {
        printf("  hits for \"\" (empty query) = %u  (expected 6)\n", r->len);
        CHECK(r->len == 6);

        /* All scores must be 0 */
        for (uint32_t i = 0; i < r->len; i++) {
            CHECK(r->hits[i].score == 0);
        }

        /* All indices 0..5 must be present */
        for (uint32_t idx = 0; idx < 6; idx++) {
            CHECK(results_contain_index(r, idx));
        }

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* SMOKE TEST 5                                                         */
/* limit=2 -> exactly 2 results                                         */
/* ------------------------------------------------------------------ */

static void smoke_test_limit(void)
{
    BEGIN_TEST("smoke_limit");

    const char *items[] = {
        "Dragon Treasure",
        "dragonfly",
        "A Dragon",
        "Golden Fortune",
        "Super Gems 1000",
        "Lucky Dragon"
    };

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    ffuzzy_corpus_add(c, items, 6);

    ffuzzy_results_t *r = ffuzzy_filter(c, "dragon", 1 /*ignore_case*/, 2 /*limit*/);
    CHECK(r != NULL);

    if (r) {
        printf("  hits with limit=2 = %u  (expected exactly 2)\n", r->len);
        CHECK(r->len == 2);

        /* Top 2 should be sorted */
        if (r->len == 2) {
            CHECK(r->hits[0].score >= r->hits[1].score);
        }

        ffuzzy_results_free(r);
    }

    ffuzzy_corpus_free(c);
}

/* ------------------------------------------------------------------ */
/* INTEGRATION TEST 6                                                   */
/* Add 10000 generated items then query, verify limit=10 returns        */
/* exactly 10                                                           */
/* ------------------------------------------------------------------ */

static void integration_test_large_corpus(void)
{
    BEGIN_TEST("integration_large_corpus");

#define LARGE_N 10000

    /* Build an array of 10000 string pointers */
    char **strs = (char **)malloc(LARGE_N * sizeof(char *));
    CHECK(strs != NULL);
    if (!strs) return;

    for (int i = 0; i < LARGE_N; i++) {
        strs[i] = (char *)malloc(32);
        if (!strs[i]) {
            /* Free already-allocated entries */
            for (int j = 0; j < i; j++) free(strs[j]);
            free(strs);
            CHECK(0); /* allocation failure */
            return;
        }
        /* Every 100th item contains "dragon" so we get hits */
        if (i % 100 == 0)
            sprintf(strs[i], "dragon item %d", i);
        else
            sprintf(strs[i], "item %d", i);
    }

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);

    if (c) {
        ffuzzy_corpus_add(c, (const char **)strs, LARGE_N);
        uint32_t clen = ffuzzy_corpus_len(c);
        printf("  corpus len after adding %d items = %u\n", LARGE_N, clen);
        CHECK(clen == LARGE_N);

        ffuzzy_results_t *r = ffuzzy_filter(c, "dragon", 1 /*ignore_case*/, 10 /*limit*/);
        CHECK(r != NULL);

        if (r) {
            printf("  hits with limit=10 = %u  (expected exactly 10)\n", r->len);
            CHECK(r->len == 10);

            /* Results must be sorted descending */
            CHECK(results_sorted_desc(r));

            ffuzzy_results_free(r);
        }

        ffuzzy_corpus_free(c);
    }

    for (int i = 0; i < LARGE_N; i++) free(strs[i]);
    free(strs);

#undef LARGE_N
}

/* ------------------------------------------------------------------ */
/* INTEGRATION TEST 7                                                   */
/* Add items incrementally (100 batches of 100) then search ->          */
/* correct results                                                      */
/* ------------------------------------------------------------------ */

static void integration_test_incremental_add(void)
{
    BEGIN_TEST("integration_incremental_add");

#define BATCH_SIZE  100
#define BATCH_COUNT 100
#define TOTAL_ITEMS (BATCH_SIZE * BATCH_COUNT)

    /* Pre-generate all strings */
    char **strs = (char **)malloc(TOTAL_ITEMS * sizeof(char *));
    CHECK(strs != NULL);
    if (!strs) return;

    for (int i = 0; i < TOTAL_ITEMS; i++) {
        strs[i] = (char *)malloc(32);
        if (!strs[i]) {
            for (int j = 0; j < i; j++) free(strs[j]);
            free(strs);
            CHECK(0);
            return;
        }
        /* Put a "treasure" item at index 42 */
        if (i == 42)
            sprintf(strs[i], "Golden Treasure");
        else if (i % 200 == 0)
            sprintf(strs[i], "treasure chest %d", i);
        else
            sprintf(strs[i], "item %d", i);
    }

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);

    if (c) {
        /* Add in 100 batches of 100 */
        for (int b = 0; b < BATCH_COUNT; b++) {
            ffuzzy_corpus_add(c,
                              (const char **)(strs + b * BATCH_SIZE),
                              BATCH_SIZE);
        }

        uint32_t clen = ffuzzy_corpus_len(c);
        printf("  corpus len after %d batches of %d = %u  (expected %d)\n",
               BATCH_COUNT, BATCH_SIZE, clen, TOTAL_ITEMS);
        CHECK(clen == TOTAL_ITEMS);

        /* Search for "treasure" - must find index 42 */
        ffuzzy_results_t *r = ffuzzy_filter(c, "treasure", 1 /*ignore_case*/, 0);
        CHECK(r != NULL);

        if (r) {
            printf("  hits for \"treasure\" = %u\n", r->len);
            CHECK(r->len >= 1);

            int found42 = results_contain_index(r, 42);
            printf("  item[42] \"Golden Treasure\" found: %d\n", found42);
            CHECK(found42);

            /* Results sorted */
            CHECK(results_sorted_desc(r));

            ffuzzy_results_free(r);
        }

        ffuzzy_corpus_free(c);
    }

    for (int i = 0; i < TOTAL_ITEMS; i++) free(strs[i]);
    free(strs);

#undef BATCH_SIZE
#undef BATCH_COUNT
#undef TOTAL_ITEMS
}

/* ------------------------------------------------------------------ */
/* INTEGRATION TEST 8                                                   */
/* ffuzzy_results_free(NULL) -> no crash                                */
/* ------------------------------------------------------------------ */

static void integration_test_free_null(void)
{
    BEGIN_TEST("integration_free_null");

    /* Must not crash */
    ffuzzy_results_free(NULL);
    CHECK(1); /* reached here without crashing */

    /* Also free a real empty result */
    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);
    if (c) {
        const char *items[] = { "alpha", "beta" };
        ffuzzy_corpus_add(c, items, 2);

        ffuzzy_results_t *r = ffuzzy_filter(c, "zzznomatch", 0, 0);
        CHECK(r != NULL);
        ffuzzy_results_free(r); /* free result with 0 hits */
        CHECK(1);

        ffuzzy_corpus_free(c);
    }
}

/* ------------------------------------------------------------------ */
/* INTEGRATION TEST 9                                                   */
/* ffuzzy_corpus_add with count=0 -> no crash                          */
/* ------------------------------------------------------------------ */

static void integration_test_add_count_zero(void)
{
    BEGIN_TEST("integration_add_count_zero");

    ffuzzy_corpus_t *c = ffuzzy_corpus_new();
    CHECK(c != NULL);

    if (c) {
        /* count=0 must not crash */
        ffuzzy_corpus_add(c, NULL, 0);
        CHECK(1);

        const char *dummy[] = { "hello" };
        ffuzzy_corpus_add(c, dummy, 0);
        CHECK(1);

        /* Corpus should still be empty */
        CHECK(ffuzzy_corpus_len(c) == 0);

        /* Now add normally - should still work */
        const char *items[] = { "alpha", "beta", "gamma" };
        ffuzzy_corpus_add(c, items, 3);
        CHECK(ffuzzy_corpus_len(c) == 3);

        ffuzzy_corpus_free(c);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== ffuzzy integration / smoke tests ===\n");

    /* Smoke tests */
    smoke_test_dragon_search();
    smoke_test_gem_search();
    smoke_test_no_match();
    smoke_test_empty_query();
    smoke_test_limit();

    /* Integration tests */
    integration_test_large_corpus();
    integration_test_incremental_add();
    integration_test_free_null();
    integration_test_add_count_zero();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
