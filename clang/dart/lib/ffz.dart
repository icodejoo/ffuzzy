/// ffz — idiomatic Dart binding for the C fuzzy matcher (clang/), via dart:ffi.
///
/// Usage:
/// ```dart
/// final corpus = FfzCorpus();          // or FfzCorpus(libraryPath: '...')
/// corpus.addAll(['src/main.rs', 'lib/ffz.dart', '中文搜索']);
/// final hits = corpus.filter('src', mode: FfzMode.fuzzy, parallel: true, limit: 50);
/// for (final h in hits) print('${h.index}  score=${h.score}  ${h.indices}');
/// corpus.dispose();                    // or rely on the NativeFinalizer
/// ```
library;

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

/// Match algorithm for a query (mirrors the C `ffz_mode`).
enum FfzMode { fuzzy, substring, prefix, postfix, exact }

/// One search result. [index] is the item's insertion order; [indices] are the
/// matched code-unit positions within the matched key (for highlighting).
class FfzHit {
  final int index;
  final int score;
  final int matchedKind; // ffz_key_kind of the key that scored best
  final List<int> indices;
  const FfzHit(this.index, this.score, this.matchedKind, this.indices);

  @override
  String toString() => 'FfzHit(index: $index, score: $score, kind: $matchedKind)';
}

// ── native function signatures ──────────────────────────────────────────────
typedef _NewN = Pointer<Void> Function();
typedef _AddN = Void Function(Pointer<Void>, Pointer<Utf8>, Size);
typedef _LenN = Size Function(Pointer<Void>);
typedef _FreeN = Void Function(Pointer<Void>);
typedef _FilterN = Pointer<Void> Function(
    Pointer<Void>, Pointer<Utf8>, Size, Int32, Int32, Int32, Size);
typedef _RLenN = Size Function(Pointer<Void>);
typedef _RItemN = Uint32 Function(Pointer<Void>, Size);
typedef _RScoreN = Int32 Function(Pointer<Void>, Size);
typedef _RKindN = Int32 Function(Pointer<Void>, Size);
typedef _RNIdxN = Size Function(Pointer<Void>, Size);
typedef _RIdxN = Uint32 Function(Pointer<Void>, Size, Size);
typedef _RFreeN = Void Function(Pointer<Void>);

/// Resolved bindings for one loaded library (cached per library path).
class _Lib {
  _Lib(this.lib)
      : nw = lib.lookupFunction<_NewN, Pointer<Void> Function()>('ffz_ffi_new'),
        add = lib.lookupFunction<_AddN,
            void Function(Pointer<Void>, Pointer<Utf8>, int)>('ffz_ffi_add'),
        len = lib.lookupFunction<_LenN, int Function(Pointer<Void>)>('ffz_ffi_len'),
        filter = lib.lookupFunction<_FilterN,
            Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
                int, int)>('ffz_ffi_filter'),
        rLen = lib.lookupFunction<_RLenN, int Function(Pointer<Void>)>('ffz_ffi_results_len'),
        rItem = lib.lookupFunction<_RItemN, int Function(Pointer<Void>, int)>('ffz_ffi_results_item'),
        rScore = lib.lookupFunction<_RScoreN, int Function(Pointer<Void>, int)>('ffz_ffi_results_score'),
        rKind = lib.lookupFunction<_RKindN, int Function(Pointer<Void>, int)>('ffz_ffi_results_kind'),
        rNIdx = lib.lookupFunction<_RNIdxN, int Function(Pointer<Void>, int)>('ffz_ffi_results_nindices'),
        rIdx = lib.lookupFunction<_RIdxN, int Function(Pointer<Void>, int, int)>('ffz_ffi_results_index'),
        rFree = lib.lookupFunction<_RFreeN, void Function(Pointer<Void>)>('ffz_ffi_results_free'),
        free = lib.lookupFunction<_FreeN, void Function(Pointer<Void>)>('ffz_ffi_free'),
        finalizer = NativeFinalizer(
            lib.lookup<NativeFunction<_FreeN>>('ffz_ffi_free').cast());

  final DynamicLibrary lib;
  final Pointer<Void> Function() nw;
  final void Function(Pointer<Void>, Pointer<Utf8>, int) add;
  final int Function(Pointer<Void>) len;
  final Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int, int, int) filter;
  final int Function(Pointer<Void>) rLen;
  final int Function(Pointer<Void>, int) rItem;
  final int Function(Pointer<Void>, int) rScore;
  final int Function(Pointer<Void>, int) rKind;
  final int Function(Pointer<Void>, int) rNIdx;
  final int Function(Pointer<Void>, int, int) rIdx;
  final void Function(Pointer<Void>) rFree;
  final void Function(Pointer<Void>) free;
  final NativeFinalizer finalizer;

  static final Map<String, _Lib> _cache = {};
  static _Lib resolve(String? path) {
    final key = path ?? '<default>';
    return _cache.putIfAbsent(key, () => _Lib(_open(path)));
  }

  static DynamicLibrary _open(String? path) {
    if (path != null) return DynamicLibrary.open(path);
    if (Platform.isWindows) return DynamicLibrary.open('ffz.dll');
    if (Platform.isMacOS || Platform.isIOS) return DynamicLibrary.process();
    return DynamicLibrary.open('libffz.so');
  }
}

/// A resident corpus of items that can be fuzzy/substring/prefix/etc. filtered.
///
/// Native memory is released by [dispose], or automatically when this object is
/// garbage-collected (via a [NativeFinalizer]) — but calling [dispose] promptly
/// is recommended for large corpora.
class FfzCorpus implements Finalizable {
  FfzCorpus({String? libraryPath}) : _l = _Lib.resolve(libraryPath) {
    _ptr = _l.nw();
    if (_ptr == nullptr) {
      throw StateError('ffz_ffi_new returned null');
    }
    _l.finalizer.attach(this, _ptr.cast(), detach: this);
  }

  final _Lib _l;
  late final Pointer<Void> _ptr;
  bool _disposed = false;

  void _check() {
    if (_disposed) throw StateError('FfzCorpus used after dispose()');
  }

  /// Append one item (UTF-8).
  void add(String item) {
    _check();
    final p = item.toNativeUtf8();
    _l.add(_ptr, p, p.length);
    malloc.free(p);
  }

  /// Append many items.
  void addAll(Iterable<String> items) {
    _check();
    for (final s in items) {
      final p = s.toNativeUtf8();
      _l.add(_ptr, p, p.length);
      malloc.free(p);
    }
  }

  /// Number of items.
  int get length {
    _check();
    return _l.len(_ptr);
  }

  /// Filter the corpus.
  ///
  /// [parallel] enables the multi-threaded scoring pass; [threads] == 0 means
  /// auto (half the CPUs, capped at 8; a global ceiling of cpu-1 always
  /// applies). [limit] == 0 returns all matches. When [highlight] is false the
  /// match indices are not read back (slightly faster).
  List<FfzHit> filter(
    String query, {
    FfzMode mode = FfzMode.fuzzy,
    bool parallel = false,
    int threads = 0,
    int limit = 0,
    bool highlight = true,
  }) {
    _check();
    final q = query.toNativeUtf8();
    final r = _l.filter(_ptr, q, q.length, mode.index, parallel ? 1 : 0,
        threads, limit);
    malloc.free(q);
    final n = _l.rLen(r);
    final out = <FfzHit>[];
    for (var i = 0; i < n; i++) {
      List<int> idx = const [];
      if (highlight) {
        final ni = _l.rNIdx(r, i);
        idx = List<int>.generate(ni, (j) => _l.rIdx(r, i, j), growable: false);
      }
      out.add(FfzHit(_l.rItem(r, i), _l.rScore(r, i), _l.rKind(r, i), idx));
    }
    _l.rFree(r);
    return out;
  }

  /// Release native memory now. Idempotent; safe to call before GC.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _l.finalizer.detach(this);
    _l.free(_ptr);
  }
}
