// Exact non-ASCII character classification table (generated; see
// difftest/src/bin/gen_class_table.rs). Each entry packs (start<<3)|class;
// the class holds for codepoints in [start, next_start).
//
// Define FFZ_COMPACT_CLASS to drop this table (~12 KB) and fall back to the
// compact approximation in ffz_chars.c — at the cost of non-byte-identical
// scoring for some non-ASCII text (match/no-match is unaffected either way).
#ifndef FFZ_CLASS_H
#define FFZ_CLASS_H

#include <stddef.h>
#include <stdint.h>

#ifndef FFZ_COMPACT_CLASS
// Checkpoint: absolute `start` codepoint and the byte offset of that entry in
// the delta-varint stream. One per `ffz_class_ckpt_stride` entries.
typedef struct {
    uint32_t start;
    uint32_t off;
} ffz_class_ckpt;

extern const uint8_t ffz_class_data[];        // LEB128 of (delta_start<<3)|class
extern const size_t ffz_class_data_len;
extern const ffz_class_ckpt ffz_class_ckpts[];
extern const size_t ffz_class_ckpts_len;
extern const unsigned ffz_class_ckpt_stride;
#endif

#endif  // FFZ_CLASS_H
