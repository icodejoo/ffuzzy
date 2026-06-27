// Character classification and Unicode folding.
//
// ASCII is handled arithmetically. For non-ASCII we use a compact scheme that
// avoids shipping full Unicode general-category tables (which would dwarf the
// matcher): a codepoint is UPPER iff it has a simple case fold (is a casefold
// run key), WHITESPACE iff in the small White_Space set, else LETTER. This is
// a deliberate, documented deviation from nucleo for *scoring* of non-ASCII
// (e.g. accented lowercase is LETTER not LOWER); it never affects whether two
// codepoints compare equal, so match/no-match sets are unaffected. See README.
#include "ffz_internal.h"
#include "ffz_class.h"
#include "ffz_unicode.h"

// --- table lookups --------------------------------------------------------
static uint32_t normalize_lookup(uint32_t cp) {
    size_t lo = 0, hi = ffz_normalize_keys_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t k = ffz_normalize_keys[mid];
        if (k == cp) return ffz_normalize_vals[mid];
        if (k < cp) lo = mid + 1; else hi = mid;
    }
    return cp;
}

static uint32_t casefold_lookup(uint32_t cp) {
    // largest run with start <= cp
    size_t lo = 0, hi = ffz_casefold_start_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ffz_casefold_start[mid] <= cp) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return cp;
    size_t i = lo - 1;
    if (cp - ffz_casefold_start[i] <= ffz_casefold_span[i])
        return (uint32_t)((int64_t)cp + ffz_casefold_off[ffz_casefold_offidx[i]]);
    return cp;
}

#ifdef FFZ_COMPACT_CLASS
static bool is_unicode_ws(uint32_t cp) {
    // Unicode White_Space, non-ASCII portion (ASCII handled elsewhere).
    switch (cp) {
        case 0x85: case 0xA0: case 0x1680:
        case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return (cp >= 0x2000 && cp <= 0x200A);
    }
}
#endif  // FFZ_COMPACT_CLASS

// --- classification -------------------------------------------------------
#ifndef FFZ_COMPACT_CLASS
static uint32_t read_varint(const uint8_t *d, size_t len, size_t *pos) {
    uint32_t r = 0;
    for (int shift = 0; shift < 35 && *pos < len; shift += 7) {
        uint8_t b = d[(*pos)++];
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return r;
    }
    return r;
}

// Exact path: binary-search the checkpoints, then linearly decode the
// delta-varint stream (<= stride entries) up to the run containing cp.
static ffz_char_class class_table_lookup(uint32_t cp) {
    size_t lo = 0, hi = ffz_class_ckpts_len;  // largest ckpt with start <= cp
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ffz_class_ckpts[mid].start <= cp) lo = mid + 1; else hi = mid;
    }
    // cp >= 0x80 == ckpts[0].start, so lo >= 1.
    const ffz_class_ckpt *ck = &ffz_class_ckpts[lo - 1];
    size_t pos = ck->off;
    uint32_t cur_start = ck->start;
    // First entry at the checkpoint: take its class; its delta is relative to
    // the previous (unknown) start, but cur_start is already known from ck.
    uint32_t v = read_varint(ffz_class_data, ffz_class_data_len, &pos);
    ffz_char_class cls = (ffz_char_class)(v & 7u);
    while (pos < ffz_class_data_len) {
        v = read_varint(ffz_class_data, ffz_class_data_len, &pos);
        uint32_t next = cur_start + (v >> 3);
        if (next > cp) break;
        cur_start = next;
        cls = (ffz_char_class)(v & 7u);
    }
    return cls;
}
#endif

ffz_char_class ffz_char_class_of(uint32_t cp, const ffz_config *cfg) {
    if (cp < 0x80) return (ffz_char_class)cfg->ascii_class[cp];  // O(1) table
#ifdef FFZ_COMPACT_CLASS
    // Compact approximation (no Unicode category tables).
    if (is_unicode_ws(cp)) return FFZ_CLASS_WHITESPACE;
    if (casefold_lookup(cp) != cp) return FFZ_CLASS_UPPER;  // has a lowercase fold
    return FFZ_CLASS_LETTER;
#else
    return class_table_lookup(cp);
#endif
}

// --- helpers for the pattern layer (smart case / normalization) -----------
uint32_t ffz_cp_to_lower(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    return casefold_lookup(cp);
}
bool ffz_cp_is_upper(uint32_t cp) {
    if (cp < 0x80) return cp >= 'A' && cp <= 'Z';
    return casefold_lookup(cp) != cp;  // has a lowercase fold
}
bool ffz_cp_has_normalize(uint32_t cp) {
    return cp >= 0x80 && normalize_lookup(cp) != cp;
}

uint32_t ffz_normalize_cp(uint32_t cp, const ffz_config *cfg) {
    if (cp < 0x80) {
        if (cfg->ignore_case && cp >= 'A' && cp <= 'Z') return cp + 32;
        return cp;
    }
    uint32_t c = cp;
    if (cfg->normalize) c = normalize_lookup(c);  // accent strip (may become ASCII)
    if (cfg->ignore_case) {
        if (c >= 'A' && c <= 'Z') c += 32;          // ASCII result of normalize
        else if (c >= 0x80) c = casefold_lookup(c); // non-ASCII case fold
    }
    return c;
}

ffz_char_class ffz_class_and_normalize(uint32_t cp, const ffz_config *cfg,
                                       uint32_t *out) {
    // Class is computed on the ORIGINAL codepoint (matches nucleo).
    ffz_char_class cls = ffz_char_class_of(cp, cfg);
    *out = ffz_normalize_cp(cp, cfg);
    return cls;
}

// --- bonus ----------------------------------------------------------------
uint16_t ffz_bonus_for(const ffz_config *cfg, ffz_char_class prev,
                       ffz_char_class cls) {
    if (cls > FFZ_CLASS_DELIMITER) {  // transition into a word character
        switch (prev) {
            case FFZ_CLASS_WHITESPACE: return cfg->bonus_boundary_white;
            case FFZ_CLASS_DELIMITER: return cfg->bonus_boundary_delimiter;
            case FFZ_CLASS_NONWORD: return FFZ_BONUS_BOUNDARY;
            default: break;
        }
    }
    if ((prev == FFZ_CLASS_LOWER && cls == FFZ_CLASS_UPPER) ||
        (prev != FFZ_CLASS_NUMBER && cls == FFZ_CLASS_NUMBER)) {
        return FFZ_BONUS_CAMEL123;  // camelCase or letter->digit
    }
    if (cls == FFZ_CLASS_WHITESPACE) return cfg->bonus_boundary_white;
    if (cls == FFZ_CLASS_NONWORD) return FFZ_BONUS_NON_WORD;
    return 0;
}

#define FFZ_FAST_BONUS_BOUNDARY 8
#define FFZ_FAST_BONUS_CAMEL    7

uint8_t ffz_fast_bonus(ffz_char_class prev, ffz_char_class cls) {
    // After any non-word / separator: word-boundary bonus.
    if (prev <= FFZ_CLASS_DELIMITER && cls > FFZ_CLASS_DELIMITER)
        return FFZ_FAST_BONUS_BOUNDARY;
    // camelCase or letter→digit transition.
    if ((prev == FFZ_CLASS_LOWER && cls == FFZ_CLASS_UPPER) ||
        (prev != FFZ_CLASS_NUMBER && cls == FFZ_CLASS_NUMBER))
        return FFZ_FAST_BONUS_CAMEL;
    return 0;
}
