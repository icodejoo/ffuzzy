// Unit tests for the ffz matcher, pattern layer, and corpus hook.
// No framework: assert helpers track pass/fail and the process exits nonzero
// on any failure.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffz.h"
#include "ffz_corpus.h"

static int g_fail = 0, g_total = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        g_total++;                                                            \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);          \
        }                                                                     \
    } while (0)

// Score `q` against `h` via the pattern layer (smart case + smart normalize).
static int32_t score(ffz_matcher *m, const char *q, const char *h,
                     ffz_indices *ix, ffz_mode mode) {
    ffz_str_buf hb = {0};
    ffz_str hs = ffz_str_from_utf8(h, strlen(h), &hb);
    ffz_pattern *p = (mode == FFZ_FUZZY)
                         ? ffz_pattern_parse(q, strlen(q), FFZ_CASE_SMART,
                                             FFZ_NORM_SMART)
                         : ffz_pattern_new(q, strlen(q), FFZ_CASE_SMART,
                                           FFZ_NORM_SMART, mode);
    int32_t s = ffz_pattern_match(m, p, hs, ix);
    ffz_pattern_free(p);
    ffz_str_buf_free(&hb);
    return s;
}

// score with explicit case/normalization (the default `score` uses smart/smart).
static int32_t score_cfg(ffz_matcher *m, const char *q, const char *h,
                         ffz_case_matching cm, ffz_normalization nm,
                         ffz_mode mode) {
    ffz_str_buf hb = {0};
    ffz_str hs = ffz_str_from_utf8(h, strlen(h), &hb);
    ffz_pattern *p = (mode == FFZ_FUZZY)
                         ? ffz_pattern_parse(q, strlen(q), cm, nm)
                         : ffz_pattern_new(q, strlen(q), cm, nm, mode);
    int32_t s = ffz_pattern_match(m, p, hs, NULL);
    ffz_pattern_free(p);
    ffz_str_buf_free(&hb);
    return s;
}

// Oversized input forces the greedy fallback (W*needle > FFZ_MAX_MATRIX_SIZE).
static void test_greedy_fallback(ffz_matcher *m) {
    char hay[601];
    memset(hay, 'a', 600);
    hay[600] = 0;
    char ndl[201];
    memset(ndl, 'a', 200);
    ndl[200] = 0;  // 600*200 = 120000 cells > FFZ_MAX_MATRIX_SIZE (102400)
    ffz_indices ix = {0};
    int32_t s = score(m, ndl, hay, &ix, FFZ_FUZZY);
    CHECK(s >= 0, "greedy: long needle matches long haystack");
    CHECK(ix.len == 200, "greedy: all needle chars indexed");
    int asc = 1;
    for (size_t i = 1; i < ix.len; i++)
        if (ix.data[i] <= ix.data[i - 1]) asc = 0;
    CHECK(asc, "greedy: indices strictly ascending");
    ffz_indices_free(&ix);
}

static void test_config_variants(ffz_matcher *m) {
    CHECK(score_cfg(m, "rust", "RUST", FFZ_CASE_RESPECT, FFZ_NORM_SMART, FFZ_FUZZY) < 0,
          "RESPECT: rust != RUST");
    CHECK(score_cfg(m, "rust", "RUST", FFZ_CASE_IGNORE, FFZ_NORM_SMART, FFZ_FUZZY) >= 0,
          "IGNORE: rust == RUST");
    CHECK(score_cfg(m, "cafe", "caf\xC3\xA9", FFZ_CASE_SMART, FFZ_NORM_NEVER, FFZ_FUZZY) < 0,
          "NORM_NEVER: cafe != café");
    CHECK(score_cfg(m, "cafe", "caf\xC3\xA9", FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY) >= 0,
          "NORM_SMART: cafe == café");
    // match_paths config: '/' is a delimiter granting a boundary bonus.
    ffz_matcher *mp = ffz_matcher_new(ffz_config_match_paths());
    CHECK(score_cfg(mp, "b", "a/bc", FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_PREFIX) < 0,
          "match_paths prefix sanity");
    CHECK(score_cfg(mp, "bc", "a/bc", FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_SUBSTRING) >= 0,
          "match_paths substring after delimiter");
    ffz_matcher_free(mp);
}

static void test_corpus_clear_limit(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_add(c, "alpha", 5);
    ffz_corpus_add(c, "alto", 4);
    ffz_corpus_add(c, "beta", 4);
    ffz_results r = {0};
    ffz_corpus_filter(c, "al", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);  // limit 0 == all matches
    CHECK(r.len == 2, "limit=0 returns all matches");
    ffz_results_free(&r);
    ffz_corpus_clear(c);
    CHECK(ffz_corpus_len(c) == 0, "clear empties the corpus");
    ffz_corpus_filter(c, "al", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 0, "filter on empty corpus -> 0");
    ffz_results_free(&r);
    ffz_corpus_add(c, "alien", 5);  // reuse after clear
    ffz_corpus_filter(c, "al", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 1 && r.hits[0].item_index == 0, "reuse after clear works");
    ffz_results_free(&r);
    ffz_corpus_free(c);
}

// Reconstruct the matched substring (by codepoint indices) and compare, after
// lowercasing both sides via ASCII (tests use ASCII reconstruction targets).
static void test_basic(ffz_matcher *m) {
    CHECK(score(m, "fb", "flutter_rust_bridge", NULL, FFZ_FUZZY) >= 0,
          "fb matches flutter_rust_bridge");
    CHECK(score(m, "zzz", "flutter_rust_bridge", NULL, FFZ_FUZZY) < 0,
          "zzz does not match");
    CHECK(score(m, "", "anything", NULL, FFZ_FUZZY) == 0, "empty query -> 0");
    CHECK(score(m, "abc", "ab", NULL, FFZ_FUZZY) < 0,
          "needle longer than haystack -> miss");
}

static void test_indices(ffz_matcher *m) {
    ffz_indices ix = {0};
    int32_t s = score(m, "fzd", "fuzzy.dart", &ix, FFZ_FUZZY);
    CHECK(s >= 0, "fzd matches fuzzy.dart");
    CHECK(ix.len == 3, "fzd -> 3 indices");
    // ascending
    int asc = 1;
    for (size_t i = 1; i < ix.len; i++)
        if (ix.data[i] <= ix.data[i - 1]) asc = 0;
    CHECK(asc, "indices ascending");
    // reconstruct
    const char *h = "fuzzy.dart";
    char got[8] = {0};
    for (size_t i = 0; i < ix.len && i < 7; i++) got[i] = h[ix.data[i]];
    CHECK(strcmp(got, "fzd") == 0, "indices reconstruct 'fzd'");
    ffz_indices_free(&ix);
}

static void test_ranking(ffz_matcher *m) {
    // word-boundary start should beat a mid-word match.
    int32_t boundary = score(m, "lens", "code_lens", NULL, FFZ_FUZZY);
    int32_t midword = score(m, "lens", "flensburg", NULL, FFZ_FUZZY);
    CHECK(boundary >= 0 && midword >= 0, "both lens variants match");
    CHECK(boundary > midword, "boundary match scores higher than mid-word");
    // consecutive should beat gapped.
    int32_t consec = score(m, "abc", "abcdef", NULL, FFZ_FUZZY);
    int32_t gapped = score(m, "abc", "axbxcx", NULL, FFZ_FUZZY);
    CHECK(consec > gapped, "consecutive beats gapped");
}

static void test_case(ffz_matcher *m) {
    // smart: lowercase query is case-insensitive.
    CHECK(score(m, "rust", "RUST", NULL, FFZ_FUZZY) >= 0,
          "lowercase query matches uppercase (smart)");
    // smart: query with uppercase becomes case-sensitive.
    CHECK(score(m, "Rust", "rust", NULL, FFZ_FUZZY) < 0,
          "Rust (uppercase) is case-sensitive, misses 'rust'");
    CHECK(score(m, "Rust", "Rust", NULL, FFZ_FUZZY) >= 0,
          "Rust matches Rust");
}

static void test_unicode(ffz_matcher *m) {
    // CJK at codepoint granularity.
    CHECK(score(m, "中", "中文搜索", NULL, FFZ_FUZZY) >= 0, "中 matches 中文搜索");
    CHECK(score(m, "搜索", "中文搜索引擎", NULL, FFZ_FUZZY) >= 0,
          "搜索 matches as subsequence");
    CHECK(score(m, "京", "東京都", NULL, FFZ_FUZZY) >= 0, "京 matches 東京都");
    // CJK index reconstruction.
    ffz_indices ix = {0};
    int32_t s = score(m, "京", "東京都", &ix, FFZ_FUZZY);
    CHECK(s >= 0 && ix.len == 1 && ix.data[0] == 1, "京 is codepoint index 1");
    ffz_indices_free(&ix);
    // accent folding: smart normalize folds haystack accents to match plain query.
    CHECK(score(m, "cafe", "caf\xC3\xA9", NULL, FFZ_FUZZY) >= 0,
          "cafe matches café (normalize)");
    // asymmetric: accented query does NOT match plain haystack.
    CHECK(score(m, "caf\xC3\xA9", "cafe", NULL, FFZ_FUZZY) < 0,
          "café does not match cafe (asymmetric)");
    // non-ascii case fold: Greek capital matches lowercase.
    CHECK(score(m, "\xCE\xB1", "\xCE\x91\xCE\xB2", NULL, FFZ_FUZZY) >= 0,
          "greek alpha matches Alpha-beta (case fold)");
}

static void test_modes(ffz_matcher *m) {
    CHECK(score(m, "gem", "Dragon Gem", NULL, FFZ_SUBSTRING) >= 0,
          "substring gem in 'Dragon Gem'");
    CHECK(score(m, "gem", "g e m", NULL, FFZ_SUBSTRING) < 0,
          "substring gem not in 'g e m'");
    CHECK(score(m, "super", "supersonic", NULL, FFZ_PREFIX) >= 0,
          "prefix super in supersonic");
    CHECK(score(m, "super", "a super thing", NULL, FFZ_PREFIX) < 0,
          "prefix super not leading");
    CHECK(score(m, "dart", "fuzzy.dart", NULL, FFZ_POSTFIX) >= 0,
          "postfix dart");
    CHECK(score(m, "gem", "gem", NULL, FFZ_EXACT) >= 0, "exact gem == gem");
    CHECK(score(m, "gem", "gems", NULL, FFZ_EXACT) < 0, "exact gem != gems");
}

static void test_pattern_syntax(ffz_matcher *m) {
    // ' forces substring
    CHECK(score(m, "'gem", "Dragon Gem", NULL, FFZ_FUZZY) >= 0, "'gem substring");
    CHECK(score(m, "'gem", "g_e_m", NULL, FFZ_FUZZY) < 0, "'gem not in g_e_m");
    // ^ prefix
    CHECK(score(m, "^sup", "supersonic", NULL, FFZ_FUZZY) >= 0, "^sup prefix");
    CHECK(score(m, "^sup", "a sup", NULL, FFZ_FUZZY) < 0, "^sup not leading");
    // $ postfix
    CHECK(score(m, "dart$", "fuzzy.dart", NULL, FFZ_FUZZY) >= 0, "dart$ postfix");
    // negative: !x rejects haystacks containing x
    CHECK(score(m, "gem !drag", "Dragon Gem", NULL, FFZ_FUZZY) < 0,
          "!drag rejects Dragon Gem");
    CHECK(score(m, "gem !foo", "Dragon Gem", NULL, FFZ_FUZZY) >= 0,
          "!foo keeps Dragon Gem");
    // multi-word
    CHECK(score(m, "fo ba", "foo/bar", NULL, FFZ_FUZZY) >= 0, "multi-word fo ba");
}

// --- corpus + transliteration hook ---------------------------------------
// Toy hook: maps two known Chinese names to pinyin + initials.
static size_t pinyin_hook(const char *item, size_t len, void *ctx,
                          ffz_key *out, size_t max_out) {
    (void)ctx;
    struct {
        const char *zh, *py, *ini;
    } table[] = {
        {"\xE5\xBC\xA0\xE4\xB8\x89", "zhangsan", "zs"},  // 张三
        {"\xE6\x9D\x8E\xE5\x9B\x9B", "lisi", "ls"},      // 李四
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].zh) == len && memcmp(item, table[i].zh, len) == 0) {
            size_t k = 0;
            if (k < max_out) {
                out[k].text = table[i].py;
                out[k].len = strlen(table[i].py);
                out[k].kind = FFZ_KEY_PINYIN;
                k++;
            }
            if (k < max_out) {
                out[k].text = table[i].ini;
                out[k].len = strlen(table[i].ini);
                out[k].kind = FFZ_KEY_INITIALS;
                k++;
            }
            return k;
        }
    }
    return 0;
}

static void test_add_keyed(void) {
    // Explicit alternate keys (no hook): find 张三 by typing pinyin/initials.
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_key keys[2];
    keys[0].text = "zhangsan"; keys[0].len = 8; keys[0].kind = FFZ_KEY_PINYIN;
    keys[1].text = "zs";       keys[1].len = 2; keys[1].kind = FFZ_KEY_INITIALS;
    ffz_corpus_add_keyed(c, "\xE5\xBC\xA0\xE4\xB8\x89", 6, keys, 2);  // 张三
    ffz_corpus_add_keyed(c, "plain", 5, NULL, 0);  // 0 extra keys is valid

    ffz_results r = {0};
    ffz_corpus_filter(c, "zhangsan", 8, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 1 && r.hits[0].item_index == 0 &&
              r.hits[0].matched_kind == FFZ_KEY_PINYIN,
          "add_keyed: pinyin key matches 张三");
    ffz_results_free(&r);

    ffz_corpus_filter(c, "zs", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len >= 1 && r.hits[0].matched_kind == FFZ_KEY_INITIALS,
          "add_keyed: initials key matches");
    ffz_results_free(&r);
    ffz_corpus_free(c);
}

static void test_corpus(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_set_transliterator(c, pinyin_hook, NULL, 4);
    ffz_corpus_add(c, "\xE5\xBC\xA0\xE4\xB8\x89", 6);  // 张三
    ffz_corpus_add(c, "\xE6\x9D\x8E\xE5\x9B\x9B", 6);  // 李四
    ffz_corpus_add(c, "Zachary", 7);
    CHECK(ffz_corpus_len(c) == 3, "corpus has 3 items");

    ffz_results r = {0};

    // pinyin match -> finds 张三 via the PINYIN key.
    ffz_corpus_filter(c, "zhangsan", 8, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len >= 1, "zhangsan finds something");
    int found_zh = 0;
    for (size_t i = 0; i < r.len; i++)
        if (r.hits[i].item_index == 0 && r.hits[i].matched_kind == FFZ_KEY_PINYIN)
            found_zh = 1;
    CHECK(found_zh, "zhangsan -> item 0 via PINYIN key");
    ffz_results_free(&r);

    // initials "zs" -> 张三 via INITIALS key.
    ffz_corpus_filter(c, "zs", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    int found_ini = 0;
    for (size_t i = 0; i < r.len; i++)
        if (r.hits[i].item_index == 0 &&
            r.hits[i].matched_kind == FFZ_KEY_INITIALS)
            found_ini = 1;
    CHECK(found_ini, "zs -> item 0 via INITIALS key");
    ffz_results_free(&r);

    // original CJK query -> matches ORIGINAL key, indices into the display text.
    ffz_corpus_filter(c, "\xE5\xBC\xA0", 3, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 1 && r.hits[0].item_index == 0 &&
              r.hits[0].matched_kind == FFZ_KEY_ORIGINAL,
          "张 -> item 0 via ORIGINAL key");
    CHECK(r.len == 1 && r.hits[0].indices.len == 1 &&
              r.hits[0].indices.data[0] == 0,
          "张 highlight index 0 in display text");
    ffz_results_free(&r);

    // limit truncates.
    ffz_corpus_filter(c, "z", 1, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 1, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 1, "limit=1 truncates");
    ffz_results_free(&r);

    ffz_corpus_free(c);
}

// Parallel scan must produce identical, identically-ordered results to serial.
static void test_parallel(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    char buf[32];
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(buf, sizeof(buf), "item_gem_%d_dragon", i);
        ffz_corpus_add(c, buf, (size_t)n);
    }
    ffz_results a = {0}, b = {0};
    ffz_corpus_filter(c, "gem", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 100, FFZ_SCORE_FAST, &a);
    ffz_corpus_filter(c, "gem", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_auto(), 100, FFZ_SCORE_FAST, &b);
    CHECK(a.len == 100 && b.len == 100, "parallel + serial both return 100");
    int same = a.len == b.len;
    for (size_t i = 0; i < a.len && i < b.len; i++)
        if (a.hits[i].item_index != b.hits[i].item_index ||
            a.hits[i].score != b.hits[i].score)
            same = 0;
    CHECK(same, "parallel result identical & same order as serial");
    // explicit thread count works too
    ffz_results d = {0};
    ffz_corpus_filter(c, "gem", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_with(4), 100, FFZ_SCORE_FAST, &d);
    CHECK(d.len == 100, "explicit 4 threads returns 100");
    ffz_results_free(&a);
    ffz_results_free(&b);
    ffz_results_free(&d);
    ffz_corpus_free(c);
}

// Property test: across several queries and limits, the parallel scan must
// return EXACTLY the serial result (same items, scores, kinds, order), and
// every reported index must be in-range and strictly increasing. Exercises the
// per-thread top-K + arena paths over a >512-item corpus.
static void test_property(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    char buf[48];
    for (int i = 0; i < 1500; i++) {
        int n = snprintf(buf, sizeof(buf), "file_%d_widget_%d.dart", i, (i * 7) % 13);
        ffz_corpus_add(c, buf, (size_t)n);
    }
    const char *queries[] = {"widget", "dart", "fwd", "file5", "z", ".dart"};
    size_t limits[] = {0, 1, 10, 50, 1000};
    int ok_det = 1, ok_idx = 1;
    for (size_t qi = 0; qi < sizeof(queries) / sizeof(queries[0]); qi++) {
        size_t ql = strlen(queries[qi]);
        for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++) {
            ffz_results s = {0}, p = {0};
            ffz_corpus_filter(c, queries[qi], ql, FFZ_CASE_SMART, FFZ_NORM_SMART,
                              FFZ_FUZZY, ffz_parallel_off(), limits[li], FFZ_SCORE_FAST, &s);
            ffz_corpus_filter(c, queries[qi], ql, FFZ_CASE_SMART, FFZ_NORM_SMART,
                              FFZ_FUZZY, ffz_parallel_with(4), limits[li], FFZ_SCORE_FAST, &p);
            if (s.len != p.len) ok_det = 0;
            for (size_t i = 0; i < s.len && i < p.len; i++) {
                if (s.hits[i].item_index != p.hits[i].item_index ||
                    s.hits[i].score != p.hits[i].score ||
                    s.hits[i].matched_kind != p.hits[i].matched_kind)
                    ok_det = 0;
                // indices strictly increasing (codepoint positions in the key)
                ffz_indices *idx = &s.hits[i].indices;
                for (size_t j = 1; j < idx->len; j++)
                    if (idx->data[j] <= idx->data[j - 1]) ok_idx = 0;
            }
            ffz_results_free(&s);
            ffz_results_free(&p);
        }
    }
    CHECK(ok_det, "property: parallel == serial across queries/limits");
    CHECK(ok_idx, "property: match indices strictly increasing");
    ffz_corpus_free(c);
}

// Helper: are two result sets identical in length AND order (index+score)?
static int results_identical(const ffz_results *a, const ffz_results *b) {
    if (a->len != b->len) return 0;
    for (size_t i = 0; i < a->len; i++)
        if (a->hits[i].item_index != b->hits[i].item_index ||
            a->hits[i].score != b->hits[i].score)
            return 0;
    return 1;
}

// Threshold boundary (511/512/513), massive score ties, and limit corners —
// all must be deterministic: parallel == serial in set AND order.
static void test_determinism_corners(void) {
    // 1) Parallel threshold boundary: serial == parallel at 511/512/513 items.
    int ok_bound = 1;
    size_t sizes[] = {511, 512, 513};
    for (size_t si = 0; si < 3; si++) {
        ffz_corpus *c = ffz_corpus_new(ffz_config_default());
        char buf[24];
        for (size_t i = 0; i < sizes[si]; i++) {
            int n = snprintf(buf, sizeof(buf), "node_%zu_db", i);
            ffz_corpus_add(c, buf, (size_t)n);
        }
        ffz_results s = {0}, p = {0};
        ffz_corpus_filter(c, "db", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                          ffz_parallel_off(), 0, FFZ_SCORE_FAST, &s);
        ffz_corpus_filter(c, "db", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                          ffz_parallel_auto(), 0, FFZ_SCORE_FAST, &p);
        if (!results_identical(&s, &p)) ok_bound = 0;
        ffz_results_free(&s);
        ffz_results_free(&p);
        ffz_corpus_free(c);
    }
    CHECK(ok_bound, "determinism across the 511/512/513 thread boundary");

    // 2) Massive score ties: 2000 identical items must order identically
    //    (tie-break by item_index) under any thread count and limit.
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    for (int i = 0; i < 2000; i++) ffz_corpus_add(c, "widget", 6);
    ffz_corpus_add(c, "wonderful_widget", 16);
    int ok_ties = 1, ok_corner = 1;
    size_t limits[] = {0, 1, 5, 2000};
    for (size_t li = 0; li < 4; li++) {
        ffz_results s = {0}, p = {0};
        ffz_corpus_filter(c, "widget", 6, FFZ_CASE_SMART, FFZ_NORM_SMART,
                          FFZ_FUZZY, ffz_parallel_off(), limits[li], FFZ_SCORE_FAST, &s);
        ffz_corpus_filter(c, "widget", 6, FFZ_CASE_SMART, FFZ_NORM_SMART,
                          FFZ_FUZZY, ffz_parallel_with(8), limits[li], FFZ_SCORE_FAST, &p);
        if (!results_identical(&s, &p)) ok_ties = 0;
        ffz_results_free(&s);
        ffz_results_free(&p);
    }
    CHECK(ok_ties, "determinism with 2000 tied scores across limits");

    // 3) Limit corners on a parallel-eligible corpus.
    ffz_results r = {0};
    ffz_corpus_filter(c, "zzzz_nomatch", 12, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_PREFIX, ffz_parallel_auto(), 0, FFZ_SCORE_FAST, &r);
    if (r.len != 0) ok_corner = 0;  // no-match -> empty, no crash
    ffz_results_free(&r);
    ffz_corpus_filter(c, "widget", 6, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_auto(), 999999, FFZ_SCORE_FAST, &r);  // limit >> matches
    if (r.len != 2001) ok_corner = 0;  // all items match "widget"
    ffz_results_free(&r);
    CHECK(ok_corner, "limit corners: no-match empty, limit>>n returns all");
    ffz_corpus_free(c);
}

static void test_corpus_scoring_modes(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_add(c, "configure", 9);
    ffz_corpus_add(c, "cfg_helper", 10);
    ffz_corpus_add(c, "my_cfg", 6);
    ffz_corpus_add(c, "ffz_config", 10);
    ffz_corpus_add(c, "no_match_xyz", 12);

    // --- OFF: original order, score=0, limit respected ---
    ffz_results r = {0};
    ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 2, FFZ_SCORE_OFF, &r);
    CHECK(r.len == 2, "OFF corpus: respects limit=2");
    CHECK(r.hits[0].item_index == 0, "OFF corpus: first hit is insertion-order first");
    CHECK(r.hits[1].item_index == 1, "OFF corpus: second hit is insertion-order second");
    CHECK(r.hits[0].score == 0, "OFF corpus: score is 0");
    CHECK(r.hits[1].score == 0, "OFF corpus: score is 0");
    ffz_results_free(&r);

    // --- FAST: ranked by rolling DP score ---
    ffz_results r2 = {0};
    ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 3, FFZ_SCORE_FAST, &r2);
    CHECK(r2.len == 3, "FAST corpus: returns 3 matches");
    bool cfg_helper_first = r2.hits[0].item_index == 1;  // cfg_helper is item 1
    CHECK(cfg_helper_first, "FAST corpus: boundary match ranked first");
    CHECK(r2.hits[0].score > 0, "FAST corpus: score is positive");
    ffz_results_free(&r2);

    // --- NUCLEO: existing behaviour ---
    ffz_results r3 = {0};
    ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 3, FFZ_SCORE_NUCLEO, &r3);
    CHECK(r3.len == 3, "NUCLEO corpus: returns 3 matches");
    CHECK(r3.hits[0].score > 0, "NUCLEO corpus: positive score");
    ffz_results_free(&r3);

    ffz_corpus_free(c);
}

static void test_scoring_modes(void) {
    // --- FAST mode ---
    ffz_config cfgf = ffz_config_default();
    cfgf.scoring_mode = FFZ_SCORE_FAST;
    ffz_matcher *mf = ffz_matcher_new(cfgf);

    CHECK(score(mf, "cfg", "ffz_config", NULL, FFZ_FUZZY) > 0,
          "FAST fuzzy: positive score on match");
    CHECK(score(mf, "xyz", "abcdef", NULL, FFZ_FUZZY) < 0,
          "FAST fuzzy: -1 on no match");
    ffz_indices ix = {0};
    int32_t sf = score(mf, "ab", "abcdef", &ix, FFZ_FUZZY);
    CHECK(sf > 0, "FAST fuzzy: positive score with indices");
    CHECK(ix.len == 2, "FAST fuzzy: index count == needle length");
    ffz_indices_free(&ix);
    CHECK(score(mf, "abc", "abcdef", NULL, FFZ_EXACT) < 0,
          "FAST exact: whole-string mismatch -> -1");
    CHECK(score(mf, "abc", "abc", NULL, FFZ_EXACT) > 0,
          "FAST exact: exact whole-string match -> positive");

    ffz_matcher_free(mf);

    // --- OFF mode ---
    ffz_config cfgo = ffz_config_default();
    cfgo.scoring_mode = FFZ_SCORE_OFF;
    ffz_matcher *mo = ffz_matcher_new(cfgo);

    CHECK(score(mo, "cfg", "ffz_config", NULL, FFZ_FUZZY) == 0,
          "OFF fuzzy: score is 0 on match");
    CHECK(score(mo, "xyz", "abcdef", NULL, FFZ_FUZZY) < 0,
          "OFF fuzzy: -1 on no match");
    ffz_indices ix2 = {0};
    int32_t so = score(mo, "ab", "abcdef", &ix2, FFZ_FUZZY);
    CHECK(so == 0, "OFF fuzzy: score is 0 when indices requested");
    CHECK(ix2.len == 2, "OFF fuzzy: index count == needle length");
    ffz_indices_free(&ix2);
    CHECK(score(mo, "abc", "abc", NULL, FFZ_EXACT) == 0,
          "OFF exact: score is 0");
    CHECK(score(mo, "abc", "abcd", NULL, FFZ_EXACT) < 0,
          "OFF exact: -1 on length mismatch");

    ffz_matcher_free(mo);

    // --- NUCLEO mode (current behaviour unchanged) ---
    ffz_config cfgn = ffz_config_default();
    cfgn.scoring_mode = FFZ_SCORE_NUCLEO;
    ffz_matcher *mn = ffz_matcher_new(cfgn);
    CHECK(score(mn, "cfg", "ffz_config", NULL, FFZ_FUZZY) > 0,
          "NUCLEO fuzzy: positive score");
    ffz_matcher_free(mn);
}

static void test_rolling_dp(void) {
    ffz_config cfg = ffz_config_default();
    cfg.scoring_mode = FFZ_SCORE_FAST;
    ffz_matcher *m = ffz_matcher_new(cfg);

    // FAST mode: fuzzy match returns a positive score.
    int32_t s = score(m, "cfg", "ffz_config", NULL, FFZ_FUZZY);
    CHECK(s > 0, "rolling: fuzzy match gives positive score");

    // No match returns -1.
    int32_t s2 = score(m, "xyz", "abcdef", NULL, FFZ_FUZZY);
    CHECK(s2 < 0, "rolling: no-match gives -1");

    // Boundary match scores higher than interior (cfg at start vs preceded by letter).
    int32_t sb = score(m, "cfg", "cfg_helper", NULL, FFZ_FUZZY);
    int32_t si = score(m, "cfg", "abcfgval", NULL, FFZ_FUZZY);
    CHECK(sb > si, "rolling: boundary match scores higher than interior");

    ffz_matcher_free(m);
}

static void test_scoring_cross(void) {
    // Create matchers for each scoring mode.
    ffz_config cfgf = ffz_config_default(); cfgf.scoring_mode = FFZ_SCORE_FAST;
    ffz_config cfgo = ffz_config_default(); cfgo.scoring_mode = FFZ_SCORE_OFF;
    ffz_config cfgn = ffz_config_default(); cfgn.scoring_mode = FFZ_SCORE_NUCLEO;
    ffz_matcher *mf = ffz_matcher_new(cfgf);
    ffz_matcher *mo = ffz_matcher_new(cfgo);
    ffz_matcher *mn = ffz_matcher_new(cfgn);

    // OFF + SUBSTRING: score==0
    CHECK(score(mo, "dart", "fuzzy.dart", NULL, FFZ_SUBSTRING) == 0,
          "OFF substring: score==0");
    // OFF + PREFIX: score==0 on match, -1 on miss
    CHECK(score(mo, "sup", "supersonic", NULL, FFZ_PREFIX) == 0,
          "OFF prefix: score==0 on match");
    CHECK(score(mo, "son", "supersonic", NULL, FFZ_PREFIX) < 0,
          "OFF prefix: -1 on miss");
    // OFF + POSTFIX: score==0 on match
    CHECK(score(mo, "sonic", "supersonic", NULL, FFZ_POSTFIX) == 0,
          "OFF postfix: score==0");
    // OFF + EXACT: score==0 on match
    CHECK(score(mo, "abc", "abc", NULL, FFZ_EXACT) == 0,
          "OFF exact: score==0");
    // FAST == NUCLEO for non-fuzzy modes (both use exact_impl)
    int32_t sf_pre = score(mf, "super", "supersonic", NULL, FFZ_PREFIX);
    int32_t sn_pre = score(mn, "super", "supersonic", NULL, FFZ_PREFIX);
    CHECK(sf_pre == sn_pre, "FAST==NUCLEO for PREFIX (both use exact_impl)");
    CHECK(sf_pre > 0, "FAST prefix: positive score");

    ffz_matcher_free(mf); ffz_matcher_free(mo); ffz_matcher_free(mn);
}

static void test_rolling_golden(void) {
    ffz_config cfg = ffz_config_default(); cfg.scoring_mode = FFZ_SCORE_FAST;
    ffz_matcher *m = ffz_matcher_new(cfg);

    // Boundary match: 'cfg' at the start of 'cfg_helper' (BOUNDARY bonus=8,
    // FIRST_CHAR_MULT=2). Row0: 8*2+16=32; Row1: 32+4+16=52; Row2: 52+4+16=72.
    int32_t s1 = score(m, "cfg", "cfg_helper", NULL, FFZ_FUZZY);
    CHECK(s1 == 72, "rolling golden: cfg/cfg_helper == 72");

    // Interior match: 'cfg' in 'abcfgval' ('c' follows 'b'=LOWER, no boundary
    // bonus). Row0: 0*2+16=16; Row1: 16+4+16=36; Row2: 36+4+16=56.
    int32_t s2 = score(m, "cfg", "abcfgval", NULL, FFZ_FUZZY);
    CHECK(s2 == 56, "rolling golden: cfg/abcfgval == 56");
    CHECK(s1 > s2, "rolling: boundary > interior");

    ffz_matcher_free(m);
}

static void test_fast_index_consistency(void) {
    ffz_config cfg = ffz_config_default(); cfg.scoring_mode = FFZ_SCORE_FAST;
    ffz_matcher *m = ffz_matcher_new(cfg);

    // score-only path (rolling DP)
    int32_t s_no_ix = score(m, "abc", "abcdef", NULL, FFZ_FUZZY);
    CHECK(s_no_ix > 0, "FAST no-ix: positive");

    // with-indices path (greedy) — score may differ from rolling, but must be valid
    ffz_indices ix = {0};
    int32_t s_with_ix = score(m, "abc", "abcdef", &ix, FFZ_FUZZY);
    CHECK(s_with_ix > 0, "FAST with-ix: positive");
    CHECK(ix.len == 3, "FAST with-ix: 3 indices");
    ffz_indices_free(&ix);

    ffz_matcher_free(m);
}

static void test_boundary_conditions(void) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());

    // nl == hn == 1, FUZZY walks exact_impl
    CHECK(score(m, "a", "a", NULL, FFZ_FUZZY) >= 0, "single char: fuzzy exact match");
    CHECK(score(m, "a", "b", NULL, FFZ_FUZZY) < 0,  "single char: fuzzy mismatch");

    // needle == haystack
    CHECK(score(m, "hello", "hello", NULL, FFZ_FUZZY) >= 0, "needle==haystack");

    // EXACT with surrounding whitespace
    CHECK(score(m, "gem", "  gem  ", NULL, FFZ_EXACT) >= 0, "EXACT: strips leading+trailing ws");
    CHECK(score(m, "gem", "  gem  ", NULL, FFZ_PREFIX) >= 0, "PREFIX: strips leading ws");

    // empty haystack
    CHECK(score(m, "a", "", NULL, FFZ_FUZZY) < 0, "empty haystack -> miss");

    // Literal modes keep the query's spaces (single literal atom, no atom
    // trimming). A leading/trailing space in the needle disables the haystack's
    // leading/trailing-ws trim on that side, so the spaces must line up exactly.
    CHECK(score(m, " gem", " gem", NULL, FFZ_EXACT) >= 0,
          "EXACT: needle's leading space matches a single leading space");
    CHECK(score(m, " gem", "  gem", NULL, FFZ_EXACT) < 0,
          "EXACT: needle's lone leading space != two haystack spaces");
    CHECK(score(m, "gem ", "gem ", NULL, FFZ_EXACT) >= 0,
          "EXACT: needle's trailing space matches a single trailing space");
    CHECK(score(m, "gem ", "gem  ", NULL, FFZ_EXACT) < 0,
          "EXACT: needle's lone trailing space != two haystack spaces");

    ffz_matcher_free(m);
}

static void test_prefer_prefix(void) {
    ffz_config cfg = ffz_config_default();
    cfg.prefer_prefix = true;
    ffz_matcher *m = ffz_matcher_new(cfg);

    int32_t s_start = score(m, "abc", "abc_xyz", NULL, FFZ_FUZZY);
    int32_t s_end   = score(m, "abc", "xyz_abc", NULL, FFZ_FUZZY);
    CHECK(s_start > s_end, "prefer_prefix: earlier match scores higher");
    CHECK(s_start > 0, "prefer_prefix: positive score for early match");

    ffz_matcher_free(m);
}

static void test_corpus_scoring(void) {
    // default corpus
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    CHECK(ffz_corpus_scoring(c) == FFZ_SCORE_FAST, "corpus_scoring: default is FAST");
    ffz_corpus_free(c);

    // OFF corpus
    ffz_config cfg = ffz_config_default(); cfg.scoring_mode = FFZ_SCORE_OFF;
    ffz_corpus *co = ffz_corpus_new(cfg);
    CHECK(ffz_corpus_scoring(co) == FFZ_SCORE_OFF, "corpus_scoring: OFF reflects config");
    ffz_corpus_free(co);
}

static void test_cjk_modes(void) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());

    // CJK SUBSTRING
    CHECK(score(m, "\xE4\xB8\xAD\xE6\x96\x87",
                   "\xE6\x90\x9C\xE7\xB4\xA2\xE4\xB8\xAD\xE6\x96\x87\xE5\xBC\x95\xE6\x93\x8E",
                   NULL, FFZ_SUBSTRING) >= 0, "CJK substring match");
    CHECK(score(m, "\xE4\xB8\xAD\xE6\x96\x87",
                   "\xE6\x90\x9C\xE7\xB4\xA2\xE5\xBC\x95\xE6\x93\x8E",
                   NULL, FFZ_SUBSTRING) < 0, "CJK substring miss");

    // CJK EXACT
    CHECK(score(m, "\xE4\xB8\xAD\xE6\x96\x87",
                   "\xE4\xB8\xAD\xE6\x96\x87",
                   NULL, FFZ_EXACT) >= 0, "CJK exact match");
    CHECK(score(m, "\xE4\xB8\xAD\xE6\x96\x87",
                   "\xE4\xB8\xAD\xE6\x96\x87\xE6\x90\x9C\xE7\xB4\xA2",
                   NULL, FFZ_EXACT) < 0, "CJK exact length mismatch");

    ffz_matcher_free(m);
}

static void test_rfind_boundaries(void) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());

    // n=15: target at last byte
    CHECK(score(m, "z", "aaaaaaaaaaaaaaz", NULL, FFZ_FUZZY) >= 0,
          "rfind 15-byte: z at last pos");
    // n=16: target at last byte
    CHECK(score(m, "z", "aaaaaaaaaaaaaaaz", NULL, FFZ_FUZZY) >= 0,
          "rfind 16-byte: z at last pos");
    // n=17: target at last byte
    CHECK(score(m, "z", "aaaaaaaaaaaaaaaaz", NULL, FFZ_FUZZY) >= 0,
          "rfind 17-byte: z at last pos");
    // prefilter end-widening: 'ab' in 'abXab' -> end=5 > greedy_end=3
    int32_t s1 = score(m, "ab", "abXab", NULL, FFZ_FUZZY);
    int32_t s2 = score(m, "ab", "abXxx", NULL, FFZ_FUZZY);
    CHECK(s1 >= s2, "prefilter end-widening: wider window >= narrower");

    ffz_matcher_free(m);
}

static void test_corpus_off_modes(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_add(c, "alpha", 5);
    ffz_corpus_add(c, "beta",  4);
    ffz_corpus_add(c, "alpha", 5);  // duplicate

    ffz_results r = {0};
    // OFF + EXACT: both "alpha" entries match, insertion order
    ffz_corpus_filter(c, "alpha", 5, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_EXACT, ffz_parallel_off(), 0, FFZ_SCORE_OFF, &r);
    CHECK(r.len == 2, "OFF+EXACT: both exact matches returned");
    CHECK(r.hits[0].item_index == 0, "OFF+EXACT: first in insertion order");
    CHECK(r.hits[1].item_index == 2, "OFF+EXACT: second in insertion order");
    CHECK(r.hits[0].score == 0 && r.hits[1].score == 0, "OFF+EXACT: score==0");
    ffz_results_free(&r);

    ffz_corpus_free(c);
}

static void test_escape_syntax(ffz_matcher *m) {
    // \! should NOT trigger negation — literal '!' in needle
    CHECK(score(m, "\\!drag", "!dragon", NULL, FFZ_FUZZY) >= 0,
          "escape: \\! is not negation, matches '!dragon'");
    // Plain !drag IS negation
    CHECK(score(m, "!drag", "!dragon", NULL, FFZ_FUZZY) < 0,
          "escape: !drag without escape IS negation");

    // dart$ with real $ is postfix and should match
    CHECK(score(m, "dart$", "fuzzy.dart", NULL, FFZ_FUZZY) >= 0,
          "escape: dart$ (real $) matches suffix");
    // \$ should NOT trigger postfix
    CHECK(score(m, "dart\\$", "nodart", NULL, FFZ_FUZZY) < 0,
          "escape: dart\\$ (escaped $) does not match 'nodart'");

    // ^sup as prefix: fails when not at start
    CHECK(score(m, "^sup", "a sup thing", NULL, FFZ_FUZZY) < 0,
          "escape: ^sup (prefix) fails when not leading");
    // ^sup matches when at start
    CHECK(score(m, "^sup", "supersonic", NULL, FFZ_FUZZY) >= 0,
          "escape: ^sup (prefix) matches at start");
    // \^ should NOT trigger prefix: needle becomes literal '^sup', so it
    // requires '^' in the haystack — here we verify it FAILS on a haystack
    // without '^' (contrast with unescaped ^sup which fails due to prefix).
    CHECK(score(m, "\\^sup", "supersonic", NULL, FFZ_FUZZY) < 0,
          "escape: \\^sup (literal '^sup' needle) misses 'supersonic' (no '^')");
}

static void test_query_edge_cases(ffz_matcher *m) {
    // ^abc$ combination = EXACT
    CHECK(score(m, "^abc$", "abc", NULL, FFZ_FUZZY) >= 0,
          "query: ^abc$ matches exact 'abc'");
    CHECK(score(m, "^abc$", "abcd", NULL, FFZ_FUZZY) < 0,
          "query: ^abc$ rejects 'abcd'");
    CHECK(score(m, "^abc$", "xabc", NULL, FFZ_FUZZY) < 0,
          "query: ^abc$ rejects 'xabc'");

    // whitespace-only query: 0 atoms -> score=0 (matches all)
    CHECK(score(m, "   ", "anything", NULL, FFZ_FUZZY) == 0,
          "query: whitespace-only gives score=0");

    // double-space between words: empty word is dropped
    CHECK(score(m, "fo  bar", "foo/bar", NULL, FFZ_FUZZY) >= 0,
          "query: double-space drops empty word, still matches");
}

static void test_off_unicode_indices(void) {
    ffz_config cfg = ffz_config_default();
    cfg.scoring_mode = FFZ_SCORE_OFF;
    ffz_matcher *m = ffz_matcher_new(cfg);

    // Unicode haystack "東京都" (3 CJK codepoints), needle "京都"
    // Expected codepoint indices: [1, 2]
    ffz_indices ix = {0};
    int32_t s = score(m,
        "\xe4\xba\xac\xe9\x83\xbd",          // "京都" UTF-8
        "\xe6\x9d\xb1\xe4\xba\xac\xe9\x83\xbd", // "東京都" UTF-8
        &ix, FFZ_FUZZY);
    CHECK(s == 0, "OFF+Unicode: score==0");
    CHECK(ix.len == 2, "OFF+Unicode: 2 indices");
    if (ix.len == 2) {
        CHECK(ix.data[0] == 1, "OFF+Unicode: '京' at codepoint idx 1");
        CHECK(ix.data[1] == 2, "OFF+Unicode: '都' at codepoint idx 2");
    }
    ffz_indices_free(&ix);

    // ASCII OFF indices: strictly ascending
    ffz_indices ix2 = {0};
    score(m, "abc", "xaxbxcxd", &ix2, FFZ_FUZZY);
    int asc = 1;
    for (size_t i = 1; i < ix2.len; i++)
        if (ix2.data[i] <= ix2.data[i-1]) asc = 0;
    CHECK(asc, "OFF+ASCII: indices strictly ascending");
    ffz_indices_free(&ix2);

    ffz_matcher_free(m);
}

static void test_corpus_empty_query(void) {
    ffz_corpus *c = ffz_corpus_new(ffz_config_default());
    ffz_corpus_add(c, "alpha", 5);
    ffz_corpus_add(c, "beta", 4);
    ffz_corpus_add(c, "gamma", 5);

    // Empty query: 0 atoms -> all items match with score=0
    ffz_results r = {0};
    ffz_corpus_filter(c, "", 0, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
    CHECK(r.len == 3, "empty query: all 3 items match");
    int all_zero = 1;
    for (size_t i = 0; i < r.len; i++)
        if (r.hits[i].score != 0) all_zero = 0;
    CHECK(all_zero, "empty query: all scores are 0");
    ffz_results_free(&r);

    // Empty query with limit
    ffz_results r2 = {0};
    ffz_corpus_filter(c, "", 0, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 2, FFZ_SCORE_FAST, &r2);
    CHECK(r2.len == 2, "empty query: limit=2 respected");
    ffz_results_free(&r2);

    ffz_corpus_free(c);
}

// P7: Malformed UTF-8 — robustness against the 6 ill-formed byte sequences.
//
// For each scenario we verify three things:
//   a) Using the malformed string as a haystack with a valid pattern does not
//      crash and returns a well-defined value (>= -1).
//   b) Using the malformed string as a pattern with a valid haystack does not
//      crash and returns a well-defined value (>= -1).
//   c) ffz_str_from_utf8 decodes every ill-formed sequence as U+FFFD (0xFFFD),
//      verified by reading the codepoint array produced by ffz_str_buf.
//
// ffz_decode_cp is an internal symbol; we reach it through the public
// ffz_str_from_utf8 API which uses it as its sole decoder.  The ffz_str_buf
// struct (cp, len, cap) is part of the public ffz.h header and safe to read.
static void test_invalid_utf8(ffz_matcher *m) {
    // Helper macro: build a ffz_str from a byte literal via ffz_str_from_utf8
    // and assert that every produced codepoint equals U+FFFD.
#define CHECK_ALL_FFFD(bytes, desc)                                           \
    do {                                                                      \
        const char *_s = (bytes);                                             \
        size_t _n = sizeof(bytes) - 1;                                        \
        ffz_str_buf _buf = {0};                                               \
        ffz_str _str = ffz_str_from_utf8(_s, _n, &_buf);                     \
        /* Non-ASCII input always goes through the codepoint path. */         \
        int _all_fffd = 1;                                                    \
        for (size_t _i = 0; _i < _str.len; _i++) {                           \
            uint32_t _cp = _str.u ? _str.u[_i] : (uint32_t)(uint8_t)_str.b[_i]; \
            if (_cp != 0xFFFDu) _all_fffd = 0;                               \
        }                                                                     \
        CHECK(_all_fffd, "decode: " desc " -> all U+FFFD");                   \
        ffz_str_buf_free(&_buf);                                              \
    } while (0)

    // ------------------------------------------------------------------
    // 1. Truncated 2-byte sequence: "\xC3" (first byte of U+00C3 'Ã', no cont.)
    // ------------------------------------------------------------------
    {
        // a) malformed haystack -> decoded as U+FFFD, 'a' cannot match U+FFFD
        int32_t s = score(m, "a", "\xC3", NULL, FFZ_FUZZY);
        CHECK(s == -1, "truncated seq haystack: no match (U+FFFD != 'a')");
        // b) malformed pattern -> Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xC3", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "truncated seq pattern: no match (Unicode needle vs ASCII hay)");
        // c) decode -> U+FFFD
        CHECK_ALL_FFFD("\xC3", "truncated 2-byte seq");
    }

    // ------------------------------------------------------------------
    // 2. Lone continuation bytes: "\x80\x80" (not a lead byte)
    // ------------------------------------------------------------------
    {
        // a) each 0x80 is an invalid lead byte -> decoded as U+FFFD each
        int32_t s = score(m, "a", "\x80\x80", NULL, FFZ_FUZZY);
        CHECK(s == -1, "lone cont bytes haystack: no match (U+FFFD != 'a')");
        // b) Unicode needle vs ASCII haystack -> no match
        s = score(m, "\x80\x80", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "lone cont bytes pattern: no match (Unicode needle vs ASCII hay)");
        CHECK_ALL_FFFD("\x80\x80", "lone continuation bytes");
    }

    // ------------------------------------------------------------------
    // 3. Overlong NUL: "\xC0\x80" (NUL encoded as 2 bytes instead of 1)
    // ------------------------------------------------------------------
    {
        // a) overlong -> U+FFFD; 'a' cannot match U+FFFD
        int32_t s = score(m, "a", "\xC0\x80", NULL, FFZ_FUZZY);
        CHECK(s == -1, "overlong NUL haystack: no match (U+FFFD != 'a')");
        // b) Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xC0\x80", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "overlong NUL pattern: no match (Unicode needle vs ASCII hay)");
        CHECK_ALL_FFFD("\xC0\x80", "overlong NUL (C0 80)");
    }

    // ------------------------------------------------------------------
    // 4. Overlong '/': "\xC0\xAF" (U+002F encoded as 2 bytes)
    // ------------------------------------------------------------------
    {
        // a) overlong -> U+FFFD; 'a' cannot match U+FFFD
        int32_t s = score(m, "a", "\xC0\xAF", NULL, FFZ_FUZZY);
        CHECK(s == -1, "overlong slash haystack: no match (U+FFFD != 'a')");
        // b) Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xC0\xAF", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "overlong slash pattern: no match (Unicode needle vs ASCII hay)");
        CHECK_ALL_FFFD("\xC0\xAF", "overlong slash (C0 AF)");
    }

    // ------------------------------------------------------------------
    // 5. Surrogate pair upper: "\xED\xA0\x80" (U+D800, forbidden in UTF-8)
    // ------------------------------------------------------------------
    {
        // a) surrogate -> U+FFFD; 'a' cannot match U+FFFD
        int32_t s = score(m, "a", "\xED\xA0\x80", NULL, FFZ_FUZZY);
        CHECK(s == -1, "surrogate haystack: no match (U+FFFD != 'a')");
        // b) Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xED\xA0\x80", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "surrogate pattern: no match (Unicode needle vs ASCII hay)");
        CHECK_ALL_FFFD("\xED\xA0\x80", "surrogate U+D800 (ED A0 80)");
    }

    // ------------------------------------------------------------------
    // 6. Out-of-range: "\xF4\x90\x80\x80" (U+110000, above U+10FFFF)
    // ------------------------------------------------------------------
    {
        // a) out-of-range -> U+FFFD; 'a' cannot match U+FFFD
        int32_t s = score(m, "a", "\xF4\x90\x80\x80", NULL, FFZ_FUZZY);
        CHECK(s == -1, "out-of-range haystack: no match (U+FFFD != 'a')");
        // b) Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xF4\x90\x80\x80", "hello", NULL, FFZ_FUZZY);
        CHECK(s == -1, "out-of-range pattern: no match (Unicode needle vs ASCII hay)");
        CHECK_ALL_FFFD("\xF4\x90\x80\x80", "out-of-range U+110000 (F4 90 80 80)");
    }

    // ------------------------------------------------------------------
    // 7. 4-byte overlong '<': "\xF0\x80\x80\xBC" (U+003C encoded as 4 bytes)
    //    need==3 overlong branch: decoded cp=0x3C < 0x10000 -> U+FFFD
    //    Pattern is Unicode (non-ASCII bytes), haystack is ASCII -> no match.
    // ------------------------------------------------------------------
    {
        // a) overlong 4-byte as haystack: decoded as U+FFFD, not '<'; literal '<'
        //    pattern cannot match
        const char hay_lt[] = "<less-than>";
        ffz_str_buf hb = {0};
        ffz_str hs = ffz_str_from_utf8("\xF0\x80\x80\xBC", 4, &hb);
        // hs is Unicode (u != NULL); pattern "<" is ASCII: ASCII hay.b is NULL
        // -> use the pattern layer helper that wraps ffz_match directly
        ffz_str_buf pb = {0};
        ffz_str ps = ffz_str_from_utf8("<", 1, &pb);
        // hs decoded: U+FFFD (one codepoint), ps: '<' (0x3C)
        CHECK(hs.len == 1 && hs.u && hs.u[0] == 0xFFFDu,
              "overlong 4-byte '<': decoded as U+FFFD");
        int32_t s = ffz_match(m, hs, ps, FFZ_FUZZY, NULL);
        CHECK(s == -1, "overlong 4-byte '<' haystack: U+FFFD does not match '<'");
        ffz_str_buf_free(&hb);
        ffz_str_buf_free(&pb);

        // b) overlong 4-byte as pattern against ASCII haystack "<less-than>":
        //    Unicode needle vs ASCII haystack -> no match
        s = score(m, "\xF0\x80\x80\xBC", hay_lt, NULL, FFZ_FUZZY);
        CHECK(s == -1,
              "overlong 4-byte '<' pattern: Unicode needle vs ASCII hay -> no match");

        // c) decode -> U+FFFD
        CHECK_ALL_FFFD("\xF0\x80\x80\xBC", "overlong 4-byte '<' (F0 80 80 BC)");
    }

#undef CHECK_ALL_FFFD
}

static void test_prefer_prefix_nucleo(void) {
    // prefer_prefix should work in NUCLEO mode (different code path from FAST)
    ffz_config cfg = ffz_config_default();
    cfg.prefer_prefix = true;
    cfg.scoring_mode = FFZ_SCORE_NUCLEO;
    ffz_matcher *m = ffz_matcher_new(cfg);

    int32_t s_start = score(m, "abc", "abc_xyz", NULL, FFZ_FUZZY);
    int32_t s_end   = score(m, "abc", "xyz_abc", NULL, FFZ_FUZZY);
    CHECK(s_start > s_end, "NUCLEO prefer_prefix: earlier match scores higher");
    CHECK(s_start > 0,     "NUCLEO prefer_prefix: positive score for early match");

    ffz_matcher_free(m);
}

// Regression tests for bugs fixed in the multi-agent review rounds.
static void test_recent_fixes(void) {
    // (1) Score saturation (ffz_score.c: ffz_sat_add_u16). A very long exact
    // match accumulates ~24 per char; without saturation it wraps past uint16
    // to a tiny value, ranking a perfect match below a poor one. The fix clamps
    // at 65535.
    {
        size_t L = 10000;
        char *big = (char *)malloc(L + 1);
        memset(big, 'a', L);
        big[L] = 0;
        ffz_matcher *m = ffz_matcher_new(ffz_config_default());
        int32_t s = score(m, big, big, NULL, FFZ_EXACT);
        CHECK(s == 65535, "sat: long exact match saturates at 65535 (no wrap)");
        ffz_matcher_free(m);
        free(big);
    }

    // (2) FAST rolling oversized -> greedy fallback. W*nl > FFZ_MAX_MATRIX_SIZE
    // with out==NULL routes through ffz_fuzzy_rolling, whose cap must bail to
    // greedy (else an uncapped O(W*nl) DP hangs). Must return promptly + scored.
    {
        size_t HW = 600, NW = 200;  // 120000 cells > 102400
        char *hay = (char *)malloc(HW + 1);
        char *ndl = (char *)malloc(NW + 1);
        memset(hay, 'a', HW); hay[HW] = 0;
        memset(ndl, 'a', NW); ndl[NW] = 0;
        ffz_config cfg = ffz_config_default();
        cfg.scoring_mode = FFZ_SCORE_FAST;
        ffz_matcher *m = ffz_matcher_new(cfg);
        int32_t s = score(m, ndl, hay, NULL, FFZ_FUZZY);  // NULL -> rolling path
        CHECK(s > 0, "rolling oversized: FAST score-only falls back to greedy");
        ffz_matcher_free(m);
        free(hay); free(ndl);
    }

    // (3) Atom cap (FFZ_MAX_ATOMS=64). A query with far more space-separated
    // words than the cap must parse without crash/OOB; atoms past the cap are
    // dropped. Each "a" atom matches the haystack.
    {
        int words = 80;
        char query[160 + 1];  // 80 'a' + 79 spaces = 159 chars
        size_t qi = 0;
        for (int i = 0; i < words; i++) {
            query[qi++] = 'a';
            if (i + 1 < words) query[qi++] = ' ';
        }
        query[qi] = 0;
        ffz_matcher *m = ffz_matcher_new(ffz_config_default());
        int32_t s = score(m, query, "aaaaaaaa", NULL, FFZ_FUZZY);
        CHECK(s >= 0, "atom cap: >64-word query parses & matches without crash");
        ffz_matcher_free(m);
    }
}

int main(void) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());
    test_basic(m);
    test_indices(m);
    test_ranking(m);
    test_case(m);
    test_unicode(m);
    test_modes(m);
    test_pattern_syntax(m);
    test_greedy_fallback(m);
    test_config_variants(m);
    ffz_matcher_free(m);
    test_corpus();
    test_add_keyed();
    test_parallel();
    test_property();
    test_determinism_corners();
    test_corpus_clear_limit();
    test_corpus_scoring_modes();
    test_scoring_modes();
    test_rolling_dp();
    test_scoring_cross();
    test_rolling_golden();
    test_fast_index_consistency();
    test_recent_fixes();
    test_boundary_conditions();
    test_prefer_prefix();
    test_corpus_scoring();
    test_cjk_modes();
    test_rfind_boundaries();
    test_corpus_off_modes();
    m = ffz_matcher_new(ffz_config_default());
    test_escape_syntax(m);
    test_query_edge_cases(m);
    ffz_matcher_free(m);
    test_off_unicode_indices();
    test_corpus_empty_query();
    test_prefer_prefix_nucleo();
    m = ffz_matcher_new(ffz_config_default());
    test_invalid_utf8(m);
    ffz_matcher_free(m);

    printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
