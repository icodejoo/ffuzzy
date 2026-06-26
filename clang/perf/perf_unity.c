// Unity build: compiles the whole library + the bench as one translation unit
// so the compiler can inline freely across "files" — the fair counterpart to
// Rust's single-crate + LTO build (this gcc lacks the LTO plugin).
#include "../src/ffz_alloc.c"
#include "../src/ffz_chars.c"
#include "../src/ffz_class_table.c"
#include "../src/ffz_corpus.c"
#include "../src/ffz_fuzzy.c"
#include "../src/ffz_match.c"
#include "../src/ffz_pattern.c"
#include "../src/ffz_prefilter.c"
#include "../src/ffz_score.c"
#include "../src/ffz_string.c"
#include "../src/ffz_unicode_tables.c"
#include "perf_c.c"
