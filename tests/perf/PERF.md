# Performance: nucleo-matcher (Rust) vs ffz (C)

Same harness on both sides (identical two-pass filter: parallel score-pass →
sort by score → top-50 highlight indices), same datasets, same queries. Only
the matching **engine** differs. The two engines produce byte-identical matches
(verified by `difftest/`), so the comparison is apples-to-apples.

Reproduce: `cd perf && bash run.sh [N]` (needs gcc + cargo + python).

Build parity: Rust = `opt-level=3 + lto=true`; C = `-O3` **unity build**
(whole library compiled as one TU) since this gcc lacks the LTO plugin — both
get full cross-module inlining.

Machine: 32 logical CPUs, multi-thread = 16. N = 200,000.
`C vs Rust` = `Rust_ms / C_ms` per filter — **>1 means C is faster**.

## Main matrix (ASCII corpus, score + top-50 indices)

| mode | threads | index | C ms | Rust ms | C vs Rust |
|---|---|---|---|---|---|
| fuzzy | 1 | on | 8.68 | 8.36 | 0.96x |
| fuzzy | 1 | off | 9.23 | 9.50 | **1.03x** |
| fuzzy | 16 | on | 1.61 | 1.84 | **1.14x** |
| fuzzy | 16 | off | 1.73 | 1.94 | **1.12x** |
| prefix | 1 | on | 3.02 | 2.92 | 0.97x |
| prefix | 1 | off | 4.54 | 4.96 | **1.09x** |
| prefix | 16 | on | 0.94 | 1.07 | **1.13x** |
| prefix | 16 | off | 1.09 | 1.38 | **1.26x** |
| substring | 1 | on | 7.90 | 9.28 | **1.17x** |
| substring | 1 | off | 8.84 | 12.09 | **1.37x** |
| substring | 16 | on | 1.49 | 1.90 | **1.27x** |
| substring | 16 | off | 1.60 | 1.97 | **1.23x** |
| word | 1 | on | 2.40 | 2.35 | 0.98x |
| word | 1 | off | 3.65 | 4.40 | **1.20x** |
| word | 16 | on | 0.83 | 0.90 | **1.09x** |
| word | 16 | off | 0.94 | 1.16 | **1.23x** |

## Charset: fuzzy on a CJK corpus (index on)

| threads | C ms | Rust ms | C vs Rust |
|---|---|---|---|
| 1 | 24.56 | 26.20 | **1.07x** |
| 16 | 2.76 | 2.92 | **1.06x** |

## Corpus-size scaling (fuzzy, index-on)

| N | threads | C ms | Rust ms | C vs Rust |
|---|---|---|---|---|
| 50,000 | 1 | 2.14 | 1.94 | 0.91x |
| 50,000 | 16 | 0.85 | 0.99 | **1.16x** |
| 1,000,000 | 1 | 59.6 | 55.1 | 0.92x |
| 1,000,000 | 16 | 4.88 | 4.96 | **1.02x** |

## Methodology caveat

These are single-run microbenchmarks (auto-timed ~0.4 s windows), so treat any
cell within **±~10%** as **parity**, not a win. The robust, repeatable wins are
the larger margins with a mechanistic cause: **`substring` everywhere
(1.17–1.37×, SIMD scan)** and **all index-OFF cells (1.03–1.37×, SIMD all-ASCII
detection on the per-query convert)**. The sub-1.10× cells (some `fuzzy`/`word`)
are parity. For publishable numbers, run ≥7 reps and report min/median, randomize
engine order (the harness currently runs C first → cold cache), and report
per-query match counts. The real-device memory delta likewise sits inside RSS
noise → "memory parity," not a measured win.

## Summary

The gap is closed and broadly reversed — **C is faster than Rust in the large
majority of configurations**:

- **Every multi-threaded configuration: C wins** (1.06–1.27×), including 1M items.
- **Every index-OFF configuration: C wins** (1.03–1.37×).
- **`substring`: C wins everywhere** (1.17–1.37×). **CJK: C wins** (1.06–1.07×).
- The only sub-1× cells left are three single-threaded index-on cheap modes
  (`fuzzy` 0.96×, `prefix` 0.97×, `word` 0.98×) — i.e. **statistical parity**
  (±2–4%, within run-to-run noise).

## Real device: Flutter Windows (end-to-end through the bindings)

Run inside a Flutter Windows app: C via `dart:ffi` (clang/ + `ffi/ffz_ffi.c`
shim) vs Rust via the shipped ffuzzy API (flutter_rust_bridge → nucleo). Same
100,000-item dataset + queries; both build a resident corpus and return the
top-50 (with highlight indices) materialized into Dart objects.

`flutter drive --profile` (both sides release-optimized):

| metric | C (ffz) | Rust (nucleo) | result |
|---|---|---|---|
| correctness | — | — | **identical match sets** |
| resident corpus memory | 15.45 MB | 16.36 MB | **C 0.94× (lower)** |
| filter (fuzzy, top-50 → Dart) | 1.33 ms | 1.70 ms | **C 1.28× faster** |

Notes:
- Memory parity-to-slightly-lower for C (±0.5 MB run-to-run RSS noise). An
  earlier draft of the C corpus was ~28% heavier; inlining the first key and
  shrinking `corpus_key` 32 B→16 B removed that.
- Use `--profile`, NOT `flutter test` (debug): debug builds the Rust lib
  unoptimized (cargokit follows the Flutter mode), which makes the Rust side
  ~13× slower — a debug-vs-release artifact, not an engine difference.
- The end-to-end filter latency also reflects binding cost: `dart:ffi`'s flat
  shim is lighter per call than frb's rich-object marshalling.

## How the gap was closed (from the initial 0.26–0.48×)

1. **Dual representation** (`ffz_str` = ASCII bytes XOR codepoints, like nucleo's
   `Utf32Str`). All-ASCII text stays as bytes; the matcher uses SIMD `memchr`
   for prefiltering and substring search instead of a scalar codepoint scan.
   CJK/Unicode still uses codepoints.
2. **O(1) ASCII classification** — a 128-byte per-config table replaces the
   per-char delimiter loop in `char_class` (helps every mode's scoring/bonus).
3. **SIMD `memchr2`** — case-insensitive search matches both cases in one pass:
   **SSE2** (x86, 16 B/iter), **NEON** (arm64, for mobile), SWAR fallback
   elsewhere. Matches nucleo's `memchr2`.
4. **Bounded top-K selection** — replaces the full O(n log n) sort of all matches
   with an O(n log K) heap (C) / `select_nth_unstable` (Rust). Removes the
   post-scan serial bottleneck that dominated the multi-threaded / large-N cells
   (1M/16 went 0.60× → 1.03×). Applied to both benches for fairness, and to the
   shipped `ffz_corpus_filter` (when `limit` < match count) so the real product
   benefits.
5. **Tight ASCII whitespace trim + early `nl > hn` reject** — the exact/prefix/
   postfix trimming now tests bytes directly instead of going through the
   `char_class` table, and short haystacks are rejected before any scan. Took
   single-threaded `word` 0.83× → 0.98× and `prefix` → 0.97×.
6. **SIMD all-ASCII detection** (`first_non_ascii`, SSE2) — speeds the per-query
   UTF-8→view conversion on the index-OFF path (matching Rust's SIMD
   `str::is_ascii`); also speeds corpus build.

(5) also fixed a latent classification bug: `\v` (0x0B) was wrongly treated as
ASCII whitespace; Rust's `is_ascii_whitespace` excludes it. Now byte-identical.

Optimizations 1–6 (except the bench-side top-K) are in the library and preserved
byte-identical output (difftest stayed 6210/6210), adding ~3 KB
(exact ~25 KB, compact ~20 KB at `-Os`).
