import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'package:ffi/ffi.dart';

import 'bindings.dart';

// ---------------------------------------------------------------------------
// Library loader
// ---------------------------------------------------------------------------

/// Returns the [ffi.DynamicLibrary] for the native ffuzzy shared library.
///
/// Platform lookup order:
///   Android / Linux : libffuzzy.so   (loaded from the Flutter bundle)
///   iOS / macOS     : DynamicLibrary.process() (static-linked via CocoaPods)
///   Windows         : ffuzzy.dll
ffi.DynamicLibrary _openLibrary() {
  if (ffi.Abi.current() == ffi.Abi.androidArm ||
      ffi.Abi.current() == ffi.Abi.androidArm64 ||
      ffi.Abi.current() == ffi.Abi.androidX64 ||
      ffi.Abi.current() == ffi.Abi.androidX86 ||
      ffi.Abi.current() == ffi.Abi.linuxX64 ||
      ffi.Abi.current() == ffi.Abi.linuxArm64) {
    return ffi.DynamicLibrary.open('libffuzzy.so');
  }
  if (ffi.Abi.current() == ffi.Abi.macosArm64 ||
      ffi.Abi.current() == ffi.Abi.macosX64 ||
      ffi.Abi.current() == ffi.Abi.iosArm64 ||
      ffi.Abi.current() == ffi.Abi.iosX64) {
    return ffi.DynamicLibrary.process();
  }
  if (ffi.Abi.current() == ffi.Abi.windowsX64 ||
      ffi.Abi.current() == ffi.Abi.windowsIA32 ||
      ffi.Abi.current() == ffi.Abi.windowsArm64) {
    return ffi.DynamicLibrary.open('ffuzzy.dll');
  }
  // Fallback: try process (covers macOS simulator etc.)
  return ffi.DynamicLibrary.process();
}

// Lazily initialised singleton bindings object.
FfuzzyBindings? _bindings;
FfuzzyBindings get _b => _bindings ??= FfuzzyBindings(_openLibrary());

// ---------------------------------------------------------------------------
// Public API types
// ---------------------------------------------------------------------------

/// A single fuzzy-match hit returned by [FuzzyCorpus.filter].
class FuzzyHit {
  /// Index of the matched item in the corpus (same order as the list passed
  /// to the [FuzzyCorpus] constructor).
  final int index;

  /// Smith-Waterman score; higher is better.
  final int score;

  /// Zero-based character positions in the original string that were matched
  /// to the query characters (in order).
  final List<int> indices;

  const FuzzyHit({
    required this.index,
    required this.score,
    required this.indices,
  });

  @override
  String toString() =>
      'FuzzyHit(index: $index, score: $score, indices: $indices)';
}

// ---------------------------------------------------------------------------
// FuzzyCorpus
// ---------------------------------------------------------------------------

/// A searchable corpus backed by the native C ffuzzy library.
///
/// ```dart
/// final corpus = FuzzyCorpus(['hello world', 'fuzzy search', 'flutter']);
/// final hits   = corpus.filter('flu');
/// corpus.dispose();
/// ```
///
/// [FuzzyCorpus] is single-threaded from the Dart perspective.  Use
/// [filterAsync] to run the search in a background [Isolate] so the UI
/// thread remains responsive.
class FuzzyCorpus {
  final ffi.Pointer<ffi.Void> _ptr;
  bool _disposed = false;

  /// Creates a corpus pre-loaded with [items].
  FuzzyCorpus(List<String> items) : _ptr = _b.corpusNew() {
    if (_ptr == ffi.nullptr) {
      throw StateError('ffuzzy_corpus_new() returned null (OOM?)');
    }
    _addItems(items);
  }

  void _addItems(List<String> items) {
    if (items.isEmpty) return;
    final arena = Arena();
    try {
      // Allocate array of C-string pointers.
      final ptrs =
          arena<ffi.Pointer<ffi.Char>>(items.length);
      for (int i = 0; i < items.length; i++) {
        ptrs[i] = items[i].toNativeUtf8(allocator: arena).cast<ffi.Char>();
      }
      _b.corpusAdd(_ptr, ptrs, items.length);
    } finally {
      arena.releaseAll();
    }
  }

  /// Appends [items] to the existing corpus.
  void add(List<String> items) {
    _assertNotDisposed();
    _addItems(items);
  }

  /// Number of items currently in the corpus.
  int get length {
    _assertNotDisposed();
    return _b.corpusLen(_ptr);
  }

  /// Synchronously search the corpus for [query].
  ///
  /// Parameters:
  /// - [ignoreCase] : if true (default), matching is case-insensitive.
  /// - [limit]      : maximum number of results; 0 means no limit.
  ///
  /// Returns a list sorted descending by score.
  List<FuzzyHit> filter(String query, {bool ignoreCase = true, int? limit}) {
    _assertNotDisposed();
    final arena = Arena();
    try {
      final queryPtr =
          query.toNativeUtf8(allocator: arena).cast<ffi.Char>();
      final nativeLimit = limit ?? 0;
      final resPtr = _b.filter(
          _ptr, queryPtr, ignoreCase ? 1 : 0, nativeLimit);
      if (resPtr == ffi.nullptr) return const [];

      final results = resPtr.ref;
      final hits = <FuzzyHit>[];
      for (int i = 0; i < results.len; i++) {
        final h = results.hits[i];
        final idxList = <int>[];
        for (int k = 0; k < h.indices_len; k++) {
          idxList.add(h.indices[k]);
        }
        hits.add(FuzzyHit(
          index: h.index,
          score: h.score,
          indices: idxList,
        ));
      }
      _b.resultsFree(resPtr);
      return hits;
    } finally {
      arena.releaseAll();
    }
  }

  /// Asynchronously search the corpus in a background [Isolate].
  ///
  /// Same parameters as [filter].  The calling isolate is not blocked.
  Future<List<FuzzyHit>> filterAsync(String query,
      {bool ignoreCase = true, int? limit}) {
    _assertNotDisposed();
    // We pass the raw integer address of the corpus pointer across isolates.
    // The corpus must stay alive (not disposed) for the duration of the call.
    final corpusAddr = _ptr.address;
    return Isolate.run(() {
      final ptr = ffi.Pointer<ffi.Void>.fromAddress(corpusAddr);
      final arena = Arena();
      try {
        final queryPtr =
            query.toNativeUtf8(allocator: arena).cast<ffi.Char>();
        final resPtr = _b.filter(ptr, queryPtr, ignoreCase ? 1 : 0, limit ?? 0);
        if (resPtr == ffi.nullptr) return const <FuzzyHit>[];

        final results = resPtr.ref;
        final hits = <FuzzyHit>[];
        for (int i = 0; i < results.len; i++) {
          final h = results.hits[i];
          final idxList = <int>[];
          for (int k = 0; k < h.indices_len; k++) {
            idxList.add(h.indices[k]);
          }
          hits.add(FuzzyHit(
            index: h.index,
            score: h.score,
            indices: List<int>.unmodifiable(idxList),
          ));
        }
        _b.resultsFree(resPtr);
        return List<FuzzyHit>.unmodifiable(hits);
      } finally {
        arena.releaseAll();
      }
    });
  }

  /// Releases all native memory.  The corpus must not be used after this.
  void dispose() {
    if (_disposed) return;
    _b.corpusFree(_ptr);
    _disposed = true;
  }

  void _assertNotDisposed() {
    if (_disposed) throw StateError('FuzzyCorpus has been disposed');
  }
}
