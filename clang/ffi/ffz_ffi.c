// Flat C-ABI shim for dart:ffi — opaque handles + scalar args/accessors only,
// so the Dart side never has to mirror the by-value structs (ffz_config /
// ffz_str / ffz_results). Compile this together with ../src/*.c.
//
// Not part of the locked Android build (that uses only src/); this is the
// integration/FFI glue layer a host would write.
#include <stdlib.h>

#include "ffz.h"
#include "ffz_corpus.h"

#ifdef _WIN32
#define FFZ_API __declspec(dllexport)
#else
#define FFZ_API __attribute__((visibility("default")))
#endif

// --- corpus lifecycle -----------------------------------------------------
FFZ_API ffz_corpus *ffz_ffi_new(void) {
    return ffz_corpus_new(ffz_config_default());
}
FFZ_API void ffz_ffi_add(ffz_corpus *c, const char *s, size_t n) {
    ffz_corpus_add(c, s, n);
}
FFZ_API size_t ffz_ffi_len(ffz_corpus *c) { return ffz_corpus_len(c); }
FFZ_API void ffz_ffi_free(ffz_corpus *c) { ffz_corpus_free(c); }

// --- filter: mode 0=fuzzy 1=substring 2=prefix 3=postfix 4=exact (word) ----
FFZ_API ffz_results *ffz_ffi_filter(ffz_corpus *c, const char *q, size_t qn,
                                    int mode, int parallel, int threads,
                                    size_t limit) {
    ffz_results *r = (ffz_results *)calloc(1, sizeof(ffz_results));
    ffz_parallel par;
    par.parallel = parallel != 0;
    par.threads = threads;
    ffz_corpus_filter(c, q, qn, FFZ_CASE_SMART, FFZ_NORM_SMART, (ffz_mode)mode,
                      par, limit, r);
    return r;
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
