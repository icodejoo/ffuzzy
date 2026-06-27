// Optional allocation tracking for the leak smoke test. When FFZ_TRACK_ALLOC
// is defined, malloc/calloc/realloc/free in the library's TUs are redirected to
// counting wrappers and ffz_alloc_live_blocks() reports outstanding blocks.
// Zero code/overhead otherwise.
//
// The macros are object-like, so they only rewrite the bare `free`/`malloc`
// tokens — `ffz_indices_free(...)` etc. are untouched. Include this AFTER
// <stdlib.h> in each translation unit.
#ifndef FFZ_ALLOC_H
#define FFZ_ALLOC_H

#include <stddef.h>

#ifdef FFZ_TRACK_ALLOC
void *ffz_dbg_malloc(size_t n);
void *ffz_dbg_calloc(size_t n, size_t sz);
void *ffz_dbg_realloc(void *p, size_t n);
void ffz_dbg_free(void *p);
size_t ffz_alloc_live_blocks(void);  // currently-outstanding allocations
void ffz_dbg_fail_after(int n);      // tests: fail every alloc after n (-1=off)

#define malloc ffz_dbg_malloc
#define calloc ffz_dbg_calloc
#define realloc ffz_dbg_realloc
#define free ffz_dbg_free
#else
static inline size_t ffz_alloc_live_blocks(void) { return 0; }
#endif

#endif  // FFZ_ALLOC_H
