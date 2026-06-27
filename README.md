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
- **Search any object** — `FuzzyCorpus<T>` searches a `List<T>`; hits carry the
  original object (`hit.obj`).
- **Match modes as methods** — `fuzzy` (fzf-style, with `! ^ ' $` operators),
  `substring`, `prefix`, `postfix`, `exact`; each with an `…Async` twin.
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

// Plain strings:
final corpus = FuzzyCorpus.strings(['src/main.dart', 'lib/widget.dart', '中文搜索']);
for (final h in corpus.fuzzy('srcmn', parallel: true, limit: 50)) {
  print('${h.obj}  score=${h.score}');   // h.obj is the matched String
}
corpus.dispose();                          // or let the NativeFinalizer reclaim it

// Any object — give a `stringOf` extractor; hits carry the object:
final files = FuzzyCorpus<File>(myFiles, stringOf: (f) => f.path);
final hit = files.prefix('lib/').firstOrNull;   // hit.obj is a File
```

> A `FuzzyCorpus` owns native memory and must be used only on the isolate that
> created it. The mode methods are synchronous on the calling isolate — for a
> large corpus use the `…Async` twins (e.g. [`fuzzyAsync`](#search-modes)) or run
> the corpus on a background isolate so searching doesn't jank the UI.

## Use cases

Type-as-you-go search over file paths, command palettes, contact/song lists,
log lines, or any in-memory list where you want fzf-quality ranking at native
speed — especially large lists (tens of thousands of items) and CJK content.

---

# API

Everything is exported from `package:ffuzzy/ffuzzy.dart`.

## `FuzzyCorpus<T>`

A resident corpus of `T` items you build once and search many times.

### Constructors

```dart
FuzzyCorpus<T>(
  Iterable<T> items, {
  required String Function(T) stringOf, // searchable text for each item
  FuzzyOptions options = const FuzzyOptions(), // default search options
  bool matchPaths = false,   // tune delimiters for path-like text
  bool preferPrefix = false, // bias scoring toward matches near the start
  String? libraryPath,       // load a specific native lib (tests / non-bundled)
})

// Convenience for plain strings (the item is its own search text):
static FuzzyCorpus<String> FuzzyCorpus.strings(Iterable<String> items, {…})
```

Throws [`FuzzyException`](#fuzzyexception) if the native library can't be loaded.

### Building & mutating

| Member | Description |
|---|---|
| `void add(T item)` | Append one item. |
| `void addAll(Iterable<T> items)` | Append many (insertion order is the item `index`). |
| `void addKeyed(T item, List<FuzzyKey> keys)` | Append `item` with [alternate search keys](#multi-key--cjk-transliteration). The original text (`stringOf(item)`) is added automatically. |
| `void update(int index, T item)` | Replace the item at `index` (drops its alternate keys). |
| `void removeAt(int index)` | Remove the item at `index`. |
| `void removeWhere(bool Function(T) test)` | Remove every matching item. |
| `void refresh()` | Rebuild after the text returned by `stringOf` changed for existing items. |
| `void clear()` | Remove all items; the corpus stays usable. |
| `int get length` | Number of items currently in the corpus. |

> The native corpus is append-only, so `update` / `removeAt` / `removeWhere` /
> `refresh` rebuild it in O(n). Cheap for occasional edits; for heavy churn
> prefer rebuilding in a batch.

### Search modes

Each match mode is a method returning `List<FuzzyHit<T>>`, plus an `…Async` twin
returning `Future<List<FuzzyHit<T>>>` that runs on a background isolate:

```dart
List<FuzzyHit<T>> fuzzy(String query, {…overrides});      Future<…> fuzzyAsync(…);
List<FuzzyHit<T>> substring(String query, {…overrides});  Future<…> substringAsync(…);
List<FuzzyHit<T>> prefix(String query, {…overrides});     Future<…> prefixAsync(…);
List<FuzzyHit<T>> postfix(String query, {…overrides});    Future<…> postfixAsync(…);
List<FuzzyHit<T>> exact(String query, {…overrides});      Future<…> exactAsync(…);
```

- **`fuzzy`** parses the query into space-separated terms and fzf-style operators
  (`!` negate, `^` prefix, `'` substring, `$` suffix) — so `'lib parse'` is an
  AND of two terms. The other modes treat the whole query as one literal atom.
- **Overrides** (`{FuzzyCase? caseMatching, FuzzyNorm? normalization, bool? parallel,
  int? threads, int? limit, bool? highlight}`): each non-null argument overrides
  the corresponding field of the corpus's [`FuzzyOptions`](#fuzzyoptions) for that
  call only. e.g. `corpus.fuzzy(q, limit: 50)`.

`…Async` calls may overlap safely (each gets its own native matcher). While one
is in flight, any mutation (`add`/`update`/`removeAt`/`clear`/…) or `dispose`
throws [`StateError`](#errors) (it would be a native use-after-free).

### Lifecycle

| Member | Description |
|---|---|
| `void dispose()` | Free native memory now. Idempotent. Throws [`StateError`](#errors) if an async search is still in flight — await pending futures first. |

A `NativeFinalizer` frees the corpus automatically if you forget to `dispose`,
but calling `dispose` is preferred for prompt release.

## `FuzzyOptions`

Bundles the per-search settings. Set corpus-wide defaults on the constructor;
override individual fields per call via the mode-method named params. Optional —
every field has a default, so `const FuzzyOptions()` is the common base.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `caseMatching` | `FuzzyCase` | `smart` | case handling |
| `normalization` | `FuzzyNorm` | `smart` | diacritic normalization |
| `parallel` | `bool` | `false` | multi-threaded scoring |
| `threads` | `int` | `0` | `0` = auto (half the CPUs, capped at 8; hard ceiling cpu-1; <512 items always serial) |
| `limit` | `int` | `0` | max hits (`0` = all) |
| `highlight` | `bool` | `true` | `false` skips reading match indices (faster) |

`FuzzyOptions` also has `copyWith(...)`. Example:

```dart
final corpus = FuzzyCorpus.strings(items,
    options: const FuzzyOptions(parallel: true, limit: 50));
corpus.fuzzy('foo');               // uses parallel + limit 50
corpus.fuzzy('bar', limit: 10);    // same defaults, but limit overridden to 10
```

## `FuzzyHit<T>`

One search result.

| Field | Type | Description |
|---|---|---|
| `obj` | `T` | The original item that matched. |
| `index` | `int` | The item's insertion order in the corpus. |
| `score` | `int` | Match score (higher is better). |
| `matchedKind` | `FuzzyKeyKind` | Which kind of key matched (original / pinyin / …). |
| `matchedKey` | `int` | Which key of the item matched (`0` == original). |
| `indices` | `List<int>` | Matched **codepoint** positions in the matched key — convert with [`fuzzyCodepointToUtf16`](#highlighting) before indexing a Dart `String`. |

## Enums

### `FuzzyCase` — case handling

| Value | Meaning |
|---|---|
| `respect` | Case-sensitive; `A` ≠ `a`. |
| `ignore` | Case-insensitive; `A` == `a`. |
| `smart` | Case-insensitive **unless** the query contains an uppercase letter, then case-sensitive (the default). |

### `FuzzyNorm` — Unicode normalization (diacritics)

| Value | Meaning |
|---|---|
| `never` | No folding; `café` ≠ `cafe`. |
| `smart` | Fold diacritics unless the query itself uses them; `cafe` matches `café` (the default). |

### `FuzzyKeyKind` — which key produced a hit

| Value | `.code` | Meaning |
|---|---|---|
| `original` | `0` | The item's own text (`stringOf`). |
| `pinyin` | `1` | A pinyin alternate key. |
| `initials` | `2` | An initials alternate key. |
| `romaji` | `3` | A romaji alternate key. |
| `custom` | `100` | Any host-defined kind (`>= 100`). |

The `FuzzyKeyKindCode` extension adds `int get code` (used when building a
[`FuzzyKey`](#fuzzykey)); `FuzzyKey.kind(...)` sets it for you.

## `FuzzyKey`

An alternate search key attached to an item via [`FuzzyCorpus.addKeyed`](#building--mutating).

| Member | Description |
|---|---|
| `final String text` | The alternate key's searchable text. |
| `final int kind` | The key's [`FuzzyKeyKind`](#fuzzykeykind--which-key-produced-a-hit) code (or any host value `>= 100`). |
| `const FuzzyKey(String text, {int kind = 1})` | `kind` defaults to `1` (pinyin). |
| `FuzzyKey.kind(String text, FuzzyKeyKind kind)` | Set `kind` from the enum (recommended). |

See [Multi-key / CJK transliteration](#multi-key--cjk-transliteration) for usage.

## Highlighting

```dart
List<int> fuzzyCodepointToUtf16(String text, List<int> codepointIndices)
```

`FuzzyHit.indices` are codepoint positions; Dart strings are UTF-16. Convert
before building a `TextSpan` so emoji / astral characters don't misalign:

```dart
final hit = corpus.fuzzy('src').first;
final text = hit.obj as String;                       // or stringOf(hit.obj)
final marks = fuzzyCodepointToUtf16(text, hit.indices).toSet();
final spans = [
  for (var i = 0; i < text.length; i++)
    TextSpan(text: text[i], style: marks.contains(i) ? boldStyle : null),
];
```

## Multi-key / CJK transliteration

The matcher has no built-in pinyin/romaji dictionary — you compute alternate
keys host-side and attach them (see [`FuzzyKey`](#fuzzykey)), so a CJK item is
findable by typing latin.

```dart
corpus.addKeyed(zhangsan, [
  FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zs', FuzzyKeyKind.initials),
]);

final h = corpus.fuzzy('zs').first;
// h.matchedKind == FuzzyKeyKind.initials, h.matchedKey == 2
```

## Errors

- **Recoverable** errors are catchable: failed library/symbol load and
  out-of-memory surface as `FuzzyException`; misuse (use after `dispose`, mutate
  while an async search is in flight) throws `StateError`. The engine is hardened
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
final report = FuzzyCrash.lastReport();        // previous run's crash, if any
if (report != null) log('ffuzzy last crash:\n$report');
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
