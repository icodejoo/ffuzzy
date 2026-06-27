# ffuzzy

Fast fuzzy search for Flutter, powered by a compact **C** engine via `dart:ffi`.

`ffuzzy` is a byte-for-byte reimplementation of [`nucleo`](https://github.com/helix-editor/nucleo)
(the matcher behind the Helix editor) in portable C. No Rust toolchain, no
codegen — the engine is a few source files that every platform's SDK compiles
on its own. The native library is **~32 KB** stripped.

- **Fast** — meets or beats the Rust `nucleo` engine: faster in every
  multi-threaded configuration and on `substring` across the board, at parity on
  CJK and single-threaded `fuzzy`. ~100k-item corpus filters in ~1.4 ms.
- **Tiny** — ~32 KB native `.so` (arm64), pure C, zero third-party deps.
- **All platforms** — Android, iOS, macOS, Linux, Windows. Sources compile and
  bundle per-platform; consumers need no extra toolchain. *(Web is not supported
  — `dart:ffi` is unavailable on web.)*
- **Match modes** — fuzzy (fzf-style, with `! ^ ' $` operators), substring,
  prefix, postfix, exact.
- **Multi-threaded** and **async** scans for large corpora without UI jank.
- **Hit highlighting** with correct Unicode (codepoint → UTF-16) offsets.
- **Unicode / CJK** — diacritic + full simple case folding; CJK matched directly.
- **Multi-key search** — attach host-computed pinyin / romaji / initials so a
  CJK item is findable by typing latin.

## Install

```yaml
dependencies:
  ffuzzy: ^0.3.0
```

## Quick start

```dart
import 'package:ffuzzy/ffuzzy.dart';

final corpus = FuzzyCorpus();
corpus.addAll(['src/main.dart', 'lib/widget.dart', 'README.md', '中文搜索']);

final hits = corpus.filter('srcmn', parallel: true, limit: 50);
for (final h in hits) {
  print('${h.index}  score=${h.score}');
}

corpus.dispose(); // or let the NativeFinalizer reclaim it on GC
```

> An `FuzzyCorpus` owns native memory and must be used only on the isolate that
> created it. Every call is synchronous on the calling isolate — for a large
> corpus either use [`filterAsync`](#filtering) or run the corpus on a background
> isolate so `filter` doesn't jank the UI.

## Use cases

Type-as-you-go search over file paths, command palettes, contact/song lists,
log lines, or any in-memory list where you want fzf-quality ranking at native
speed — especially large lists (tens of thousands of items) and CJK content.

---

# API

Everything is exported from `package:ffuzzy/ffuzzy.dart`.

## `FuzzyCorpus`

A resident corpus you build once and filter many times.

### Constructor

```dart
FuzzyCorpus({
  bool matchPaths = false,   // tune delimiters for path-like text
  bool preferPrefix = false, // bias scoring toward matches near the start
  String? libraryPath,       // load a specific native lib (tests / non-bundled)
})
```

Throws [`FuzzyException`](#fuzzyexception) if the native library can't be loaded.

### Building the corpus

| Member | Description |
|---|---|
| `void add(String item)` | Append one item. |
| `void addAll(Iterable<String> items)` | Append many (insertion order is the item `index`). |
| `void addKeyed(String item, List<FuzzyKey> keys)` | Append `item` with [alternate search keys](#multi-key--cjk-transliteration). The original text is added automatically. |
| `int get length` | Number of items currently in the corpus. |
| `void clear()` | Remove all items; the corpus stays usable. |

### Filtering

```dart
List<FuzzyHit> filter(
  String query, {
  FuzzyMode mode = FuzzyMode.fuzzy,
  FuzzyCase caseMatching = FuzzyCase.smart,
  FuzzyNorm normalization = FuzzyNorm.smart,
  bool parallel = false,
  int threads = 0,
  int limit = 0,        // 0 = return all matches
  bool highlight = true, // false skips reading match indices (faster)
})
```

```dart
Future<List<FuzzyHit>> filterAsync(String query, { /* same options */ })
```

`filterAsync` runs the native scan + result marshaling on a background isolate,
so a large corpus never janks the UI. Combine with `parallel: true` to also fan
the C scan across threads. Multiple `filterAsync` calls may overlap safely.
While one is in flight, `add` / `addKeyed` / `clear` / `dispose` throw
[`StateError`](#errors) (mutating shared native memory would be a use-after-free).

**Threading:** `parallel: false` (default) is single-threaded. When `parallel`
is on, `threads: 0` auto-selects half the logical CPUs (capped at 8); a positive
count is used verbatim but a hard ceiling of `cpu - 1` always applies. Corpora
below 512 items always run serial. Results are deterministic and identical to
the serial path regardless of thread count.

### Lifecycle

| Member | Description |
|---|---|
| `void dispose()` | Free native memory now. Idempotent. Throws [`StateError`](#errors) if a `filterAsync` is still in flight — await pending futures first. |

A `NativeFinalizer` frees the corpus automatically if you forget to `dispose`,
but calling `dispose` is preferred for prompt release.

## `FuzzyHit`

One search result.

| Field | Type | Description |
|---|---|---|
| `index` | `int` | The item's insertion order in the corpus. |
| `score` | `int` | Match score (higher is better). |
| `matchedKind` | `FuzzyKeyKind` | Which kind of key matched (original / pinyin / …). |
| `matchedKey` | `int` | Which key of the item matched (`0` == original). |
| `indices` | `List<int>` | Matched **codepoint** positions in the matched key — convert with [`fuzzyCodepointToUtf16`](#highlighting) before indexing a Dart `String`. |

## Enums

```dart
enum FuzzyMode { fuzzy, substring, prefix, postfix, exact }
enum FuzzyCase { respect, ignore, smart }   // case handling
enum FuzzyNorm { never, smart }             // Unicode normalization (diacritics)
enum FuzzyKeyKind { original, pinyin, initials, romaji, custom }
```

- **`FuzzyMode.fuzzy`** also parses the query into space-separated terms and
  fzf-style operators: `!` negate, `^` prefix, `'` substring, `$` suffix — so
  `'lib parse'` is an AND of two terms. The other modes treat the whole query as
  one literal atom.
- **`FuzzyKeyKind`** has a `.code` getter (`original`=0 … `romaji`=3, `custom`=100)
  for use with `FuzzyKey`.

## Highlighting

```dart
List<int> fuzzyCodepointToUtf16(String text, List<int> codepointIndices)
```

`FuzzyHit.indices` are codepoint positions; Dart strings are UTF-16. Convert
before building a `TextSpan` so emoji / astral characters don't misalign:

```dart
final hit = corpus.filter('src').first;
final text = items[hit.index];
final marks = fuzzyCodepointToUtf16(text, hit.indices).toSet();
final spans = [
  for (var i = 0; i < text.length; i++)
    TextSpan(
      text: text[i],
      style: marks.contains(i) ? boldStyle : null,
    ),
];
```

## Multi-key / CJK transliteration

The matcher has no built-in pinyin/romaji dictionary — you compute alternate
keys host-side and attach them, so a CJK item is findable by typing latin.

```dart
class FuzzyKey {
  const FuzzyKey(String text, {int kind = 1 /* pinyin */});
  FuzzyKey.kind(String text, FuzzyKeyKind kind);
}

corpus.addKeyed('张三', [
  FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zs', FuzzyKeyKind.initials),
]);

final h = corpus.filter('zs').first;
// h.matchedKind == FuzzyKeyKind.initials, h.matchedKey == 2
```

## Errors

- **Recoverable** errors are catchable: failed library/symbol load and
  out-of-memory surface as `FuzzyException`; misuse (use after `dispose`, mutate
  while a `filterAsync` is in flight) throws `StateError`. The engine is hardened
  to degrade rather than crash (drop-on-OOM, bounded scratch, no recursion,
  invalid UTF-8 → U+FFFD).
- **Hard native faults** (segfault/abort) can't become Dart exceptions — see
  [`FuzzyCrash`](#fuzzycrash).

### `FuzzyException`

```dart
class FuzzyException implements Exception { final String message; }
```

## `FuzzyCrash`

Optional, opt-in last-gasp handler for **non-recoverable** native faults. It
prints a backtrace to stderr (logcat on Android) just before the process dies
and, with a `breadcrumbPath`, writes the same report to a file so you can show
"last crash" on the next launch. Install once at startup.

```dart
// previous run's crash, if any (also clears it)
final report = FuzzyCrash.lastReport();
if (report != null) log('ffuzzy last crash:\n$report');

// returns true if the native handler was installed
FuzzyCrash.install(breadcrumbPath: '${dir.path}/ffuzzy_crash.log');
```

| Member | Signature | Description |
|---|---|---|
| `install` | `static bool install({String? breadcrumbPath, String? libraryPath})` | Register the handler. Returns `false` if the library lacks the symbol (e.g. a stripped release build that omits it). |
| `lastReport` | `static String? lastReport({String? breadcrumbPath})` | Read and clear the crash report left by a previous run, or `null`. |

Backtrace readability follows the build automatically: debug/profile keep
symbols (Windows shows `file:line`); stripped release prints offsets you
symbolize offline with the shipped `.debug` / `.pdb` / `.dSYM`. See
[`doc/INTERNALS.md`](doc/INTERNALS.md) for the debug/release split.

---

## Platforms & how the native library ships

`ffuzzy` is an FFI plugin: the C sources are compiled and bundled per platform
(Android NDK / CMake, iOS & macOS static-linked via podspec, Linux & Windows
CMake). Consumers need **no** Rust, no extra toolchain — just the standard
platform SDK. The Dart side loads `ffz.dll` / `libffz.so` or resolves
static-linked symbols via `DynamicLibrary.process()` on Apple.

## Performance

Real-device comparison (Flutter Windows, profile mode, 100k items, C engine vs
the Rust `nucleo` engine):

| | C (ffuzzy) | Rust (nucleo) |
|---|---|---|
| resident corpus memory | 15.25 MB | 16.54 MB |
| filter (fuzzy, top-50) | 1.36 ms | 1.65 ms |

The full methodology, the differential-test guarantee (6210/6210 byte-identical
to nucleo), Unicode coverage, sizing, and the engine design live in
[`doc/INTERNALS.md`](doc/INTERNALS.md). The deprecated Rust engine is retained
under [`benchmark/`](benchmark/) purely to reproduce these comparisons.

## License

MIT — see [LICENSE](LICENSE).
