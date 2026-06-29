// ffz_corpus — resident multi-key index with a transliteration hook.
//
// Two-pass filter (mirrors the Rust ffuzzy corpus): pass 1 scores every key of
// every item (no index allocation) to find each item's best key; pass 2 sorts,
// truncates to `limit`, then recomputes match indices only for the surviving
// hits on their winning key.
#include <stdlib.h>
#include <string.h>

#include "ffz_corpus.h"
#include "ffz_internal.h"

// --- portable threads (Win32 / pthreads) ----------------------------------
#if defined(_WIN32)
#include <windows.h>
typedef HANDLE ffz_thr;
static unsigned ffz_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}
#elif defined(FFZ_NO_THREADS)
// Single-thread build (e.g. wasm without -pthread): no thread API at all.
// resolve_threads() returns 1 because ffz_cpu_count()==1, so the thr_* helpers
// are never called and the corpus filter runs serially.
typedef int ffz_thr;
static unsigned ffz_cpu_count(void) { return 1; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t ffz_thr;
static unsigned ffz_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    // Clamp to a sane range: old Android Bionic may return -1 or an inflated
    // count that includes offline cores.
    return (n >= 1 && n <= 256) ? (unsigned)n : 1;
}
#endif

// Allocation tracking must come AFTER the system thread headers above (which
// reference malloc/free themselves); it covers all corpus allocation below.
#include "ffz_alloc.h"

// Below this many items, threading overhead isn't worth it.
#define FFZ_PARALLEL_MIN 512
// Default ceiling for AUTO selection (threads == 0). An explicit thread count
// may exceed this, but never the global hard ceiling of (cpu_count - 1).
#define FFZ_AUTO_THREADS_MAX 8

ffz_parallel ffz_parallel_off(void) { ffz_parallel p = {false, 0}; return p; }
ffz_parallel ffz_parallel_auto(void) { ffz_parallel p = {true, 0}; return p; }
ffz_parallel ffz_parallel_with(int t) { ffz_parallel p = {true, t}; return p; }

// --- string arena ----------------------------------------------------------
// Key bytes/codepoints are bump-allocated from large blocks, so a corpus of N
// keys does N cheap pointer bumps instead of N malloc()s (less per-allocation
// heap overhead and fragmentation). Keys are never freed piecemeal; the whole
// arena is released on clear/free. 8-byte alignment fits uint32_t codepoints.
typedef struct ffz_arena_block {
    struct ffz_arena_block *next;
    size_t used, cap;
    // payload bytes follow the header
} ffz_arena_block;
#define FFZ_ARENA_BLOCK (64 * 1024)
typedef struct { ffz_arena_block *head; } ffz_arena;

static inline unsigned char *blk_data(ffz_arena_block *b) {
    return (unsigned char *)b + sizeof(ffz_arena_block);
}
// Returns NULL on OOM; callers drop the key (file-wide drop-on-OOM policy).
static void *arena_alloc(ffz_arena *a, size_t n) {
    n = (n + 7u) & ~(size_t)7u;  // 8-byte align (fits uint32_t codepoints)
    ffz_arena_block *b = a->head;
    if (b && b->used + n <= b->cap) {  // fits the current head block
        void *p = blk_data(b) + b->used;
        b->used += n;
        return p;
    }
    size_t cap = n > FFZ_ARENA_BLOCK ? n : FFZ_ARENA_BLOCK;
    ffz_arena_block *nb = (ffz_arena_block *)malloc(sizeof(*nb) + cap);
    if (!nb) return NULL;
    nb->cap = cap;
    nb->used = n;
    if (n > FFZ_ARENA_BLOCK && b) {
        // Oversized dedicated block: splice it BEHIND the head so the head's
        // remaining tail stays available for subsequent small allocations.
        nb->next = b->next;
        b->next = nb;
    } else {
        nb->next = a->head;
        a->head = nb;
    }
    return blk_data(nb);
}
static void arena_free(ffz_arena *a) {
    for (ffz_arena_block *b = a->head; b;) {
        ffz_arena_block *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
}

// Compact key: one data pointer (bytes XOR codepoints) + small fields = 16 B
// (was 32 B with two pointers, one always NULL).
typedef struct {
    void *data;     // uint8_t[] if `ascii`, else uint32_t[]
    uint32_t len;   // code units (haystacks are bounded to 2^32-1)
    uint16_t kind;
    uint8_t ascii;
} corpus_key;

// The first (ORIGINAL) key is inlined, so items with no transliteration keys
// need NO separate keys-array allocation — the common case.
typedef struct {
    corpus_key key0;
    corpus_key *extra;  // hook-generated keys; NULL when none
    uint32_t n_extra;
} corpus_item;

static inline size_t item_nkeys(const corpus_item *it) { return 1 + it->n_extra; }
static inline const corpus_key *item_key(const corpus_item *it, size_t k) {
    return k == 0 ? &it->key0 : &it->extra[k - 1];
}
static inline ffz_str key_str(const corpus_key *k) {
    ffz_str s;
    s.len = k->len;
    s.b = k->ascii ? (const uint8_t *)k->data : NULL;
    s.u = k->ascii ? NULL : (const uint32_t *)k->data;
    return s;
}

// --- filtering result type (forward-declared here so ffz_corpus can cache it)
typedef struct {
    uint32_t item_index;
    int32_t score;
    int matched_kind;
    uint32_t matched_key;
} scored;

struct ffz_corpus {
    ffz_config cfg;
    corpus_item *items;
    size_t n, cap;
    ffz_transliterator hook;
    void *hook_ctx;
    size_t max_keys;
    ffz_str_buf scratch;
    ffz_arena arena;  // owns all key byte/codepoint storage
};

ffz_corpus *ffz_corpus_new(ffz_config cfg) {
    ffz_corpus *c = (ffz_corpus *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = cfg;
    return c;
}

void ffz_corpus_clear(ffz_corpus *c) {
    // Key data lives in the arena (released wholesale); only the per-item
    // `extra` key array is individually heap-allocated.
    for (size_t i = 0; i < c->n; i++) free(c->items[i].extra);
    arena_free(&c->arena);
    c->n = 0;
}

void ffz_corpus_free(ffz_corpus *c) {
    if (!c) return;
    ffz_corpus_clear(c);
    free(c->items);
    ffz_str_buf_free(&c->scratch);
    free(c);
}

void ffz_corpus_set_transliterator(ffz_corpus *c, ffz_transliterator fn,
                                    void *ctx, size_t max_keys_per_item) {
    c->hook = fn;
    c->hook_ctx = ctx;
    c->max_keys = max_keys_per_item;
}

size_t ffz_corpus_len(const ffz_corpus *c) { return c->n; }

// Decode UTF-8 and store an owned copy of the key in the arena (bytes if ASCII,
// else codepoints). On arena OOM the key degrades to empty (data NULL, len 0).
// Uses the corpus-wide `c->scratch`, so this is add-path (single-threaded) only
// — filtering never calls it. Do not invoke from worker threads.
static void dup_key(ffz_corpus *c, const char *s, size_t n, int kind,
                    corpus_key *out) {
    ffz_str v = ffz_str_from_utf8(s, n, &c->scratch);
    out->kind = (uint16_t)kind;
    out->len = (uint32_t)v.len;
    if (v.b) {
        out->ascii = 1;
        uint8_t *p = (uint8_t *)arena_alloc(&c->arena, v.len ? v.len : 1);
        if (p && v.len) memcpy(p, v.b, v.len);
        out->data = p;
        if (!p) out->len = 0;
    } else {
        out->ascii = 0;
        uint32_t *p = (uint32_t *)arena_alloc(&c->arena,
                                              (v.len ? v.len : 1) * sizeof(uint32_t));
        if (p && v.len) memcpy(p, v.u, v.len * sizeof(uint32_t));
        out->data = p;
        if (!p) out->len = 0;
    }
}

// Build one item = [ORIGINAL, extra_keys...] and append it. Shared by the
// hook-driven add and the explicit add_keyed. Degrades gracefully on OOM.
static void emit_item(ffz_corpus *c, const char *item, size_t len,
                      const ffz_key *ek, size_t extra) {
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        corpus_item *ni = (corpus_item *)realloc(c->items, ncap * sizeof(corpus_item));
        if (!ni) return;  // OOM: drop this add rather than deref NULL
        c->items = ni;
        c->cap = ncap;
    }
    corpus_item *it = &c->items[c->n];
    dup_key(c, item, len, FFZ_KEY_ORIGINAL, &it->key0);
    it->extra = NULL;
    it->n_extra = 0;
    if (extra > 0) {
        corpus_key *xk = (corpus_key *)malloc(extra * sizeof(corpus_key));
        if (xk) {  // OOM: keep just the ORIGINAL key rather than deref NULL
            for (size_t k = 0; k < extra; k++)
                dup_key(c, ek[k].text, ek[k].len, ek[k].kind, &xk[k]);
            it->extra = xk;
            it->n_extra = (uint32_t)extra;
        }
    }
    c->n++;
}

void ffz_corpus_add(ffz_corpus *c, const char *item, size_t len) {
    // Gather alternate keys from the hook (if any), then build [ORIGINAL, ...].
    size_t extra = 0;
    ffz_key *tmp = NULL;
    if (c->hook && c->max_keys > 0) {
        tmp = (ffz_key *)calloc(c->max_keys, sizeof(ffz_key));
        if (tmp) {
            extra = c->hook(item, len, c->hook_ctx, tmp, c->max_keys);
            if (extra > c->max_keys) extra = c->max_keys;
        }
    }
    emit_item(c, item, len, tmp, extra);
    free(tmp);
}

void ffz_corpus_add_keyed(ffz_corpus *c, const char *item, size_t len,
                          const ffz_key *keys, size_t nkeys) {
    emit_item(c, item, len, keys, nkeys);
}

// --- filtering ------------------------------------------------------------
static int cmp_scored(const void *a, const void *b) {
    const scored *x = (const scored *)a, *y = (const scored *)b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;  // desc
    return (x->item_index > y->item_index) - (x->item_index < y->item_index);
}

// `a` ranks worse than `b` (lower score, or higher index on a tie).
static inline int scored_worse(scored a, scored b) {
    if (a.score != b.score) return a.score < b.score;
    return a.item_index > b.item_index;
}
static void scored_sift(scored *h, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < n && scored_worse(h[l], h[s])) s = l;
        if (r < n && scored_worse(h[r], h[s])) s = r;
        if (s == i) break;
        scored t = h[i]; h[i] = h[s]; h[s] = t;
        i = s;
    }
}
// Select the top-`k` of `sc[0..ns)` into `out` (min-heap on rank, root=worst),
// then sort best-first. O(ns log k) — avoids a full sort when only `k` are kept.
static void scored_topk(const scored *sc, size_t ns, size_t k, scored *out) {
    size_t hn = 0;
    for (size_t i = 0; i < ns; i++) {
        if (hn < k) {
            size_t j = hn++;
            out[j] = sc[i];
            while (j > 0) {
                size_t p = (j - 1) / 2;
                if (scored_worse(out[j], out[p])) {
                    scored t = out[p]; out[p] = out[j]; out[j] = t;
                    j = p;
                } else break;
            }
        } else if (scored_worse(out[0], sc[i])) {
            out[0] = sc[i];
            scored_sift(out, k, 0);
        }
    }
    qsort(out, hn, sizeof(scored), cmp_scored);
}

static void results_push(ffz_results *r, ffz_hit hit) {
    if (r->len == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 32;
        ffz_hit *h = (ffz_hit *)realloc(r->hits, ncap * sizeof(ffz_hit));
        if (!h) { ffz_indices_free(&hit.indices); return; }  // OOM: drop hit
        r->hits = h;
        r->cap = ncap;
    }
    r->hits[r->len++] = hit;
}

void ffz_results_free(ffz_results *r) {
    if (!r->hits) { r->len = r->cap = 0; return; }
    for (size_t i = 0; i < r->len; i++) ffz_indices_free(&r->hits[i].indices);
    free(r->hits);
    r->hits = NULL;
    r->len = r->cap = 0;
}

// A scored collector: either append-all (bounded=false) or keep only the
// best `cap` via a bounded min-heap (root = worst), so a worker that only has
// to surface the global top-`limit` never materializes more than `limit` rows.
typedef struct {
    scored *buf;
    size_t n, cap;
    bool bounded;
} collector;

static inline void coll_push(collector *col, scored s) {
    if (!col->bounded) {
        col->buf[col->n++] = s;
        return;
    }
    if (col->n < col->cap) {  // sift-up into a min-heap on rank
        size_t j = col->n++;
        col->buf[j] = s;
        while (j > 0) {
            size_t p = (j - 1) / 2;
            if (scored_worse(col->buf[j], col->buf[p])) {
                scored t = col->buf[p]; col->buf[p] = col->buf[j]; col->buf[j] = t;
                j = p;
            } else break;
        }
    } else if (col->cap && scored_worse(col->buf[0], s)) {
        col->buf[0] = s;             // displace the current worst
        scored_sift(col->buf, col->cap, 0);
    }
}

// Pass 1 over items [lo,hi): pick each item's best-scoring key and push it to
// `col`. `m` is a matcher private to this caller/thread; `pat` is shared
// read-only.
static void scan_range(ffz_corpus *c, ffz_matcher *m, const ffz_pattern *pat,
                       size_t lo, size_t hi, collector *col) {
    for (size_t i = lo; i < hi; i++) {
        corpus_item *it = &c->items[i];
        int32_t best = -1;
        int best_kind = 0;
        uint32_t best_key = 0;
        size_t nk = item_nkeys(it);
        for (size_t k = 0; k < nk; k++) {
            const corpus_key *key = item_key(it, k);
            int32_t s = ffz_pattern_match(m, pat, key_str(key), NULL);
            if (s > best) {
                best = s;
                best_kind = key->kind;
                best_key = (uint32_t)k;
            }
        }
        if (best >= 0) {
            scored sc = {(uint32_t)i, best, best_kind, best_key};
            coll_push(col, sc);
        }
    }
}

typedef struct {
    ffz_corpus *c;
    const ffz_pattern *pat;
    size_t lo, hi;
    scored *out;   // capacity `cap`
    size_t cap;    // = hi-lo (append-all) or `limit` (bounded top-K)
    bool bounded;
    ffz_scoring_mode scoring;
    size_t n;      // filled by the worker
} scan_job;

static void scan_job_run(scan_job *j) {
    if (!j->out) { j->n = 0; return; }
    // Each thread owns its matcher (the matcher holds mutable scratch).
    // Apply scoring_mode before construction so the matcher is fully configured
    // from the start — avoids a post-init field overwrite.
    ffz_config cfg = j->c->cfg;
    cfg.scoring_mode = j->scoring;
    ffz_matcher *m = ffz_matcher_new(cfg);
    if (m) {
        collector col = {j->out, 0, j->cap, j->bounded};
        scan_range(j->c, m, j->pat, j->lo, j->hi, &col);
        j->n = col.n;
    } else {
        j->n = 0;
    }
    ffz_matcher_free(m);
}

#if defined(_WIN32)
static DWORD WINAPI scan_trampoline(LPVOID p) { scan_job_run((scan_job *)p); return 0; }
static bool thr_start(scan_job *j, ffz_thr *out) {
    HANDLE h = CreateThread(NULL, 0, scan_trampoline, j, 0, NULL);
    if (!h) return false;
    *out = h;
    return true;
}
static void thr_join(ffz_thr t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#elif defined(FFZ_NO_THREADS)
// Never called (resolve_threads()==1); present so the serial path links.
static bool thr_start(scan_job *j, ffz_thr *out) { (void)j; (void)out; return false; }
static void thr_join(ffz_thr t) { (void)t; }
#else
static void *scan_trampoline(void *p) { scan_job_run((scan_job *)p); return NULL; }
static bool thr_start(scan_job *j, ffz_thr *out) {
    return pthread_create(out, NULL, scan_trampoline, j) == 0;
}
static void thr_join(ffz_thr t) { pthread_join(t, NULL); }
#endif

static unsigned resolve_threads(ffz_parallel par, size_t nitems) {
    if (!par.parallel || nitems < FFZ_PARALLEL_MIN) return 1;
    unsigned cpu = ffz_cpu_count();
    unsigned hard_max = cpu > 1 ? cpu - 1 : 1;  // global ceiling: leave 1 core
    unsigned t;
    if (par.threads > 0) {
        t = (unsigned)par.threads;              // explicit may exceed the 8 default
    } else {
        t = cpu / 2;                            // auto default: half the cores...
        if (t > FFZ_AUTO_THREADS_MAX) t = FFZ_AUTO_THREADS_MAX;  // ...capped at 8
    }
    if (t > hard_max) t = hard_max;             // hard cap (cpu-1): can't be broken
    if (t < 1) t = 1;
    if ((size_t)t > nitems) t = (unsigned)nitems;
    return t;
}

ffz_scoring_mode ffz_corpus_scoring(const ffz_corpus *c) {
    return c->cfg.scoring_mode;
}

// Internal: shared implementation for ffz_corpus_filter and ffz_corpus_filter_raws.
// skip_idx=true omits Pass 2 (index computation) — results have empty indices.
static void _corpus_filter_impl(ffz_corpus *c, const char *query, size_t query_len,
                                ffz_case_matching cm, ffz_normalization nm,
                                ffz_mode mode, ffz_parallel par, size_t limit,
                                ffz_scoring_mode scoring, bool skip_idx,
                                ffz_results *out) {
    ffz_results_free(out);
    if ((unsigned)scoring > FFZ_SCORE_NUCLEO) scoring = FFZ_SCORE_FAST;
    out->hits = NULL;
    out->len = out->cap = 0;

    ffz_pattern *pat = (mode == FFZ_FUZZY)
                           ? ffz_pattern_parse(query, query_len, cm, nm)
                           : ffz_pattern_new(query, query_len, cm, nm, mode);
    /* Each call gets its own matcher (mutable DP scratch), so concurrent
     * filter calls on the same corpus are safe. */
    ffz_matcher *fm = ffz_matcher_new(c->cfg);
    if (!pat || !fm) {
        ffz_pattern_free(pat);
        ffz_matcher_free(fm);
        return;
    }
    fm->cfg.scoring_mode = scoring;

    // Pass 1: best key per item (no indices). Optionally multi-threaded.
    // On any allocation/thread failure we degrade gracefully (serial / fewer
    // threads / inline) rather than crash.
    scored *sc = (scored *)malloc((c->n ? c->n : 1) * sizeof(scored));
    if (!sc) {
        ffz_pattern_free(pat);
        ffz_matcher_free(fm);
        return;
    }
    size_t ns = 0;
    unsigned nthreads = resolve_threads(par, c->n);
    if (nthreads <= 1) {
        collector col = {sc, 0, c->n, false};  // serial keeps all then sorts
        scan_range(c, fm, pat, 0, c->n, &col);
        ns = col.n;
    } else {
        size_t chunk = (c->n + nthreads - 1) / nthreads;
        scan_job *jobs = (scan_job *)calloc(nthreads, sizeof(scan_job));
        ffz_thr *ths = (ffz_thr *)calloc(nthreads, sizeof(ffz_thr));
        char *started = (char *)calloc(nthreads, 1);
        if (!jobs || !ths || !started) {
            collector col = {sc, 0, c->n, false};  // serial fallback
            scan_range(c, fm, pat, 0, c->n, &col);
            ns = col.n;
        } else {
            unsigned spawned = 0;
            for (unsigned t = 0; t < nthreads; t++) {
                size_t lo = t * chunk;
                if (lo >= c->n) break;
                size_t hi = lo + chunk < c->n ? lo + chunk : c->n;
                // Per-thread top-K: when a limit caps output below the chunk
                // size, each worker keeps only its best `limit` (a global
                // top-`limit` element is always in some chunk's top-`limit`),
                // so the merged buffer is <= nthreads*limit instead of c->n.
                bool bounded = (limit > 0 && limit < (hi - lo));
                size_t cap = bounded ? limit : (hi - lo);
                scored *obuf = (scored *)malloc(cap * sizeof(scored));
                jobs[t] = (scan_job){c, pat, lo, hi, obuf, cap, bounded, scoring, 0};
                spawned = t + 1;
                if (!obuf) continue;  // OOM: this chunk yields 0 results
                if (thr_start(&jobs[t], &ths[t])) started[t] = 1;
                else scan_job_run(&jobs[t]);  // spawn failed: run inline
            }
            for (unsigned t = 0; t < spawned; t++) {
                if (started[t]) thr_join(ths[t]);
                if (jobs[t].out) {
                    memcpy(sc + ns, jobs[t].out, jobs[t].n * sizeof(scored));
                    ns += jobs[t].n;
                    free(jobs[t].out);
                }
            }
        }
        free(jobs);
        free(ths);
        free(started);
    }

    // OFF mode: results arrived in corpus insertion order (serial) or chunk
    // order (parallel, which also preserves insertion order since chunks are
    // contiguous). Skip sort; just truncate to limit.
    size_t keep;
    if (scoring == FFZ_SCORE_OFF) {
        keep = (limit && limit < ns) ? limit : ns;
    } else {
        // When a limit caps the output well below the match count, select the
        // top-`limit` with a bounded heap instead of sorting everything.
        scored *top = (limit && limit < ns) ? (scored *)malloc(limit * sizeof(scored))
                                            : NULL;
        if (top) {
            scored_topk(sc, ns, limit, top);
            free(sc);
            sc = top;
            keep = limit;
        } else {
            if (ns) qsort(sc, ns, sizeof(scored), cmp_scored);  // NULL/0-safe
            keep = ns;
            top = NULL;
        }
    }

    // Pass 2: recompute indices for survivors on their winning key.
    // Skipped when skip_idx=true (caller only needs item identity, not positions).
    for (size_t r = 0; r < keep; r++) {
        ffz_hit hit;
        hit.item_index = sc[r].item_index;
        hit.score = sc[r].score;
        hit.matched_kind = sc[r].matched_kind;
        hit.matched_key = sc[r].matched_key;
        hit.indices.data = NULL;
        hit.indices.len = hit.indices.cap = 0;
        if (!skip_idx) {
            corpus_item *it = &c->items[sc[r].item_index];
            const corpus_key *key = item_key(it, sc[r].matched_key);
            ffz_pattern_match(fm, pat, key_str(key), &hit.indices);
            ffz_indices_sort_dedup(&hit.indices);
        }
        results_push(out, hit);
    }

    free(sc);
    ffz_matcher_free(fm);
    ffz_pattern_free(pat);
}

void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                       ffz_case_matching cm, ffz_normalization nm,
                       ffz_mode mode, ffz_parallel par, size_t limit,
                       ffz_scoring_mode scoring,
                       ffz_results *out) {
    _corpus_filter_impl(c, query, query_len, cm, nm, mode, par, limit,
                        scoring, false, out);
}

// Like ffz_corpus_filter but skips Pass 2 (no index computation).
// All hit.indices will be empty. Use when only item identity/order is needed.
void ffz_corpus_filter_raws(ffz_corpus *c, const char *query, size_t query_len,
                             ffz_case_matching cm, ffz_normalization nm,
                             ffz_mode mode, ffz_parallel par, size_t limit,
                             ffz_scoring_mode scoring,
                             ffz_results *out) {
    _corpus_filter_impl(c, query, query_len, cm, nm, mode, par, limit,
                        scoring, true, out);
}
