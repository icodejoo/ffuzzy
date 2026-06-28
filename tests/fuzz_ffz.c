// libFuzzer target: drive the full corpus pipeline (UTF-8 decode -> pattern
// parse -> match -> highlight indices) with arbitrary bytes and let ASan catch
// any out-of-bounds / use-after-free / overflow. The first two bytes pick a
// match mode and a query/haystack split point; everything else is payload.
//
// Build & run (CI): clang -fsanitize=fuzzer,address,undefined -Iinclude
// src/*.c ffi/ffz_ffi.c tests/fuzz_ffz.c -o fuzz_ffz && ./fuzz_ffz
// -max_total_time=60 -rss_limit_mb=2048
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ffz.h"
#include "ffz_corpus.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    ffz_mode mode = (ffz_mode)(data[0] % 5);          // fuzzy..exact
    int parallel = data[2] & 1;                       // exercise the thread path
    ffz_scoring_mode scoring = (ffz_scoring_mode)((data[2] >> 1) % 3); // off/fast/nucleo
    size_t limit = data[3];                           // 0..255 (incl. 0 = all)
    size_t body = size - 4;
    size_t split = body ? (data[1] % (body + 1)) : 0; // query | haystack
    const char *q = (const char *)data + 4;
    size_t qn = split;
    const char *h = (const char *)data + 4 + split;
    size_t hn = body - split;

    // Exercise both path-style config and parallel/serial; corpus copies input.
    ffz_corpus *c = ffz_corpus_new((size & 1) ? ffz_config_match_paths()
                                              : ffz_config_default());
    if (!c) return 0;
    // Push past the 512-item parallel threshold, and exercise add_keyed (the
    // haystack doubles as an alternate key) so the multi-key arena layout and
    // the per-thread top-K merge get fuzzed too.
    ffz_key k = {h, hn, 1 /* pinyin */};
    for (int i = 0; i < 600; i++) {
        if (i & 1)
            ffz_corpus_add_keyed(c, h, hn, &k, 1);
        else
            ffz_corpus_add(c, h, hn);
    }

    ffz_results r;
    memset(&r, 0, sizeof(r));
    ffz_corpus_filter(c, q, qn, FFZ_CASE_SMART, FFZ_NORM_SMART, mode,
                      parallel ? ffz_parallel_auto() : ffz_parallel_off(), limit,
                      scoring, &r);

    // Sanity invariants (ASan-independent): every item_index is in range, and
    // reported indices are strictly increasing (no duplicate/backwards pos).
    size_t clen = ffz_corpus_len(c);
    for (size_t i = 0; i < r.len; i++) {
        if (r.hits[i].item_index >= clen) __builtin_trap();
        ffz_indices *idx = &r.hits[i].indices;
        for (size_t j = 1; j < idx->len; j++)
            if (idx->data[j] <= idx->data[j - 1]) __builtin_trap();
    }

    ffz_results_free(&r);
    ffz_corpus_free(c);
    return 0;
}
