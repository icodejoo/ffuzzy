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
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t ffz_thr;
static unsigned ffz_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
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

struct ffz_corpus {
    ffz_config cfg;
    ffz_matcher *m;
    corpus_item *items;
    size_t n, cap;
    ffz_transliterator hook;
    void *hook_ctx;
    size_t max_keys;
    ffz_str_buf scratch;
};

ffz_corpus *ffz_corpus_new(ffz_config cfg) {
    ffz_corpus *c = (ffz_corpus *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = cfg;
    c->m = ffz_matcher_new(cfg);
    if (!c->m) { free(c); return NULL; }
    return c;
}

static void free_item(corpus_item *it) {
    free(it->key0.data);
    for (size_t k = 0; k < it->n_extra; k++) free(it->extra[k].data);
    free(it->extra);
}

void ffz_corpus_clear(ffz_corpus *c) {
    for (size_t i = 0; i < c->n; i++) free_item(&c->items[i]);
    c->n = 0;
}

void ffz_corpus_free(ffz_corpus *c) {
    if (!c) return;
    ffz_corpus_clear(c);
    free(c->items);
    ffz_matcher_free(c->m);
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

// Decode UTF-8 and store an owned copy in the key (bytes if ASCII, else cps).
static void dup_key(ffz_corpus *c, const char *s, size_t n, int kind,
                    corpus_key *out) {
    ffz_str v = ffz_str_from_utf8(s, n, &c->scratch);
    out->kind = (uint16_t)kind;
    out->len = (uint32_t)v.len;
    if (v.b) {
        out->ascii = 1;
        uint8_t *p = (uint8_t *)malloc(v.len ? v.len : 1);
        memcpy(p, v.b, v.len);
        out->data = p;
    } else {
        out->ascii = 0;
        uint32_t *p = (uint32_t *)malloc((v.len ? v.len : 1) * sizeof(uint32_t));
        memcpy(p, v.u, v.len * sizeof(uint32_t));
        out->data = p;
    }
}

void ffz_corpus_add(ffz_corpus *c, const char *item, size_t len) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->items = (corpus_item *)realloc(c->items, c->cap * sizeof(corpus_item));
    }
    corpus_item *it = &c->items[c->n];

    // Gather alternate keys from the hook (if any), then build [ORIGINAL, ...].
    size_t extra = 0;
    ffz_key *tmp = NULL;
    if (c->hook && c->max_keys > 0) {
        tmp = (ffz_key *)calloc(c->max_keys, sizeof(ffz_key));
        extra = c->hook(item, len, c->hook_ctx, tmp, c->max_keys);
        if (extra > c->max_keys) extra = c->max_keys;
    }

    dup_key(c, item, len, FFZ_KEY_ORIGINAL, &it->key0);
    if (extra > 0) {
        it->extra = (corpus_key *)malloc(extra * sizeof(corpus_key));
        for (size_t k = 0; k < extra; k++)
            dup_key(c, tmp[k].text, tmp[k].len, tmp[k].kind, &it->extra[k]);
        it->n_extra = (uint32_t)extra;
    } else {
        it->extra = NULL;
        it->n_extra = 0;
    }
    free(tmp);
    c->n++;
}

// --- filtering ------------------------------------------------------------
typedef struct {
    uint32_t item_index;
    int32_t score;
    int matched_kind;
    uint32_t matched_key;
} scored;

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
        r->cap = r->cap ? r->cap * 2 : 32;
        r->hits = (ffz_hit *)realloc(r->hits, r->cap * sizeof(ffz_hit));
    }
    r->hits[r->len++] = hit;
}

void ffz_results_free(ffz_results *r) {
    for (size_t i = 0; i < r->len; i++) ffz_indices_free(&r->hits[i].indices);
    free(r->hits);
    r->hits = NULL;
    r->len = r->cap = 0;
}

// Pass 1 over items [lo,hi): pick each item's best-scoring key. `m` is a
// matcher private to this caller/thread; `pat` is shared read-only. Writes
// compactly into `out` and returns the number of matched items.
static size_t scan_range(ffz_corpus *c, ffz_matcher *m, const ffz_pattern *pat,
                         size_t lo, size_t hi, scored *out) {
    size_t n = 0;
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
            out[n].item_index = (uint32_t)i;
            out[n].score = best;
            out[n].matched_kind = best_kind;
            out[n].matched_key = best_key;
            n++;
        }
    }
    return n;
}

typedef struct {
    ffz_corpus *c;
    const ffz_pattern *pat;
    size_t lo, hi;
    scored *out;  // capacity hi-lo
    size_t n;     // filled by the worker
} scan_job;

static void scan_job_run(scan_job *j) {
    // Each thread owns its matcher (the matcher holds mutable scratch).
    ffz_matcher *m = ffz_matcher_new(j->c->cfg);
    j->n = m ? scan_range(j->c, m, j->pat, j->lo, j->hi, j->out) : 0;
    ffz_matcher_free(m);
}

#if defined(_WIN32)
static DWORD WINAPI scan_trampoline(LPVOID p) { scan_job_run((scan_job *)p); return 0; }
static ffz_thr thr_start(scan_job *j) {
    return CreateThread(NULL, 0, scan_trampoline, j, 0, NULL);
}
static void thr_join(ffz_thr t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static void *scan_trampoline(void *p) { scan_job_run((scan_job *)p); return NULL; }
static ffz_thr thr_start(scan_job *j) {
    pthread_t t;
    pthread_create(&t, NULL, scan_trampoline, j);
    return t;
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

void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                       ffz_case_matching cm, ffz_normalization nm,
                       ffz_mode mode, ffz_parallel par, size_t limit,
                       ffz_results *out) {
    out->hits = NULL;
    out->len = out->cap = 0;

    ffz_pattern *pat = (mode == FFZ_FUZZY)
                           ? ffz_pattern_parse(query, query_len, cm, nm)
                           : ffz_pattern_new(query, query_len, cm, nm, mode);

    // Pass 1: best key per item (no indices). Optionally multi-threaded.
    scored *sc = (scored *)malloc((c->n ? c->n : 1) * sizeof(scored));
    size_t ns = 0;
    unsigned nthreads = resolve_threads(par, c->n);
    if (nthreads <= 1) {
        ns = scan_range(c, c->m, pat, 0, c->n, sc);
    } else {
        size_t chunk = (c->n + nthreads - 1) / nthreads;
        scan_job *jobs = (scan_job *)calloc(nthreads, sizeof(scan_job));
        ffz_thr *ths = (ffz_thr *)calloc(nthreads, sizeof(ffz_thr));
        unsigned spawned = 0;
        for (unsigned t = 0; t < nthreads; t++) {
            size_t lo = t * chunk;
            if (lo >= c->n) break;
            size_t hi = lo + chunk < c->n ? lo + chunk : c->n;
            jobs[t] = (scan_job){c, pat, lo, hi,
                                 (scored *)malloc((hi - lo) * sizeof(scored)), 0};
            ths[t] = thr_start(&jobs[t]);
            spawned++;
        }
        // Concatenate in ascending range order -> same order as the serial scan.
        for (unsigned t = 0; t < spawned; t++) {
            thr_join(ths[t]);
            memcpy(sc + ns, jobs[t].out, jobs[t].n * sizeof(scored));
            ns += jobs[t].n;
            free(jobs[t].out);
        }
        free(jobs);
        free(ths);
    }

    // When a limit caps the output well below the match count, select the
    // top-`limit` with a bounded heap instead of sorting everything.
    size_t keep;
    if (limit && limit < ns) {
        scored *top = (scored *)malloc(limit * sizeof(scored));
        scored_topk(sc, ns, limit, top);
        free(sc);
        sc = top;
        keep = limit;
    } else {
        qsort(sc, ns, sizeof(scored), cmp_scored);
        keep = ns;
    }

    // Pass 2: recompute indices only for the survivors, on their winning key.
    for (size_t r = 0; r < keep; r++) {
        ffz_hit hit;
        hit.item_index = sc[r].item_index;
        hit.score = sc[r].score;
        hit.matched_kind = sc[r].matched_kind;
        hit.matched_key = sc[r].matched_key;
        hit.indices.data = NULL;
        hit.indices.len = hit.indices.cap = 0;
        corpus_item *it = &c->items[sc[r].item_index];
        const corpus_key *key = item_key(it, sc[r].matched_key);
        ffz_pattern_match(c->m, pat, key_str(key), &hit.indices);
        ffz_indices_sort_dedup(&hit.indices);
        results_push(out, hit);
    }

    free(sc);
    ffz_pattern_free(pat);
}
