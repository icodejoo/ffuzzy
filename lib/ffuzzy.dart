/// ffuzzy — idiomatic Dart binding for the compact C fuzzy matcher, via dart:ffi.
///
/// ```dart
/// // Search a list of objects; results carry the original object via .obj.
/// final corpus = FuzzyCorpus<File>(files, stringOf: (f) => f.path);
/// for (final h in corpus.fuzzy('src', limit: 50)) {
///   final u16 = fuzzyCodepointToUtf16(h.obj.path, h.indices); // for TextSpan
///   print('${h.obj.path}  score=${h.score}  $u16');
/// }
/// corpus.dispose();                          // or rely on the NativeFinalizer
///
/// // Plain strings:
/// final c = FuzzyCorpus.strings(['a/b.dart', '中文搜索']);
/// c.substring('中文');                        // mode = a method, not a flag
/// ```
///
/// Default search options are set once on the constructor ([FuzzyOptions]) and
/// overridden per call field-by-field via the mode methods' named params.
///
/// NOTE: every call is synchronous and runs on the calling isolate; for a large
/// corpus use the `…Async` methods (background isolate) or create the corpus on
/// a background isolate so searching does not jank the UI. A `FuzzyCorpus` owns
/// a native pointer and must only be used on the isolate that created it.
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

/// Case handling (mirrors `ffz_case_matching`).
enum FuzzyCase { respect, ignore, smart }

/// Unicode normalization (mirrors `ffz_normalization`).
enum FuzzyNorm { never, smart }

/// Which key produced a hit. Custom host kinds use values >= 100.
enum FuzzyKeyKind { original, pinyin, initials, romaji, custom }

/// The raw C `ffz_key_kind` code for a kind (original=0..romaji=3, custom=100).
extension FuzzyKeyKindCode on FuzzyKeyKind {
  int get code => switch (this) {
        FuzzyKeyKind.original => 0,
        FuzzyKeyKind.pinyin => 1,
        FuzzyKeyKind.initials => 2,
        FuzzyKeyKind.romaji => 3,
        FuzzyKeyKind.custom => 100,
      };
}

FuzzyKeyKind _kindOf(int v) => switch (v) {
      0 => FuzzyKeyKind.original,
      1 => FuzzyKeyKind.pinyin,
      2 => FuzzyKeyKind.initials,
      3 => FuzzyKeyKind.romaji,
      _ => FuzzyKeyKind.custom,
    };

/// Search options. Set the corpus-wide defaults on the [FuzzyCorpus]
/// constructor; the mode methods ([FuzzyCorpus.fuzzy], etc.) override individual
/// fields per call via their named parameters. Every field has a sensible
/// default, so `const FuzzyOptions()` is the common starting point.
///
/// - [caseMatching]/[normalization]: per-query case & diacritic handling.
/// - [parallel]/[threads]: multi-threaded scoring (`threads:0` = auto, half the
///   CPUs capped at 8; a hard ceiling of cpu-1 always applies; corpora < 512
///   items run single-threaded regardless).
/// - [limit]: max hits to return (`0` = all).
/// - [highlight]: when false, match indices are skipped (faster, no [FuzzyHit.indices]).
class FuzzyOptions {
  final FuzzyCase caseMatching;
  final FuzzyNorm normalization;
  final bool parallel;
  final int threads;
  final int limit;
  final bool highlight;

  const FuzzyOptions({
    this.caseMatching = FuzzyCase.smart,
    this.normalization = FuzzyNorm.smart,
    this.parallel = false,
    this.threads = 0,
    this.limit = 0,
    this.highlight = true,
  });

  /// A copy with the given fields replaced (null keeps the current value).
  FuzzyOptions copyWith({
    FuzzyCase? caseMatching,
    FuzzyNorm? normalization,
    bool? parallel,
    int? threads,
    int? limit,
    bool? highlight,
  }) =>
      FuzzyOptions(
        caseMatching: caseMatching ?? this.caseMatching,
        normalization: normalization ?? this.normalization,
        parallel: parallel ?? this.parallel,
        threads: threads ?? this.threads,
        limit: limit ?? this.limit,
        highlight: highlight ?? this.highlight,
      );
}

/// An alternate search key for an item (e.g. host-computed pinyin/romaji), for
/// [FuzzyCorpus.addKeyed]. [kind] is a [FuzzyKeyKind] code (use `FuzzyKeyKind.x.code`)
/// or any host-defined value >= 100.
class FuzzyKey {
  final String text;
  final int kind;
  const FuzzyKey(this.text, {this.kind = 1 /* pinyin */});
  FuzzyKey.kind(this.text, FuzzyKeyKind kind) : kind = kind.code;
}

/// Thrown when the native library can't be loaded or a symbol is missing.
class FuzzyException implements Exception {
  final String message;
  const FuzzyException(this.message);
  @override
  String toString() => 'FuzzyException: $message';
}

/// One search result for a [FuzzyCorpus] of `T`.
///
/// [obj] is the original item that matched. [index] is its insertion order in
/// the corpus. [indices] are the matched **codepoint** positions within the
/// matched key — use [fuzzyCodepointToUtf16] before applying them to a Dart
/// `String`.
class FuzzyHit<T> {
  final T obj;
  final int index;
  final int score;
  final FuzzyKeyKind matchedKind;
  final int matchedKey; // which key of the item matched (0 == original)
  final List<int> indices;
  const FuzzyHit(this.obj, this.index, this.score, this.matchedKind,
      this.matchedKey, this.indices);

  @override
  String toString() =>
      'FuzzyHit(index: $index, score: $score, kind: $matchedKind, obj: $obj)';
}

/// Convert codepoint indices (as in [FuzzyHit.indices]) to UTF-16 code-unit
/// offsets into [text], suitable for Dart `String`/`TextSpan` highlighting.
/// (Dart strings are UTF-16; astral chars/emoji occupy two code units.)
List<int> fuzzyCodepointToUtf16(String text, List<int> codepointIndices) {
  if (codepointIndices.isEmpty) return const <int>[];
  final offsets = <int>[];
  var u16 = 0;
  for (final r in text.runes) {
    offsets.add(u16);
    u16 += r > 0xFFFF ? 2 : 1;
  }
  return [
    for (final c in codepointIndices)
      (c >= 0 && c < offsets.length) ? offsets[c] : u16
  ];
}

// ── native signatures ───────────────────────────────────────────────────────
typedef _NewCfgN = Pointer<Void> Function(Int32, Int32);
typedef _AddN = Void Function(Pointer<Void>, Pointer<Utf8>, Size);
typedef _AddKeyedN = Void Function(Pointer<Void>, Pointer<Utf8>, Size,
    Pointer<Pointer<Utf8>>, Pointer<Size>, Pointer<Int32>, Size);
typedef _LenN = Size Function(Pointer<Void>);
typedef _FreeN = Void Function(Pointer<Void>);
typedef _VoidPtrN = Void Function(Pointer<Void>);
typedef _FilterExN = Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, Size,
    Int32, Int32, Int32, Int32, Int32, Size);
typedef _RLenN = Size Function(Pointer<Void>);
typedef _RU32N = Uint32 Function(Pointer<Void>, Size);
typedef _RI32N = Int32 Function(Pointer<Void>, Size);
typedef _RNIdxN = Size Function(Pointer<Void>, Size);
typedef _RIdxN = Uint32 Function(Pointer<Void>, Size, Size);

class _Lib {
  _Lib(this.lib)
      : newCfg = lib.lookupFunction<_NewCfgN, Pointer<Void> Function(int, int)>(
            'ffz_ffi_new_cfg'),
        add = lib.lookupFunction<_AddN,
            void Function(Pointer<Void>, Pointer<Utf8>, int)>('ffz_ffi_add'),
        len = lib
            .lookupFunction<_LenN, int Function(Pointer<Void>)>('ffz_ffi_len'),
        clear = lib.lookupFunction<_VoidPtrN, void Function(Pointer<Void>)>(
            'ffz_ffi_clear'),
        filterEx = lib.lookupFunction<
            _FilterExN,
            Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
                int, int, int, int)>('ffz_ffi_filter_ex'),
        rLen = lib.lookupFunction<_RLenN, int Function(Pointer<Void>)>(
            'ffz_ffi_results_len'),
        rItem = lib.lookupFunction<_RU32N, int Function(Pointer<Void>, int)>(
            'ffz_ffi_results_item'),
        rScore = lib.lookupFunction<_RI32N, int Function(Pointer<Void>, int)>(
            'ffz_ffi_results_score'),
        rKind = lib.lookupFunction<_RI32N, int Function(Pointer<Void>, int)>(
            'ffz_ffi_results_kind'),
        rKey = lib.lookupFunction<_RU32N, int Function(Pointer<Void>, int)>(
            'ffz_ffi_results_key'),
        rNIdx = lib.lookupFunction<_RNIdxN, int Function(Pointer<Void>, int)>(
            'ffz_ffi_results_nindices'),
        rIdx =
            lib.lookupFunction<_RIdxN, int Function(Pointer<Void>, int, int)>(
                'ffz_ffi_results_index'),
        rFree = lib.lookupFunction<_VoidPtrN, void Function(Pointer<Void>)>(
            'ffz_ffi_results_free'),
        free = lib.lookupFunction<_FreeN, void Function(Pointer<Void>)>(
            'ffz_ffi_free'),
        installCrash = _lookupCrash(lib),
        addKeyed = _lookupAddKeyed(lib),
        finalizer = NativeFinalizer(
            lib.lookup<NativeFunction<_FreeN>>('ffz_ffi_free').cast());

  // Tolerant: a custom libraryPath might predate the crash-handler export.
  static int Function(Pointer<Utf8>)? _lookupCrash(DynamicLibrary lib) {
    try {
      return lib.lookupFunction<Int32 Function(Pointer<Utf8>),
          int Function(Pointer<Utf8>)>('ffz_ffi_install_crash_handler');
    } catch (_) {
      return null;
    }
  }

  // Tolerant: a custom libraryPath might predate the keyed-add export.
  static void Function(
      Pointer<Void>,
      Pointer<Utf8>,
      int,
      Pointer<Pointer<Utf8>>,
      Pointer<Size>,
      Pointer<Int32>,
      int)? _lookupAddKeyed(DynamicLibrary lib) {
    try {
      return lib.lookupFunction<
          _AddKeyedN,
          void Function(
              Pointer<Void>,
              Pointer<Utf8>,
              int,
              Pointer<Pointer<Utf8>>,
              Pointer<Size>,
              Pointer<Int32>,
              int)>('ffz_ffi_add_keyed');
    } catch (_) {
      return null;
    }
  }

  final DynamicLibrary lib;
  final Pointer<Void> Function(int, int) newCfg;
  final void Function(Pointer<Void>, Pointer<Utf8>, int) add;
  final int Function(Pointer<Void>) len;
  final void Function(Pointer<Void>) clear;
  final Pointer<Void> Function(
      Pointer<Void>, Pointer<Utf8>, int, int, int, int, int, int, int) filterEx;
  final int Function(Pointer<Void>) rLen;
  final int Function(Pointer<Void>, int) rItem;
  final int Function(Pointer<Void>, int) rScore;
  final int Function(Pointer<Void>, int) rKind;
  final int Function(Pointer<Void>, int) rKey;
  final int Function(Pointer<Void>, int) rNIdx;
  final int Function(Pointer<Void>, int, int) rIdx;
  final void Function(Pointer<Void>) rFree;
  final void Function(Pointer<Void>) free;
  final int Function(Pointer<Utf8>)? installCrash;
  final void Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Pointer<Utf8>>,
      Pointer<Size>, Pointer<Int32>, int)? addKeyed;
  final NativeFinalizer finalizer;

  static final Map<String, _Lib> _cache = {};
  static _Lib resolve(String? path) =>
      _cache.putIfAbsent(path ?? '<default>', () => _Lib(_open(path)));

  static DynamicLibrary _open(String? path) {
    try {
      if (path != null) return DynamicLibrary.open(path);
      if (Platform.isWindows) return DynamicLibrary.open('ffz.dll');
      // iOS and macOS both static-link the sources via the podspec, so the
      // symbols live in the host process image.
      if (Platform.isIOS || Platform.isMacOS) return DynamicLibrary.process();
      return DynamicLibrary.open('libffz.so');
    } on ArgumentError catch (e) {
      throw FuzzyException('failed to load ffz native library: $e');
    }
  }
}

// Marshal a Dart string as UTF-8 into native memory WITHOUT relying on a NUL
// terminator (so embedded U+0000 is preserved). Caller frees via malloc.free.
extension on String {
  ({Pointer<Utf8> ptr, int len}) _toUtf8() {
    final bytes = utf8.encode(this);
    final p = malloc<Uint8>(bytes.isEmpty ? 1 : bytes.length);
    if (bytes.isNotEmpty) p.asTypedList(bytes.length).setAll(0, bytes);
    return (ptr: p.cast<Utf8>(), len: bytes.length);
  }
}

// Native ffz_mode codes. Exposed to users as methods, not an enum/flag.
const int _mFuzzy = 0;
const int _mSubstring = 1;
const int _mPrefix = 2;
const int _mPostfix = 3;
const int _mExact = 4;

// A sendable raw result row (no `T`), read from native on any isolate; the
// owner isolate maps it to a FuzzyHit<T> by looking [index] up in its items.
class _RawHit {
  final int index;
  final int score;
  final int kind;
  final int key;
  final List<int> indices;
  const _RawHit(this.index, this.score, this.kind, this.key, this.indices);
}

String _identityString(String s) => s;

/// A resident corpus of `T` items, searchable by [fuzzy]/[substring]/[prefix]/
/// [postfix]/[exact]. Build it once, search it many times; release the native
/// memory with [dispose] (or rely on the [NativeFinalizer] on GC).
///
/// Each item's searchable text comes from [stringOf]. Results ([FuzzyHit.obj])
/// carry the original `T`. For plain strings use [FuzzyCorpus.strings].
class FuzzyCorpus<T> implements Finalizable {
  /// [stringOf] extracts the searchable text from each item. [options] are the
  /// default search options (overridable per call). [matchPaths] tunes
  /// delimiters for path-like text; [preferPrefix] biases scoring toward matches
  /// near the start. [libraryPath] loads a specific native library file (tests /
  /// non-bundled use).
  FuzzyCorpus(
    Iterable<T> items, {
    required String Function(T) stringOf,
    this.options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  })  : _stringOf = stringOf,
        _l = _Lib.resolve(libraryPath),
        _libPath = libraryPath {
    _ptr = _l.newCfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    if (_ptr == nullptr) {
      throw const FuzzyException('ffz_ffi_new_cfg returned null');
    }
    _l.finalizer.attach(this, _ptr.cast(), detach: this);
    addAll(items);
  }

  /// Convenience constructor for a corpus of plain strings (the item is its own
  /// search text). Equivalent to `FuzzyCorpus<String>(items, stringOf: (s) => s)`.
  static FuzzyCorpus<String> strings(
    Iterable<String> items, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) =>
      FuzzyCorpus<String>(
        items,
        stringOf: _identityString,
        options: options,
        matchPaths: matchPaths,
        preferPrefix: preferPrefix,
        libraryPath: libraryPath,
      );

  /// Convenience constructor for a `List<Map>` searched by one string [field]
  /// (e.g. `'name'`). Hits carry the whole map as [FuzzyHit.obj]. A missing or
  /// non-string field reads as `''`. (This is unrelated to [addKeyed], which
  /// attaches *alternate* keys to an item.)
  static FuzzyCorpus<Map<String, dynamic>> keyed(
    Iterable<Map<String, dynamic>> items,
    String field, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) =>
      FuzzyCorpus<Map<String, dynamic>>(
        items,
        stringOf: (m) => (m[field] as String?) ?? '',
        options: options,
        matchPaths: matchPaths,
        preferPrefix: preferPrefix,
        libraryPath: libraryPath,
      );

  /// Default search options, applied unless overridden on a mode method call.
  final FuzzyOptions options;

  final String Function(T) _stringOf;
  final _Lib _l;
  final String? _libPath; // remembered so async calls can reopen on a worker

  // Dart-side mirror of the native corpus, kept 1:1 with native indices so a
  // hit's index resolves back to its object. `_keys[i]` is the alternate-key
  // list for item i (null = added without keys).
  final List<T> _items = [];
  final List<List<FuzzyKey>?> _keys = [];

  late final Pointer<Void> _ptr;
  bool _disposed = false;
  int _inFlight = 0; // in-flight async searches (concurrent readers, OK)
  bool _building = false; // an async build is writing the corpus (exclusive)
  Completer<void>? _idle; // completes when fully idle (for disposeAndWait)

  // Concurrency model: the native corpus tolerates concurrent *readers* (each
  // search uses its own matcher scratch), but a writer must be exclusive. So a
  // search throws while an async build is writing, and any mutate/build throws
  // while a search is reading — fail fast rather than risk a native data race.
  void _check() {
    if (_disposed) throw StateError('FuzzyCorpus used after dispose()');
    if (_building) {
      throw StateError('FuzzyCorpus used while an async build is in progress');
    }
  }

  // Mutating/freeing the corpus while a search reads it from a worker isolate
  // (or while another build writes it) would be a native data race / UAF.
  void _checkMutate() {
    _check();
    if (_inFlight > 0) {
      throw StateError(
          'FuzzyCorpus mutated while $_inFlight async search(es) in flight');
    }
  }

  void _signalIfIdle() {
    if (_inFlight == 0 && !_building && _idle != null) {
      _idle!.complete();
      _idle = null;
    }
  }

  /// Number of items in the corpus.
  int get length {
    _check();
    return _items.length;
  }

  /// Append one item.
  void add(T item) {
    _checkMutate();
    _items.add(item);
    _keys.add(null);
    _nativeAdd(item, null);
  }

  /// Append many items (insertion order becomes each hit's [FuzzyHit.index]).
  void addAll(Iterable<T> items) {
    _checkMutate();
    for (final it in items) {
      _items.add(it);
      _keys.add(null);
      _nativeAdd(it, null);
    }
  }

  /// Asynchronously append [items], doing the native inserts on a background
  /// isolate — so building a large corpus never janks the UI. The text is
  /// projected via [stringOf] on this isolate (so the closure needn't be
  /// sendable), then added on the worker.
  ///
  /// The build is **exclusive**: while it runs, any search / mutation /
  /// [dispose] on this corpus throws [StateError] (the worker is writing shared
  /// native memory). Items added this way get no alternate keys (use [addKeyed]
  /// for those). See also [FuzzyCorpus.buildAsync].
  Future<void> addAllAsync(Iterable<T> items) async {
    _checkMutate();
    final list = List<T>.of(items);
    final texts = <String>[for (final it in list) _stringOf(it)];
    final addr = _ptr.address;
    final libPath = _libPath;
    _building = true;
    try {
      await Isolate.run(() {
        final lib = _Lib.resolve(libPath);
        final p = Pointer<Void>.fromAddress(addr);
        for (final s in texts) {
          final u = s._toUtf8();
          lib.add(p, u.ptr, u.len);
          malloc.free(u.ptr);
        }
      });
      _items.addAll(list);
      _keys.addAll(List<List<FuzzyKey>?>.filled(list.length, null));
    } catch (_) {
      // The worker failed mid-build; restore native to match the Dart mirror
      // (which still holds only the pre-build items) so indices stay 1:1.
      _l.clear(_ptr);
      _rebuild();
      rethrow;
    } finally {
      _building = false;
      _signalIfIdle();
    }
  }

  /// Create a corpus and populate it asynchronously (the inserts run on a
  /// background isolate). Convenience for `FuzzyCorpus(<T>[], …)` + [addAllAsync]
  /// — the recommended way to build a large corpus without UI jank.
  static Future<FuzzyCorpus<T>> buildAsync<T>(
    Iterable<T> items, {
    required String Function(T) stringOf,
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) async {
    final c = FuzzyCorpus<T>(
      <T>[],
      stringOf: stringOf,
      options: options,
      matchPaths: matchPaths,
      preferPrefix: preferPrefix,
      libraryPath: libraryPath,
    );
    await c.addAllAsync(items);
    return c;
  }

  // ── one-shot best match (no persistent corpus) ─────────────────────────────
  // Build a throwaway corpus, fuzzy-search once, return the best hit, free it.
  // Convenient for a single query; do NOT call in a hot loop (it rebuilds the
  // corpus every time) — keep a [FuzzyCorpus] and use `fuzzy(q, limit: 1)` then.

  /// Best fuzzy hit of [query] over [items], or null. One-shot: no corpus to
  /// keep or dispose. [stringOf] projects each item to its searchable text.
  static FuzzyHit<T>? one<T>(
    Iterable<T> items,
    String query, {
    required String Function(T) stringOf,
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) {
    final c = FuzzyCorpus<T>(items,
        stringOf: stringOf,
        options: options,
        matchPaths: matchPaths,
        preferPrefix: preferPrefix,
        libraryPath: libraryPath);
    try {
      final r = c.fuzzy(query, limit: 1);
      return r.isEmpty ? null : r.first;
    } finally {
      c.dispose();
    }
  }

  /// One-shot [one] for a list of plain strings.
  static FuzzyHit<String>? oneStrings(
    Iterable<String> items,
    String query, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) =>
      one<String>(items, query,
          stringOf: _identityString,
          options: options,
          matchPaths: matchPaths,
          preferPrefix: preferPrefix,
          libraryPath: libraryPath);

  /// One-shot [one] for a `List<Map>` searched by one string [field]; the hit's
  /// [FuzzyHit.obj] is the whole map.
  static FuzzyHit<Map<String, dynamic>>? oneKeyed(
    Iterable<Map<String, dynamic>> items,
    String field,
    String query, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) =>
      one<Map<String, dynamic>>(items, query,
          stringOf: (m) => (m[field] as String?) ?? '',
          options: options,
          matchPaths: matchPaths,
          preferPrefix: preferPrefix,
          libraryPath: libraryPath);

  /// Append [item] with explicit alternate search [keys] — e.g. host-computed
  /// pinyin/romaji/initials, so a CJK item is findable by typing latin. The
  /// ORIGINAL key ([stringOf] of the item) is added automatically. A hit reports
  /// which key matched via [FuzzyHit.matchedKind]/[FuzzyHit.matchedKey].
  /// ```dart
  /// corpus.addKeyed(zhang, [
  ///   FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
  ///   FuzzyKey.kind('zs', FuzzyKeyKind.initials),
  /// ]);
  /// ```
  void addKeyed(T item, List<FuzzyKey> keys) {
    _checkMutate();
    final ks = keys.isEmpty ? null : keys;
    _items.add(item);
    _keys.add(ks);
    _nativeAdd(item, ks);
  }

  /// Replace the item at [index] (its alternate keys, if any, are dropped).
  /// O(n): the native corpus is append-only, so this rebuilds it.
  void update(int index, T item) {
    _checkMutate();
    _items[index] = item;
    _keys[index] = null;
    _rebuild();
  }

  /// Remove the item at [index]. O(n) (rebuilds the native corpus).
  void removeAt(int index) {
    _checkMutate();
    _items.removeAt(index);
    _keys.removeAt(index);
    _rebuild();
  }

  /// Remove every item matching [test]; returns how many were removed.
  /// O(n) (rebuilds the native corpus once, only if something was removed).
  int removeWhere(bool Function(T item) test) {
    _checkMutate();
    var removed = 0;
    for (var i = _items.length - 1; i >= 0; i--) {
      if (test(_items[i])) {
        _items.removeAt(i);
        _keys.removeAt(i);
        removed++;
      }
    }
    if (removed > 0) _rebuild();
    return removed;
  }

  /// Rebuild the native corpus. With no argument, re-adds the current items
  /// (call after the text [stringOf] returns for existing items changed). With
  /// [source], replaces the entire data set (alternate keys are dropped). O(n).
  void refresh([Iterable<T>? source]) {
    _checkMutate();
    if (source != null) {
      _items
        ..clear()
        ..addAll(source);
      _keys
        ..clear()
        ..addAll(List<List<FuzzyKey>?>.filled(_items.length, null));
    }
    _rebuild();
  }

  /// Remove all items (the corpus stays usable).
  void clear() {
    _checkMutate();
    _items.clear();
    _keys.clear();
    _l.clear(_ptr);
  }

  void _rebuild() {
    _l.clear(_ptr);
    for (var i = 0; i < _items.length; i++) {
      _nativeAdd(_items[i], _keys[i]);
    }
  }

  void _nativeAdd(T item, List<FuzzyKey>? keys) {
    final s = _stringOf(item);
    if (keys == null) {
      final u = s._toUtf8();
      _l.add(_ptr, u.ptr, u.len);
      malloc.free(u.ptr);
      return;
    }
    final f = _l.addKeyed;
    if (f == null) {
      throw const FuzzyException('ffz_ffi_add_keyed missing in native library');
    }
    final iu = s._toUtf8();
    final n = keys.length;
    final texts = malloc<Pointer<Utf8>>(n);
    final lens = malloc<Size>(n);
    final kinds = malloc<Int32>(n);
    final keyPtrs = <Pointer<Utf8>>[];
    try {
      for (var i = 0; i < n; i++) {
        final ku = keys[i].text._toUtf8();
        texts[i] = ku.ptr;
        lens[i] = ku.len;
        kinds[i] = keys[i].kind;
        keyPtrs.add(ku.ptr);
      }
      f(_ptr, iu.ptr, iu.len, texts, lens, kinds, n);
    } finally {
      for (final p in keyPtrs) {
        malloc.free(p);
      }
      malloc.free(texts);
      malloc.free(lens);
      malloc.free(kinds);
      malloc.free(iu.ptr);
    }
  }

  // ── search: one method per mode (sync) + an async twin ─────────────────────

  /// fzf-style subsequence match. The query is parsed into space-separated terms
  /// and operators (`!` negate, `^` prefix, `'` substring, `$` suffix) — so
  /// `'lib parse'` is an AND of terms.
  List<FuzzyHit<T>> fuzzy(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _search(
          _mFuzzy,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Contiguous-substring match (the whole query as one literal atom).
  List<FuzzyHit<T>> substring(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _search(
          _mSubstring,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Prefix match (the item starts with the query).
  List<FuzzyHit<T>> prefix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _search(
          _mPrefix,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Suffix match (the item ends with the query).
  List<FuzzyHit<T>> postfix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _search(
          _mPostfix,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Exact, whole-string match.
  List<FuzzyHit<T>> exact(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _search(
          _mExact,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Async [fuzzy] — runs the native scan + marshaling on a background isolate.
  Future<List<FuzzyHit<T>>> fuzzyAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _searchAsync(
          _mFuzzy,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Async [substring].
  Future<List<FuzzyHit<T>>> substringAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _searchAsync(
          _mSubstring,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Async [prefix].
  Future<List<FuzzyHit<T>>> prefixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _searchAsync(
          _mPrefix,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Async [postfix].
  Future<List<FuzzyHit<T>>> postfixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _searchAsync(
          _mPostfix,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  /// Async [exact].
  Future<List<FuzzyHit<T>>> exactAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight}) =>
      _searchAsync(
          _mExact,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight));

  FuzzyOptions _eff(FuzzyCase? cm, FuzzyNorm? nm, bool? par, int? th, int? lim,
          bool? hl) =>
      options.copyWith(
          caseMatching: cm,
          normalization: nm,
          parallel: par,
          threads: th,
          limit: lim,
          highlight: hl);

  List<FuzzyHit<T>> _search(int mode, String query, FuzzyOptions o) {
    _check();
    return _toHits(_rawFilter(_l, _ptr, query, mode, o));
  }

  /// Runs the native scan + result marshaling on a background isolate, so a
  /// large corpus never janks the UI isolate. Multiple async calls may overlap
  /// safely (each gets its own native matcher). While one is in flight,
  /// mutating ([add]/[update]/[removeAt]/[clear]/…) or [dispose] throws
  /// [StateError]. Awaiting the future also keeps this corpus alive across the
  /// call so the finalizer can't free it mid-scan.
  Future<List<FuzzyHit<T>>> _searchAsync(
      int mode, String query, FuzzyOptions o) async {
    _check();
    final addr = _ptr.address;
    final libPath = _libPath;
    _inFlight++;
    try {
      final raws = await Isolate.run(() {
        // New isolate: statics are fresh, so reopen the library (the OS returns
        // the already-loaded image) and address the shared corpus by pointer.
        final lib = _Lib.resolve(libPath);
        return _rawFilter(lib, Pointer<Void>.fromAddress(addr), query, mode, o);
      });
      return _toHits(raws);
    } finally {
      // Touching the instance field after the await keeps `this` (and thus the
      // native corpus) reachable for the whole call, defeating the finalizer.
      _inFlight--;
      _signalIfIdle();
    }
  }

  List<FuzzyHit<T>> _toHits(List<_RawHit> raws) => [
        for (final r in raws)
          FuzzyHit<T>(_items[r.index], r.index, r.score, _kindOf(r.kind), r.key,
              r.indices)
      ];

  // Shared native call + result read, usable on any isolate (the returned list
  // is sendable). `ptr` must be a live corpus in this process.
  static List<_RawHit> _rawFilter(
      _Lib lib, Pointer<Void> ptr, String query, int mode, FuzzyOptions o) {
    final u = query._toUtf8();
    final r = lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching.index,
        o.normalization.index, o.parallel ? 1 : 0, o.threads, o.limit);
    malloc.free(u.ptr);
    if (r == nullptr) {
      throw const FuzzyException('filter failed (out of memory)');
    }
    final n = lib.rLen(r);
    final out = <_RawHit>[];
    for (var i = 0; i < n; i++) {
      List<int> idx = const [];
      if (o.highlight) {
        final ni = lib.rNIdx(r, i);
        idx = List<int>.generate(ni, (j) => lib.rIdx(r, i, j), growable: false);
      }
      out.add(_RawHit(lib.rItem(r, i), lib.rScore(r, i), lib.rKind(r, i),
          lib.rKey(r, i), idx));
    }
    lib.rFree(r);
    return out;
  }

  /// Release native memory now. Idempotent. Throws [StateError] if an async
  /// search is still in flight (freeing would be a use-after-free) — await the
  /// pending futures first.
  void dispose() {
    if (_disposed) return;
    if (_inFlight > 0) {
      throw StateError(
          'FuzzyCorpus.dispose() with $_inFlight async search(es) in flight');
    }
    _disposed = true;
    _l.finalizer.detach(this);
    _l.free(_ptr);
  }

  /// Like [dispose], but first awaits any in-flight async search or build, so it
  /// never throws on pending work. Safe to call while async work is running.
  Future<void> disposeAndWait() async {
    if (_disposed) return;
    if (_inFlight > 0 || _building) {
      await (_idle ??= Completer<void>()).future;
    }
    dispose();
  }
}

/// Optional native crash handler for **non-recoverable** faults.
///
/// Recoverable errors already surface as [FuzzyException]/[StateError] and are
/// catchable. A genuine native fault (segfault / abort) cannot be turned into a
/// Dart exception — `dart:ffi` has no such mechanism and the process dies.
/// Installing this handler makes that death *diagnosable*: it prints a
/// backtrace to stderr (logcat on Android) just before exit and, if you pass a
/// [breadcrumbPath], writes the same report to that file so you can show
/// "last crash" on the next launch via [lastReport].
///
/// How readable the backtrace is depends on the **build**, automatically:
/// debug/profile libraries keep symbols, so you get function names (and, on
/// Windows, `file:line` from the PDB); stripped release libraries print address
/// offsets you symbolize offline with the shipped `.debug`/`.pdb`/`.dSYM`.
///
/// This is opt-in (it installs process-wide signal handlers; call it once at
/// startup, before your other crash reporter if you chain them):
/// ```dart
/// final report = FuzzyCrash.lastReport();    // previous run's crash, if any
/// if (report != null) log('ffuzzy last crash:\n$report');
/// FuzzyCrash.install(breadcrumbPath: '${dir.path}/ffuzzy_crash.log');
/// ```
class FuzzyCrash {
  FuzzyCrash._();
  static String? _path;

  /// Install the handler. [breadcrumbPath] (optional) receives the backtrace of
  /// the next crash. [libraryPath] mirrors [FuzzyCorpus]. Returns true if the
  /// native handler was installed (false if the library lacks the symbol).
  static bool install({String? breadcrumbPath, String? libraryPath}) {
    final f = _Lib.resolve(libraryPath).installCrash;
    if (f == null) return false;
    _path = breadcrumbPath;
    // NUL-terminated (C uses strlen); a file path never has an embedded NUL.
    final p = breadcrumbPath == null ? nullptr : breadcrumbPath.toNativeUtf8();
    try {
      return f(p.cast()) != 0;
    } finally {
      if (p != nullptr) malloc.free(p);
    }
  }

  /// Read (and clear) the crash report left by a previous run, or null if none.
  /// Pass the same [breadcrumbPath] used at [install], or rely on the stored one.
  static String? lastReport({String? breadcrumbPath}) {
    final p = breadcrumbPath ?? _path;
    if (p == null) return null;
    final f = File(p);
    if (!f.existsSync()) return null;
    final s = f.readAsStringSync();
    try {
      f.deleteSync();
    } catch (_) {}
    return s.isEmpty ? null : s;
  }
}
