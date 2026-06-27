# Changelog

## 0.3.0

**The `ffuzzy` engine is now the compact C matcher** (previously a separate `ffz`
package). The original Rust + `flutter_rust_bridge` implementation is deprecated
and retained only for performance comparison under `benchmark/`.

- **Breaking — engine and API replaced.** The Rust-era API (`FuzzyMatcher`,
  `FuzzyStringMatcher`, `fuzzyMatch`, …) is gone. Use the C-engine API:
  `FfzCorpus` + `filter`/`filterAsync`, `FfzHit`, `FfzMode`/`FfzCase`/`FfzNorm`,
  `FfzKey`/`addKeyed`, `ffzCodepointToUtf16`, `FfzException`, `FfzCrash`. See the
  README for the full surface.
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
- Resident `FfzCorpus` with `add`/`addAll`/`addKeyed`/`clear`/`filter`.
- `addKeyed` for host-computed alternate keys (pinyin/romaji/initials).
- `filterAsync` runs the scan on a background isolate (UI never janks);
  overlapping calls are safe (per-call native matcher). Mutating a corpus while
  a `filterAsync` is in flight throws `StateError` (would be a use-after-free).
- Optional multi-threaded scoring (off by default; auto = half the CPUs capped
  at 8; hard ceiling cpu-1). Results are deterministic and identical to serial.
- `ffzCodepointToUtf16` to map match indices to UTF-16 offsets for highlighting.

### Build & diagnostics
- Native debug/release split is automatic per Flutter mode: debug/profile keep
  symbols + an optional in-process crash handler (`FfzCrash`); release is
  stripped/small (~32 KB arm64) with a `.debug`/`.pdb`/`.dSYM` sidecar for
  offline symbolization. `FFZ_CRASH_IN_RELEASE` forces the handler into release.

### Memory safety
- Drop-on-OOM throughout (no crash on allocation failure); bounded scratch;
  invalid UTF-8 → U+FFFD. Verified by unit + leak + OOM-injection + libFuzzer
  (ASan/UBSan) + the differential test in CI across Linux/macOS/Windows/Android.
