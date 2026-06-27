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
FFZ_API void ffz_ffi_add(ffz_corpus *c, const char *s, size_t n) {
    ffz_corpus_add(c, s, n);
}
// Add an item with explicit alternate keys (host-computed pinyin/romaji/etc).
// Keys are passed as parallel arrays so the Dart side never mirrors a struct:
// texts[i]/lens[i]/kinds[i] for i in [0,nkeys). The ORIGINAL key is implicit.
FFZ_API void ffz_ffi_add_keyed(ffz_corpus *c, const char *s, size_t n,
                               const char *const *texts, const size_t *lens,
                               const int *kinds, size_t nkeys) {
    if (nkeys == 0) { ffz_corpus_add(c, s, n); return; }
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
FFZ_API size_t ffz_ffi_len(ffz_corpus *c) { return ffz_corpus_len(c); }
FFZ_API void ffz_ffi_clear(ffz_corpus *c) { ffz_corpus_clear(c); }
FFZ_API void ffz_ffi_free(ffz_corpus *c) { ffz_corpus_free(c); }

// --- filter: mode 0=fuzzy 1=substring 2=prefix 3=postfix 4=exact (word);
//     cm 0=respect 1=ignore 2=smart;  nm 0=never 1=smart ---------------------
FFZ_API ffz_results *ffz_ffi_filter_ex(ffz_corpus *c, const char *q, size_t qn,
                                       int mode, int cm, int nm, int parallel,
                                       int threads, size_t limit) {
    ffz_results *r = (ffz_results *)calloc(1, sizeof(ffz_results));
    if (!r) return NULL;
    ffz_parallel par;
    par.parallel = parallel != 0;
    par.threads = threads;
    ffz_corpus_filter(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                      (ffz_mode)mode, par, limit, ffz_corpus_scoring(c), r);
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
FFZ_API size_t ffz_ffi_results_len(ffz_results *r) { return r->len; }
FFZ_API uint32_t ffz_ffi_results_item(ffz_results *r, size_t i) {
    return r->hits[i].item_index;
}
FFZ_API int32_t ffz_ffi_results_score(ffz_results *r, size_t i) {
    return r->hits[i].score;
}
FFZ_API int ffz_ffi_results_kind(ffz_results *r, size_t i) {
    return r->hits[i].matched_kind;
}
FFZ_API uint32_t ffz_ffi_results_key(ffz_results *r, size_t i) {
    return r->hits[i].matched_key;
}
FFZ_API size_t ffz_ffi_results_nindices(ffz_results *r, size_t i) {
    return r->hits[i].indices.len;
}
FFZ_API uint32_t ffz_ffi_results_index(ffz_results *r, size_t i, size_t j) {
    return r->hits[i].indices.data[j];
}
FFZ_API void ffz_ffi_results_free(ffz_results *r) {
    ffz_results_free(r);
    free(r);
}
