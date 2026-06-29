// ffz_corpus — resident multi-key index with a transliteration hook.
//
// This is the "index layer" that sits above the matcher. Each item can carry
// several *search keys* in addition to its display text — e.g. a Chinese name
// "张三" indexed alongside its pinyin "zhangsan" and initials "zs", so the user
// can find it by typing latin. The matcher itself stays tiny and Unicode-only;
// the transliteration dictionary lives entirely in the HOST via a callback.
//
//   item 0: display "张三"
//           keys: [ {ORIGINAL,"张三"}, {PINYIN,"zhangsan"}, {INITIALS,"zs"} ]
//
// On query, every key of every item is matched; the item takes its best key's
// score, and the hit reports WHICH key matched (kind + index) plus the matched
// codepoint indices *within that key* — so the UI knows whether/how to
// highlight (you usually only highlight when the matched key is ORIGINAL).
#ifndef FFZ_CORPUS_H
#define FFZ_CORPUS_H

#include "ffz.h"

#ifdef __cplusplus
extern "C" {
#endif

// Semantic tag for a generated key (extend freely; values are opaque to ffz).
typedef enum {
    FFZ_KEY_ORIGINAL = 0,  // the display text itself (always key 0)
    FFZ_KEY_PINYIN = 1,    // full pinyin, e.g. "zhongwen"
    FFZ_KEY_INITIALS = 2,  // initial letters, e.g. "zw"
    FFZ_KEY_ROMAJI = 3,    // japanese romaji
    FFZ_KEY_CUSTOM = 100   // host-defined kinds start here
} ffz_key_kind;

// One alternate search key emitted by the transliteration hook.
typedef struct {
    const char *text;  // UTF-8; ffz copies it, so host-owned/temporary is fine
    size_t len;        // byte length
    int kind;          // ffz_key_kind or a host-defined value
} ffz_key;

// Transliteration hook: called once per item at insert time. The host fills
// `out` (capacity `max_out`) with alternate keys for `item` and returns how
// many it wrote (clamped to `max_out`). Return 0 for "no extra keys". The
// ORIGINAL key is added by ffz automatically — do NOT emit it here.
//
// `ctx` is the user pointer registered with ffz_corpus_set_transliterator.
typedef size_t (*ffz_transliterator)(const char *item, size_t item_len,
                                     void *ctx, ffz_key *out, size_t max_out);

typedef struct ffz_corpus ffz_corpus;

// Create an empty corpus that matches with `cfg`.
ffz_corpus *ffz_corpus_new(ffz_config cfg);
void ffz_corpus_free(ffz_corpus *c);

// Register the transliteration hook BEFORE adding items (applies to subsequent
// adds). `max_keys_per_item` caps how many alternate keys are requested.
void ffz_corpus_set_transliterator(ffz_corpus *c, ffz_transliterator fn,
                                    void *ctx, size_t max_keys_per_item);

// Append items (UTF-8). The hook (if set) is invoked per item to build keys.
void ffz_corpus_add(ffz_corpus *c, const char *item, size_t len);

// Append an item with explicit alternate keys (UTF-8), bypassing the hook —
// the host computes pinyin/romaji/initials itself and passes them here. The
// ORIGINAL key is added automatically; `keys`/`nkeys` are the extras (copied).
void ffz_corpus_add_keyed(ffz_corpus *c, const char *item, size_t len,
                          const ffz_key *keys, size_t nkeys);

size_t ffz_corpus_len(const ffz_corpus *c);
void ffz_corpus_clear(ffz_corpus *c);

// One search result.
typedef struct {
    uint32_t item_index;   // index into the corpus (insertion order)
    int32_t score;         // best score across the item's keys
    int matched_kind;      // kind of the key that produced the best score
    uint32_t matched_key;  // index of that key within the item
    ffz_indices indices;   // matched codepoint positions WITHIN the matched key
} ffz_hit;

typedef struct {
    ffz_hit *hits;
    size_t len;
    size_t cap;
} ffz_results;

void ffz_results_free(ffz_results *r);

// Multi-threading control for the (CPU-bound) scoring pass. Off by default.
// When `parallel` is true:
//   threads == 0  -> auto: half the logical CPUs, default-capped at 8.
//   threads  > 0  -> explicit; may exceed 8.
// A global hard ceiling of (cpu_count - 1) is ALWAYS enforced and cannot be
// exceeded by any value (leaves one core free). Also clamped to the item count;
// corpora < 512 items run single-threaded. Results are identical and
// deterministically ordered regardless of thread count.
typedef struct {
    bool parallel;
    int threads;
} ffz_parallel;

ffz_parallel ffz_parallel_off(void);             // {false, 0}
ffz_parallel ffz_parallel_auto(void);            // {true, 0}  -> half CPUs
ffz_parallel ffz_parallel_with(int threads);     // {true, threads}

// Filter the corpus with `query` (parsed with `cm`/`nm`). Results are sorted by
// score descending (ties broken by item index). If `limit > 0`, at most
// `limit` hits are returned. `mode` selects the match algorithm for the query.
// `scoring` overrides the corpus-wide default for this call.
//
// `out` is reset (any prior hits freed) unconditionally at the start of every
// call — do NOT hold pointers into a previous result across a filter call on
// the same `out`. Must be zero-initialised before the very first call:
//   ffz_results r = {0};
void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                       ffz_case_matching cm, ffz_normalization nm,
                       ffz_mode mode, ffz_parallel par, size_t limit,
                       ffz_scoring_mode scoring,
                       ffz_results *out);

// Like ffz_corpus_filter but skips Pass 2 (index computation). All hit.indices
// will be empty (len=0). Faster when only item identity/order is needed.
void ffz_corpus_filter_raws(ffz_corpus *c, const char *query, size_t query_len,
                             ffz_case_matching cm, ffz_normalization nm,
                             ffz_mode mode, ffz_parallel par, size_t limit,
                             ffz_scoring_mode scoring,
                             ffz_results *out);

// Return the corpus-level scoring mode (stored in its ffz_config).
ffz_scoring_mode ffz_corpus_scoring(const ffz_corpus *c);

#ifdef __cplusplus
}
#endif
#endif  // FFZ_CORPUS_H
