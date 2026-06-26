// Unicode folding tables (auto-generated data lives in ffz_unicode_tables.c).
//
// Two tables, both derived 1:1 from nucleo-matcher 0.3.1:
//   * normalize: Latin diacritic stripping (e.g. U+00C0 -> 'A'), ASCII targets,
//     stored as parallel flat arrays + binary search.
//   * casefold:  simple Unicode case folding (uppercase -> lowercase) for
//     non-ASCII codepoints, range+offset compressed.
#ifndef FFZ_UNICODE_H
#define FFZ_UNICODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// normalize: sorted keys with ASCII targets (parallel arrays).
extern const uint32_t ffz_normalize_keys[];
extern const uint8_t ffz_normalize_vals[];
extern const size_t ffz_normalize_keys_len;

// casefold (non-ASCII): dict-packed sorted runs. Run i covers codepoints
// [start[i], start[i]+span[i]] and folds them by off[offidx[i]].
extern const uint32_t ffz_casefold_start[];
extern const uint8_t ffz_casefold_span[];
extern const uint8_t ffz_casefold_offidx[];
extern const int32_t ffz_casefold_off[];
extern const size_t ffz_casefold_start_len;

#ifdef __cplusplus
}
#endif
#endif  // FFZ_UNICODE_H
