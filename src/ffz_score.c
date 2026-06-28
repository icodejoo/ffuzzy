// ffz_calculate_score — score a window [start,end) of the haystack against the
// needle, assuming the needle matches as a subsequence within it (the prefilter
// / greedy step guarantees this). Faithful port of nucleo `score.rs`.
#include "ffz_internal.h"


uint16_t ffz_calculate_score(ffz_matcher *m, ffz_str hay, ffz_str needle,
                             size_t start, size_t end, ffz_indices *out) {
    if (needle.len == 0) return 0;
    const ffz_config *cfg = &m->cfg;
    size_t needle_len = needle.len;

    ffz_char_class prev_class =
        start > 0 ? ffz_char_class_of(ffz_at(hay, start - 1), cfg)
                  : cfg->initial_char_class;

    size_t ni = 0;                 // needle cursor
    uint32_t needle_char = ffz_at(needle, ni);

    bool in_gap = false;
    int consecutive = 1;

    // Unrolled first iteration so the first-char multiplier is applied cleanly.
    if (out) ffz_indices_push(out, (uint32_t)start);
    ffz_char_class cls = ffz_char_class_of(ffz_at(hay, start), cfg);
    uint16_t first_bonus = ffz_bonus_for(cfg, prev_class, cls);
    uint16_t score =
        (uint16_t)(FFZ_SCORE_MATCH + first_bonus * FFZ_BONUS_FIRST_CHAR_MULTIPLIER);
    prev_class = cls;
    // advance needle (saturating at last char, like the Rust unwrap_or)
    ni++;
    needle_char = ni < needle_len ? ffz_at(needle, ni) : needle_char;

    for (size_t i = start + 1; i < end; i++) {
        uint32_t c;
        cls = ffz_class_and_normalize(ffz_at(hay, i), cfg, &c);
        if (c == needle_char) {
            if (out) ffz_indices_push(out, (uint32_t)i);
            uint16_t bonus = ffz_bonus_for(cfg, prev_class, cls);
            if (consecutive != 0) {
                if (bonus >= FFZ_BONUS_BOUNDARY && bonus > first_bonus)
                    first_bonus = bonus;
                bonus = ffz_u16_max(ffz_u16_max(bonus, first_bonus), FFZ_BONUS_CONSECUTIVE);
            } else {
                first_bonus = bonus;
            }
            score = ffz_sat_add_u16(score, (uint16_t)(FFZ_SCORE_MATCH + bonus));
            in_gap = false;
            consecutive += 1;
            if (ni + 1 < needle_len) {
                ni++;
                needle_char = ffz_at(needle, ni);
            }
        } else {
            uint16_t penalty =
                in_gap ? FFZ_PENALTY_GAP_EXTENSION : FFZ_PENALTY_GAP_START;
            score = ffz_sat_sub_u16(score, penalty);
            in_gap = true;
            consecutive = 0;
        }
        prev_class = cls;
    }

    if (cfg->prefer_prefix) {
        if (start != 0) {
            size_t s1 = start - 1;
            if (s1 > 0xFFFF) s1 = 0xFFFF;
            // [H-8] Use the same scaled formula as ffz_fuzzy_optimal (row-0
            // prefix block): compute the bonus in PREFIX_BONUS_SCALE units,
            // saturating-subtract gap penalties, then divide by the scale.
            // This keeps ffz_calculate_score (used by the greedy path) aligned
            // with the DP result to within rounding of integer division.
            //
            // [C-2] Work in uint32_t to avoid overflow: the intermediate value
            // (FFZ_MAX_PREFIX_BONUS * FFZ_PREFIX_BONUS_SCALE - FFZ_PENALTY_GAP_START)
            // fits in uint16_t, but the extension term s1 * FFZ_PENALTY_GAP_EXTENSION
            // can reach 65535 — keeping everything in uint16_t before the sat_sub
            // is safe here because ffz_sat_sub_u16 clamps at 0.
            uint32_t ext = (uint32_t)s1 * (uint32_t)FFZ_PENALTY_GAP_EXTENSION;
            uint16_t pb = ffz_sat_sub_u16(
                (uint16_t)(FFZ_MAX_PREFIX_BONUS * FFZ_PREFIX_BONUS_SCALE
                           - FFZ_PENALTY_GAP_START),
                ext > 0xFFFFu ? (uint16_t)0xFFFF : (uint16_t)ext);
            score = (uint16_t)(score + pb / FFZ_PREFIX_BONUS_SCALE);
        } else {
            score = (uint16_t)(score + FFZ_MAX_PREFIX_BONUS);
        }
    }
    return score;
}
