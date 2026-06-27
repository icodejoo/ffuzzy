/// ffuzzy — idiomatic Dart binding for the compact C fuzzy matcher, via dart:ffi.
///
/// ```dart
/// final corpus = FfzCorpus();                  // or matchPaths/preferPrefix
/// corpus.addAll(['src/main.rs', 'lib/ffz.dart', '中文搜索']);
/// final hits = corpus.filter('src', parallel: true, limit: 50);
/// for (final h in hits) {
///   final u16 = ffzCodepointToUtf16('src/main.rs', h.indices); // for TextSpan
///   print('${h.index}  score=${h.score}  kind=${h.matchedKind}  $u16');
/// }
/// corpus.dispose();                             // or rely on the NativeFinalizer
/// ```
///
/// NOTE: every call is synchronous and runs on the calling isolate; for a large
/// corpus, create and use the `FfzCorpus` on a background isolate so `filter`
/// does not jank the UI. An `FfzCorpus` must only be used on the isolate that
/// created it (it owns a native pointer).
library;

import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

/// Match algorithm for a query (mirrors the C `ffz_mode`).
///
/// [fuzzy] also parses the query into space-separated terms and fzf-style
/// operators (`!` negate, `^` prefix, `'` substring, `$` suffix) — so a
/// multi-word query like `'lib parse'` is an AND of terms (this is what
/// `perf/PERF.md` calls "word"). The other modes treat the whole query as one
/// literal atom: [substring]/[prefix]/[postfix] match a contiguous run, [exact]
/// matches the whole string.
enum FfzMode { fuzzy, substring, prefix, postfix, exact }

/// Case handling (mirrors `ffz_case_matching`).
enum FfzCase { respect, ignore, smart }

/// Unicode normalization (mirrors `ffz_normalization`).
enum FfzNorm { never, smart }

/// Which key produced a hit. Custom host kinds use values >= 100.
enum FfzKeyKind { original, pinyin, initials, romaji, custom }

/// The raw C `ffz_key_kind` code for a kind (original=0..romaji=3, custom=100).
extension FfzKeyKindCode on FfzKeyKind {
  int get code => switch (this) {
        FfzKeyKind.original => 0,
        FfzKeyKind.pinyin => 1,
        FfzKeyKind.initials => 2,
        FfzKeyKind.romaji => 3,
        FfzKeyKind.custom => 100,
      };
}

/// An alternate search key for an item (e.g. host-computed pinyin/romaji), for
/// [FfzCorpus.addKeyed]. [kind] is a [FfzKeyKind] code (use `FfzKeyKind.x.code`)
/// or any host-defined value >= 100.
class FfzKey {
  final String text;
  final int kind;
  const FfzKey(this.text, {this.kind = 1 /* pinyin */});
  FfzKey.kind(this.text, FfzKeyKind kind) : kind = kind.code;
}

FfzKeyKind _kindOf(int v) => switch (v) {
      0 => FfzKeyKind.original,
      1 => FfzKeyKind.pinyin,
      2 => FfzKeyKind.initials,
      3 => FfzKeyKind.romaji,
      _ => FfzKeyKind.custom,
    };

/// Thrown when the native library can't be loaded or a symbol is missing.
class FfzException implements Exception {
  final String message;
  const FfzException(this.message);
  @override
  String toString() => 'FfzException: $message';
}

/// One search result. [index] is the item's insertion order; [indices] are the
/// matched **codepoint** positions within the matched key — use
/// [ffzCodepointToUtf16] before applying them to a Dart `String`.
class FfzHit {
  final int index;
  final int score;
  final FfzKeyKind matchedKind;
  final int matchedKey; // which key of the item matched (0 == original)
  final List<int> indices;
  const FfzHit(
      this.index, this.score, this.matchedKind, this.matchedKey, this.indices);

  @override
  String toString() =>
      'FfzHit(index: $index, score: $score, kind: $matchedKind)';
}

/// Convert codepoint indices (as in [FfzHit.indices]) to UTF-16 code-unit
/// offsets into [text], suitable for Dart `String`/`TextSpan` highlighting.
/// (Dart strings are UTF-16; astral chars/emoji occupy two code units.)
List<int> ffzCodepointToUtf16(String text, List<int> codepointIndices) {
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
      throw FfzException('failed to load ffz native library: $e');
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

/// A resident corpus of items that can be fuzzy/substring/prefix/etc. filtered.
/// Release with [dispose], or rely on the [NativeFinalizer] on GC.
class FfzCorpus implements Finalizable {
  /// [matchPaths] tunes delimiters for path-like text; [preferPrefix] biases
  /// scoring toward matches near the start. [libraryPath] loads a specific
  /// native library file (tests / non-bundled use).
  FfzCorpus(
      {bool matchPaths = false, bool preferPrefix = false, String? libraryPath})
      : _l = _Lib.resolve(libraryPath),
        _libPath = libraryPath {
    _ptr = _l.newCfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    if (_ptr == nullptr) {
      throw const FfzException('ffz_ffi_new_cfg returned null');
    }
    _l.finalizer.attach(this, _ptr.cast(), detach: this);
  }

  final _Lib _l;
  final String? _libPath; // remembered so filterAsync can reopen on a worker
  late final Pointer<Void> _ptr;
  bool _disposed = false;
  int _inFlight = 0; // pending filterAsync calls reading the native corpus

  void _check() {
    if (_disposed) throw StateError('FfzCorpus used after dispose()');
  }

  // Mutating/freeing the corpus while a filterAsync reads it from a worker
  // isolate would be a native use-after-free; refuse with a catchable error.
  void _checkMutate() {
    _check();
    if (_inFlight > 0) {
      throw StateError(
          'FfzCorpus mutated while $_inFlight filterAsync call(s) in flight');
    }
  }

  void add(String item) {
    _checkMutate();
    final u = item._toUtf8();
    _l.add(_ptr, u.ptr, u.len);
    malloc.free(u.ptr);
  }

  void addAll(Iterable<String> items) {
    _check();
    for (final s in items) {
      add(s);
    }
  }

  /// Add [item] with explicit alternate search [keys] — e.g. host-computed
  /// pinyin/romaji/initials, so a CJK item is findable by typing latin. The
  /// ORIGINAL key (the item text) is added automatically. A hit reports which
  /// key matched via [FfzHit.matchedKind]/[FfzHit.matchedKey].
  /// ```dart
  /// corpus.addKeyed('张三', [
  ///   FfzKey.kind('zhangsan', FfzKeyKind.pinyin),
  ///   FfzKey.kind('zs', FfzKeyKind.initials),
  /// ]);
  /// ```
  void addKeyed(String item, List<FfzKey> keys) {
    _checkMutate();
    final f = _l.addKeyed;
    if (f == null) {
      throw const FfzException('ffz_ffi_add_keyed missing in native library');
    }
    final iu = item._toUtf8();
    final n = keys.length;
    if (n == 0) {
      _l.add(_ptr, iu.ptr, iu.len);
      malloc.free(iu.ptr);
      return;
    }
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

  int get length {
    _check();
    return _l.len(_ptr);
  }

  /// Remove all items (the corpus stays usable).
  void clear() {
    _checkMutate();
    _l.clear(_ptr);
  }

  /// Filter the corpus.
  ///
  /// [parallel]/[threads]: multi-threaded scoring (`threads:0` = auto, half the
  /// CPUs capped at 8; a hard ceiling of cpu-1 always applies; corpora < 512
  /// items run single-threaded). [limit] == 0 returns all matches.
  /// [highlight] false skips reading match indices.
  List<FfzHit> filter(
    String query, {
    FfzMode mode = FfzMode.fuzzy,
    FfzCase caseMatching = FfzCase.smart,
    FfzNorm normalization = FfzNorm.smart,
    bool parallel = false,
    int threads = 0,
    int limit = 0,
    bool highlight = true,
  }) {
    _check();
    return _filterWith(_l, _ptr, query, mode.index, caseMatching.index,
        normalization.index, parallel ? 1 : 0, threads, limit, highlight);
  }

  /// Like [filter] but runs the native scan + result marshaling on a background
  /// isolate, so a large corpus never janks the UI isolate. Combine with
  /// `parallel: true` to also fan the C scan across threads. Multiple
  /// `filterAsync` calls may overlap safely (each gets its own native matcher).
  ///
  /// The corpus's native memory is process-global, so the worker reads it
  /// directly. While a `filterAsync` is in flight, [add]/[addKeyed]/[clear]/
  /// [dispose] throw [StateError] (mutating it would be a native
  /// use-after-free). Awaiting the returned future also keeps this corpus alive
  /// across the call, so the finalizer can't free it mid-scan — but do keep a
  /// reference and don't drop the future if you rely on that.
  Future<List<FfzHit>> filterAsync(
    String query, {
    FfzMode mode = FfzMode.fuzzy,
    FfzCase caseMatching = FfzCase.smart,
    FfzNorm normalization = FfzNorm.smart,
    bool parallel = false,
    int threads = 0,
    int limit = 0,
    bool highlight = true,
  }) async {
    _check();
    final addr = _ptr.address;
    final libPath = _libPath;
    final m = mode.index, cmI = caseMatching.index, nmI = normalization.index;
    final par = parallel ? 1 : 0;
    _inFlight++;
    try {
      return await Isolate.run(() {
        // New isolate: statics are fresh, so reopen the library (the OS returns
        // the already-loaded image) and address the shared corpus by pointer.
        final lib = _Lib.resolve(libPath);
        return _filterWith(lib, Pointer<Void>.fromAddress(addr), query, m, cmI,
            nmI, par, threads, limit, highlight);
      });
    } finally {
      // Touching the instance field after the await keeps `this` (and thus the
      // native corpus) reachable for the whole call, defeating the finalizer.
      _inFlight--;
    }
  }

  // Shared native call + result read, usable on any isolate (the FfzHit list it
  // returns is sendable). `ptr` must be a live corpus in this process.
  static List<FfzHit> _filterWith(
      _Lib lib,
      Pointer<Void> ptr,
      String query,
      int mode,
      int cm,
      int nm,
      int par,
      int threads,
      int limit,
      bool highlight) {
    final u = query._toUtf8();
    final r =
        lib.filterEx(ptr, u.ptr, u.len, mode, cm, nm, par, threads, limit);
    malloc.free(u.ptr);
    if (r == nullptr) throw const FfzException('filter failed (out of memory)');
    final n = lib.rLen(r);
    final out = <FfzHit>[];
    for (var i = 0; i < n; i++) {
      List<int> idx = const [];
      if (highlight) {
        final ni = lib.rNIdx(r, i);
        idx = List<int>.generate(ni, (j) => lib.rIdx(r, i, j), growable: false);
      }
      out.add(FfzHit(lib.rItem(r, i), lib.rScore(r, i),
          _kindOf(lib.rKind(r, i)), lib.rKey(r, i), idx));
    }
    lib.rFree(r);
    return out;
  }

  /// Release native memory now. Idempotent. Throws [StateError] if a
  /// [filterAsync] is still in flight (freeing would be a use-after-free) —
  /// await the pending futures first.
  void dispose() {
    if (_disposed) return;
    if (_inFlight > 0) {
      throw StateError(
          'FfzCorpus.dispose() with $_inFlight filterAsync call(s) in flight');
    }
    _disposed = true;
    _l.finalizer.detach(this);
    _l.free(_ptr);
  }
}

/// Optional native crash handler for **non-recoverable** faults.
///
/// Recoverable errors already surface as [FfzException]/[StateError] and are
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
/// final report = FfzCrash.lastReport();      // previous run's crash, if any
/// if (report != null) log('ffz last crash:\n$report');
/// FfzCrash.install(breadcrumbPath: '${dir.path}/ffz_crash.log');
/// ```
class FfzCrash {
  FfzCrash._();
  static String? _path;

  /// Install the handler. [breadcrumbPath] (optional) receives the backtrace of
  /// the next crash. [libraryPath] mirrors [FfzCorpus]. Returns true if the
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
