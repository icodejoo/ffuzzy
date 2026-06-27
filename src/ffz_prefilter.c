// Subsequence prefilter: computes (start, greedy_end, end) window bounds.
// ASCII haystacks use SIMD libc memchr (fast rejection + scan); Unicode uses a
// scalar codepoint scan. Port of nucleo prefilter_ascii / prefilter_non_ascii.
#include "ffz_internal.h"

#define NF FFZ_NF

// --- ASCII (byte) path ----------------------------------------------------
static bool prefilter_ascii(const ffz_config *cfg, const uint8_t *h, size_t hn,
                            const uint8_t *nd, size_t nl, bool only_greedy,
                            size_t *start, size_t *greedy_end, size_t *end) {
    bool ic = cfg->ignore_case;
    size_t s = ffz_find_ci(h, hn - nl + 1, nd[0], ic);
    if (s == NF) return false;
    size_t ge = s + 1;
    for (size_t k = 1; k < nl; k++) {
        size_t idx = ffz_find_ci(h + ge, hn - ge, nd[k], ic);
        if (idx == NF) return false;
        ge += idx + 1;
    }
    *start = s;
    *greedy_end = ge;
    if (only_greedy) {
        *end = ge;
        return true;
    }
    // end = (last occurrence of last needle char in the tail) + 1.
    // SIMD reverse scan: 16 bytes/iter on SSE2/NEON.
    uint8_t last = nd[nl - 1];
    size_t tail = hn - ge;
    size_t ri = ffz_rfind_ci(h + ge, tail, last, ic);
    *end = (ri != NF) ? ge + ri + 1 : ge;
    return true;
}

// --- Unicode (codepoint) path ---------------------------------------------
static size_t find_norm(const ffz_config *cfg, const uint32_t *hay, size_t from,
                        size_t to, uint32_t target) {
    for (size_t i = from; i < to; i++)
        if (ffz_normalize_cp(hay[i], cfg) == target) return i;
    return NF;
}

bool ffz_prefilter(const ffz_config *cfg, ffz_str hay, ffz_str needle,
                   bool only_greedy, size_t *start, size_t *greedy_end,
                   size_t *end) {
    size_t nl = needle.len, hn = hay.len;
    if (nl == 0 || nl > hn) return false;

    if (hay.b) {
        // ASCII haystack. A non-ASCII needle can never match -> caller handles
        // (needle.b is set for ASCII needles); here needle is ASCII bytes.
        return prefilter_ascii(cfg, hay.b, hn, needle.b, nl, only_greedy, start,
                               greedy_end, end);
    }

    const uint32_t *h = hay.u;
    uint32_t n0 = ffz_at(needle, 0);
    size_t s = find_norm(cfg, h, 0, hn - nl + 1, n0);
    if (s == NF) return false;
    size_t ge = s + 1;
    for (size_t k = 1; k < nl; k++) {
        size_t j = find_norm(cfg, h, ge, hn, ffz_at(needle, k));
        if (j == NF) return false;
        ge = j + 1;
    }
    *start = s;
    *greedy_end = ge;
    if (only_greedy) {
        *end = ge;
        return true;
    }
    uint32_t last = ffz_at(needle, nl - 1);
    size_t e = ge;
    for (size_t i = hn; i > ge; i--)
        if (ffz_normalize_cp(h[i - 1], cfg) == last) { e = i; break; }
    *end = e;
    return true;
}
