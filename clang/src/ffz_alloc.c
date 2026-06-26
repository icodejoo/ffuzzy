// Counting allocator wrappers for the leak smoke test (FFZ_TRACK_ALLOC).
// This TU does NOT include ffz_alloc.h's macros, so it calls the real
// stdlib functions. Block counting (not byte counting) is enough to prove that
// every allocation is eventually freed; the counter is atomic because the
// parallel scan allocates from worker threads.
#ifdef FFZ_TRACK_ALLOC

#include <stdatomic.h>
#include <stdlib.h>

static atomic_size_t g_live = 0;

void *ffz_dbg_malloc(size_t n) {
    void *p = malloc(n);
    if (p) atomic_fetch_add(&g_live, 1);
    return p;
}

void *ffz_dbg_calloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (p) atomic_fetch_add(&g_live, 1);
    return p;
}

void *ffz_dbg_realloc(void *q, size_t n) {
    void *p = realloc(q, n);
    // q==NULL behaves like malloc (new block); otherwise the block count is
    // unchanged (grown in place or moved). We never realloc to size 0.
    if (!q && p) atomic_fetch_add(&g_live, 1);
    return p;
}

void ffz_dbg_free(void *q) {
    if (q) {
        atomic_fetch_sub(&g_live, 1);
        free(q);
    }
}

size_t ffz_alloc_live_blocks(void) { return atomic_load(&g_live); }

#endif  // FFZ_TRACK_ALLOC
