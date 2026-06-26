// C benchmark: two-pass parallel filter (score-pass -> sort -> top-K indices)
// over the ffz engine. Mirrors the Rust bench exactly so only the engine
// differs. Usage:
//   perf_c <dataset> <queryfile> <fuzzy|prefix|substring|word> \
//          <threads> <index 0|1> <withIndices 0|1>
// Prints one CSV line: c,<mode>,<threads>,<index>,<n>,<nq>,<ms/filter>,<Mitems/s>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffz.h"

#define TOPK 50
#define MIN_SECONDS 0.4

// --- timer ----------------------------------------------------------------
#if defined(_WIN32)
#include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#include <process.h>
typedef HANDLE bthr_t;
#else
#include <pthread.h>
#include <time.h>
static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
typedef pthread_t bthr_t;
#endif

// --- file loading ---------------------------------------------------------
static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    size_t rd = fread(b, 1, (size_t)n, f);
    b[rd] = 0;
    fclose(f);
    *out_len = rd;
    return b;
}
static size_t split_lines(char *buf, size_t n, char ***lines, size_t **lens) {
    size_t cap = 64, cnt = 0, start = 0;
    char **ls = malloc(cap * sizeof(char *));
    size_t *ll = malloc(cap * sizeof(size_t));
    for (size_t i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            size_t end = i;
            if (end > start && buf[end - 1] == '\r') end--;
            if (i == n && end == start) break;
            if (cnt == cap) {
                cap *= 2;
                ls = realloc(ls, cap * sizeof(char *));
                ll = realloc(ll, cap * sizeof(size_t));
            }
            ls[cnt] = buf + start;
            ll[cnt] = end - start;
            cnt++;
            start = i + 1;
        }
    }
    *lines = ls;
    *lens = ll;
    return cnt;
}

// --- shared bench context -------------------------------------------------
typedef struct {
    int index_on;
    ffz_mode mode;
    ffz_str *idx;   // index on: owned per-item ffz_str (bytes or codepoints)
    char **raw;     // index off: raw utf-8
    size_t *rlen;
    size_t n;
    const ffz_pattern *pat;  // per-filter, read-only across threads
} bench;

typedef struct {
    uint32_t idx;
    int32_t score;
} bscored;

// Score items [lo,hi) into `out`; `m`/`buf` are private to this caller/thread.
static size_t bscan_range(const bench *b, ffz_matcher *m, ffz_str_buf *buf,
                         size_t lo, size_t hi, bscored *out) {
    size_t k = 0;
    for (size_t i = lo; i < hi; i++) {
        ffz_str hay = b->index_on ? b->idx[i]
                                  : ffz_str_from_utf8(b->raw[i], b->rlen[i], buf);
        int32_t s = ffz_pattern_match(m, b->pat, hay, NULL);
        if (s >= 0) {
            out[k].idx = (uint32_t)i;
            out[k].score = s;
            k++;
        }
    }
    return k;
}

typedef struct {
    const bench *b;
    size_t lo, hi;
    bscored *out;
    size_t n;
} job;

static void job_run(job *j) {
    ffz_matcher *m = ffz_matcher_new(ffz_config_default());
    ffz_str_buf buf = {0};
    j->n = bscan_range(j->b, m, &buf, j->lo, j->hi, j->out);
    ffz_str_buf_free(&buf);
    ffz_matcher_free(m);
}
#if defined(_WIN32)
static DWORD WINAPI tramp(LPVOID p) { job_run((job *)p); return 0; }
static bthr_t bthr_start(job *j) { return CreateThread(NULL, 0, tramp, j, 0, NULL); }
static void bthr_join(bthr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static void *tramp(void *p) { job_run((job *)p); return NULL; }
static bthr_t bthr_start(job *j) { pthread_t t; pthread_create(&t, NULL, tramp, j); return t; }
static void bthr_join(bthr_t t) { pthread_join(t, NULL); }
#endif

static int bcmp_scored(const void *a, const void *b) {
    const bscored *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

// `a` ranks worse than `b` (lower score, or higher index on a tie).
static inline int worse(bscored a, bscored b) {
    if (a.score != b.score) return a.score < b.score;
    return a.idx > b.idx;
}
static void sift_down(bscored *h, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < n && worse(h[l], h[s])) s = l;
        if (r < n && worse(h[r], h[s])) s = r;
        if (s == i) break;
        bscored t = h[i]; h[i] = h[s]; h[s] = t;
        i = s;
    }
}
// Select the top-`k` of `sc[0..ns)` into `heap` (min-heap on rank, root=worst),
// then sort them best-first. O(ns log k) with inlined compares — far cheaper
// than a full qsort when ns >> k. Returns the count written (<= k).
static size_t top_k(const bscored *sc, size_t ns, size_t k, bscored *heap) {
    size_t hn = 0;
    for (size_t i = 0; i < ns; i++) {
        if (hn < k) {
            size_t j = hn++;
            heap[j] = sc[i];
            while (j > 0) {  // sift up: worst bubbles toward the root
                size_t p = (j - 1) / 2;
                if (worse(heap[j], heap[p])) {
                    bscored t = heap[p]; heap[p] = heap[j]; heap[j] = t;
                    j = p;
                } else break;
            }
        } else if (worse(heap[0], sc[i])) {  // sc[i] beats the current worst
            heap[0] = sc[i];
            sift_down(heap, k, 0);
        }
    }
    qsort(heap, hn, sizeof(bscored), bcmp_scored);
    return hn;
}

// One filter() call. Returns a sink value to defeat dead-code elimination.
static long filter(bench *b, const char *q, size_t qlen, int nthreads,
                   int with_indices, bscored *sc, ffz_matcher *mmain,
                   ffz_str_buf *bufmain, ffz_indices *ix) {
    ffz_pattern *pat = (b->mode == FFZ_FUZZY)
                           ? ffz_pattern_parse(q, qlen, FFZ_CASE_SMART, FFZ_NORM_SMART)
                           : ffz_pattern_new(q, qlen, FFZ_CASE_SMART,
                                             FFZ_NORM_SMART, b->mode);
    b->pat = pat;
    size_t ns = 0;
    if (nthreads <= 1) {
        ns = bscan_range(b, mmain, bufmain, 0, b->n, sc);
    } else {
        size_t chunk = (b->n + nthreads - 1) / nthreads;
        job *jobs = calloc(nthreads, sizeof(job));
        bthr_t *ths = calloc(nthreads, sizeof(bthr_t));
        int spawned = 0;
        for (int t = 0; t < nthreads; t++) {
            size_t lo = (size_t)t * chunk;
            if (lo >= b->n) break;
            size_t hi = lo + chunk < b->n ? lo + chunk : b->n;
            jobs[t] = (job){b, lo, hi, sc + lo, 0};
            ths[t] = bthr_start(&jobs[t]);
            spawned++;
        }
        // Compact thread outputs (each wrote into sc[lo..lo+n_t)).
        for (int t = 0; t < spawned; t++) {
            bthr_join(ths[t]);
            if (jobs[t].out != sc + ns)
                memmove(sc + ns, jobs[t].out, jobs[t].n * sizeof(bscored));
            ns += jobs[t].n;
        }
        free(jobs);
        free(ths);
    }
    bscored top[TOPK];
    size_t keep = top_k(sc, ns, TOPK, top);
    long sink = (long)ns;
    if (with_indices) {
        for (size_t r = 0; r < keep; r++) {
            uint32_t i = top[r].idx;
            ffz_str hay = b->index_on ? b->idx[i]
                            : ffz_str_from_utf8(b->raw[i], b->rlen[i], bufmain);
            ffz_indices_clear(ix);
            ffz_pattern_match(mmain, pat, hay, ix);
            sink += (long)ix->len;
        }
    }
    ffz_pattern_free(pat);
    return sink;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s data queries mode threads index withIndices\n",
                argv[0]);
        return 2;
    }
    const char *dataf = argv[1], *queryf = argv[2], *modes = argv[3];
    int nthreads = atoi(argv[4]);
    int index_on = atoi(argv[5]);
    int with_indices = atoi(argv[6]);

    ffz_mode mode = FFZ_FUZZY;
    if (!strcmp(modes, "prefix")) mode = FFZ_PREFIX;
    else if (!strcmp(modes, "substring")) mode = FFZ_SUBSTRING;
    else if (!strcmp(modes, "word")) mode = FFZ_EXACT;

    size_t dn, qn;
    char *dbuf = slurp(dataf, &dn), *qbuf = slurp(queryf, &qn);
    char **items, **queries;
    size_t *ilen, *qlen;
    size_t n = split_lines(dbuf, dn, &items, &ilen);
    size_t nq = split_lines(qbuf, qn, &queries, &qlen);

    bench b = {0};
    b.index_on = index_on;
    b.mode = mode;
    b.n = n;
    if (index_on) {
        b.idx = malloc(n * sizeof(ffz_str));
        ffz_str_buf tmp = {0};
        for (size_t i = 0; i < n; i++) {
            ffz_str s = ffz_str_from_utf8(items[i], ilen[i], &tmp);
            if (s.b) {  // ASCII: own a byte copy
                uint8_t *p = malloc(s.len ? s.len : 1);
                memcpy(p, s.b, s.len);
                b.idx[i] = (ffz_str){p, NULL, s.len};
            } else {    // Unicode: own a codepoint copy
                uint32_t *p = malloc((s.len ? s.len : 1) * sizeof(uint32_t));
                memcpy(p, s.u, s.len * sizeof(uint32_t));
                b.idx[i] = (ffz_str){NULL, p, s.len};
            }
        }
        ffz_str_buf_free(&tmp);
    } else {
        b.raw = items;
        b.rlen = ilen;
    }

    bscored *sc = malloc((n ? n : 1) * sizeof(bscored));
    ffz_matcher *mmain = ffz_matcher_new(ffz_config_default());
    ffz_str_buf bufmain = {0};
    ffz_indices ix = {0};
    volatile long sink = 0;

    // warmup
    for (size_t q = 0; q < nq; q++)
        sink += filter(&b, queries[q], qlen[q], nthreads, with_indices, sc,
                       mmain, &bufmain, &ix);

    double t0 = now_sec();
    long filters = 0;
    double elapsed;
    do {
        for (size_t q = 0; q < nq; q++)
            sink += filter(&b, queries[q], qlen[q], nthreads, with_indices, sc,
                           mmain, &bufmain, &ix);
        filters += (long)nq;
        elapsed = now_sec() - t0;
    } while (elapsed < MIN_SECONDS);

    double ms_per_filter = elapsed * 1000.0 / filters;
    double mitems_s = (double)n * filters / elapsed / 1e6;
    printf("c,%s,%d,%d,%zu,%zu,%.3f,%.1f\n", modes, nthreads, index_on, n, nq,
           ms_per_filter, mitems_s);
    fprintf(stderr, "sink=%ld\n", sink);
    return 0;
}
