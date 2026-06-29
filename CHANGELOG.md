# Changelog

## 0.5.0

- **New: raw-object search.** Every match mode gains a `…Raws` variant that
  returns the matched objects directly — no `FuzzyHit` wrapper — and an
  async/single-result family alongside:
  - `fuzzyRaws` / `substringRaws` / `prefixRaws` / `postfixRaws` / `suffixRaws`
    / `exactRaws` (lists), their `…RawsAsync` twins, and `fuzzyRaw` /
    `substringRaw` / `prefixRaw` / `postfixRaw` / `exactRaw` (+ `…RawAsync`) for
    the single best hit.
  - These skip Pass-2 highlight-index computation, so they're faster than the
    `FuzzyHit`-returning methods when you only need the items, not
    score/indices/metadata.
- **New: `suffix` / `suffixAsync` / `suffixRaws` / `suffixRawsAsync`** — aliases
  for the `postfix` family, matching the more idiomatic Dart naming.
- **Fix: literal queries keep their spaces.** In the non-fuzzy modes
  (`exact` / `prefix` / `postfix` / `substring`) the query is now treated as a
  single literal atom, so `exact("Super Gems 1000")` matches that exact string
  instead of being split into three space-separated terms. (Fuzzy mode still
  parses space-separated terms and `! ^ ' $` operators as before.)
- **C engine.** Added `ffz_corpus_filter_raws` (the skip-index scan behind the
  `…Raws` methods) and bulk result accessors (`ffz_ffi_results_bulk`,
  `ffz_ffi_results_items_bulk`) that fill caller arrays in one call — these cut
  the result-read boundary crossings to O(1), chiefly benefiting the WASM build.

## 0.4.0

- **New: scoring modes.** A `scoring:` parameter on `FuzzyOptions` and on every
  search method selects the ranking algorithm via the `FuzzyScoring` enum:
  - `fast` (default) — 2-row rolling DP, tuned for names / paths / symbols.
  - `off` — no ranking; results returned in insertion order (ID / unique-match).
  - `nucleo` — full-matrix DP, highest fidelity (~2× CPU).
- **Performance.** SIMD reverse scan (`ffz_rfind_ci`) speeds the prefilter tail
  window 20–30%; an ASCII fast path in the rolling preprocess loop adds ~5–10%
  on ASCII input.
- **Hardening** (from a multi-agent security / correctness review):
  - Cap the FAST rolling DP and the per-query atom count so hostile input can't
    drive a CPU hang.
  - Saturate the greedy scorer so a very long match can't wrap its 16-bit score
    and rank below a poor one.
  - `add` / `addAll` / `addKey` now perform the native insert *before* updating
    the Dart mirror, so a failed native add can't desync the corpus and return
    the wrong items afterwards.
  - Use `ptrdiff_t` for substring positions (correct on 64-bit Windows for
    >2 GB inputs); link `Threads::Threads` (musl / older glibc); NUL-terminate
    the Android crash log; honor `FFZ_NO_THREADS`; ship the Android `x86` ABI.

## 0.3.1

**The `ffuzzy` engine is now the compact C matcher** (previously a separate `ffz`
package). The original Rust + `flutter_rust_bridge` implementation is deprecated
and retained only for performance comparison under `benchmark/`.

- **Breaking — engine and API replaced.** The Rust-era API (the old
  `FuzzyMatcher`, `FuzzyStringMatcher`, `fuzzyMatch`, …) is gone. Use the
  C-engine API:
  - `FuzzyCorpus<T>` — generic object search via a `stringOf` extractor (or
    `FuzzyCorpus.strings(...)` for plain strings); hits carry the object as
    `FuzzyHit<T>.obj`.
  - **Match modes are methods**, not a flag: `fuzzy` / `substring` / `prefix` /
    `postfix` / `exact`, each with an `…Async` twin (background isolate).
  - **`FuzzyOptions`** bundles `caseMatching`/`normalization`/`parallel`/
    `threads`/`limit`/`highlight` as corpus-wide defaults (constructor),
    overridable field-by-field via each method's named params.
  - Mutation: `add`/`addAll`/`addKey`/`update`/`removeAt`/`removeWhere`/
    `refresh`/`clear` (append-only native corpus → edits rebuild in O(n)).
  - Plus `FuzzyHit`, `FuzzyKey`/`FuzzyKeyKind`, `fuzzyCodepointToUtf16`,
    `FuzzyException`, `FuzzyCrash`. See the README for the full surface.
- **No Rust toolchain required.** Native code is plain C, compiled and bundled
  per platform by the standard SDK; the previous precompiled-binary download
  step is gone.
- Smaller native library (~32 KB arm64) and equal-or-better performance vs the
  Rust engine (see README / `doc/INTERNALS.md`).
- Web is no longer offered (FFI is unavailable on web).

Functionally this is the former `ffz` 0.1.0 engine, published under the `ffuzzy`
name. Everything below describes that engine.

## 0.1.0 (as `ffz`)

Initial release of the standalone C reimplementation of
[`nucleo-matcher`](https://github.com/helix-editor/nucleo) 0.3.1 with an
idiomatic Dart/Flutter FFI binding. C-only; no Rust dependency.

### Matching
- Fuzzy / substring / prefix / postfix / exact modes; fuzzy parses fzf-style
  operators (`! ^ ' $`) and space-separated terms.
- Per-query case (`respect`/`ignore`/`smart`) and Unicode normalization.
- **Byte-identical to nucleo** (6210/6210 differential pairs, score + indices)
  in the exact build; CJK/Latin-fold/full-case-fold Unicode support.

### Corpus & API
- Resident `FuzzyCorpus` with `add`/`addAll`/`addKey`/`clear`/`filter`.
- `addKey` for host-computed alternate keys (pinyin/romaji/initials).
- `filterAsync` runs the scan on a background isolate (UI never janks);
  overlapping calls are safe (per-call native matcher). Mutating a corpus while
  a `filterAsync` is in flight throws `StateError` (would be a use-after-free).
- Optional multi-threaded scoring (off by default; auto = half the CPUs capped
  at 8; hard ceiling cpu-1). Results are deterministic and identical to serial.
- `fuzzyCodepointToUtf16` to map match indices to UTF-16 offsets for highlighting.

### Build & diagnostics
- Native debug/release split is automatic per Flutter mode: debug/profile keep
  symbols + an optional in-process crash handler (`FuzzyCrash`); release is
  stripped/small (~32 KB arm64) with a `.debug`/`.pdb`/`.dSYM` sidecar for
  offline symbolization. `FFZ_CRASH_IN_RELEASE` forces the handler into release.

### Memory safety
- Drop-on-OOM throughout (no crash on allocation failure); bounded scratch;
  invalid UTF-8 → U+FFFD. Verified by unit + leak + OOM-injection + libFuzzer
  (ASan/UBSan) + the differential test in CI across Linux/macOS/Windows/Android.
