// Fuzzy (subsequence) matching: an explicit two-track dynamic program plus a
// linear greedy fallback for oversized inputs.
//
// This is NOT a line-by-line port of nucleo's space-optimized rolling matrix;
// it is a semantically equivalent, readable DP using the SAME scoring model
// (score.rs constants, next_m_cell / p_score recurrences). Functional &
// performance parity, not byte-identical internals. See README "DP design".
//
//   M[k][i]  best score with needle[k] matched AT haystack column i
//   P[k][i]  best score with needle[0..k] matched, sitting in a gap, ready to
//            match needle[k] at column >= i   (the "skip" track)
//
//   P[k][i] = max( M[k-1][i-1] - GAP_START,  P[k][i-1] - GAP_EXTENSION )
//   M[k][i] = next_m_cell( P[k][i], bonus[i], M[k-1][i-1] )   when h[i]==nd[k]
#include <stdlib.h>
#include <string.h>

#include "ffz_internal.h"


// --- greedy fallback (linear) --------------------------------------------
// [start,end) is a window in which `needle` is already a subsequence (the
// prefilter guarantees it). Minimize the window from the left, then score it.
int32_t ffz_fuzzy_greedy(ffz_matcher *m, ffz_str hay, ffz_str needle,
                         size_t start, size_t end, ffz_indices *out) {
    const ffz_config *cfg = &m->cfg;
    if (needle.len == 0) return 0;
    size_t ni = needle.len;           // walk needle in reverse
    uint32_t nc = ffz_at(needle, ni - 1);
    size_t new_start = start;
    for (size_t i = end; i > start; i--) {
        if (ffz_normalize_cp(ffz_at(hay, i - 1), cfg) == nc) {
            if (ni == 1) { new_start = i - 1; break; }
            ni--;
            nc = ffz_at(needle, ni - 1);
        }
    }
    return (int32_t)ffz_calculate_score(m, hay, needle, new_start, end, out);
}

// --- optimal DP -----------------------------------------------------------
int32_t ffz_fuzzy_optimal(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t greedy_end, size_t end,
                          ffz_indices *out) {
    const ffz_config *cfg = &m->cfg;
    size_t needle_len = needle.len;
    size_t W = end - start;

    // Oversized -> linear greedy (matches nucleo's fallback policy).
    if ((size_t)W * needle_len > FFZ_MAX_MATRIX_SIZE || W > 0xFFFF ||
        needle_len > FFZ_MAX_NEEDLE_LEN || !ffz_matcher_reserve(m, W, needle_len)) {
        return ffz_fuzzy_greedy(m, hay, needle, start, greedy_end, out);
    }

    uint32_t *h = m->hay;
    uint8_t *bonus = m->bonus;
    ffz_mcell *M = m->mgrid;   // needle_len x W
    uint8_t *pmat = m->pmat;   // needle_len x W

    // Normalize the window and precompute per-column bonus.
    ffz_char_class prev_class =
        start > 0 ? ffz_char_class_of(ffz_at(hay, start - 1), cfg)
                  : cfg->initial_char_class;
    if (hay.b) {
        // ASCII fast path: avoid ffz_class_and_normalize indirection.
        for (size_t i = 0; i < W; i++) {
            uint8_t raw = hay.b[start + i];
            uint8_t folded = (cfg->ignore_case && raw >= 'A' && raw <= 'Z')
                                 ? (uint8_t)(raw + 32) : raw;
            ffz_char_class cls = (ffz_char_class)cfg->ascii_class[raw];
            h[i] = (uint32_t)folded;
            bonus[i] = (uint8_t)ffz_bonus_for(cfg, prev_class, cls);
            prev_class = cls;
        }
    } else {
        for (size_t i = 0; i < W; i++) {
            uint32_t c;
            ffz_char_class cls = ffz_class_and_normalize(ffz_at(hay, start + i), cfg, &c);
            h[i] = c;
            bonus[i] = (uint8_t)ffz_bonus_for(cfg, prev_class, cls);
            prev_class = cls;
        }
    }

    // --- row 0 (first needle char), with optional prefix bonus ---
    uint16_t prefix = 0;
    if (cfg->prefer_prefix) {
        if (start == 0) {
            prefix = (uint16_t)(FFZ_MAX_PREFIX_BONUS * FFZ_PREFIX_BONUS_SCALE);
        } else {
            size_t s1 = start - 1;
            if (s1 > 0xFFFF) s1 = 0xFFFF;
            uint32_t ext0 = (uint32_t)s1 * (uint32_t)FFZ_PENALTY_GAP_EXTENSION;
            prefix = ffz_sat_sub_u16(
                (uint16_t)(FFZ_MAX_PREFIX_BONUS * FFZ_PREFIX_BONUS_SCALE -
                           FFZ_PENALTY_GAP_START),
                ext0 > 0xFFFFu ? (uint16_t)0xFFFF : (uint16_t)ext0);
        }
    }
    uint32_t nd0 = ffz_at(needle, 0);
    for (size_t i = 0; i < W; i++) {
        ffz_mcell *cell = &M[i];
        if (h[i] == nd0) {
            cell->valid = 1;
            cell->matched = 0;
            cell->consec = bonus[i];
            cell->score = (uint16_t)(bonus[i] * FFZ_BONUS_FIRST_CHAR_MULTIPLIER +
                                     FFZ_SCORE_MATCH + prefix / FFZ_PREFIX_BONUS_SCALE);
        } else {
            cell->valid = 0;
        }
        prefix = ffz_sat_sub_u16(prefix, FFZ_PENALTY_GAP_EXTENSION);
    }

    // --- rows 1..needle_len-1 ---
    // The gap track P lags one column behind the match: M[k][i] consumes
    // P[i-1] (mirrors nucleo's score_row, where p_score(col w) feeds the cell
    // written at col w+1). pprev_* carries P[i-1] into column i.
    for (size_t k = 1; k < needle_len; k++) {
        const ffz_mcell *prev = &M[(k - 1) * W];
        ffz_mcell *cur = &M[k * W];
        uint8_t *pm = &pmat[k * W];
        uint32_t ndk = ffz_at(needle, k);

        uint16_t pprev_score = 0;  // P[i-1].score  (P[-1] invalid)
        uint8_t pprev_valid = 0;
        for (size_t i = 0; i < W; i++) {
            // ---- M[k][i] consumes the lagged gap value P[i-1] ----
            ffz_mcell *cell = &cur[i];
            uint8_t diag_valid = (i > 0) && prev[i - 1].valid;
            if (h[i] != ndk || (!diag_valid && !pprev_valid)) {
                cell->valid = 0;
            } else {
                uint16_t b = bonus[i];
                if (!diag_valid) {  // only reachable after a gap
                    cell->valid = 1;
                    cell->matched = 0;
                    cell->consec = (uint8_t)b;
                    cell->score = (uint16_t)(pprev_score + b + FFZ_SCORE_MATCH);
                } else {
                    uint16_t cb =
                        ffz_u16_max(prev[i - 1].consec, FFZ_BONUS_CONSECUTIVE);
                    if (b >= FFZ_BONUS_BOUNDARY && b > cb) cb = b;
                    uint16_t score_match =
                        (uint16_t)(prev[i - 1].score + ffz_u16_max(cb, b));
                    cell->valid = 1;
                    uint16_t score_skip =
                        pprev_valid ? (uint16_t)(pprev_score + b) : 0;
                    if (!pprev_valid || score_match > score_skip) {
                        cell->matched = 1;
                        cell->consec = (uint8_t)cb;
                        cell->score = (uint16_t)(score_match + FFZ_SCORE_MATCH);
                    } else {
                        cell->matched = 0;
                        cell->consec = (uint8_t)b;
                        cell->score = (uint16_t)(score_skip + FFZ_SCORE_MATCH);
                    }
                }
            }

            // ---- compute P[i] for the next column ----
            // P[i] = max( M[k-1][i-1] - GAP_START,  P[i-1] - GAP_EXTENSION )
            uint16_t c1 = 0, c2 = 0;
            uint8_t v1 = 0, v2 = 0;
            if (i > 0 && prev[i - 1].valid) {
                v1 = 1;
                c1 = ffz_sat_sub_u16(prev[i - 1].score, FFZ_PENALTY_GAP_START);
            }
            if (pprev_valid) {
                v2 = 1;
                c2 = ffz_sat_sub_u16(pprev_score, FFZ_PENALTY_GAP_EXTENSION);
            }
            uint16_t pcur = 0;
            uint8_t pcur_valid = 0, pcur_from = 0;
            if (v1 && v2) {
                pcur_valid = 1;
                if (c1 > c2) { pcur = c1; pcur_from = 1; }
                else { pcur = c2; pcur_from = 0; }
            } else if (v1) { pcur_valid = 1; pcur = c1; pcur_from = 1; }
            else if (v2) { pcur_valid = 1; pcur = c2; pcur_from = 0; }
            pm[i] = pcur_from;
            pprev_score = pcur;
            pprev_valid = pcur_valid;
        }
    }

    // --- find best end cell in the last row (ties: pick the LAST max) ---
    const ffz_mcell *last = &M[(needle_len - 1) * W];
    int found = 0;
    size_t best_i = 0;
    uint16_t best = 0;
    for (size_t i = 0; i < W; i++) {
        if (last[i].valid && (!found || last[i].score >= best)) {
            best = last[i].score;
            best_i = i;
            found = 1;
        }
    }
    if (!found) {
        // Should not happen (prefilter guaranteed a subsequence); be safe.
        return ffz_fuzzy_greedy(m, hay, needle, start, greedy_end, out);
    }

    // --- backtrack for indices ---
    if (out) {
        // [L-1] Use heap allocation: FFZ_MAX_NEEDLE_LEN is 2048, so a stack
        // array would be ~16 KB — too large for typical stack budgets on embedded
        // and mobile targets. Fall back to greedy on OOM.
        size_t *idx = (size_t *)malloc(needle_len * sizeof(size_t));
        if (!idx) return ffz_fuzzy_greedy(m, hay, needle, start, greedy_end, out);

        size_t k = needle_len - 1;
        size_t i = best_i;
        idx[k] = i;
        while (k > 0) {
            const ffz_mcell *cell = &M[k * W + i];
            if (cell->matched) {
                if (i == 0) goto bt_fail;  // underflow guard
                i--;
            } else {
                // gap entry consumed P[i-1]; follow the gap track to its origin.
                if (i == 0) goto bt_fail;
                size_t col = i - 1;
                uint8_t *pm = &pmat[k * W];
                while (col > 0 && !pm[col]) col--;
                if (col == 0) goto bt_fail;  // ran off the left edge
                i = col - 1;
            }
            k--;
            idx[k] = i;
        }
        for (size_t t = 0; t < needle_len; t++)
            ffz_indices_push(out, (uint32_t)(start + idx[t]));
        free(idx);
        goto bt_done;
        bt_fail:
            free(idx);
            return ffz_fuzzy_greedy(m, hay, needle, start, greedy_end, out);
        bt_done:;
    }
    return (int32_t)best;
}

// --- 2-row rolling DP (FAST scoring mode) --------------------------------
// Uses simplified bonuses (ffz_fast_bonus) and O(W) scratch instead of the
// full needle_len×W matrix. Score-only: never fills indices.
//
// Recurrences (H = match track, C = gap/skip track, lagged by 1 column):
//   H[k][i] = max(H[k-1][i-1] + max(b, CONSECUTIVE),   // consecutive
//                 C[k][i-1]   + b)                       // after gap
//             + SCORE_MATCH   (when hay[i] == needle[k])
//   C[k][i] = max(H[k-1][i-1] - GAP_START,
//                 C[k][i-1]   - GAP_EXTENSION)
// C[k][i] is computed during the scan and feeds column i+1 via pprev_c.
int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t greedy_end, size_t end) {
    const ffz_config *cfg = &m->cfg;
    size_t W = end - start;
    size_t nl = needle.len;

    // Oversized -> linear greedy (score-only), mirroring ffz_fuzzy_optimal's cap
    // (ffz_fuzzy.c:50). Without it the default FAST path runs an uncapped O(W*nl)
    // DP, so a large corpus item + long matching query hangs the isolate. Use
    // greedy_end (not end) so the fallback window matches optimal's and the
    // index-producing FAST path (ffz_match.c).
    if ((size_t)W * nl > FFZ_MAX_MATRIX_SIZE || W > 0xFFFF ||
        nl > FFZ_MAX_NEEDLE_LEN)
        return ffz_fuzzy_greedy(m, hay, needle, start, greedy_end, NULL);

    if (!ffz_matcher_reserve(m, W, 1)) return -1;  // OOM

    // [H-10] 0 is used as the "invalid / no match" sentinel in H_prev / H_curr.
    // Values are stored as (actual_score + 1) so that a gap penalty reducing a
    // score to 0 is still distinguishable from an empty cell (stored as 0).
    // INVARIANT: valid cells store (score + 1) >= 1; 0 means "no match here."
    uint32_t *H_prev = m->roll;     // row k-1
    uint32_t *H_curr = m->roll + W; // row k — stride W keeps both rows adjacent

    // Normalize window and precompute fast bonuses.
    // ASCII fast path: class lookup via ascii_class table + inline case fold;
    // avoids two function calls (ffz_class_and_normalize / ffz_normalize_cp)
    // and the ffz_at branch on every iteration — gives ~5-10% on ASCII input.
    if (hay.b) {
        bool ic = cfg->ignore_case;
        ffz_char_class prev_cls = start > 0
            ? (ffz_char_class)cfg->ascii_class[hay.b[start - 1]]
            : cfg->initial_char_class;
        for (size_t i = 0; i < W; i++) {
            uint8_t b = hay.b[start + i];
            ffz_char_class cls = (ffz_char_class)cfg->ascii_class[b];
            if (ic && b >= 'A' && b <= 'Z') b += 32;
            m->hay[i]   = b;
            m->bonus[i] = ffz_fast_bonus(prev_cls, cls);
            prev_cls    = cls;
        }
    } else {
        ffz_char_class prev_cls = start > 0
            ? ffz_char_class_of(ffz_at(hay, start - 1), cfg)
            : cfg->initial_char_class;
        for (size_t i = 0; i < W; i++) {
            uint32_t c;
            ffz_char_class cls =
                ffz_class_and_normalize(ffz_at(hay, start + i), cfg, &c);
            m->hay[i]   = c;
            m->bonus[i] = ffz_fast_bonus(prev_cls, cls);
            prev_cls    = cls;
        }
    }

    // Row 0: match needle[0].  Store (score + 1); 0 means no match.
    uint32_t nd0 = ffz_at(needle, 0);
    for (size_t i = 0; i < W; i++) {
        H_prev[i] = (m->hay[i] == nd0)
            ? (uint32_t)(m->bonus[i] * FFZ_BONUS_FIRST_CHAR_MULTIPLIER +
                         FFZ_SCORE_MATCH) + 1u
            : 0;
    }

    // Rows 1 .. nl-1.
    // H_prev/H_curr store (actual_score + 1); 0 = invalid cell.
    for (size_t k = 1; k < nl; k++) {
        uint32_t ndk = ffz_at(needle, k);
        uint32_t pprev_c = 0;  // C[k][i-1] (gap score from the previous column)

        for (size_t i = 0; i < W; i++) {
            // Cache H_prev[i-1] once: used by both the consecutive path and the
            // gap-score computation below, avoiding a second memory load.
            uint32_t hp = (i > 0) ? H_prev[i - 1] : 0;

            uint32_t new_h = 0;
            if (m->hay[i] == ndk) {
                uint8_t b = m->bonus[i];
                // Consecutive path: from H[k-1][i-1].
                if (hp) {
                    uint32_t prev_score = hp - 1u;  // decode stored value
                    uint8_t cb = b > FFZ_BONUS_CONSECUTIVE
                                     ? b
                                     : (uint8_t)FFZ_BONUS_CONSECUTIVE;
                    new_h = (prev_score + cb + FFZ_SCORE_MATCH) + 1u;
                }
                // Gap path: from C[k][i-1] = pprev_c.
                if (pprev_c) {
                    uint32_t g = (pprev_c + b + FFZ_SCORE_MATCH) + 1u;
                    if (g > new_h) new_h = g;
                }
            }
            H_curr[i] = new_h;

            // Compute C[k][i] (for use at column i+1 as pprev_c).
            // C stores actual gap scores (not +1 offset), as it feeds arithmetic
            // directly; 0 means invalid.
            uint32_t new_c = 0;
            if (hp) {
                uint32_t prev_score = hp - 1u;
                if (prev_score > FFZ_PENALTY_GAP_START)
                    new_c = prev_score - FFZ_PENALTY_GAP_START;
            }
            if (pprev_c > FFZ_PENALTY_GAP_EXTENSION) {
                uint32_t ext = pprev_c - FFZ_PENALTY_GAP_EXTENSION;
                if (ext > new_c) new_c = ext;
            }
            pprev_c = new_c;
        }
        // Rotate rows.
        uint32_t *tmp = H_prev;
        H_prev = H_curr;
        H_curr = tmp;
    }

    // Find best score in the final row.  Cells store (score + 1); 0 = invalid.
    uint32_t best_enc = 0;
    for (size_t i = 0; i < W; i++)
        if (H_prev[i] > best_enc) best_enc = H_prev[i];
    if (!best_enc) return -1;
    uint16_t best = (uint16_t)(best_enc - 1u);  // decode back to actual score

    // Apply a window-level prefix bonus: the entire window [start..end) gets a
    // uniform bonus based on start offset, unlike ffz_fuzzy_optimal which applies
    // per-column bonuses that decay across the window.  The two modes may differ
    // by up to FFZ_MAX_PREFIX_BONUS when the best hit column > 0.
    if (cfg->prefer_prefix) {
        uint16_t pb;
        if (start == 0) {
            pb = FFZ_MAX_PREFIX_BONUS;
        } else {
            size_t s1 = start - 1;
            if (s1 > 0xFFFF) s1 = 0xFFFF;
            uint32_t ext = (uint32_t)s1 * (uint32_t)FFZ_PENALTY_GAP_EXTENSION;
            uint16_t raw = ffz_sat_sub_u16(
                (uint16_t)(FFZ_MAX_PREFIX_BONUS * FFZ_PREFIX_BONUS_SCALE -
                           FFZ_PENALTY_GAP_START),
                ext > 0xFFFFu ? (uint16_t)0xFFFF : (uint16_t)ext);
            pb = raw / FFZ_PREFIX_BONUS_SCALE;
        }
        best = ffz_sat_add_u16(best, pb);
    }
    return (int32_t)best;
}
