#include "thread_pool.h"
#include "scorer.h"
#include "bitmap.h"
#include "corpus.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Platform-specific CPU count and thread primitives                    */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

static int get_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;

static int thread_create(thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg)
{
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}

static void thread_join(thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else
#  include <pthread.h>
#  ifdef __APPLE__
#    include <sys/sysctl.h>
static int get_cpu_count(void)
{
    int n = 1;
    size_t len = sizeof(n);
    sysctlbyname("hw.logicalcpu", &n, &len, NULL, 0);
    return n > 0 ? n : 1;
}
#  else
#    include <unistd.h>
static int get_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}
#  endif

typedef pthread_t thread_t;

static int thread_create(thread_t *t, void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

static void thread_join(thread_t t)
{
    pthread_join(t, NULL);
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Per-thread work descriptor                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Input */
    ffuzzy_corpus_t *corpus;
    uint32_t         start;       /* first corpus index for this chunk */
    uint32_t         end;         /* one-past-last                     */
    uint32_t        *pat_u32;     /* query as UTF-32                   */
    int              pat_len;
    uint64_t         query_bm;    /* bitmap of query for prefilter     */
    int              ignore_case;

    /* Output */
    ffuzzy_hit_t    *hits;        /* caller-allocated: end-start slots */
    uint32_t         hit_count;
} work_t;

/* ------------------------------------------------------------------ */
/* Worker function (platform-agnostic logic)                            */
/* ------------------------------------------------------------------ */

static void do_work(work_t *w)
{
    ffuzzy_corpus_t *corpus = w->corpus;
    w->hit_count = 0;

    for (uint32_t i = w->start; i < w->end; i++) {
        /* Bitmap prefilter */
        if (!bitmap_could_match(corpus->bitmaps[i], w->query_bm))
            continue;

        int      slen  = corpus->u32lens[i];
        int8_t  *bonus = corpus->bonuses[i];

        /* Compute score without position tracking first */
        int32_t score = scorer_score(
            w->pat_u32, w->pat_len,
            corpus->u32items[i], slen,
            bonus,
            w->ignore_case);

        if (score < 0) continue;

        /* Compute positions */
        uint32_t *pos = (uint32_t *)malloc((size_t)w->pat_len * sizeof(uint32_t));
        if (pos) {
            scorer_score_positions(
                w->pat_u32, w->pat_len,
                corpus->u32items[i], slen,
                bonus,
                w->ignore_case,
                pos);
        }

        ffuzzy_hit_t *h = &w->hits[w->hit_count++];
        h->index       = i;
        h->score       = score;
        h->indices     = pos;
        h->indices_len = pos ? (uint32_t)w->pat_len : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Thread entry points                                                   */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static DWORD WINAPI win32_worker(LPVOID arg)
{
    do_work((work_t *)arg);
    return 0;
}
#else
static void *posix_worker(void *arg)
{
    do_work((work_t *)arg);
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Merge + sort helpers                                                  */
/* ------------------------------------------------------------------ */

static int hit_cmp(const void *a, const void *b)
{
    const ffuzzy_hit_t *ha = (const ffuzzy_hit_t *)a;
    const ffuzzy_hit_t *hb = (const ffuzzy_hit_t *)b;
    /* Descending score */
    if (hb->score > ha->score) return  1;
    if (hb->score < ha->score) return -1;
    /* Ascending index as tiebreaker */
    if (ha->index < hb->index) return -1;
    if (ha->index > hb->index) return  1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: thread_pool_filter                                            */
/* ------------------------------------------------------------------ */

ffuzzy_results_t *thread_pool_filter(ffuzzy_corpus_t *corpus,
                                     const char      *query,
                                     int              ignore_case,
                                     uint32_t         limit)
{
    if (!corpus || !query) return NULL;

    uint32_t total = corpus->len;
    if (total == 0) {
        ffuzzy_results_t *r = (ffuzzy_results_t *)calloc(1, sizeof(ffuzzy_results_t));
        return r;
    }

    /* Decode query to UTF-32 */
    uint32_t *pat_u32 = NULL;
    int       pat_len = 0;
    if (utf8_to_utf32(query, &pat_u32, &pat_len) != 0) {
        free(pat_u32);
        return NULL;
    }

    /* Empty query: return all corpus items with score 0 */
    if (pat_len == 0) {
        free(pat_u32);
        ffuzzy_hit_t *hits = (ffuzzy_hit_t *)calloc(total, sizeof(ffuzzy_hit_t));
        if (!hits) return NULL;
        for (uint32_t i = 0; i < total; i++) {
            hits[i].index       = i;
            hits[i].score       = 0;
            hits[i].indices     = NULL;
            hits[i].indices_len = 0;
        }
        uint32_t result_len = total;
        /* Apply limit */
        /* (limit == 0 means no limit) */
        if (limit > 0 && result_len > limit) result_len = limit;
        ffuzzy_results_t *r = (ffuzzy_results_t *)malloc(sizeof(ffuzzy_results_t));
        if (!r) { free(hits); return NULL; }
        r->hits = hits;
        r->len  = result_len;
        return r;
    }

    uint64_t query_bm = bitmap_from_utf8(query);

    /* Determine thread count */
    int nthreads = get_cpu_count();
    if (nthreads < 1) nthreads = 1;
    if ((uint32_t)nthreads > total) nthreads = (int)total;

    /* Allocate work structs and per-thread hit buffers */
    work_t *works = (work_t *)calloc((size_t)nthreads, sizeof(work_t));
    /* Each thread can match at most its chunk size items */
    uint32_t chunk = (total + (uint32_t)nthreads - 1) / (uint32_t)nthreads;

    if (!works) { free(pat_u32); return NULL; }

    int alloc_ok = 1;
    for (int t = 0; t < nthreads; t++) {
        uint32_t s = (uint32_t)t * chunk;
        uint32_t e = s + chunk;
        if (e > total) e = total;

        works[t].corpus      = corpus;
        works[t].start       = s;
        works[t].end         = e;
        works[t].pat_u32     = pat_u32;
        works[t].pat_len     = pat_len;
        works[t].query_bm    = query_bm;
        works[t].ignore_case = ignore_case;
        works[t].hit_count   = 0;

        uint32_t sz = e - s;
        works[t].hits = (ffuzzy_hit_t *)malloc(sz * sizeof(ffuzzy_hit_t));
        if (!works[t].hits) { alloc_ok = 0; break; }
    }

    if (!alloc_ok) {
        for (int t = 0; t < nthreads; t++) free(works[t].hits);
        free(works);
        free(pat_u32);
        return NULL;
    }

    /* Launch threads */
    if (nthreads == 1) {
        /* Avoid thread overhead for small corpora */
        do_work(&works[0]);
    } else {
        thread_t *threads = (thread_t *)malloc((size_t)nthreads * sizeof(thread_t));
        if (!threads) {
            /* Fall back to single-threaded */
            for (int t = 0; t < nthreads; t++) do_work(&works[t]);
        } else {
            for (int t = 0; t < nthreads; t++) {
#ifdef _WIN32
                thread_create(&threads[t], win32_worker, &works[t]);
#else
                thread_create(&threads[t], posix_worker, &works[t]);
#endif
            }
            for (int t = 0; t < nthreads; t++)
                thread_join(threads[t]);
            free(threads);
        }
    }

    /* Count total hits */
    uint32_t total_hits = 0;
    for (int t = 0; t < nthreads; t++)
        total_hits += works[t].hit_count;

    /* Merge into a single array */
    ffuzzy_hit_t *merged = NULL;
    if (total_hits > 0) {
        merged = (ffuzzy_hit_t *)malloc(total_hits * sizeof(ffuzzy_hit_t));
        if (merged) {
            uint32_t off = 0;
            for (int t = 0; t < nthreads; t++) {
                memcpy(merged + off, works[t].hits,
                       works[t].hit_count * sizeof(ffuzzy_hit_t));
                off += works[t].hit_count;
            }
        }
    }

    /* Free per-thread hit arrays (merged owns the indices pointers now) */
    for (int t = 0; t < nthreads; t++) free(works[t].hits);
    free(works);
    free(pat_u32);

    if (total_hits > 0 && !merged) return NULL;

    /* Sort descending by score */
    if (merged && total_hits > 1)
        qsort(merged, total_hits, sizeof(ffuzzy_hit_t), hit_cmp);

    /* Apply limit */
    uint32_t result_len = total_hits;
    if (limit > 0 && result_len > limit) result_len = limit;

    /* Free positions of truncated hits */
    for (uint32_t i = result_len; i < total_hits; i++)
        free(merged[i].indices);

    ffuzzy_results_t *result = (ffuzzy_results_t *)malloc(sizeof(ffuzzy_results_t));
    if (!result) {
        for (uint32_t i = 0; i < result_len; i++) free(merged[i].indices);
        free(merged);
        return NULL;
    }

    result->hits = merged;
    result->len  = result_len;
    return result;
}
