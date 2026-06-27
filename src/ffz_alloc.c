// Counting allocator wrappers for the leak smoke test (FFZ_TRACK_ALLOC).
// This TU does NOT include ffz_alloc.h's macros, so it calls the real
// stdlib functions. Block counting (not byte counting) is enough to prove that
// every allocation is eventually freed; the counter is atomic because the
// parallel scan allocates from worker threads.
#ifdef FFZ_TRACK_ALLOC

#include <stdlib.h>

// The counters are touched from parallel-scan worker threads, so they must be
// atomic. MSVC's <stdatomic.h> needs an opt-in switch that varies across
// toolset versions (/experimental:c11atomics, /std:c11, ...); rather than chase
// it, use the Interlocked intrinsics on MSVC and C11 atomics everywhere else.
#if defined(_MSC_VER)
#include <intrin.h>
typedef volatile long long ffz_atomic;
#define FFZ_LOAD(p)      _InterlockedOr64((p), 0)
#define FFZ_STORE(p, v)  _InterlockedExchange64((p), (long long)(v))
#define FFZ_INC(p)       (void)_InterlockedIncrement64(p)
#define FFZ_DEC(p)       (void)_InterlockedDecrement64(p)
#else
#include <stdatomic.h>
typedef _Atomic long long ffz_atomic;
#define FFZ_LOAD(p)      atomic_load(p)
#define FFZ_STORE(p, v)  atomic_store((p), (long long)(v))
#define FFZ_INC(p)       (void)atomic_fetch_add((p), 1)
#define FFZ_DEC(p)       (void)atomic_fetch_sub((p), 1)
#endif

static ffz_atomic g_live = 0;

// OOM fault injection for tests: after `g_fail_after` successful allocations,
// every further allocation returns NULL (simulating sustained OOM), exercising
// the library's drop-on-OOM paths. -1 disables (the default).
static ffz_atomic g_fail_after = -1;

void ffz_dbg_fail_after(int n) { FFZ_STORE(&g_fail_after, n); }

// Returns 1 and consumes a budget slot if this allocation should fail.
static int fail_now(void) {
    long long c = FFZ_LOAD(&g_fail_after);
    if (c < 0) return 0;            // injection disabled
    if (c == 0) return 1;           // budget exhausted -> fail
    FFZ_DEC(&g_fail_after);
    return 0;
}

void *ffz_dbg_malloc(size_t n) {
    if (fail_now()) return NULL;
    void *p = malloc(n);
    if (p) FFZ_INC(&g_live);
    return p;
}

void *ffz_dbg_calloc(size_t n, size_t sz) {
    if (fail_now()) return NULL;
    void *p = calloc(n, sz);
    if (p) FFZ_INC(&g_live);
    return p;
}

void *ffz_dbg_realloc(void *q, size_t n) {
    if (fail_now()) return NULL;    // realloc failure leaves the old block valid
    void *p = realloc(q, n);
    // q==NULL behaves like malloc (new block); otherwise the block count is
    // unchanged (grown in place or moved). We never realloc to size 0.
    if (!q && p) FFZ_INC(&g_live);
    return p;
}

void ffz_dbg_free(void *q) {
    if (q) {
        FFZ_DEC(&g_live);
        free(q);
    }
}

size_t ffz_alloc_live_blocks(void) { return (size_t)FFZ_LOAD(&g_live); }

#endif  // FFZ_TRACK_ALLOC
