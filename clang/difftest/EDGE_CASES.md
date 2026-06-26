# Differential test — results & the two edge flags

`run.sh` compares the C `ffz_pattern` against the Rust **nucleo-matcher 0.3.1**
`Pattern` oracle over every `(query, haystack)` pair, checking **score AND
matched indices** byte-for-byte. Same `Config::DEFAULT` + `CaseMatching::Smart`
+ `Normalization::Smart` on both sides.

## Result: strict byte-equivalence

**6210 / 6210 pairs identical** (90 queries × 69 haystacks) over an adversarial
corpus: repeated-char DP backtracking (mississippi / banana / abababab),
accented camelCase (fooéBar / straßeTest / İstanbulCity / résuméÉdit), non-ASCII
digits (Ⅷ / ② / ½ / ٢٣), non-ASCII symbols & emoji (、 / € / ∑ / 😀 / ！), CJK +
Latin mix, kana, Cyrillic/Greek case, ligatures (ﬁ / ß / ẞ), substring tails,
and the full `! ^ ' $` syntax.

Two flags are needed for *exact* equivalence; both are documented below.

## 1. Exact non-ASCII classification (`ffz_class_table.c`, default ON)
nucleo classifies characters with full Unicode category data (e.g. `ë` is
`Lower`, `Ⅷ` is `Number`, `、` is `NonWord`), which feeds the camelCase / number
/ word-boundary bonuses. To reproduce this byte-for-byte without guessing,
`gen_class_table` (a difftest binary) walks every codepoint through nucleo's own
`char_class_non_ascii` logic — using the **same rustc/Unicode version** that
compiles nucleo — and emits a packed breakpoint table. Building with
`-DFFZ_COMPACT_CLASS` drops the table (~12 KB) and approximates instead; scores
for some non-ASCII text then differ, but match/no-match never does.

## 2. nucleo substring tail bug (`-DFFZ_NUCLEO_SUBSTRING_BUGCOMPAT`)
nucleo's `substring_match_non_ascii` scans `[start .. len-needle_len)`, so a
substring whose match ends exactly at the last codepoint of a **non-ASCII**
haystack is missed (`"Bar"` at the tail of `"fooéBar"` → the negative atom
`!bar` fails to reject). ffz finds it correctly. The flag reproduces nucleo's
off-by-one for exact equivalence; it is **off by default** so production code
stays correct. nucleo's ASCII substring path (`memmem`) has no such bug, and the
flag only narrows the non-ASCII multi-char case, so ASCII behaviour is unchanged.

## Reproduce

```sh
export PATH="/c/w64devkit/bin:$PATH"   # gcc + cargo
cd difftest && bash run.sh
```

`run.sh` regenerates the class table, builds the C harness with both flags, runs
both implementations, and diffs. Exit 0 + "PASS" means byte-identical.
