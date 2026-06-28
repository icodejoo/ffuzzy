# ffz — a small C fuzzy matcher

A standalone C reimplementation of [`nucleo-matcher`](https://github.com/helix-editor/nucleo)
0.3.1, plus an **index layer with a transliteration hook** for pinyin / romaji /
initials search.

Two build modes:

- **Exact (default)** — *byte-identical* to nucleo (verified: 6210/6210
  query×haystack pairs, score + indices, with `-DFFZ_NUCLEO_SUBSTRING_BUGCOMPAT`;
  the shipped default deliberately fixes one nucleo non-ASCII substring
  off-by-one — see Design notes). **~25 KB** (`-Os`).
- **Compact** (`-DFFZ_COMPACT_CLASS`) — functional parity, slightly different
  scores for some non-ASCII text; drops the Unicode class table. **~20 KB**.

Dual ASCII/codepoint representation (bytes for ASCII → SIMD `memchr`; codepoints
for Unicode), O(1) ASCII classification, compressed Unicode tables. Optional
multi-threaded corpus scan; a counting-allocator leak smoke test (`make leak`).
**Performance meets or beats nucleo** — C is faster in every multi-threaded
configuration and on `substring` across the board, at parity on CJK and
single-threaded `fuzzy`; see `perf/PERF.md`.

## Layout

```
include/
  ffz.h            public matcher + pattern API
  ffz_corpus.h     resident multi-key corpus + transliteration hook
  ffz_unicode.h    generated-table declarations
src/
  ffz_chars.c      char classification + Latin/Unicode folding
  ffz_string.c     UTF-8 -> codepoints, config, containers
  ffz_score.c      calculate_score (fzf bonus model)
  ffz_prefilter.c  subsequence window bounds
  ffz_fuzzy.c      optimal DP + greedy fallback
  ffz_match.c      dispatch + exact/substring/prefix/postfix
  ffz_pattern.c    `! ^ ' $` parsing, word splitting, needle normalization
  ffz_corpus.c     multi-key index, hook, two-pass filter
  ffz_unicode_tables.c   AUTO-GENERATED (do not edit)
tools/gen_unicode_tables.py   regenerates the tables from nucleo source
tests/test_ffz.c              unit tests (43 checks)
```

## Build & test

Requires a C11 compiler. On this machine: w64devkit (`C:\w64devkit\bin`).

```sh
export PATH="/c/w64devkit/bin:$PATH"   # if gcc isn't on PATH
make test     # compile + run the unit suite (incl. threading)
make leak     # memory-leak smoke test (-DFFZ_TRACK_ALLOC)
make lib      # build build/libffz.a
make size     # report -Os section sizes
```

### Parity with nucleo

The engine was verified **byte-identical to nucleo-matcher 0.3.1** — score AND
indices over all 6210 `(query, haystack)` pairs of a 90×69 adversarial corpus
(repeated-char DP backtracking, accented camelCase, non-ASCII digits/symbols/
emoji, kana, Cyrillic/Greek case, ligatures, substring tails), with
`-DFFZ_NUCLEO_SUBSTRING_BUGCOMPAT` for the one nucleo substring-tail quirk. The
differential harness (and the Rust engine it compared against) has since been
removed; parity is a historical guarantee of the current scoring code.

## Design notes

### One codepoint path
Everything matches over `uint32_t` codepoints (UTF-32). nucleo monomorphizes
its hot loops over `AsciiChar`/`char` × `const INDICES` (up to 8 copies); we keep
a single copy and a runtime indices flag. This is the main size win and is why
the matcher core is ~10 KB rather than nucleo's tens of KB of contribution.

### Scoring (identical model)
`ffz_score.c` ports `score.rs` constant-for-constant: `SCORE_MATCH=16`, gap
penalties 3/1, word-boundary / camelCase / consecutive bonuses, first-char
multiplier. Word boundaries, delimiters and whitespace produce the same bonuses.

### Optimal DP (semantically equivalent, not a line port)
`ffz_fuzzy.c` uses an explicit two-track dynamic program:

```
M[k][i]  best score with needle[k] matched AT haystack column i
P[k][i]  best score with needle[0..k] matched, sitting in a gap before i
P[k][i] = max( M[k-1][i-1] - GAP_START,  P[k][i-1] - GAP_EXTENSION )
M[k][i] = next_m_cell( P[k][i], bonus[i], M[k-1][i-1] )   when h[i]==needle[k]
```

This reproduces nucleo's `next_m_cell` / `p_score` recurrences and its
consecutive-bonus handling, but as a readable grid rather than nucleo's
space-optimized rolling array with diagonal index shifting. Backtracking over
the stored cells yields highlight indices. Oversized inputs (`width*needle >
100K` cells) fall back to the O(n) greedy matcher, matching nucleo's policy.

### Prefilter & ASCII fast path
`ffz_str` is a dual representation (ASCII bytes XOR UTF-32 codepoints). ASCII
haystacks use a SIMD prefilter — libc `memchr` (case-sensitive) and a one-pass
SWAR `memchr2` (case-insensitive, both cases in one 8-byte scan) — plus a
`memchr`-driven substring search; scoring reads bytes directly. Unicode uses a
scalar codepoint scan. This matches nucleo's `Utf32Str` strategy and brings
ASCII throughput to parity (see `perf/PERF.md`).

## Unicode coverage

Tables are generated from nucleo's data by `tools/gen_unicode_tables.py`, which
verifies its range compression is lossless before writing.

| Capability | Supported | Notes |
|---|---|---|
| Latin + diacritic folding (café≈cafe) | yes | `normalize` table |
| Full simple case folding (Greek/Cyrillic/Armenian/…) | yes | `casefold` runs |
| CJK (Chinese / Japanese kanji / kana / precomposed Hangul) | yes | codepoint-level direct match |
| Astral (emoji, rare scripts) | yes | internal `uint32_t` |
| Full-width ↔ half-width, kana folding, NFC/NFD, pinyin | no | preprocessing / index-layer concern |

**Pinyin / romaji / Korean initials** are **not** in the matcher. They are an
index-layer feature: the host generates alternate search keys (the dictionary
stays host-side) and the matcher just matches more strings. From **C** you can
register a `ffz_transliterator` callback (`ffz_corpus.h`) invoked per item; from
**Dart** the equivalent is `FuzzyCorpus.addKey(item, [FuzzyKey(...)])` — you
compute the keys host-side and pass them in (the C function-pointer hook isn't
bridged over FFI). A hit reports which key matched via `matchedKind`/`matchedKey`.

### Char classification: exact vs compact
- **Exact (default)** ships `ffz_class_table.c`, a packed breakpoint table
  generated from nucleo's exact `char_class_non_ascii` via the same rustc, so
  classification — and therefore *every score* — is byte-identical to nucleo.
- **Compact** (`-DFFZ_COMPACT_CLASS`) drops that ~4.8 KB compressed table (~12 KB
  uncompressed source) and approximates
  (UPPER iff it has a case fold; White_Space set; else LETTER). It only perturbs
  the camelCase/number *bonus* for some non-ASCII text; it never changes whether
  two codepoints compare equal, so **match/no-match is unaffected**.

### Other notes
- **Codepoint, not grapheme** granularity (same as nucleo with
  `unicode-segmentation` off). NFC text behaves identically; NFD/combining
  sequences differ slightly. The differential corpus is NFC.
- `-DFFZ_NUCLEO_SUBSTRING_BUGCOMPAT` reproduces a nucleo off-by-one (non-ASCII
  substring at the string tail). **Off by default** — production stays correct;
  the differential test enables it to prove exact equivalence.

## Size

| build | total (`-Os`) | tables |
|---|---|---|
| exact (default) | **~25 KB** | normalize 2.2 KB + casefold 4.4 KB + class 4.8 KB |
| `-DFFZ_COMPACT_CLASS` | **~20 KB** | normalize 2.2 KB + casefold 4.4 KB |

Tables are losslessly compressed (verified by the differential test): the class
table is a delta-varint stream with checkpoints (11.6 KB → 4.8 KB); case folding
is dict-packed parallel arrays (8.2 KB → 4.4 KB). Casefold can be trimmed
further by script range in `gen_unicode_tables.py` (Latin+Greek+Cyrillic →
~2 KB); CJK has no case folding, so trimming never affects CJK search.

## Multi-threading

The scoring pass of `ffz_corpus_filter` can run multi-threaded (Win32 / pthreads):

```c
ffz_corpus_filter(c, q, qlen, cm, nm, mode, ffz_parallel_off(),   limit, &r); // serial (default)
ffz_corpus_filter(c, q, qlen, cm, nm, mode, ffz_parallel_auto(),  limit, &r); // half the CPUs
ffz_corpus_filter(c, q, qlen, cm, nm, mode, ffz_parallel_with(8), limit, &r); // explicit count
```

`{parallel:false, threads:0}` is the default (off). When on, `threads==0`
auto-selects half the logical CPUs **capped at 8**; a positive count is used
verbatim and **may exceed 8**, but a **global hard ceiling of (cpu-1)** is always
enforced (leaves one core free) and the count is also clamped to the item count.
Corpora below 512 items always run serial. Results are **deterministic and
identical** to the serial path regardless of thread count. Each call (and each
worker) uses its own matcher, so concurrent filters never race. From Dart, pass
the bool + count straight through the FFI binding.

## Memory & leaks

C is manually managed; every `*_new`/`*_add`/filter has a matching
`*_free`/`ffz_results_free`. `make leak` builds with `-DFFZ_TRACK_ALLOC` (a
counting allocator via macro interposition) and asserts the live-block count
returns to baseline after every teardown — across matcher/pattern cycles and
serial + parallel corpus lifecycles — so missing or late frees fail loudly.

## Errors & crash debugging

Two failure classes, handled differently:

- **Recoverable errors are catchable in Dart.** Library-load/symbol failures and
  out-of-memory in `filter` surface as `FuzzyException`; misuse (e.g. use after
  `dispose`) throws `StateError`. The engine is hardened to *degrade, not crash*:
  allocations drop-on-OOM, scratch is bounded, no recursion, invalid UTF-8 →
  U+FFFD. Wrap calls in `try/catch` for an actionable Dart error.

- **Hard native faults are NOT catchable** — but they are *localizable*. A
  genuine memory fault (segfault/abort) terminates the process; `dart:ffi`
  cannot turn a native signal into a Dart exception. The optional crash handler
  (below) prints a backtrace before the process dies instead of failing
  silently.

### Automatic debug/release split (no manual flags)

The native build is keyed off `CMAKE_BUILD_TYPE`, which Flutter sets per run mode
— so localization fidelity follows the build automatically:

| `flutter run` mode | build | `.so` (arm64) | crash handler | a native crash shows |
|---|---|---|---|---|
| debug | `-O1 -g`, **not stripped** | ~218 KB | compiled in | function + offset in-process (Windows: **`file:line`** via PDB) |
| profile | `-O2 -g`, **not stripped** (optimized + locatable) | ~190 KB | compiled in | same as debug, at full speed |
| release | `-Os`/`-Oz`, **stripped** + sidecar | **~32 KB** | omitted | OS tombstone + offline symbolize with the sidecar |

(`build_android.sh` mirrors this: default = release; `FFZ_SELFDEBUG=1` = the
locatable build. iOS/macOS use Xcode's per-config defaults + `.dSYM`.)

### Crash handler (debug/profile by default)

`FuzzyCrash.install()` registers a last-gasp handler (POSIX `sigaction` /
Windows `SetUnhandledExceptionFilter`) that, on a fault, writes a backtrace to
stderr (logcat on Android) and optionally a breadcrumb file, then re-raises so
your OS crash reporter still fires. It never pretends to recover.

```dart
final report = FuzzyCrash.lastReport();          // previous run's crash, if any
if (report != null) log('ffz last crash:\n$report');
FuzzyCrash.install(breadcrumbPath: '${dir.path}/ffz_crash.log');
```

A verified debug (MSVC+PDB) crash prints the faulting line directly:

```
*** ffz native crash: exception 0xc0000005 at 0x7ff6...
  #7  boom+0xa   (crash_harness.c:3)     <- exact faulting line
  #8  main+0x41  (crash_harness.c:10)
```

The handler is **only compiled into debug/profile** builds: walking the stack
in-process needs `.eh_frame` unwind tables across the whole library, which would
inflate the stripped release `.so` from ~32 KB to ~58 KB. A plain release lib
therefore omits it (`FuzzyCrash.install()` returns `false`) and you diagnose
release crashes from the OS tombstone + the shipped `libffz.so.debug` /
`.pdb` / `.dSYM` sidecar (`ndk-stack`/`addr2line`/Crashlytics-NDK). To force the
in-process handler into release anyway, build with `-DFFZ_CRASH_IN_RELEASE=ON`
(or `FFZ_CRASH_IN_RELEASE=1 scripts/build_android.sh`) — the ~58 KB path — and
ship the sidecar.

For local repro, the **ASan/UBSan** variant (the `sanitizers` CI job, or
`-O1 -g -fsanitize=address,undefined`) pinpoints the faulting line; the
differential test + CI fuzz/sanitizer runs keep the crash surface small.

## Flutter plugin

This directory **is** a Flutter FFI plugin (`pubspec.yaml` + `lib/ffuzzy.dart` +
`windows/`/`linux/`/`android/`/`ios/`/`macos/` native build). Depend on it with
`ffuzzy:` from pub.dev; the native library is built+bundled per platform
from this `CMakeLists.txt`. It is **C-only** — no Rust dependency (the Rust engine now lives in `benchmark/`
for comparison only). See `lib/ffuzzy.dart` for the Dart API.
