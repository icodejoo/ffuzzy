// Matcher lifecycle, scratch reservation, and the top-level match dispatch
// (fuzzy / substring / prefix / postfix / exact). Mirrors nucleo `lib.rs`.
#include <stdlib.h>
#include <string.h>

#include "ffz_alloc.h"
#include "ffz_internal.h"

// --- lifecycle ------------------------------------------------------------
ffz_matcher *ffz_matcher_new(ffz_config cfg) {
    ffz_matcher *m = (ffz_matcher *)calloc(1, sizeof(*m));
    if (m) m->cfg = cfg;
    return m;
}

void ffz_matcher_free(ffz_matcher *m) {
    if (!m) return;
    free(m->hay);
    free(m->bonus);
    free(m->mgrid);
    free(m->pmat);
    free(m->roll);
    free(m);
}

ffz_config *ffz_matcher_config(ffz_matcher *m) { return &m->cfg; }

bool ffz_matcher_reserve(ffz_matcher *m, size_t width, size_t needle_len) {
    if (width > m->cap_hay) {
        size_t nc = m->cap_hay ? m->cap_hay : 64;
        while (nc < width) {
            if (nc > (SIZE_MAX >> 1)) return false;  // doubling overflow guard
            nc *= 2;
        }
        // Assign on success before the next realloc so a partial failure never
        // leaves a dangling (moved-and-freed) pointer in the struct.
        uint32_t *h = (uint32_t *)realloc(m->hay, nc * sizeof(uint32_t));
        if (!h) return false;
        m->hay = h;
        uint8_t *b = (uint8_t *)realloc(m->bonus, nc);
        if (!b) return false;
        m->bonus = b;
        uint16_t *rl = (uint16_t *)realloc(m->roll, nc * 2 * sizeof(uint16_t));
        if (!rl) return false;
        m->roll = rl;
        m->cap_hay = nc;
    }
    size_t need = width * needle_len;  // bounded < FFZ_MAX_MATRIX_SIZE by caller
    if (need > m->cap_grid) {
        size_t nc = m->cap_grid ? m->cap_grid : 256;
        while (nc < need) {
            if (nc > (SIZE_MAX >> 1)) return false;
            nc *= 2;
        }
        ffz_mcell *g = (ffz_mcell *)realloc(m->mgrid, nc * sizeof(ffz_mcell));
        if (!g) return false;
        m->mgrid = g;
        uint8_t *pm = (uint8_t *)realloc(m->pmat, nc);
        if (!pm) return false;
        m->pmat = pm;
        m->cap_grid = nc;
    }
    return true;
}

// --- whitespace trimming helpers -----------------------------------------
// Rust u8::is_ascii_whitespace: space, \t(09), \n(0a), \f(0c), \r(0d). No \v.
static inline bool is_ascii_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}
static bool cp_is_ws(uint32_t c, const ffz_config *cfg) {
    return ffz_char_class_of(c, cfg) == FFZ_CLASS_WHITESPACE;
}
// Is code unit `i` of `s` whitespace? Direct byte test for ASCII strings.
static inline bool ffz_str_ws(ffz_str s, size_t i, const ffz_config *cfg) {
    return s.b ? is_ascii_ws(s.b[i]) : cp_is_ws(s.u[i], cfg);
}
static size_t leading_ws(ffz_str h, const ffz_config *cfg) {
    size_t i = 0;
    if (h.b) {  // ASCII fast path: skip the char_class table indirection
        while (i < h.len && is_ascii_ws(h.b[i])) i++;
        return i;
    }
    while (i < h.len && cp_is_ws(h.u[i], cfg)) i++;
    return i;
}
static size_t trailing_ws(ffz_str h, const ffz_config *cfg) {
    size_t i = 0;
    if (h.b) {
        while (i < h.len && is_ascii_ws(h.b[h.len - 1 - i])) i++;
        return i;
    }
    while (i < h.len && cp_is_ws(h.u[h.len - 1 - i], cfg)) i++;
    return i;
}

// --- exact comparison over a window --------------------------------------
static int32_t exact_impl(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t end, ffz_indices *out) {
    size_t nl = needle.len;
    if (nl != end - start) return -1;
    const ffz_config *cfg = &m->cfg;
    if (hay.b && needle.b) {  // ASCII fast path: byte compare with folding
        bool ic = cfg->ignore_case;
        for (size_t i = 0; i < nl; i++) {
            uint8_t hc = hay.b[start + i];
            if (ic && hc >= 'A' && hc <= 'Z') hc += 32;
            if (hc != needle.b[i]) return -1;
        }
    } else {
        for (size_t i = 0; i < nl; i++)
            if (ffz_normalize_cp(ffz_at(hay, start + i), cfg) != ffz_at(needle, i))
                return -1;
    }
    return (int32_t)ffz_calculate_score(m, hay, needle, start, end, out);
}

// --- substring (incl. single char) ---------------------------------------
// ASCII substring: SWAR/memchr the first byte, verify, keep best-bonus match.
static long substring_best_ascii(const ffz_config *cfg, const uint8_t *h,
                                 size_t hn, const uint8_t *nd, size_t nl) {
    bool ic = cfg->ignore_case;
    long best = -1;
    uint16_t best_score = 0;
    size_t pos = 0;
    while (pos + nl <= hn) {
        size_t off = ffz_find_ci(h + pos, hn - nl + 1 - pos, nd[0], ic);
        if (off == FFZ_NF) break;
        size_t start = pos + off;
        bool ok = true;
        for (size_t k = 1; k < nl; k++) {
            uint8_t hc = h[start + k];
            if (ic && hc >= 'A' && hc <= 'Z') hc += 32;
            if (hc != nd[k]) { ok = false; break; }
        }
        if (ok) {
            ffz_char_class pc = start > 0 ? ffz_char_class_of(h[start - 1], cfg)
                                          : cfg->initial_char_class;
            ffz_char_class cc = ffz_char_class_of(h[start], cfg);
            uint16_t bonus = ffz_bonus_for(cfg, pc, cc);
            uint16_t score = (uint16_t)(bonus * FFZ_BONUS_FIRST_CHAR_MULTIPLIER +
                                        FFZ_SCORE_MATCH);
            if (score > best_score) {
                best_score = score;
                best = (long)start;
                if (bonus >= cfg->bonus_boundary_white) break;
            }
        }
        pos = start + 1;
    }
    return best;
}

// Unicode substring (scalar). nucleo's non-ASCII substring has a tail
// off-by-one; reproduce it under bugcompat (haystack here is always non-ASCII).
static long substring_best_uni(const ffz_config *cfg, const uint32_t *h,
                               size_t hn, ffz_str needle, size_t nl) {
    long best = -1;
    uint16_t best_score = 0;
    size_t last_start = hn - nl;
#ifdef FFZ_NUCLEO_SUBSTRING_BUGCOMPAT
    if (nl > 1 && last_start > 0) last_start--;
#endif
    for (size_t i = 0; i <= last_start; i++) {
        uint32_t c0;
        ffz_char_class cls = ffz_class_and_normalize(h[i], cfg, &c0);
        if (c0 != ffz_at(needle, 0)) continue;
        bool ok = true;
        for (size_t k = 1; k < nl; k++)
            if (ffz_normalize_cp(h[i + k], cfg) != ffz_at(needle, k)) { ok = false; break; }
        if (!ok) continue;
        ffz_char_class prev = i > 0 ? ffz_char_class_of(h[i - 1], cfg)
                                    : cfg->initial_char_class;
        uint16_t bonus = ffz_bonus_for(cfg, prev, cls);
        uint16_t score = (uint16_t)(bonus * FFZ_BONUS_FIRST_CHAR_MULTIPLIER +
                                    FFZ_SCORE_MATCH);
        if (score > best_score) {
            best_score = score;
            best = (long)i;
            if (bonus >= cfg->bonus_boundary_white) break;
        }
    }
    return best;
}

static long substring_best(ffz_matcher *m, ffz_str hay, ffz_str needle) {
    const ffz_config *cfg = &m->cfg;
    size_t nl = needle.len, hn = hay.len;
    if (nl > hn) return -1;
    if (hay.b) {
        if (!needle.b) return -1;  // ASCII haystack can't contain a Unicode needle
        return substring_best_ascii(cfg, hay.b, hn, needle.b, nl);
    }
    return substring_best_uni(cfg, hay.u, hn, needle, nl);
}

static int32_t substring_match(ffz_matcher *m, ffz_str hay, ffz_str needle,
                               ffz_indices *out) {
    size_t nl = needle.len, hn = hay.len;
    if (nl == 0) return 0;
    if (nl > hn) return -1;
    if (nl == hn) return exact_impl(m, hay, needle, 0, hn, out);
    long pos = substring_best(m, hay, needle);
    if (pos < 0) return -1;
    return (int32_t)ffz_calculate_score(m, hay, needle, (size_t)pos,
                                        (size_t)pos + nl, out);
}

// --- top-level dispatch ---------------------------------------------------
static int32_t ffz_match_impl(ffz_matcher *m, ffz_str haystack, ffz_str needle,
                              ffz_mode mode, ffz_indices *out) {
    const ffz_config *cfg = &m->cfg;
    size_t hn = haystack.len, nl = needle.len;

    switch (mode) {
        case FFZ_EXACT: {
            size_t lead = ffz_str_ws(needle, 0, cfg) ? 0 : leading_ws(haystack, cfg);
            size_t trail =
                ffz_str_ws(needle, nl - 1, cfg) ? 0 : trailing_ws(haystack, cfg);
            if (trail == hn) return -1;
            return exact_impl(m, haystack, needle, lead, hn - trail, out);
        }
        case FFZ_PREFIX: {
            size_t lead = ffz_str_ws(needle, 0, cfg) ? 0 : leading_ws(haystack, cfg);
            if (hn - lead < nl) return -1;
            return exact_impl(m, haystack, needle, lead, nl + lead, out);
        }
        case FFZ_POSTFIX: {
            size_t trail =
                ffz_str_ws(needle, nl - 1, cfg) ? 0 : trailing_ws(haystack, cfg);
            if (hn - trail < nl) return -1;
            return exact_impl(m, haystack, needle, hn - nl - trail, hn - trail, out);
        }
        case FFZ_SUBSTRING:
            return substring_match(m, haystack, needle, out);

        case FFZ_FUZZY:
        default: {
            if (nl == hn)
                return exact_impl(m, haystack, needle, 0, hn, out);
            if (nl == 1) {
                long pos = substring_best(m, haystack, needle);
                if (pos < 0) return -1;
                return (int32_t)ffz_calculate_score(m, haystack, needle,
                                                    (size_t)pos, (size_t)pos + 1, out);
            }
            size_t start, greedy_end, end;
            if (!ffz_prefilter(cfg, haystack, needle, false, &start, &greedy_end,
                               &end))
                return -1;
            if (nl == end - start)
                return (int32_t)ffz_calculate_score(m, haystack, needle, start,
                                                    start + nl, out);
            switch (cfg->scoring_mode) {
                case FFZ_SCORE_OFF:
                    if (out)
                        ffz_calculate_score(m, haystack, needle, start,
                                            greedy_end, out);
                    return 0;
                case FFZ_SCORE_FAST:
                    if (!out)
                        return ffz_fuzzy_rolling(m, haystack, needle, start, end);
                    return ffz_fuzzy_greedy(m, haystack, needle, start, greedy_end,
                                           out);
                default: /* FFZ_SCORE_NUCLEO */
                    return ffz_fuzzy_optimal(m, haystack, needle, start, greedy_end,
                                            end, out);
            }
        }
    }
}

int32_t ffz_match(ffz_matcher *m, ffz_str haystack, ffz_str needle,
                  ffz_mode mode, ffz_indices *out) {
    const ffz_config *cfg = &m->cfg;
    size_t hn = haystack.len, nl = needle.len;

    if (nl == 0) return 0;
    // ASCII haystack vs Unicode needle: no byte can equal a non-ASCII codepoint.
    if (haystack.b && !needle.b) return -1;
    // Needle longer than the haystack can never match in any mode.
    if (nl > hn) return -1;

    int32_t s = ffz_match_impl(m, haystack, needle, mode, out);

    // OFF mode: suppress non-zero score (FUZZY OFF already returns 0 directly).
    if (s > 0 && cfg->scoring_mode == FFZ_SCORE_OFF) return 0;
    return s;
}
