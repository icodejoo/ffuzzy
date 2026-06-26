# ffz (Dart/Flutter binding)

Idiomatic Dart FFI binding for the `ffz` C fuzzy matcher (`clang/`).

```dart
import 'package:ffz/ffz.dart';

final corpus = FfzCorpus();                 // loads ffz.dll / libffz.so
corpus.addAll(['src/main.rs', 'lib/ffz.dart', '中文搜索引擎']);

final hits = corpus.filter('src', mode: FfzMode.fuzzy, parallel: true, limit: 50);
for (final h in hits) {
  print('item ${h.index}  score ${h.score}  highlight ${h.indices}');
}
corpus.dispose();   // or let the NativeFinalizer reclaim it on GC
```

## API

- `FfzCorpus({String? libraryPath})` — open the native lib and create a corpus.
  Pass `libraryPath` to load a specific file (tests / non-bundled use).
- `add(String)`, `addAll(Iterable<String>)`, `int get length`.
- `filter(query, {mode, parallel, threads, limit, highlight}) → List<FfzHit>`.
  - `mode`: `FfzMode.{fuzzy,substring,prefix,postfix,exact}`.
  - `parallel`/`threads`: multi-threaded scoring (`threads:0` = auto, half CPUs
    capped at 8; hard ceiling cpu-1). Small corpora run single-threaded.
  - `limit:0` returns all matches; otherwise the top-N by score.
  - `highlight:false` skips reading match indices (a bit faster).
- `FfzHit(index, score, matchedKind, indices)`.
- `dispose()` — release native memory now (idempotent); otherwise a
  `NativeFinalizer` frees it on GC.

Memory: results are copied into Dart objects; the native `ffz_results` is freed
inside `filter`. The corpus handle is freed by `dispose()`/finalizer.

## Native library

The Dart side loads `ffz.dll` (Windows), `libffz.so` (Linux/Android), or expects
the symbols in the process (iOS/macOS, static-linked).

Build it with the project CMake (`clang/CMakeLists.txt`) — MSVC and GCC/Clang
both work. As a **Flutter FFI plugin**, add to the plugin `pubspec.yaml`:

```yaml
flutter:
  plugin:
    platforms:
      windows: { ffiPlugin: true }
      linux:   { ffiPlugin: true }
      android: { ffiPlugin: true }
      macos:   { ffiPlugin: true }
      ios:     { ffiPlugin: true }
```

and have each platform's native build include `clang/CMakeLists.txt` (Android &
desktop) or static-link the sources via a podspec (iOS/macOS). The CMake exports
`ffz_bundled_libraries` for Flutter to bundle.

For Android release `.so`s directly (NDK), see `clang/scripts/build_android.sh`.

## Transliteration / multi-key (pinyin, initials, romaji)

The C core supports multiple search keys per item (e.g. 张三 + "zhangsan" +
"zs") via `ffz_corpus_set_transliterator` (see `clang/include/ffz_corpus.h`).
The current Dart binding exposes single-key add; multi-key from Dart is a
planned addition (host generates keys, passes them at add time).
