// Unit tests for the ffz matcher, pattern layer, and corpus hook.
// No framework: assert helpers track pass/fail and the process exits nonzero
// on any failure.
#include <stdio.h>
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
                      FFZ_FUZZY, ffz_parallel_off(), 0, &r);
    CHECK(r.len >= 1, "zhangsan finds something");
    int found_zh = 0;
    for (size_t i = 0; i < r.len; i++)
        if (r.hits[i].item_index == 0 && r.hits[i].matched_kind == FFZ_KEY_PINYIN)
            found_zh = 1;
    CHECK(found_zh, "zhangsan -> item 0 via PINYIN key");
    ffz_results_free(&r);

    // initials "zs" -> 张三 via INITIALS key.
    ffz_corpus_filter(c, "zs", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 0, &r);
    int found_ini = 0;
    for (size_t i = 0; i < r.len; i++)
        if (r.hits[i].item_index == 0 &&
            r.hits[i].matched_kind == FFZ_KEY_INITIALS)
            found_ini = 1;
    CHECK(found_ini, "zs -> item 0 via INITIALS key");
    ffz_results_free(&r);

    // original CJK query -> matches ORIGINAL key, indices into the display text.
    ffz_corpus_filter(c, "\xE5\xBC\xA0", 3, FFZ_CASE_SMART, FFZ_NORM_SMART,
                      FFZ_FUZZY, ffz_parallel_off(), 0, &r);
    CHECK(r.len == 1 && r.hits[0].item_index == 0 &&
              r.hits[0].matched_kind == FFZ_KEY_ORIGINAL,
          "张 -> item 0 via ORIGINAL key");
    CHECK(r.len == 1 && r.hits[0].indices.len == 1 &&
              r.hits[0].indices.data[0] == 0,
          "张 highlight index 0 in display text");
    ffz_results_free(&r);

    // limit truncates.
    ffz_corpus_filter(c, "z", 1, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_off(), 1, &r);
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
                      ffz_parallel_off(), 100, &a);
    ffz_corpus_filter(c, "gem", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                      ffz_parallel_auto(), 100, &b);
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
                      ffz_parallel_with(4), 100, &d);
    CHECK(d.len == 100, "explicit 4 threads returns 100");
    ffz_results_free(&a);
    ffz_results_free(&b);
    ffz_results_free(&d);
    ffz_corpus_free(c);
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
    ffz_matcher_free(m);
    test_corpus();
    test_parallel();

    printf("\n%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
