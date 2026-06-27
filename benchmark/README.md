# ffuzzy

**English** | [中文](README.zh-CN.md)

High-performance fuzzy search for Flutter, powered by [`nucleo-matcher`](https://crates.io/crates/nucleo)
(Rust — the same engine behind the Helix editor) via [`flutter_rust_bridge`](https://pub.dev/packages/flutter_rust_bridge).
fzf-style subsequence matching, **~250–1600× faster** than common pure-Dart fuzzy libraries at hundreds-of-thousands scale.

## Features
- ⚡ **Very fast**: Rust nucleo engine + resident index — a single query over 488k items is ~3ms (multi-core; ~18ms single-threaded).
- 🎯 **Objects or strings**: `FuzzyMatcher<T>` returns the matched object directly; `FuzzyStringMatcher` returns the original list index.
- ✨ **Match highlighting**: returns the matched character indices, ready for highlighting.
- 🧵 **Sync + async**: `matchAsync` runs on a background thread, never blocking the UI.
- 🗂️ **Index control**: `buildIndices` / `freeIndices` on demand — you decide the memory footprint.
- 🔁 **Mutable**: `add` / `update` / `removeWhere` / `clear` / `refresh` maintain the index incrementally — no full rebuild.
- ⚙️ **Match modes**: `fuzzy` (default), `substring`, `prefix`, `word`; per-query `ignoreCase` / `mode` override; optional multi-core & incremental.

## Platforms
Android · iOS · macOS · Windows · Linux (native cross-compiled via cargokit).

**Web/WASM: supported but not recommended for production.** The Rust side is WASM-compatible (multi-core auto-degrades
to single-threaded) and the example can be built on Linux via the `Web Build (WASM)` GitHub Actions workflow
(`flutter_rust_bridge_codegen build-web`). But the **wasm payload is large** (~450KB+ before extra optimization), so on
web a lighter pure-Dart library is usually the better trade — ffuzzy's sweet spot is native (Android/iOS/desktop).
(build-web must run on Linux/CI: on Windows frb cannot spawn the `flutter`/`dart` `.bat` wrappers.)

## Install
```yaml
dependencies:
  ffuzzy: ^0.2.0
```
Run `flutter pub get`. Entry library: `package:ffuzzy/ffuzzy.dart` (every class ships a copy-pasteable example).

## Contents
- [Initialize](#initialize)
- [Quick start](#quick-start)
- [Which class to use](#which-class-to-use)
- [Core concept: building & freeing the index](#core-concept-building--freeing-the-index)
- [Incremental updates & cross-module sync](#incremental-updates--cross-module-sync)
- [FuzzyMatcher&lt;T&gt;](#fuzzymatchert)
- [FuzzyStringMatcher](#fuzzystringmatcher)
- [Standalone functions](#standalone-functions)
- [Data types](#data-types)
- [Match modes](#match-modes)
- [Full Flutter example (search box + highlight)](#full-flutter-example-search-box--highlight)
- [Performance](#performance)

---

## Initialize

`ffuzzy.ensureInitialized()` is **lazy and idempotent** — safe to call repeatedly.

```dart
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main() async {
  await ffuzzy.ensureInitialized(); // or await it right before first use
  runApp(const MyApp());
}
```

> ⚠️ Sync methods (`match` / `buildIndices` / `fuzzyFilter` …) require initialization to be complete.
> Async methods (`matchAsync`) ensure initialization internally.

---

## Quick start

```dart
import 'package:ffuzzy/ffuzzy.dart';

await ffuzzy.ensureInitialized();

final matcher = FuzzyMatcher<Game>(games, (g) => g.name) // project by name
  ..buildIndices();                                       // build the index explicitly

for (final out in matcher.match('drgn', limit: 20)) {
  print('${out.obj.name}  score=${out.score}  highlight=${out.indices}');
}

final best = matcher.single('drgn'); // best one -> FuzzyOutput<Game>?; object via best?.obj
matcher.dispose();                    // destroy when done
```

---

## Which class to use

| Your data | Class | `match` returns | `single` returns |
|---|---|---|---|
| Objects / Maps (want the object back) | `FuzzyMatcher<T>` | `List<FuzzyOutput<T>>` (`.obj`) | `FuzzyOutput<T>?` |
| Plain string list (want the index) | `FuzzyStringMatcher` | `List<FuzzyHit>` (`.index`) | `FuzzyHit?` |
| One-off, no index | `fuzzyFilter` / `fuzzyMatchIndices` | `List<FuzzyHit>` / `FuzzyMatch?` | — |

> The three result types differ only in the first field: `FuzzyOutput.obj` (object) / `FuzzyHit.index` (list index) / `FuzzyMatch` (single string, no locator); all three carry `score` + `indices`.
> `limit` must be ≥ 0 (negative throws `ArgumentError`); `limit: 0` returns empty.

---

## Core concept: building & freeing the index

The index is an **optional speed cache** — you control when it costs memory:

- `match` / `matchAsync` **never auto-build/rebuild**: with an index they use it (fast); **without one they fall back to a full scan**
  (slow, but allocates no persistent index and never silently grows memory). Call `buildIndices()` (skips if present) or `refresh()` to go fast.
- So "forgot to `buildIndices()`" never crashes — it just searches slower (same as `indexed:false` / standalone functions).
  Check `hasIndices` to see whether you're in fast mode.

| Operation | Rust index | Dart data | `match` after |
|---|---|---|---|
| create only / `freeIndices()` | none | kept | yes (slow scan) |
| `buildIndices()` | built | kept | yes (fast) |
| `dispose()` | freed | all freed | no (throws `StateError`) |
| `refresh(src)` | rebuilt | replaced | yes (fast) |

> If an async search is in flight, `freeIndices` / `dispose` **wait for it to drain** before freeing — never freeing an index a background thread is using.

For large datasets, build the index off the UI thread with `await m.buildIndicesAsync()`.

---

## Incremental updates & cross-module sync

**A matcher holds its own snapshot; it does not observe external collections.** If you build `FuzzyMatcher(A)` and
later do `A.add(x)` elsewhere, the matcher **won't find `x`** — it doesn't know A changed. This is intentional.

The rule: **whoever changes the data feeds the change to the matcher**; the incremental API is cheap.

```dart
final m = FuzzyMatcher<Game>(games, (g) => g.name)..buildIndices();
m.add(newGame);                          // append one, straight into the Rust index, no rebuild
m.addAll(moreGames);                     // batch append
m.update(0, editedGame);                 // replace object at index 0
m.removeAt(2);                           // remove by index
final n = m.removeWhere((g) => g.disabled); // remove by predicate, returns count
m.clear();                               // clear all (instance kept)
m.refresh(reloadedGames);                // replace source + rebuild
```

> `add` only appends and does not disturb in-flight searches; `update` / `remove*` / `clear` change indices/content, so they
> **discard in-flight `matchAsync` results** (return empty; re-query on the new state). `FuzzyStringMatcher` has the same methods with `String`.
> `add` is a `&mut` op: if a `matchAsync` is reading the Rust index right then, `add` waits for it (frb guards with a read-write lock — no data race); brief stalls only with huge data + long in-flight searches.

---

## FuzzyMatcher&lt;T&gt;

Indexes any type `T`; `match` returns the matched **objects** (`FuzzyOutput<T>`).

### Create

```dart
final m1 = FuzzyMatcher<Game>(games, (g) => g.name);          // function projection
final m2 = FuzzyMatcher.key(jsonList, 'gameName');            // field-name projection (Map/JSON)
final m3 = FuzzyMatcher<Game>(games, (g) => '${g.name} ${g.id}'); // multi-field
final m4 = FuzzyMatcher<Game>(games, (g) => g.name, indexed: false); // no resident index
final m5 = FuzzyMatcher<Game>(games, (g) => g.name,           // custom config (copyWith)
    config: kDefaultFuzzyConfig.copyWith(ignoreCase: false));
```

### Search

```dart
final m = FuzzyMatcher<Game>(games, (g) => g.name)..buildIndices();

final List<FuzzyOutput<Game>> hits = m.match('dragon', limit: 20); // by score desc
for (final h in hits) { print(h.obj); print(h.score); print(h.indices); }

final hitsAsync = await m.matchAsync('dragon', limit: 20); // background thread

final FuzzyOutput<Game>? best = m.single('dragon');
final Game? obj = best?.obj;

// per-query override of mode / ignoreCase
final pre = m.match('dra', mode: MatchMode.prefix, ignoreCase: false);
```

### Lifecycle

```dart
final m = FuzzyMatcher.key(jsonList, 'gameName');
m.hasIndices;             // false
m.buildIndices();         // build (or buildIndicesAsync() for big data)
m.match('gold');          // fast
m.freeIndices();          // free Rust index only (Dart source/projection kept)
m.match('gold');          // still works, degraded to slow scan
m.buildIndices();         // rebuild in ms (from kept projection)
m.dispose();              // destroy both sides
```

### Members

| Member | Signature | Notes |
|---|---|---|
| ctor | `FuzzyMatcher<T>(List<T> items, String Function(T) stringOf, {bool indexed = true, FuzzyConfig config = kDefaultFuzzyConfig})` | `stringOf` projects a searchable string |
| ctor | `static FuzzyMatcher<Map<String,dynamic>> FuzzyMatcher.key(List<Map<String,dynamic>> items, String key, {bool indexed = true, FuzzyConfig config = kDefaultFuzzyConfig})` | search by field name |
| `buildIndices` | `void buildIndices()` | build (skips if present) |
| `buildIndicesAsync` | `Future<void> buildIndicesAsync()` | build off the UI thread (Utf32 conversion on a worker; `stringOf` projection still on the caller) |
| `add` / `addAll` | `void add(T)` / `void addAll(Iterable<T>)` | append; goes straight into the index (no rebuild) |
| `update` | `void update(int index, T item)` | replace object at index |
| `removeAt` / `removeWhere` | `void removeAt(int)` / `int removeWhere(bool Function(T))` | remove by index / predicate |
| `clear` | `void clear()` | clear all (instance kept) |
| `refresh` | `void refresh(List<T>)` | replace source + rebuild |
| `match` | `List<FuzzyOutput<T>> match(String query, {int? limit, bool? ignoreCase, MatchMode? mode})` | sync; needs an index. `ignoreCase`/`mode` override per query |
| `matchAsync` | `Future<List<FuzzyOutput<T>>> matchAsync(String query, {int? limit, bool? ignoreCase, MatchMode? mode})` | async; discards result if `refresh`/`dispose` happens meanwhile |
| `single` / `singleAsync` | `FuzzyOutput<T>? single(String, {bool? ignoreCase, MatchMode? mode})` / async | best one, or null |
| `freeIndices` | `void freeIndices()` | free Rust index only |
| `dispose` / `disposeAndWait` | `void dispose()` / `Future<void> disposeAndWait()` | destroy both sides (the latter waits for in-flight) |
| `length` / `hasIndices` / `isDisposed` | `int` / `bool` / `bool` | status |

---

## FuzzyStringMatcher

For `List<String>`; `match` returns `FuzzyHit` carrying the original list index.

```dart
await ffuzzy.ensureInitialized();
final m = FuzzyStringMatcher(['alpha', 'beta', 'alphabet'])..buildIndices();

for (final h in m.match('alph', limit: 10)) {
  print(m.items[h.index]); // index back into the list
  print(h.score);
  print(h.indices);
}

final FuzzyHit? best = m.single('bet');   // text via m.items[best!.index]
m.refresh(['gold', 'golden']);
m.dispose();
```

Same methods as `FuzzyMatcher` (lifecycle, mutation, `refresh`, `single`/`singleAsync`); only the types differ
(`match`→`List<FuzzyHit>`, args take `String`). Extra read-only props: `List<String> items`, `bool indexed`, `FuzzyConfig config`.

---

## Standalone functions

For one-off queries without an index. **Call `await ffuzzy.ensureInitialized()` first.**

```dart
const cfg = kDefaultFuzzyConfig;
int? score = fuzzyMatch(query: 'dt', haystack: 'Dragon Treasure', config: cfg); // null if no match
FuzzyMatch? m = fuzzyMatchIndices(query: 'dt', haystack: 'Dragon Treasure', config: cfg);
final hits = fuzzyFilter(query: 'drg', items: ['Dragon', 'Golden'], config: cfg, limit: 50);
final hitsAsync = await fuzzyFilterAsync(query: 'drg', items: ['Dragon'], config: cfg);
```

| Function | Signature |
|---|---|
| `fuzzyMatch` | `int? fuzzyMatch({required String query, required String haystack, required FuzzyConfig config})` |
| `fuzzyMatchIndices` | `FuzzyMatch? fuzzyMatchIndices({required String query, required String haystack, required FuzzyConfig config})` |
| `fuzzyFilter` | `List<FuzzyHit> fuzzyFilter({required String query, required List<String> items, required FuzzyConfig config, int? limit})` |
| `fuzzyFilterAsync` | `Future<List<FuzzyHit>> fuzzyFilterAsync({required String query, required List<String> items, required FuzzyConfig config, int? limit})` |

---

## Data types

```dart
class FuzzyOutput<T> { final T obj; final int score; final Uint32List indices; }       // FuzzyMatcher result
class FuzzyHit       { final int index; final int score; final Uint32List indices; }    // FuzzyStringMatcher result
class FuzzyMatch     { final int score; final Uint32List indices; }                     // single-string result

// Match config (use kDefaultFuzzyConfig.copyWith to change just a field)
const kDefaultFuzzyConfig = FuzzyConfig(
  ignoreCase: true,      // case-insensitive (also overridable per query: match(q, ignoreCase: ...))
  normalize: true,       // Unicode normalization (Fuzzy mode only)
  preferPrefix: true,    // prefix-first ranking (Fuzzy mode only)
  mode: MatchMode.fuzzy, // match mode (also overridable per query: match(q, mode: ...))
  parallel: true,        // multi-core for big Fuzzy searches (kicks in above ~20k candidates; serial otherwise/web)
  incremental: false,    // incremental cache (Fuzzy + FuzzyCorpus only; good for type-as-you-go on web/low-core)
);

final c = kDefaultFuzzyConfig.copyWith(mode: MatchMode.substring);
```

### Match modes

| Mode | Meaning | Typical use |
|---|---|---|
| `fuzzy` (default) | subsequence fuzzy + scored ranking (typo-tolerant; space-separated terms match in any order) | command palette, typo-tolerant search |
| `substring` | contains (`contains`) | "contains" filter |
| `prefix` | starts-with (`startsWith`) | autocomplete, prefix filter |
| `word` | **whole-string equality** (equals — **not** word-boundary matching) | exact match / lookup |

- **`mode` / `ignoreCase` are per-query overridable**: `m.match(q, mode: MatchMode.prefix, ignoreCase: false)`. No rebuild needed to switch modes on the same matcher.
- Simple modes (substring/prefix/word) return in **original order**, unranked, and stop once `limit` is reached.
- `parallel` / `incremental` are matcher-level (set via `config`), not per-query: `incremental` depends on "the next query extends the last", so it shines for type-as-you-go (`fuzzy` + `FuzzyCorpus` only).
- See [Performance](#performance) for measured gains.

---

## Full Flutter example (search box + highlight)

Copy-paste runnable: live fuzzy filtering with highlighted matched characters.

```dart
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main() async {
  await ffuzzy.ensureInitialized();
  runApp(const MaterialApp(home: SearchDemo()));
}

class SearchDemo extends StatefulWidget {
  const SearchDemo({super.key});
  @override
  State<SearchDemo> createState() => _SearchDemoState();
}

class _SearchDemoState extends State<SearchDemo> {
  static const _items = ['Dragon Treasure', 'Golden Fortune', 'Super Gems 1000', 'Lucky Dragon'];
  late final FuzzyStringMatcher _matcher = FuzzyStringMatcher(_items)..buildIndices();
  List<FuzzyHit> _hits = const [];
  int _token = 0; // anti-race: only accept the latest query's result

  @override
  void dispose() { _matcher.dispose(); super.dispose(); }

  Future<void> _onChanged(String q) async {
    final token = ++_token;
    if (q.isEmpty) {
      setState(() => _hits = [
        for (int i = 0; i < _items.length; i++) FuzzyHit(index: i, score: 0, indices: Uint32List(0)),
      ]);
      return;
    }
    final hits = await _matcher.matchAsync(q, limit: 50); // background thread, never blocks UI
    if (!mounted || token != _token) return;              // drop stale results
    setState(() => _hits = hits);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('ffuzzy')),
      body: Column(children: [
        Padding(
          padding: const EdgeInsets.all(12),
          child: TextField(autofocus: true, onChanged: _onChanged,
              decoration: const InputDecoration(border: OutlineInputBorder(), hintText: 'fuzzy query')),
        ),
        Expanded(
          child: ListView.builder(
            itemCount: _hits.length,
            itemBuilder: (_, i) {
              final hit = _hits[i];
              final text = _items[hit.index];
              final matched = hit.indices.toSet();
              // indices are CHARACTER (rune) indices — split by runes when highlighting;
              // using text[c] / UTF-16 indexing would misalign emoji and other non-BMP chars.
              final runes = text.runes.toList();
              return ListTile(
                title: Text.rich(TextSpan(children: [
                  for (int c = 0; c < runes.length; c++)
                    TextSpan(
                      text: String.fromCharCode(runes[c]),
                      style: TextStyle(
                        fontWeight: matched.contains(c) ? FontWeight.bold : FontWeight.normal,
                        color: matched.contains(c) ? Theme.of(context).colorScheme.primary : null,
                      ),
                    ),
                ])),
              );
            },
          ),
        ),
      ]),
    );
  }
}
```

> The example uses `matchAsync` + a `_token` version, the recommended pattern for a search box: non-blocking and drops stale results.
> Small datasets (a few thousand) are fine with sync `match`. **Highlight by `runes`** (`indices` are character indices, not UTF-16 code units).

---

## Performance

### vs other Dart fuzzy libraries (488,600 items, Windows x86_64 release)

| Library | Per-query | vs ffuzzy |
|---|---:|---:|
| **ffuzzy (cached)** | **~3 ms** (parallel) | **1×** |
| string_similarity | ~755 ms | ~250× |
| fuzzy_bolt | ~1276 ms | ~420× |
| fuzzy (Fuse) | ~1585 ms | ~520× |
| fuzzywuzzy | ~5066 ms | ~1600× |

> `nucleo` is subsequence fuzzy (fzf-style), solving a different problem than edit-distance/Dice libraries; the table compares throughput of "similar feature, different implementation" — match sets are not strictly equivalent.

### Effect of each API / switch on speed & memory (488,600 items, same machine)

One-time: `buildIndices` ~120ms; resident Utf32 index ~35MB (488k). Baseline = default Fuzzy + parallel, ~2.9ms/query.

| API / switch | Default | Effect | Measured gain | Memory / cost |
|---|---|---|---:|---|
| `parallel` (multi-core) | on | chunked multi-core for big Fuzzy search (>20k candidates) | **6.3×** (18.1→2.9ms) | momentary multi-core during search only; threads = cores−2 (leaves UI headroom), ≤3 cores → serial |
| `mode: prefix` | — | prefix (vs parallel fuzzy) | **3.5×** (→0.85ms) | none |
| `mode: word` | — | whole-string equality (vs parallel fuzzy) | **2.3×** (→1.30ms) | none |
| `mode: substring` | — | contains (vs parallel fuzzy; vs **serial** fuzzy ~6×) | ~1.0× (→2.9ms) | none |
| `incremental` (**serial**/web/low-core) | off | type-as-you-go rescans only the last hit set | **2.2×** (7 keystrokes 117→53ms) | caches last hit indices (≤20k u32) |
| `incremental` (multi-core) | off | parallel is already fast; incremental ~neutral | ~1.0× (never slower) | same |
| `ignoreCase` simple modes (lazy fold cache) | auto | first query builds a lowercase copy, then reuses | **13×** after first (39→3ms) | one lowercase copy (≈ source size, built lazily on first query; invalidated on mutate/free) |
| `buildIndicesAsync()` | — | move index build to a worker thread | no speedup but **never blocks UI** | same total time as sync build |

Highlights:
- **Multi-core is the biggest lever for big Fuzzy data (6×+)** — automatic on desktop / multi-core Android.
- **Simple modes (prefix/word/substring)** run single-threaded; at large scale their edge over *parallel* fuzzy narrows (substring ~1×), but they offer semantics fuzzy can't (prefix/equals/contains), and the edge is larger on small data or serial (web).
- **`incremental` is for web/WASM/low-core** type-as-you-go: where multi-core can't help, it gives ~2×; on multi-core it's neutral and never slower (opt-in, default off).
- **`ignoreCase` simple modes** lazily build a lowercase copy on first query (~40ms one-time @488k); steady state then matches the case-sensitive path.

> Tight on memory: `freeIndices()` releases the Rust index when idle; `buildIndices()` / `refresh()` restore in ms (Dart source is kept).

---

## Development

```bash
flutter test                                  # Dart tests
cargo test --manifest-path rust/Cargo.toml    # Rust unit tests
cd example && flutter run -d <device>         # run the example app
flutter_rust_bridge_codegen generate          # regenerate bindings after editing Rust
```

The first build cross-compiles Rust automatically via cargokit; you need the Rust toolchain and the target's platform toolchain installed.
