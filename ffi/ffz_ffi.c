// Flat C-ABI shim for dart:ffi — opaque handles + scalar args/accessors only,
// so the Dart side never has to mirror the by-value structs (ffz_config /
// ffz_str / ffz_results). Compile this together with ../src/*.c.
//
// Not part of the locked Android build (that uses only src/); this is the
// integration/FFI glue layer a host would write.
#include <stdlib.h>

#include "ffz.h"
#include "ffz_corpus.h"
#ifdef FFZ_HAVE_CRASH_HANDLER
#include "ffz_crash.h"
#endif

// FFZ_API marks symbols for export from the shared library.
// dllimport is intentionally omitted: all consumers (Dart FFI) use dynamic
// symbol lookup and never link against an import library, so the export-only
// definition is correct for both the implementation and any C test drivers
// that compile ffz_ffi.c directly into the same translation unit.
#ifdef _WIN32
#define FFZ_API __declspec(dllexport)
#else
#define FFZ_API __attribute__((visibility("default")))
#endif

// --- optional native crash handler ----------------------------------------
// Native faults can't be caught as Dart exceptions; this installs a last-gasp
// handler that prints/persists a backtrace before the process dies. Pass a
// writable file path to also leave a "last crash" breadcrumb the host reads on
// next launch, or NULL for stderr/logcat only.
//
// Only present when the handler is compiled in (LOCATABLE builds, or release
// with FFZ_CRASH_IN_RELEASE). In a plain release lib the symbol is absent and
// the Dart binding's tolerant lookup makes FfzCrash.install() return false.
#ifdef FFZ_HAVE_CRASH_HANDLER
FFZ_API int ffz_ffi_install_crash_handler(const char *breadcrumb_path) {
    return ffz_install_crash_handler(breadcrumb_path);
}
#endif

// --- corpus lifecycle -----------------------------------------------------
FFZ_API ffz_corpus *ffz_ffi_new(void) {
    return ffz_corpus_new(ffz_config_default());
}
// Configurable corpus: paths!=0 uses path-style delimiters; prefer_prefix
// biases scoring toward matches near the start.
FFZ_API ffz_corpus *ffz_ffi_new_cfg(int paths, int prefer_prefix) {
    ffz_config cfg = paths ? ffz_config_match_paths() : ffz_config_default();
    cfg.prefer_prefix = prefer_prefix != 0;
    return ffz_corpus_new(cfg);
}
// Like ffz_ffi_new_cfg but also sets the corpus-level scoring mode.
// scoring: 0=FAST (default), 1=OFF, 2=NUCLEO.
FFZ_API ffz_corpus *ffz_ffi_new_cfg2(int paths, int prefer_prefix, int scoring) {
    if ((unsigned)scoring > FFZ_SCORE_NUCLEO) scoring = FFZ_SCORE_FAST;
    ffz_config cfg = paths ? ffz_config_match_paths() : ffz_config_default();
    cfg.prefer_prefix = prefer_prefix != 0;
    cfg.scoring_mode  = (ffz_scoring_mode)scoring;
    return ffz_corpus_new(cfg);
}
FFZ_API void ffz_ffi_add(ffz_corpus *c, const char *s, size_t n) {
    if (!c) return;
    ffz_corpus_add(c, s, n);
}
// Add an item with explicit alternate keys (host-computed pinyin/romaji/etc).
// Keys are passed as parallel arrays so the Dart side never mirrors a struct:
// texts[i]/lens[i]/kinds[i] for i in [0,nkeys). The ORIGINAL key is implicit.
FFZ_API void ffz_ffi_add_keyed(ffz_corpus *c, const char *s, size_t n,
                               const char *const *texts, const size_t *lens,
                               const int *kinds, size_t nkeys) {
    if (!c) return;
    if (nkeys == 0) { ffz_corpus_add(c, s, n); return; }
    if (!texts || !lens || !kinds) { ffz_corpus_add(c, s, n); return; }
    if (nkeys > SIZE_MAX / sizeof(ffz_key)) { ffz_corpus_add(c, s, n); return; }
    ffz_key *keys = (ffz_key *)malloc(nkeys * sizeof(ffz_key));
    if (!keys) { ffz_corpus_add(c, s, n); return; }  // OOM: keep ORIGINAL only
    for (size_t i = 0; i < nkeys; i++) {
        keys[i].text = texts[i];
        keys[i].len = lens[i];
        keys[i].kind = kinds[i];
    }
    ffz_corpus_add_keyed(c, s, n, keys, nkeys);
    free(keys);
}
FFZ_API size_t ffz_ffi_len(ffz_corpus *c) { if (!c) return 0; return ffz_corpus_len(c); }
FFZ_API void ffz_ffi_clear(ffz_corpus *c) { if (!c) return; ffz_corpus_clear(c); }
FFZ_API void ffz_ffi_free(ffz_corpus *c) { if (!c) return; ffz_corpus_free(c); }

// --- filter: mode 0=fuzzy 1=substring 2=prefix 3=postfix 4=exact (word);
//     cm 0=respect 1=ignore 2=smart;  nm 0=never 1=smart ---------------------
// Like ffz_ffi_filter_ex but takes an explicit scoring mode. The Dart layer
// passes the already-resolved effective scoring (corpus default merged with
// per-call override). scoring: 0=FAST, 1=OFF, 2=NUCLEO.
FFZ_API ffz_results *ffz_ffi_filter_ex2(ffz_corpus *c, const char *q, size_t qn,
                                        int mode, int cm, int nm,
                                        int parallel, int threads,
                                        size_t limit, int scoring) {
    if (!c || !q) return NULL;  /* use q="" for an empty query (match all) */
    if ((unsigned)mode    > FFZ_EXACT        ||
        (unsigned)cm      > FFZ_CASE_SMART   ||
        (unsigned)nm      > FFZ_NORM_SMART   ||
        (unsigned)scoring > FFZ_SCORE_NUCLEO) return NULL;
    ffz_results *r = (ffz_results *)calloc(1, sizeof(ffz_results));
    if (!r) return NULL;
    ffz_parallel par;
    par.parallel = parallel != 0;
    par.threads  = threads;
    ffz_corpus_filter(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                      (ffz_mode)mode, par, limit, (ffz_scoring_mode)scoring, r);
    return r;
}
FFZ_API ffz_results *ffz_ffi_filter_ex(ffz_corpus *c, const char *q, size_t qn,
                                       int mode, int cm, int nm, int parallel,
                                       int threads, size_t limit) {
    // Old callers get FAST (the new default) for backward compatibility.
    return ffz_ffi_filter_ex2(c, q, qn, mode, cm, nm, parallel, threads, limit,
                              FFZ_SCORE_FAST);
}
// Like ffz_ffi_filter_ex2 but skips per-survivor index computation (Pass 2).
// Results have empty indices; score/kind/key/item are still valid.
// Use ffz_ffi_results_item() to map results back to corpus items.
FFZ_API ffz_results *ffz_ffi_filter_raws(ffz_corpus *c, const char *q, size_t qn,
                                          int mode, int cm, int nm,
                                          int parallel, int threads,
                                          size_t limit, int scoring) {
    if (!c || !q) return NULL;
    if ((unsigned)mode    > FFZ_EXACT        ||
        (unsigned)cm      > FFZ_CASE_SMART   ||
        (unsigned)nm      > FFZ_NORM_SMART   ||
        (unsigned)scoring > FFZ_SCORE_NUCLEO) return NULL;
    ffz_results *r = (ffz_results *)calloc(1, sizeof(ffz_results));
    if (!r) return NULL;
    ffz_parallel par;
    par.parallel = parallel != 0;
    par.threads  = threads;
    ffz_corpus_filter_raws(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                           (ffz_mode)mode, par, limit, (ffz_scoring_mode)scoring, r);
    return r;
}
// Back-compat default (smart case + smart normalize).
FFZ_API ffz_results *ffz_ffi_filter(ffz_corpus *c, const char *q, size_t qn,
                                    int mode, int parallel, int threads,
                                    size_t limit) {
    return ffz_ffi_filter_ex(c, q, qn, mode, FFZ_CASE_SMART, FFZ_NORM_SMART,
                             parallel, threads, limit);
}

// --- result accessors (no struct layout needed on the Dart side) ----------
FFZ_API size_t ffz_ffi_results_len(ffz_results *r) {
    return r ? r->len : 0;
}
FFZ_API uint32_t ffz_ffi_results_item(ffz_results *r, size_t i) {
    if (!r || i >= r->len) return UINT32_MAX;
    return r->hits[i].item_index;
}
FFZ_API int32_t ffz_ffi_results_score(ffz_results *r, size_t i) {
    if (!r || i >= r->len) return -1;
    return r->hits[i].score;
}
FFZ_API int ffz_ffi_results_kind(ffz_results *r, size_t i) {
    if (!r || i >= r->len) return -1;
    return r->hits[i].matched_kind;
}
FFZ_API uint32_t ffz_ffi_results_key(ffz_results *r, size_t i) {
    if (!r || i >= r->len) return UINT32_MAX;
    return r->hits[i].matched_key;
}
FFZ_API size_t ffz_ffi_results_nindices(ffz_results *r, size_t i) {
    if (!r || i >= r->len) return 0;
    return r->hits[i].indices.len;
}
FFZ_API uint32_t ffz_ffi_results_index(ffz_results *r, size_t i, size_t j) {
    if (!r || i >= r->len || j >= r->hits[i].indices.len) return UINT32_MAX;
    return r->hits[i].indices.data[j];
}
FFZ_API void ffz_ffi_results_free(ffz_results *r) {
    if (!r) return;
    ffz_results_free(r);
    free(r);
}

// --- bulk accessors: fill caller-provided arrays in one call ----------------
// Reduces JS→WASM boundary crossings from O(N) to O(1) when reading results.

// Fill out[0..n-1] with item indices. Returns actual count written (min(r->len, n)).
FFZ_API size_t ffz_ffi_results_items_bulk(ffz_results *r,
                                           uint32_t *out, size_t n) {
    if (!r || !out) return 0;
    size_t count = r->len < n ? r->len : n;
    for (size_t i = 0; i < count; i++) out[i] = r->hits[i].item_index;
    return count;
}

// Fill parallel arrays for item_index, score, matched_kind, matched_key.
// Each array must hold at least min(r->len, n) elements. Returns count written.
FFZ_API size_t ffz_ffi_results_bulk(ffz_results *r,
                                     uint32_t *items, int32_t *scores,
                                     int32_t *kinds,  uint32_t *keys,
                                     size_t n) {
    if (!r || !items || !scores || !kinds || !keys) return 0;
    size_t count = r->len < n ? r->len : n;
    for (size_t i = 0; i < count; i++) {
        items[i]  = r->hits[i].item_index;
        scores[i] = r->hits[i].score;
        kinds[i]  = r->hits[i].matched_kind;
        keys[i]   = r->hits[i].matched_key;
    }
    return count;
}
