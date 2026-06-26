// C side of the differential test. Reads the same queries.txt / corpus.txt and
// emits the identical line format as the Rust oracle, so the two outputs can be
// compared byte-for-byte.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffz.h"

// Read a whole file into a malloc'd NUL-terminated buffer.
static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(n + 1);
    size_t rd = fread(b, 1, (size_t)n, f);
    b[rd] = 0;
    fclose(f);
    *out_len = rd;
    return b;
}

// Split into lines (handles \n and \r\n); returns count, fills `lines`/`lens`.
static size_t split_lines(char *buf, size_t n, char ***lines, size_t **lens) {
    size_t cap = 64, cnt = 0;
    char **ls = (char **)malloc(cap * sizeof(char *));
    size_t *ll = (size_t *)malloc(cap * sizeof(size_t));
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            size_t end = i;
            if (end > start && buf[end - 1] == '\r') end--;  // strip CR
            if (i == n && end == start) break;  // ignore trailing empty
            if (cnt == cap) {
                cap *= 2;
                ls = (char **)realloc(ls, cap * sizeof(char *));
                ll = (size_t *)realloc(ll, cap * sizeof(size_t));
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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char qpath[1024], cpath[1024];
    snprintf(qpath, sizeof(qpath), "%s/queries.txt", dir);
    snprintf(cpath, sizeof(cpath), "%s/corpus.txt", dir);

    size_t qn, cn;
    char *qbuf = slurp(qpath, &qn);
    char *cbuf = slurp(cpath, &cn);
    char **queries, **haystacks;
    size_t *qlens, *hlens;
    size_t nq = split_lines(qbuf, qn, &queries, &qlens);
    size_t nh = split_lines(cbuf, cn, &haystacks, &hlens);

    ffz_matcher *m = ffz_matcher_new(ffz_config_default());
    ffz_str_buf hb = {0};
    ffz_indices ix = {0};

    for (size_t qi = 0; qi < nq; qi++) {
        ffz_pattern *pat = ffz_pattern_parse(queries[qi], qlens[qi],
                                             FFZ_CASE_SMART, FFZ_NORM_SMART);
        for (size_t hi = 0; hi < nh; hi++) {
            ffz_str hs = ffz_str_from_utf8(haystacks[hi], hlens[hi], &hb);
            ffz_indices_clear(&ix);
            int32_t s = ffz_pattern_match(m, pat, hs, &ix);
            if (s < 0) {
                printf("%zu %zu MISS\n", qi, hi);
            } else {
                ffz_indices_sort_dedup(&ix);
                printf("%zu %zu %d|", qi, hi, s);
                for (size_t k = 0; k < ix.len; k++)
                    printf("%s%u", k ? "," : "", ix.data[k]);
                printf("\n");
            }
        }
        ffz_pattern_free(pat);
    }

    ffz_indices_free(&ix);
    ffz_str_buf_free(&hb);
    ffz_matcher_free(m);
    free(queries); free(qlens); free(haystacks); free(hlens);
    free(qbuf); free(cbuf);
    return 0;
}
