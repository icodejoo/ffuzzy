/// ffuzzy — idiomatic Dart binding for the compact C fuzzy matcher, via dart:ffi.
///
/// ```dart
/// // Search a list of objects; results carry the original object via .raw.
/// final corpus = FuzzyCorpus<File>(files, stringOf: (f) => f.path);
/// for (final h in corpus.fuzzy('src', limit: 50, highlight: true)) {
///   final u16 = fuzzyCodepointToUtf16(h.raw.path, h.indices); // for TextSpan
///   print('${h.raw.path}  score=${h.score}  $u16');
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
import 'package:flutter/foundation.dart' show debugPrint;

/// Case handling (mirrors `ffz_case_matching`).
enum FuzzyCase { respect, ignore, smart }

/// Unicode normalization (mirrors `ffz_normalization`).
enum FuzzyNorm { never, smart }

/// Scoring model for corpus queries (mirrors `ffz_scoring_mode`).
///
/// Set the corpus-wide default in [FuzzyOptions]; override per call via the
/// `scoring` named parameter on [FuzzyCorpus.fuzzy], [FuzzyCorpus.exact], etc.
enum FuzzyScoring {
  /// 2-row rolling DP with simplified bonuses (default). Faster than [nucleo]
  /// in single-threaded scenarios; good ranking quality for typical use.
  /// 好的默认值，适合姓名/路径/代码符号搜索。
  fast,

  /// No DP. Prefilter only; all matching items get [FuzzyHit.score] == 0 and
  /// are returned in corpus insertion order. Use for programmatic
  /// exact/ID matching where ranking is irrelevant.
  /// 仅需匹配是否存在时（ID查找/唯一匹配），按插入顺序返回。
  off,

  /// Full-matrix DP, nucleo 0.3.1 compatible. Highest ranking accuracy.
  /// 排名精度要求高时，约需 fast 2倍 CPU。
  nucleo,
}

extension _FuzzyScoringCValue on FuzzyScoring {
  // Maps to the C enum values (FFZ_SCORE_FAST=0, FFZ_SCORE_OFF=1, FFZ_SCORE_NUCLEO=2).
  // Using an explicit mapping avoids breakage if the Dart enum order ever changes.
  int get _cValue => switch (this) {
        FuzzyScoring.fast => 0, // FFZ_SCORE_FAST
        FuzzyScoring.off => 1, // FFZ_SCORE_OFF
        FuzzyScoring.nucleo => 2, // FFZ_SCORE_NUCLEO
      };
}

extension _FuzzyCaseCValue on FuzzyCase {
  // Explicit C enum mapping — avoids breakage if Dart enum order changes.
  int get _cValue => switch (this) {
        FuzzyCase.respect => 0,
        FuzzyCase.ignore => 1,
        FuzzyCase.smart => 2,
      };
}

extension _FuzzyNormCValue on FuzzyNorm {
  // Explicit C enum mapping — avoids breakage if Dart enum order changes.
  int get _cValue => switch (this) {
        FuzzyNorm.never => 0,
        FuzzyNorm.smart => 1,
      };
}

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
/// - [highlight]: when `true`, each [FuzzyHit.indices] is populated with the
///   matched codepoint positions (Pass 2 runs). Default `false` for speed.
class FuzzyOptions {
  final FuzzyScoring scoring;
  final FuzzyCase caseMatching;
  final FuzzyNorm normalization;
  final bool parallel;
  final int threads;
  final int limit;
  final bool highlight;

  const FuzzyOptions({
    this.scoring = FuzzyScoring.fast,
    this.caseMatching = FuzzyCase.smart,
    this.normalization = FuzzyNorm.smart,
    this.parallel = false,
    this.threads = 0,
    this.limit = 0,
    this.highlight = false,
  });

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      (other is FuzzyOptions &&
          scoring == other.scoring &&
          caseMatching == other.caseMatching &&
          normalization == other.normalization &&
          parallel == other.parallel &&
          threads == other.threads &&
          limit == other.limit &&
          highlight == other.highlight);

  @override
  int get hashCode => Object.hash(
      scoring, caseMatching, normalization, parallel, threads, limit, highlight);

  /// A copy with the given fields replaced (null keeps the current value).
  FuzzyOptions copyWith({
    FuzzyScoring? scoring,
    FuzzyCase? caseMatching,
    FuzzyNorm? normalization,
    bool? parallel,
    int? threads,
    int? limit,
    bool? highlight,
  }) =>
      FuzzyOptions(
        scoring: scoring ?? this.scoring,
        caseMatching: caseMatching ?? this.caseMatching,
        normalization: normalization ?? this.normalization,
        parallel: parallel ?? this.parallel,
        threads: threads ?? this.threads,
        limit: limit ?? this.limit,
        highlight: highlight ?? this.highlight,
      );
}

/// An alternate search key for an item (e.g. host-computed pinyin/romaji), for
/// [FuzzyCorpus.addKey]. [kind] is a [FuzzyKeyKind] code (use `FuzzyKeyKind.x.code`)
/// or any host-defined value >= 100.
class FuzzyKey {
  final String text;

  /// The key kind code. Defaults to 1 (pinyin). For custom fields, prefer
  /// `FuzzyKey.kind(text, FuzzyKeyKind.custom)` instead of passing a raw int.
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
/// [raw] is the original item that matched. [index] is its insertion order in
/// the corpus. [indices] contains matched codepoint positions only when the
/// search was called with `highlight: true`; otherwise it is empty.
class FuzzyHit<T> {
  final T raw;
  final int index;

  /// Non-negative integer; higher is a better match. Comparable only within
  /// results of the same query. Always 0 when [FuzzyScoring.off] is in effect.
  final int score;
  final FuzzyKeyKind matchedKind;

  /// The raw integer kind code of the matched key — equal to [matchedKind]`.code`
  /// for the built-in kinds (`original`=0, `pinyin`=1, `initials`=2, `romaji`=3).
  /// For host-defined keys added via [FuzzyCorpus.addKey] or [FuzzyCorpus.byKeys],
  /// this preserves the original value (e.g. 100, 101, 200, …), allowing callers
  /// to distinguish multiple custom key types where [matchedKind] would only report
  /// [FuzzyKeyKind.custom] for all of them.
  final int matchedKindCode;

  final int matchedKey; // which key of the item matched (0 == original)

  /// Matched codepoint positions within the matched key. Populated only when
  /// the search was called with `highlight: true`; empty otherwise.
  /// Pass through [fuzzyCodepointToUtf16] before using with [TextSpan].
  final List<int> indices;

  const FuzzyHit(this.raw, this.index, this.score, this.matchedKind,
      this.matchedKindCode, this.matchedKey, this.indices);

  @override
  String toString() =>
      'FuzzyHit(index: $index, score: $score, kind: $matchedKind($matchedKindCode), raw: $raw)';

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      (other is FuzzyHit<T> &&
          index == other.index &&
          score == other.score &&
          matchedKind == other.matchedKind &&
          matchedKindCode == other.matchedKindCode &&
          matchedKey == other.matchedKey &&
          raw == other.raw &&
          indices.length == other.indices.length &&
          Iterable.generate(indices.length, (i) => indices[i] == other.indices[i])
              .every((v) => v));

  @override
  int get hashCode => Object.hash(index, score, matchedKind, matchedKindCode,
      matchedKey, raw, indices.fold<int>(0, (h, e) => h ^ e.hashCode));
}

/// Convert codepoint indices (as in [FuzzyHit.indices]) to UTF-16 code-unit
/// offsets into [text], suitable for Dart `String`/`TextSpan` highlighting.
/// (Dart strings are UTF-16; astral chars/emoji occupy two code units.)
///
/// For BMP-only text (ASCII, CJK, most scripts) this is a no-op.
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
typedef _NewCfg2N = Pointer<Void> Function(Int32, Int32, Int32);
typedef _AddN = Void Function(Pointer<Void>, Pointer<Utf8>, Size);
typedef _AddKeyedN = Void Function(Pointer<Void>, Pointer<Utf8>, Size,
    Pointer<Pointer<Utf8>>, Pointer<Size>, Pointer<Int32>, Size);
typedef _LenN = Size Function(Pointer<Void>);
typedef _FreeN = Void Function(Pointer<Void>);
typedef _VoidPtrN = Void Function(Pointer<Void>);
typedef _FilterExN = Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, Size,
    Int32, Int32, Int32, Int32, Int32, Size);
typedef _FilterEx2N = Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>,
    Size, Int32, Int32, Int32, Int32, Int32, Size, Int32);
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
        rIdx = lib.lookupFunction<_RIdxN, int Function(Pointer<Void>, int, int)>(
            'ffz_ffi_results_index'),
        rFree = lib.lookupFunction<_VoidPtrN, void Function(Pointer<Void>)>(
            'ffz_ffi_results_free'),
        free = lib.lookupFunction<_FreeN, void Function(Pointer<Void>)>(
            'ffz_ffi_free'),
        installCrash = _lookupCrash(lib),
        addKey = _lookupAddKeyed(lib),
        newCfg2 = _lookupNewCfg2(lib),
        filterEx2 = _lookupFilterEx2(lib),
        filterRaws = _lookupFilterRaws(lib),
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

  // Tolerant: a custom libraryPath might predate the scoring-mode exports.
  static Pointer<Void> Function(int, int, int)? _lookupNewCfg2(
      DynamicLibrary lib) {
    try {
      return lib.lookupFunction<_NewCfg2N,
          Pointer<Void> Function(int, int, int)>('ffz_ffi_new_cfg2');
    } catch (_) {
      return null;
    }
  }

  static Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? _lookupFilterEx2(DynamicLibrary lib) {
    try {
      return lib.lookupFunction<
          _FilterEx2N,
          Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
              int, int, int, int, int)>('ffz_ffi_filter_ex2');
    } catch (_) {
      return null;
    }
  }

  static Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? _lookupFilterRaws(DynamicLibrary lib) {
    try {
      return lib.lookupFunction<
          _FilterEx2N,
          Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
              int, int, int, int, int)>('ffz_ffi_filter_raws');
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
      Pointer<Size>, Pointer<Int32>, int)? addKey;
  final Pointer<Void> Function(int, int, int)? newCfg2;
  final Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? filterEx2;
  final Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? filterRaws;
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
    if (bytes.isNotEmpty) {
      p.asTypedList(bytes.length).setAll(0, bytes);
    } else {
      p[0] = 0; // NUL-terminate: defensive against C-side strlen on empty query
    }
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
/// Each item's searchable text comes from [stringOf]. Results ([FuzzyHit.raw])
/// carry the original `T`. For plain strings use [FuzzyCorpus.strings].
///
/// **Lifecycle in Flutter**: call [dispose] inside `State.dispose()` of any
/// [StatefulWidget] that owns a corpus. The [NativeFinalizer] is a safety
/// net for GC — it does NOT replace an explicit `dispose()` call, because GC
/// timing is unpredictable and the native memory may persist for much longer
/// than the widget lifetime.
///
/// **Isolate safety**: [FuzzyCorpus] instances must not be sent to other
/// Isolates. Native memory is managed by the creating Isolate's
/// [NativeFinalizer]; cross-Isolate use may result in double-free or
/// use-after-free. The async search methods ([fuzzyAsync] etc.) are safe
/// because they only send the raw pointer *address* (an `int`) to the worker
/// Isolate, which opens its own independent [DynamicLibrary] handle — the
/// [FuzzyCorpus] object itself never crosses the Isolate boundary.
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
    final sc = options.scoring._cValue;
    _ptr = (_l.newCfg2 != null)
        ? _l.newCfg2!(matchPaths ? 1 : 0, preferPrefix ? 1 : 0, sc)
        : _l.newCfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    if (_l.newCfg2 == null && options.scoring != FuzzyScoring.fast) {
      // ignore: avoid_print
      print('[ffuzzy] WARNING: corpus-level scoring=${options.scoring} ignored '
          '(native library predates ffz_ffi_new_cfg2; upgrade the .so/.dll)');
    }
    if (_ptr == nullptr) {
      throw FuzzyException(
          '${_l.newCfg2 != null ? 'ffz_ffi_new_cfg2' : 'ffz_ffi_new_cfg'} '
          'returned null (out of memory)');
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

  /// Creates a corpus from a collection of [Map]s, searching the value at [field]
  /// (e.g. `'name'`). Hits carry the whole map as [FuzzyHit.raw]. A missing or
  /// non-string field is treated as `''`. (Unrelated to [addKey], which attaches
  /// *alternate* keys to an item.)
  static FuzzyCorpus<Map<String, dynamic>> byKey(
    Iterable<Map<String, dynamic>> items,
    String field, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) =>
      FuzzyCorpus<Map<String, dynamic>>(
        items,
        stringOf: (m) => (m[field] ?? '').toString(),
        options: options,
        matchPaths: matchPaths,
        preferPrefix: preferPrefix,
        libraryPath: libraryPath,
      );

  /// Creates a corpus from a collection of [Map]s, searching across multiple
  /// [fields]. The first field is the primary search key; subsequent fields are
  /// added as alternate keys via [addKey]. Hits carry the whole map as
  /// [FuzzyHit.raw]; [FuzzyHit.matchedKey] is the index into [fields] that
  /// produced the hit (`0` = first field, `1` = second field, etc.).
  ///
  /// ```dart
  /// final corpus = FuzzyCorpus.byKeys(contacts, ['name', 'email', 'company']);
  /// final hits = corpus.fuzzy('acme');
  /// for (final h in hits) {
  ///   final matchedField = ['name', 'email', 'company'][h.matchedKey];
  /// }
  /// ```
  static FuzzyCorpus<Map<String, dynamic>> byKeys(
    Iterable<Map<String, dynamic>> items,
    List<String> fields, {
    FuzzyOptions options = const FuzzyOptions(),
    bool matchPaths = false,
    bool preferPrefix = false,
    String? libraryPath,
  }) {
    if (fields.isEmpty) {
      throw ArgumentError.value(fields, 'fields', 'must not be empty');
    }
    final corpus = FuzzyCorpus<Map<String, dynamic>>(
      [],
      stringOf: (m) => (m[fields.first] ?? '').toString(),
      options: options,
      matchPaths: matchPaths,
      preferPrefix: preferPrefix,
      libraryPath: libraryPath,
    );
    for (final item in items) {
      if (fields.length == 1) {
        corpus.add(item);
      } else {
        corpus.addKey(item, [
          for (var i = 1; i < fields.length; i++)
            FuzzyKey(
              (item[fields[i]] ?? '').toString(),
              kind: FuzzyKeyKind.custom.code,
            ),
        ]);
      }
    }
    return corpus;
  }

  /// Default search options, applied unless overridden on a mode method call.
  final FuzzyOptions options;

  /// Single-best-hit view: the same five search modes, but each returns the top
  /// [FuzzyHit] (or null) instead of a list — `corpus.one.fuzzy(q)`. It runs the
  /// identical native scan as `fuzzy(q, limit: 1)`, so there's no extra cost.
  late final FuzzyOne<T> one = FuzzyOne._(this);

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
  bool _freed = false; // guards against double-free when dispose() + disposeAndWait() race
  int _inFlight = 0; // in-flight async searches (concurrent readers, OK)
  bool _building = false; // an async build is writing the corpus (exclusive)
  Completer<void>? _idle; // completes when fully idle (for disposeAndWait)
  int _estBytes = 0; // ~UTF-8 bytes handed to native; the finalizer's GC-pressure hint

  // Re-arm the NativeFinalizer with the current native footprint so GC sees the
  // real memory pressure (attach takes externalSize once; detach+attach updates
  // it). detach(this) clears the prior arming, so exactly one stays attached.
  void _refreshFinalizer() {
    _l.finalizer.detach(this);
    _l.finalizer.attach(this, _ptr.cast(), detach: this, externalSize: _estBytes);
  }

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

  // Reading any instance field after an `await` keeps `this` reachable, which
  // prevents the NativeFinalizer from firing while the native corpus is in use
  // by a worker Isolate. Explicit helper makes the intent clear at call sites.
  void _keepAlive() => _disposed; // ignore: unnecessary_getters_setters

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
    // Native first: if _nativeAdd throws (e.g. OOM), the Dart mirror stays in
    // sync with the native corpus instead of growing past it.
    _nativeAdd(item, null);
    _items.add(item);
    _keys.add(null);
    _refreshFinalizer();
  }

  /// Append many items (insertion order becomes each hit's [FuzzyHit.index]).
  void addAll(Iterable<T> items) {
    _checkMutate();
    for (final it in items) {
      // Native first per item: a throw mid-loop leaves both sides consistent
      // (the items added so far are in both; this one in neither).
      _nativeAdd(it, null);
      _items.add(it);
      _keys.add(null);
    }
    _refreshFinalizer();
  }

  /// Asynchronously append [items], doing the native inserts on a background
  /// isolate — so building a large corpus never janks the UI. The text is
  /// projected via [stringOf] on this isolate (so the closure needn't be
  /// sendable), then added on the worker.
  ///
  /// The build is **exclusive**: while it runs, any search / mutation /
  /// [dispose] on this corpus throws [StateError] (the worker is writing shared
  /// native memory). Items added this way get no alternate keys (use [addKey]
  /// for those). See also [FuzzyCorpus.buildAsync].
  ///
  /// **UI input during build**: if the user can type while a build is in
  /// progress, debounce or queue the search calls — a call during build throws
  /// [StateError]. A typical strategy is to await the build future before
  /// enabling the search field, or to catch the error and retry after the
  /// build completes.
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
      // Worker added via lib.add (not _nativeAdd), so update the GC hint here.
      // Approximate: UTF-16 length (== bytes for ASCII, under-counts multibyte).
      for (final s in texts) { _estBytes += s.length; }
      _refreshFinalizer();
    } catch (primaryError, primaryStack) {
      // The worker failed mid-build; restore native to match the Dart mirror
      // (which still holds only the pre-build items) so indices stay 1:1.
      _l.clear(_ptr);
      try {
        _rebuild();
      } catch (rebuildError) {
        // _rebuild() also failed — the corpus is in an unknown state; mark it
        // disposed to prevent any further use-after-free. Re-throw a chained
        // error that surfaces BOTH the original worker failure and the rebuild
        // failure so neither is silently discarded.
        _disposed = true;
        _freed = true;
        _building = false;
        _l.finalizer.detach(this);
        _l.free(_ptr);
        Error.throwWithStackTrace(
          StateError(
            'addAllAsync: worker failed ($primaryError) '
            'and corpus rebuild also failed: $rebuildError',
          ),
          primaryStack, // ← 用原始 worker 失败的堆栈，而非 rebuild 失败的堆栈
        );
      }
      Error.throwWithStackTrace(primaryError, primaryStack);
    } finally {
      _building = false;
      _signalIfIdle();
      _keepAlive(); // touch instance field → object stays reachable through await
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

  /// Append [item] with explicit alternate search [keys] — e.g. host-computed
  /// pinyin/romaji/initials, so a CJK item is findable by typing latin. The
  /// ORIGINAL key ([stringOf] of the item) is added automatically. A hit reports
  /// which key matched via [FuzzyHit.matchedKind]/[FuzzyHit.matchedKey].
  /// ```dart
  /// corpus.addKey(zhang, [
  ///   FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
  ///   FuzzyKey.kind('zs', FuzzyKeyKind.initials),
  /// ]);
  /// ```
  void addKey(T item, List<FuzzyKey> keys) {
    _checkMutate();
    final ks = keys.isEmpty ? null : keys;
    // Native first: _nativeAdd throws if the native lib predates
    // ffz_ffi_add_keyed (or on OOM). Pushing the mirror only after it succeeds
    // keeps _items/_keys 1:1 with the native corpus (else later searches map
    // native indices to the wrong items).
    _nativeAdd(item, ks);
    _items.add(item);
    _keys.add(ks);
    _refreshFinalizer();
  }

  /// Replace the item at [index] (its alternate keys, if any, are dropped).
  /// O(n): the native corpus is append-only, so this rebuilds it.
  ///
  /// **Warning**: if the item at [index] was added via [addKey], its
  /// alternate keys are permanently discarded. To update a keyed entry and
  /// preserve (or change) its alternate keys, use `removeAt(index)` followed
  /// by [addKey].
  void update(int index, T item) {
    _checkMutate();
    if (_keys[index] != null) {
      assert(
        false,
        'FuzzyCorpus.update() at index $index discards alternate keys. '
        'Use removeAt() + addKey() to update a keyed entry.',
      );
      // ignore: avoid_print
      debugPrint('[ffuzzy] WARNING: update() at index $index discards '
          'alternate keys added via addKey(). '
          'Use removeAt() + addKey() instead.');
    }
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
    _estBytes = 0;
    _refreshFinalizer();
  }

  void _rebuild() {
    _l.clear(_ptr);
    _estBytes = 0;
    for (var i = 0; i < _items.length; i++) {
      _nativeAdd(_items[i], _keys[i]);
    }
    _refreshFinalizer();
  }

  void _nativeAdd(T item, List<FuzzyKey>? keys) {
    final s = _stringOf(item);
    if (keys == null) {
      final u = s._toUtf8();
      _l.add(_ptr, u.ptr, u.len);
      _estBytes += u.len;
      malloc.free(u.ptr);
      return;
    }
    final f = _l.addKey;
    if (f == null) {
      throw const FuzzyException('ffz_ffi_add_keyed missing in native library');
    }
    final iu = s._toUtf8();
    try {
      _estBytes += iu.len;
      // Allocate arrays inside the outer try so iu.ptr is freed even if
      // any of these mallocs throw (e.g. OOM).
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
          _estBytes += ku.len;
        }
        f(_ptr, iu.ptr, iu.len, texts, lens, kinds, n);
      } finally {
        for (final p in keyPtrs) { malloc.free(p); }
        malloc.free(texts);
        malloc.free(lens);
        malloc.free(kinds);
      }
    } finally {
      malloc.free(iu.ptr);
    }
  }

  // ── search: one method per mode (sync) + an async twin ─────────────────────

  /// fzf-style subsequence match. The query is parsed into space-separated terms
  /// and operators (`!` negate, `^` prefix, `'` substring, `$` suffix) — so
  /// `'lib parse'` is an AND of terms. Within a term: `'xxx` is a substring
  /// match, `^xxx` is a prefix match, `!xxx` is an exclusion (negation).
  ///
  /// **Performance note**: runs synchronously on the calling isolate.
  /// For corpora with more than ~5 000 items, prefer the `…Async` variant
  /// to avoid frame jank.
  List<FuzzyHit<T>> fuzzy(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _search(_mFuzzy, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Contiguous-substring match (the whole query as one literal atom).
  ///
  /// **Performance note**: runs synchronously on the calling isolate.
  /// For corpora with more than ~5 000 items, prefer the `…Async` variant
  /// to avoid frame jank.
  List<FuzzyHit<T>> substring(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _search(_mSubstring, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Prefix match (the item starts with the query).
  ///
  /// **Performance note**: runs synchronously on the calling isolate.
  /// For corpora with more than ~5 000 items, prefer the `…Async` variant
  /// to avoid frame jank.
  List<FuzzyHit<T>> prefix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _search(_mPrefix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Suffix match (the item ends with the query).
  ///
  /// **Performance note**: runs synchronously on the calling isolate.
  /// For corpora with more than ~5 000 items, prefer the `…Async` variant
  /// to avoid frame jank.
  List<FuzzyHit<T>> postfix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _search(_mPostfix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Suffix match — alias for [postfix] (preferred Dart naming convention).
  List<FuzzyHit<T>> suffix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      postfix(query,
          caseMatching: caseMatching,
          normalization: normalization,
          parallel: parallel,
          threads: threads,
          limit: limit,
          highlight: highlight,
          scoring: scoring);

  /// Exact, whole-string match.
  ///
  /// **Performance note**: runs synchronously on the calling isolate.
  /// For corpora with more than ~5 000 items, prefer the `…Async` variant
  /// to avoid frame jank.
  List<FuzzyHit<T>> exact(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _search(_mExact, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  // ── Raw-object variants (*Raws) ──────────────────────────────────────────
  // These skip FuzzyHit wrapping and Pass 2 index computation. Prefer them
  // when you only need matched items and not score/indices/metadata.

  /// Fuzzy search — returns matched items without [FuzzyHit] wrapper.
  /// Faster than [fuzzy]: skips per-survivor highlight-index computation.
  List<T> fuzzyRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRaws(_mFuzzy, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Substring search — raw objects. See [fuzzyRaws].
  List<T> substringRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRaws(_mSubstring, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Prefix search — raw objects. See [fuzzyRaws].
  List<T> prefixRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRaws(_mPrefix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Postfix search — raw objects. See [fuzzyRaws].
  List<T> postfixRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRaws(_mPostfix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Suffix search — raw objects. Alias for [postfixRaws].
  List<T> suffixRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      postfixRaws(query,
          caseMatching: caseMatching,
          normalization: normalization,
          parallel: parallel,
          threads: threads,
          limit: limit,
          scoring: scoring);

  /// Exact search — raw objects. See [fuzzyRaws].
  List<T> exactRaws(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRaws(_mExact, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async [fuzzyRaws].
  Future<List<T>> fuzzyRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRawsAsync(_mFuzzy, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async [substringRaws].
  Future<List<T>> substringRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRawsAsync(_mSubstring, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async [prefixRaws].
  Future<List<T>> prefixRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRawsAsync(_mPrefix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async [postfixRaws].
  Future<List<T>> postfixRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRawsAsync(_mPostfix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async suffix — raw objects. Alias for [postfixRawsAsync].
  Future<List<T>> suffixRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      postfixRawsAsync(query,
          caseMatching: caseMatching,
          normalization: normalization,
          parallel: parallel,
          threads: threads,
          limit: limit,
          scoring: scoring);

  /// Async [exactRaws].
  Future<List<T>> exactRawsAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          FuzzyScoring? scoring}) =>
      _searchRawsAsync(_mExact, query,
          _eff(caseMatching, normalization, parallel, threads, limit, false, scoring));

  /// Async [fuzzy] — runs the native scan + marshaling on a background isolate.
  /// Each call creates a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<List<FuzzyHit<T>>> fuzzyAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _searchAsync(_mFuzzy, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Async [substring].
  Future<List<FuzzyHit<T>>> substringAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _searchAsync(_mSubstring, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Async [prefix].
  Future<List<FuzzyHit<T>>> prefixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _searchAsync(_mPrefix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Async [postfix].
  Future<List<FuzzyHit<T>>> postfixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _searchAsync(_mPostfix, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  /// Async suffix match — alias for [postfixAsync] (preferred Dart naming convention).
  Future<List<FuzzyHit<T>>> suffixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      postfixAsync(query,
          caseMatching: caseMatching,
          normalization: normalization,
          parallel: parallel,
          threads: threads,
          limit: limit,
          highlight: highlight,
          scoring: scoring);

  /// Async [exact].
  Future<List<FuzzyHit<T>>> exactAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _searchAsync(_mExact, query,
          _eff(caseMatching, normalization, parallel, threads, limit, highlight, scoring));

  FuzzyOptions _eff(FuzzyCase? cm, FuzzyNorm? nm, bool? par, int? th, int? lim,
          bool? hl, FuzzyScoring? sc) =>
      options.copyWith(
          scoring: sc,
          caseMatching: cm,
          normalization: nm,
          parallel: par,
          threads: th,
          limit: lim,
          highlight: hl);

  // Sync path.
  // highlight=false (default): filterRaws skips Pass 2 — no indices, fast.
  // highlight=true: filterEx2 runs Pass 2 — populates indices for rendering.
  List<FuzzyHit<T>> _search(int mode, String query, FuzzyOptions o) {
    _check();
    RangeError.checkValueInInterval(mode, 0, 4, 'mode');
    RangeError.checkValueInInterval(o.scoring._cValue, 0, 2, 'scoring');
    final u = query._toUtf8();
    var r = Pointer<Void>.fromAddress(0);
    try {
      if (o.highlight) {
        r = (_l.filterEx2 != null)
            ? _l.filterEx2!(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
                o.scoring._cValue)
            : _l.filterEx(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit);
      } else {
        r = (_l.filterRaws != null)
            ? _l.filterRaws!(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
                o.scoring._cValue)
            : (_l.filterEx2 != null)
                ? _l.filterEx2!(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                    o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                    o.limit, o.scoring._cValue)
                : _l.filterEx(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                    o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                    o.limit);
      }
      if (r == nullptr) {
        throw StateError(
            'ffz_ffi_filter returned null — out of memory or invalid parameters.');
      }
      final n = _l.rLen(r);
      final out = <FuzzyHit<T>>[];
      for (var i = 0; i < n; i++) {
        final idx = _l.rItem(r, i);
        if (idx >= _items.length) continue; // UINT32_MAX sentinel on C error
        List<int> indices = const [];
        if (o.highlight) {
          final ni = _l.rNIdx(r, i);
          indices =
              List<int>.generate(ni, (j) => _l.rIdx(r, i, j), growable: false);
        }
        final kind = _l.rKind(r, i);
        out.add(FuzzyHit<T>(_items[idx], idx, _l.rScore(r, i), _kindOf(kind),
            kind, _l.rKey(r, i), indices));
      }
      return out;
    } finally {
      malloc.free(u.ptr);
      if (r != nullptr) _l.rFree(r);
    }
  }

  // Sync path for *Raws: skip Pass 2 via ffz_ffi_filter_raws (no index computation).
  // Falls back to regular filter when ffz_ffi_filter_raws is unavailable.
  List<T> _searchRaws(int mode, String query, FuzzyOptions o) {
    _check();
    RangeError.checkValueInInterval(mode, 0, 4, 'mode');
    RangeError.checkValueInInterval(o.scoring._cValue, 0, 2, 'scoring');
    final u = query._toUtf8();
    var r = Pointer<Void>.fromAddress(0);
    try {
      r = (_l.filterRaws != null)
          ? _l.filterRaws!(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
              o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
              o.scoring._cValue)
          : (_l.filterEx2 != null)
              ? _l.filterEx2!(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                  o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                  o.limit, o.scoring._cValue)
              : _l.filterEx(_ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                  o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                  o.limit);
      if (r == nullptr) return const [];
      final n = _l.rLen(r);
      final out = <T>[];
      for (var i = 0; i < n; i++) {
        final idx = _l.rItem(r, i);
        if (idx < _items.length) out.add(_items[idx]);
      }
      return out;
    } finally {
      malloc.free(u.ptr);
      if (r != nullptr) _l.rFree(r);
    }
  }

  // Async raws: sends only item indices across the isolate boundary.
  Future<List<T>> _searchRawsAsync(int mode, String query, FuzzyOptions o) async {
    _check();
    final addr = _ptr.address;
    final libPath = _libPath;
    _inFlight++;
    try {
      final indices = await Isolate.run(() {
        final lib = _Lib.resolve(libPath);
        return _rawsFilter(lib, Pointer<Void>.fromAddress(addr), query, mode, o);
      });
      return [for (final idx in indices) if (idx < _items.length) _items[idx]];
    } finally {
      _inFlight--;
      _signalIfIdle();
      _keepAlive();
    }
  }

  // Sendable helper for the async raws path: returns item indices only.
  static List<int> _rawsFilter(
      _Lib lib, Pointer<Void> ptr, String query, int mode, FuzzyOptions o) {
    RangeError.checkValueInInterval(mode, 0, 4, 'mode');
    RangeError.checkValueInInterval(o.scoring._cValue, 0, 2, 'scoring');
    final u = query._toUtf8();
    var r = Pointer<Void>.fromAddress(0);
    try {
      r = (lib.filterRaws != null)
          ? lib.filterRaws!(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
              o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
              o.scoring._cValue)
          : (lib.filterEx2 != null)
              ? lib.filterEx2!(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                  o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                  o.limit, o.scoring._cValue)
              : lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                  o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                  o.limit);
      if (r == nullptr) return const [];
      final n = lib.rLen(r);
      final out = List<int>.generate(n, (i) => lib.rItem(r, i), growable: false);
      lib.rFree(r);
      r = nullptr;
      return out;
    } finally {
      malloc.free(u.ptr);
      if (r != nullptr) lib.rFree(r);
    }
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
      _keepAlive(); // touch instance field → object stays reachable through await
    }
  }

  List<FuzzyHit<T>> _toHits(List<_RawHit> raws) => [
        for (final r in raws)
          if (r.index < _items.length) // guard: UINT32_MAX sentinel on C error
            FuzzyHit<T>(_items[r.index], r.index, r.score, _kindOf(r.kind),
                r.kind, r.key, r.indices)
      ];

  // Shared native call for async FuzzyHit path (sendable across isolate boundary).
  // highlight=false → filterRaws (fast); highlight=true → filterEx2 (with indices).
  static List<_RawHit> _rawFilter(
      _Lib lib, Pointer<Void> ptr, String query, int mode, FuzzyOptions o) {
    RangeError.checkValueInInterval(mode, 0, 4, 'mode');
    RangeError.checkValueInInterval(o.scoring._cValue, 0, 2, 'scoring');
    final u = query._toUtf8();
    var r = Pointer<Void>.fromAddress(0);
    try {
      if (o.highlight) {
        r = (lib.filterEx2 != null)
            ? lib.filterEx2!(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
                o.scoring._cValue)
            : lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit);
      } else {
        r = (lib.filterRaws != null)
            ? lib.filterRaws!(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                o.normalization._cValue, o.parallel ? 1 : 0, o.threads, o.limit,
                o.scoring._cValue)
            : (lib.filterEx2 != null)
                ? lib.filterEx2!(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                    o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                    o.limit, o.scoring._cValue)
                : lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching._cValue,
                    o.normalization._cValue, o.parallel ? 1 : 0, o.threads,
                    o.limit);
      }
      if (r == nullptr) {
        throw StateError(
            'ffz_ffi_filter returned null — out of memory or invalid parameters.');
      }
      final n = lib.rLen(r);
      final out = <_RawHit>[];
      for (var i = 0; i < n; i++) {
        List<int> idx = const [];
        if (o.highlight) {
          final ni = lib.rNIdx(r, i);
          idx = List<int>.generate(ni, (j) => lib.rIdx(r, i, j),
              growable: false);
        }
        out.add(_RawHit(lib.rItem(r, i), lib.rScore(r, i), lib.rKind(r, i),
            lib.rKey(r, i), idx));
      }
      lib.rFree(r);
      r = nullptr;
      return out;
    } finally {
      malloc.free(u.ptr);
      if (r != nullptr) lib.rFree(r);
    }
  }

  /// Disposes this instance and releases native resources.
  ///
  /// Idempotent. If async operations (searches or a build) are in-flight,
  /// native memory is freed asynchronously once they complete — this method
  /// returns immediately and does **not** throw. For an awaitable guarantee
  /// that native memory has been freed before the next line executes, use
  /// [disposeAndWait] instead.
  ///
  /// This behaviour makes it safe to call from `State.dispose()` in Flutter,
  /// where `await` is not available.
  void dispose() {
    if (_disposed) return;
    _disposed = true; // ← 立即标记，消除两次并发调用都能通过首次检查的竞态窗口
    if (_inFlight > 0 || _building) {
      // In-flight async operations are still reading/writing native memory.
      // Delegate to the async path so they can finish safely before freeing.
      // disposeAndWait() skips the _disposed guard and proceeds to wait+free.
      unawaited(disposeAndWait());
      return;
    }
    _freed = true;
    _l.finalizer.detach(this);
    _l.free(_ptr);
  }

  /// Like [dispose], but first awaits any in-flight async search or build, so it
  /// never throws on pending work. Safe to call while async work is running.
  ///
  /// May also be called by [dispose] when work is in flight (in which case
  /// `_disposed` is already `true` — we still need to wait and free via `_freed`).
  Future<void> disposeAndWait() async {
    // Mark disposed so no new operations start (idempotent if dispose() already set it).
    _disposed = true;
    if (_inFlight > 0 || _building) {
      await (_idle ??= Completer<void>()).future;
    }
    // _freed guards against double-free: dispose() may have called us while also
    // freeing synchronously in a race, or the user may call disposeAndWait() twice.
    if (_freed) return;
    _freed = true;
    _l.finalizer.detach(this);
    _l.free(_ptr);
  }
}

/// Single-best-hit view of a [FuzzyCorpus], obtained via [FuzzyCorpus.one].
///
/// Exposes the same five search modes as the corpus, but each returns the top
/// [FuzzyHit] (or `null` when nothing matches) instead of a `List`. It runs the
/// **identical** native scan as the corresponding list method with `limit: 1`,
/// so there is no extra cost — it just keeps the single best hit.
class FuzzyOne<T> {
  FuzzyOne._(this._c);
  final FuzzyCorpus<T> _c;

  /// Best fuzzy hit, or null. See [FuzzyCorpus.fuzzy].
  FuzzyHit<T>? fuzzy(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _one(_mFuzzy, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Best substring hit, or null. See [FuzzyCorpus.substring].
  FuzzyHit<T>? substring(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _one(_mSubstring, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Best prefix hit, or null. See [FuzzyCorpus.prefix].
  FuzzyHit<T>? prefix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _one(_mPrefix, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Best postfix hit, or null. See [FuzzyCorpus.postfix].
  FuzzyHit<T>? postfix(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _one(_mPostfix, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Best exact hit, or null. See [FuzzyCorpus.exact].
  FuzzyHit<T>? exact(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _one(_mExact, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Async [fuzzy].
  ///
  /// Each call spawns a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<FuzzyHit<T>?> fuzzyAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _oneAsync(_mFuzzy, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Async [substring].
  ///
  /// Each call spawns a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<FuzzyHit<T>?> substringAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _oneAsync(_mSubstring, query, caseMatching, normalization, parallel,
          threads, highlight, scoring);

  /// Async [prefix].
  ///
  /// Each call spawns a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<FuzzyHit<T>?> prefixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _oneAsync(_mPrefix, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  /// Async [postfix].
  ///
  /// Each call spawns a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<FuzzyHit<T>?> postfixAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _oneAsync(_mPostfix, query, caseMatching, normalization, parallel,
          threads, highlight, scoring);

  /// Async [exact].
  ///
  /// Each call spawns a new isolate; for high-frequency input (e.g. typing),
  /// debounce 100–200 ms to avoid excessive isolate churn.
  Future<FuzzyHit<T>?> exactAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          bool? highlight,
          FuzzyScoring? scoring}) =>
      _oneAsync(_mExact, query, caseMatching, normalization, parallel, threads,
          highlight, scoring);

  FuzzyHit<T>? _one(int mode, String q, FuzzyCase? cm, FuzzyNorm? nm, bool? par,
      int? th, bool? hl, FuzzyScoring? sc) {
    final r = _c._search(mode, q, _c._eff(cm, nm, par, th, 1, hl, sc));
    return r.isEmpty ? null : r.first;
  }

  Future<FuzzyHit<T>?> _oneAsync(int mode, String q, FuzzyCase? cm,
      FuzzyNorm? nm, bool? par, int? th, bool? hl, FuzzyScoring? sc) async {
    final r = await _c._searchAsync(mode, q, _c._eff(cm, nm, par, th, 1, hl, sc));
    return r.isEmpty ? null : r.first;
  }

  // ── Raw-object single-result variants ─────────────────────────────────────

  /// Best fuzzy match as raw object, or null. See [FuzzyCorpus.fuzzyRaws].
  T? fuzzyRaw(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRaw(_mFuzzy, query, caseMatching, normalization, parallel, threads,
          scoring);

  /// Best substring match — raw object. See [FuzzyCorpus.substringRaws].
  T? substringRaw(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRaw(_mSubstring, query, caseMatching, normalization, parallel, threads,
          scoring);

  /// Best prefix match — raw object. See [FuzzyCorpus.prefixRaws].
  T? prefixRaw(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRaw(_mPrefix, query, caseMatching, normalization, parallel, threads,
          scoring);

  /// Best postfix match — raw object. See [FuzzyCorpus.postfixRaws].
  T? postfixRaw(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRaw(_mPostfix, query, caseMatching, normalization, parallel, threads,
          scoring);

  /// Best exact match — raw object. See [FuzzyCorpus.exactRaws].
  T? exactRaw(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRaw(_mExact, query, caseMatching, normalization, parallel, threads,
          scoring);

  /// Async [fuzzyRaw].
  Future<T?> fuzzyRawAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRawAsync(_mFuzzy, query, caseMatching, normalization, parallel,
          threads, scoring);

  /// Async [substringRaw].
  Future<T?> substringRawAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRawAsync(_mSubstring, query, caseMatching, normalization, parallel,
          threads, scoring);

  /// Async [prefixRaw].
  Future<T?> prefixRawAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRawAsync(_mPrefix, query, caseMatching, normalization, parallel,
          threads, scoring);

  /// Async [postfixRaw].
  Future<T?> postfixRawAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRawAsync(_mPostfix, query, caseMatching, normalization, parallel,
          threads, scoring);

  /// Async [exactRaw].
  Future<T?> exactRawAsync(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          FuzzyScoring? scoring}) =>
      _oneRawAsync(_mExact, query, caseMatching, normalization, parallel,
          threads, scoring);

  T? _oneRaw(int mode, String q, FuzzyCase? cm, FuzzyNorm? nm, bool? par,
      int? th, FuzzyScoring? sc) {
    final r = _c._searchRaws(mode, q, _c._eff(cm, nm, par, th, 1, false, sc));
    return r.isEmpty ? null : r.first;
  }

  Future<T?> _oneRawAsync(int mode, String q, FuzzyCase? cm, FuzzyNorm? nm,
      bool? par, int? th, FuzzyScoring? sc) async {
    final r = await _c._searchRawsAsync(mode, q, _c._eff(cm, nm, par, th, 1, false, sc));
    return r.isEmpty ? null : r.first;
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
    } catch (_) {
      try { f.writeAsBytesSync(const []); } catch (_) {}
    }
    return s.isEmpty ? null : s;
  }
}
