// Memory-leak smoke test. Build with -DFFZ_TRACK_ALLOC so every library
// allocation is counted; we then assert that each teardown returns the live
// block count to its baseline — i.e. manual frees are timely and complete,
// with no per-iteration growth. Exercises the alloc-heavy paths: matcher,
// pattern (multi-atom), indices, UTF-8 buffers, and the corpus (with the
// transliteration hook) under both serial and parallel filtering.
#include <stdio.h>
#include <string.h>

#include "ffz.h"
#include "ffz_alloc.h"
#include "ffz_corpus.h"

static int g_fail = 0, g_total = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        g_total++;                                                        \
        if (!(cond)) {                                                    \
            g_fail++;                                                     \
            printf("FAIL: %s  (live=%zu)  (%s:%d)\n", msg,               \
                   ffz_alloc_live_blocks(), __FILE__, __LINE__);          \
        }                                                                 \
    } while (0)

static size_t pinyin_hook(const char *item, size_t len, void *ctx,
                          ffz_key *out, size_t max_out) {
    (void)ctx;
    if (len == 6 && memcmp(item, "\xE5\xBC\xA0\xE4\xB8\x89", 6) == 0) {  // 张三
        size_t k = 0;
        if (k < max_out) { out[k] = (ffz_key){"zhangsan", 8, FFZ_KEY_PINYIN}; k++; }
        if (k < max_out) { out[k] = (ffz_key){"zs", 2, FFZ_KEY_INITIALS}; k++; }
        return k;
    }
    return 0;
}

// One matcher/pattern/indices round-trip; everything allocated is freed here.
static void matcher_cycle(void) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());
    ffz_pattern *p =
        ffz_pattern_parse("fo ba !x ^src dart$ 'lens", 25, FFZ_CASE_SMART,
                          FFZ_NORM_SMART);
    ffz_str_buf hb = {0};
    ffz_indices ix = {0};
    const char *hay = "src/foo_bar_lens.dart \xC3\xA9\xE4\xB8\xAD\xE6\x96\x87";
    ffz_str hs = ffz_str_from_utf8(hay, strlen(hay), &hb);
    ffz_pattern_match(m, p, hs, &ix);
    ffz_indices_free(&ix);
    ffz_str_buf_free(&hb);
    ffz_pattern_free(p);
    ffz_matcher_free(m);
}

// One corpus lifecycle: build (with hook) + filter + free.
static void corpus_cycle(ffz_parallel par) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_set_transliterator(c, pinyin_hook, NULL, 4);
    char buf[32];
    for (int i = 0; i < 1200; i++) {
        int n = snprintf(buf, sizeof(buf), "gem_item_%d", i);
        ffz_corpus_add(c, buf, (size_t)n);
    }
    ffz_corpus_add(c, "\xE5\xBC\xA0\xE4\xB8\x89", 6);  // 张三 (+hook keys)
    ffz_results r = {0};
    ffz_corpus_filter(c, "gem", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      par, 50, FFZ_SCORE_FAST, &r);
    ffz_results_free(&r);
    ffz_corpus_filter(c, "zs", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      par, 50, FFZ_SCORE_FAST, &r);
    ffz_results_free(&r);
    ffz_corpus_free(c);
}

// Arena stress: an oversized key (> one 64 KB block) + many small keys forces
// multiple blocks and the dedicated-block splice; clear+reuse must fully free.
static void arena_cycle(void) {
    static char big[80000];
    memset(big, 'a', sizeof(big));
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_add(c, big, sizeof(big));  // oversized -> dedicated arena block
    char buf[32];
    for (int i = 0; i < 3000; i++) {      // many small -> multiple blocks
        int n = snprintf(buf, sizeof(buf), "small_%d", i);
        ffz_corpus_add(c, buf, (size_t)n);
    }
    ffz_results r = {0};
    ffz_corpus_filter(c, "small", 5, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 10, FFZ_SCORE_FAST, &r);
    ffz_results_free(&r);
    ffz_corpus_clear(c);          // arena_free all blocks
    ffz_corpus_add(c, "reuse", 5);  // reuse after clear
    ffz_corpus_free(c);
}

// Drive sustained OOM from allocation #`budget` onward: every add/filter must
// degrade (drop-on-OOM) without crashing, and everything actually allocated
// must still be freed (asserted by the caller via the live-block baseline).
static void oom_cycle(int budget) {
    ffz_dbg_fail_after(budget);
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    if (c) {
        char buf[40];
        for (int i = 0; i < 800; i++) {
            int n = snprintf(buf, sizeof(buf), "oom_widget_%d.dart", i);
            ffz_corpus_add(c, buf, (size_t)n);  // many will be dropped on OOM
        }
        ffz_results r = {0};
        ffz_corpus_filter(c, "widget", 6, FFZ_CASE_SMART, FFZ_NORM_SMART,
                          FFZ_FUZZY, ffz_parallel_off(), 25, FFZ_SCORE_FAST, &r);
        ffz_results_free(&r);
        ffz_corpus_filter(c, "dart", 4, FFZ_CASE_SMART, FFZ_NORM_SMART,
                          FFZ_FUZZY, ffz_parallel_auto(), 0, FFZ_SCORE_FAST, &r);
        ffz_results_free(&r);
        ffz_corpus_free(c);  // frees succeed (no allocation) even under injection
    }
    ffz_dbg_fail_after(-1);  // disable before returning
}

// NUCLEO mode OOM injection: every allocation from #budget onward fails.
static void oom_cycle_nucleo(int budget) {
    ffz_dbg_fail_after(budget);
    ffz_config cfg = ffz_config_default();
    cfg.scoring_mode = FFZ_SCORE_NUCLEO;
    ffz_corpus *c = ffz_corpus_new(cfg);
    if (c) {
        char buf[40];
        for (int i = 0; i < 400; i++) {
            int n = snprintf(buf, sizeof(buf), "oom_nucleo_%d_long", i);
            ffz_corpus_add(c, buf, (size_t)n);
        }
        ffz_results r = {0};
        ffz_corpus_filter(c, "oom", 3, FFZ_CASE_SMART, FFZ_NORM_SMART,
                          FFZ_FUZZY, ffz_parallel_off(), 10, FFZ_SCORE_NUCLEO, &r);
        ffz_results_free(&r);
        ffz_corpus_free(c);
    }
    ffz_dbg_fail_after(-1);
}

int main(void) {
    size_t base = ffz_alloc_live_blocks();
    CHECK(base == 0, "baseline live blocks == 0");

    // Arena over-block / multi-block / clear-reuse: no leak across cycles.
    for (int i = 0; i < 30; i++) arena_cycle();
    CHECK(ffz_alloc_live_blocks() == base, "no leak after arena stress cycles");

    // OOM injection across a spread of budgets: never crash, never leak.
    for (int budget = 1; budget <= 60; budget++) oom_cycle(budget);
    CHECK(ffz_alloc_live_blocks() == base, "no leak across OOM-injected cycles");

    // NUCLEO mode OOM injection: same guarantee in the full-matrix DP path.
    for (int budget = 1; budget <= 80; budget++) oom_cycle_nucleo(budget);
    CHECK(ffz_alloc_live_blocks() == base, "no leak across NUCLEO OOM-injected cycles");

    // Many matcher/pattern cycles: live must return to baseline every time.
    for (int i = 0; i < 2000; i++) matcher_cycle();
    CHECK(ffz_alloc_live_blocks() == base, "no leak after 2000 matcher cycles");

    // Corpus cycles, serial and parallel, with the hook.
    for (int i = 0; i < 50; i++) corpus_cycle(ffz_parallel_off());
    CHECK(ffz_alloc_live_blocks() == base, "no leak after serial corpus cycles");
    for (int i = 0; i < 50; i++) corpus_cycle(ffz_parallel_auto());
    CHECK(ffz_alloc_live_blocks() == base,
          "no leak after parallel corpus cycles");

    // Timeliness: live count must not creep up across iterations (catches
    // slow leaks that a single cycle would hide).
    size_t after_first = 0;
    for (int i = 0; i < 100; i++) {
        corpus_cycle(ffz_parallel_auto());
        if (i == 0) after_first = ffz_alloc_live_blocks();
        CHECK(ffz_alloc_live_blocks() == after_first, "live count stable per cycle");
        if (g_fail) break;  // stop spamming if it's leaking
    }

    // A corpus held open while filtering repeatedly must not accumulate.
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    for (int i = 0; i < 300; i++) {
        char b[16];
        int n = snprintf(b, sizeof(b), "svc_%d_x", i);
        ffz_corpus_add(c, b, (size_t)n);
    }
    size_t held = ffz_alloc_live_blocks();
    for (int q = 0; q < 200; q++) {
        ffz_results r = {0};
        ffz_corpus_filter(c, "svc", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                          ffz_parallel_off(), 20, FFZ_SCORE_FAST, &r);
        ffz_results_free(&r);
        CHECK(ffz_alloc_live_blocks() == held, "filter+results_free leaves no residue");
        if (g_fail) break;
    }
    ffz_corpus_free(c);
    CHECK(ffz_alloc_live_blocks() == base, "all freed at end");

    printf("\n%d/%d leak checks passed (final live=%zu)\n", g_total - g_fail,
           g_total, ffz_alloc_live_blocks());
    return g_fail ? 1 : 0;
}
