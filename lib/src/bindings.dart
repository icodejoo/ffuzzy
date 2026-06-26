// Hand-written FFI bindings — mirrors src/ffuzzy.h.
// Do NOT replace this with ffigen output without verifying ABI compatibility.
//
// To regenerate via ffigen: dart run ffigen --config ffigen.yaml
// Compare the generated output against this file before committing.

// ignore_for_file: camel_case_types, non_constant_identifier_names
// ignore_for_file: unused_element, unused_field

import 'dart:ffi' as ffi;

// ---------------------------------------------------------------------------
// Structs (mirrors src/ffuzzy.h)
// ---------------------------------------------------------------------------

/// ffuzzy_hit_t
final class FfuzzyHitT extends ffi.Struct {
  @ffi.Uint32()
  external int index;

  @ffi.Int32()
  external int score;

  external ffi.Pointer<ffi.Uint32> indices;

  @ffi.Uint32()
  external int indices_len;
}

/// ffuzzy_results_t
final class FfuzzyResultsT extends ffi.Struct {
  external ffi.Pointer<FfuzzyHitT> hits;

  @ffi.Uint32()
  external int len;
}

// ---------------------------------------------------------------------------
// Function typedefs (native side)
// ---------------------------------------------------------------------------

typedef _ffuzzy_corpus_new_native = ffi.Pointer<ffi.Void> Function();
typedef _ffuzzy_corpus_new_dart = ffi.Pointer<ffi.Void> Function();

typedef _ffuzzy_corpus_add_native = ffi.Void Function(
    ffi.Pointer<ffi.Void> corpus,
    ffi.Pointer<ffi.Pointer<ffi.Char>> items,
    ffi.Uint32 count);
typedef _ffuzzy_corpus_add_dart = void Function(
    ffi.Pointer<ffi.Void> corpus,
    ffi.Pointer<ffi.Pointer<ffi.Char>> items,
    int count);

typedef _ffuzzy_corpus_len_native = ffi.Uint32 Function(
    ffi.Pointer<ffi.Void> corpus);
typedef _ffuzzy_corpus_len_dart = int Function(ffi.Pointer<ffi.Void> corpus);

typedef _ffuzzy_corpus_free_native = ffi.Void Function(
    ffi.Pointer<ffi.Void> corpus);
typedef _ffuzzy_corpus_free_dart = void Function(ffi.Pointer<ffi.Void> corpus);

typedef _ffuzzy_filter_native = ffi.Pointer<FfuzzyResultsT> Function(
    ffi.Pointer<ffi.Void> corpus,
    ffi.Pointer<ffi.Char> query,
    ffi.Int32 ignore_case,
    ffi.Uint32 limit);
typedef _ffuzzy_filter_dart = ffi.Pointer<FfuzzyResultsT> Function(
    ffi.Pointer<ffi.Void> corpus,
    ffi.Pointer<ffi.Char> query,
    int ignore_case,
    int limit);

typedef _ffuzzy_results_free_native = ffi.Void Function(
    ffi.Pointer<FfuzzyResultsT> results);
typedef _ffuzzy_results_free_dart = void Function(
    ffi.Pointer<FfuzzyResultsT> results);

// ---------------------------------------------------------------------------
// Generated binding class (hand-written placeholder)
// ---------------------------------------------------------------------------

/// Low-level bindings to the native ffuzzy library.
///
/// Prefer using [FuzzyCorpus] from matcher.dart instead.
class FfuzzyBindings {
  final ffi.DynamicLibrary _lib;

  FfuzzyBindings(this._lib);

  late final _corpus_new =
      _lib.lookupFunction<_ffuzzy_corpus_new_native, _ffuzzy_corpus_new_dart>(
          'ffuzzy_corpus_new');

  late final _corpus_add =
      _lib.lookupFunction<_ffuzzy_corpus_add_native, _ffuzzy_corpus_add_dart>(
          'ffuzzy_corpus_add');

  late final _corpus_len =
      _lib.lookupFunction<_ffuzzy_corpus_len_native, _ffuzzy_corpus_len_dart>(
          'ffuzzy_corpus_len');

  late final _corpus_free =
      _lib.lookupFunction<_ffuzzy_corpus_free_native, _ffuzzy_corpus_free_dart>(
          'ffuzzy_corpus_free');

  /// Raw native function pointer for ffuzzy_corpus_free, used by NativeFinalizer.
  late final ffi.Pointer<ffi.NativeFunction<_ffuzzy_corpus_free_native>>
      corpusFreePointer =
          _lib.lookup<ffi.NativeFunction<_ffuzzy_corpus_free_native>>(
              'ffuzzy_corpus_free');

  late final _filter =
      _lib.lookupFunction<_ffuzzy_filter_native, _ffuzzy_filter_dart>(
          'ffuzzy_filter');

  late final _results_free = _lib.lookupFunction<_ffuzzy_results_free_native,
      _ffuzzy_results_free_dart>('ffuzzy_results_free');

  ffi.Pointer<ffi.Void> corpusNew() => _corpus_new();

  void corpusAdd(ffi.Pointer<ffi.Void> corpus,
      ffi.Pointer<ffi.Pointer<ffi.Char>> items, int count) =>
      _corpus_add(corpus, items, count);

  int corpusLen(ffi.Pointer<ffi.Void> corpus) => _corpus_len(corpus);

  void corpusFree(ffi.Pointer<ffi.Void> corpus) => _corpus_free(corpus);

  ffi.Pointer<FfuzzyResultsT> filter(ffi.Pointer<ffi.Void> corpus,
      ffi.Pointer<ffi.Char> query, int ignoreCase, int limit) =>
      _filter(corpus, query, ignoreCase, limit);

  void resultsFree(ffi.Pointer<FfuzzyResultsT> results) =>
      _results_free(results);
}
