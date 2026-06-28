// Lite Unicode tables — empty (passthrough). Compiled into the "lite" engine
// INSTEAD of src/ffz_unicode_tables.c (see build-engine.sh): with the two table
// lengths at 0, the binary searches in ffz_chars.c (normalize_lookup /
// casefold_lookup) never run and return the codepoint unchanged. Effect:
//   - no accent stripping, no Cyrillic/Greek case folding (non-ASCII passthrough)
//   - ASCII case folding (inline) and CJK direct matching still work
// This drops the ~17 KB full tables, giving the smaller ASCII+CJK lite build.
#include <stddef.h>
#include <stdint.h>

const uint32_t ffz_normalize_keys[1] = {0};
const uint8_t  ffz_normalize_vals[1] = {0};
const size_t   ffz_normalize_keys_len = 0;

const uint32_t ffz_casefold_start[1]  = {0};
const uint8_t  ffz_casefold_span[1]   = {0};
const uint8_t  ffz_casefold_offidx[1] = {0};
const int32_t  ffz_casefold_off[1]    = {0};
const size_t   ffz_casefold_start_len = 0;
