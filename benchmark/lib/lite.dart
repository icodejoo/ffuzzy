/// ffuzzy **lite** — fuzzy-only, single-threaded, no incremental cache.
///
/// Use this library instead of `package:ffuzzy/ffuzzy.dart` when binary size
/// is critical.  Import this file and Gradle/CocoaPods auto-detect it,
/// selecting the lite native library (~254 KB vs ~531 KB full).
///
/// API surface (no `mode` parameter):
///  - [LiteConfig] — three-field config (ignoreCase / normalize / preferPrefix).
///  - [LiteFuzzyStringMatcher] / [LiteFuzzyMatcher] — indexed searchers.
///  - Top-level: [liteFuzzyMatch] / [liteFuzzyMatchIndices] /
///    [liteFuzzyFilter] / [liteFuzzyFilterAsync].
library;

// ignore_for_file: camel_case_types

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

import 'src/rust/api/fuzzy.dart';
import 'src/rust/frb_generated.dart';

export 'src/rust/api/fuzzy.dart' show FuzzyHit, FuzzyMatch;

// ─────────────────── Lifecycle ───────────────────

/// Plugin entry-point: same as `package:ffuzzy/ffuzzy.dart`.
class ffuzzy {
  ffuzzy._();

  static Future<void>? _initFuture;
  static bool _initialized = false;

  static Future<void> ensureInitialized() =>
      _initFuture ??= RustLib.init().then((_) => _initialized = true);

  static bool get isInitialized => _initialized;
}

// ─────────────────── Config ───────────────────

/// Lite configuration: fuzzy-only, single-threaded, no incremental cache.
///
/// Replaces [FuzzyConfig] for lite users; maps to a fixed Rust [FuzzyConfig]
/// with `mode=fuzzy`, `parallel=false`, `incremental=false`.
class LiteConfig {
  const LiteConfig({
    this.ignoreCase = true,
    this.normalize = true,
    this.preferPrefix = true,
  });

  final bool ignoreCase;
  final bool normalize;
  final bool preferPrefix;

  FuzzyConfig _toFuzzy() => FuzzyConfig(
        ignoreCase: ignoreCase,
        normalize: normalize,
        preferPrefix: preferPrefix,
        mode: MatchMode.fuzzy,
        parallel: false,
        incremental: false,
      );

  LiteConfig copyWith({bool? ignoreCase, bool? normalize, bool? preferPrefix}) =>
      LiteConfig(
        ignoreCase: ignoreCase ?? this.ignoreCase,
        normalize: normalize ?? this.normalize,
        preferPrefix: preferPrefix ?? this.preferPrefix,
      );
}

/// Default lite config: ignoreCase=true, normalize=true, preferPrefix=true.
const LiteConfig kDefaultLiteConfig = LiteConfig();

// ─────────────────── Result types ───────────────────

/// Typed match result for [LiteFuzzyMatcher] (mirrors [FuzzyOutput] from full).
class LiteOutput<T> {
  const LiteOutput(this.obj, this.score, this.indices);
  final T obj;
  final int score;
  final Uint32List indices;
}

// ─────────────────── Standalone functions ───────────────────

void _checkLimit(int? limit) {
  if (limit != null && limit < 0) {
    throw ArgumentError.value(limit, 'limit', 'limit 不能为负');
  }
}

/// Score a single string; returns `null` when no match.
int? liteFuzzyMatch(
  String query,
  String haystack, {
  LiteConfig config = kDefaultLiteConfig,
}) =>
    fuzzyMatch(query: query, haystack: haystack, config: config._toFuzzy());

/// Score a single string and return highlighted indices; returns `null` on no match.
FuzzyMatch? liteFuzzyMatchIndices(
  String query,
  String haystack, {
  LiteConfig config = kDefaultLiteConfig,
}) =>
    fuzzyMatchIndices(query: query, haystack: haystack, config: config._toFuzzy());

/// Filter a list synchronously. Results are sorted by score descending.
List<FuzzyHit> liteFuzzyFilter(
  String query,
  List<String> items, {
  LiteConfig config = kDefaultLiteConfig,
  int? limit,
}) {
  _checkLimit(limit);
  return fuzzyFilter(query: query, items: items, config: config._toFuzzy(), limit: limit);
}

/// Async variant of [liteFuzzyFilter]; runs on a worker thread.
Future<List<FuzzyHit>> liteFuzzyFilterAsync(
  String query,
  List<String> items, {
  LiteConfig config = kDefaultLiteConfig,
  int? limit,
}) {
  _checkLimit(limit);
  return fuzzyFilterAsync(query: query, items: items, config: config._toFuzzy(), limit: limit);
}

// ─────────────────── String matcher ───────────────────

/// Lite indexed string searcher.
///
/// API is identical to [FuzzyStringMatcher] from `ffuzzy.dart` except
/// the `mode` parameter is absent from [match] / [matchAsync] / [single] /
/// [singleAsync] (always fuzzy).
class LiteFuzzyStringMatcher {
  LiteFuzzyStringMatcher(
    List<String> items, {
    bool indexed = true,
    LiteConfig config = kDefaultLiteConfig,
  })  : _src = List<String>.of(items),
        _indexed = indexed,
        _config = config;

  final bool _indexed;
  final LiteConfig _config;
  List<String>? _src;

  FuzzyCorpus? _corpus;
  bool _disposed = false;
  int _inFlight = 0;
  bool _freeWhenIdle = false;
  int _generation = 0;
  final List<Completer<void>> _idleWaiters = <Completer<void>>[];
  bool _warnedScan = false;

  bool get hasIndices => _indexed && _corpus != null && !_freeWhenIdle;
  bool get isDisposed => _disposed;
  int get length => _src?.length ?? 0;
  List<String> get items => _src == null ? const <String>[] : List<String>.unmodifiable(_src!);

  FuzzyConfig get _fuzzyConfig => _config._toFuzzy();

  void buildIndices() {
    _ensureAlive();
    if (!_indexed) return;
    if (_corpus != null && !_freeWhenIdle) return;
    final hs = _src;
    if (hs == null) throw StateError('无数据源,请先 refresh(source) 再 buildIndices()');
    _freeWhenIdle = false;
    _corpus = FuzzyCorpus(items: hs);
  }

  Future<void> buildIndicesAsync() async {
    _ensureAlive();
    if (!_indexed) return;
    if (_corpus != null && !_freeWhenIdle) return;
    final hs = _src;
    if (hs == null) throw StateError('无数据源,请先 refresh(source) 再 buildIndicesAsync()');
    final gen = _generation;
    await ffuzzy.ensureInitialized();
    if (_disposed || gen != _generation) return;
    final corpus = await fuzzyCorpusNewAsync(items: hs);
    if (_disposed || gen != _generation) {
      corpus.dispose();
      return;
    }
    _freeWhenIdle = false;
    _corpus = corpus;
  }

  void freeIndices() {
    if (_disposed || !_indexed) return;
    if (_inFlight == 0) {
      _releaseCorpus();
    } else {
      _freeWhenIdle = true;
    }
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _generation++;
    if (_inFlight == 0) {
      _releaseCorpus();
      _src = null;
    }
  }

  Future<void> disposeAndWait() {
    dispose();
    if (_inFlight == 0) return Future<void>.value();
    final c = Completer<void>();
    _idleWaiters.add(c);
    return c.future;
  }

  void add(String item) => addAll(<String>[item]);

  void addAll(Iterable<String> newItems) {
    _ensureAlive();
    final list = newItems.toList();
    if (list.isEmpty) return;
    _src!.addAll(list);
    if (_corpus != null) _corpus!.add(items: list);
  }

  void update(int index, String item) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _src!);
    _generation++;
    _src![index] = item;
    _corpus?.setAt(index: index, item: item);
  }

  void removeAt(int index) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _src!);
    _generation++;
    _src!.removeAt(index);
    if (_corpus != null) _corpus!.removeIndices(indices: <int>[index]);
  }

  int removeWhere(bool Function(String) test) {
    _ensureAlive();
    final idx = <int>[for (var i = 0; i < _src!.length; i++) if (test(_src![i])) i];
    if (idx.isEmpty) return 0;
    _generation++;
    for (var k = idx.length - 1; k >= 0; k--) {
      _src!.removeAt(idx[k]);
    }
    if (_corpus != null && idx.isNotEmpty) _corpus!.removeIndices(indices: idx);
    return idx.length;
  }

  void clear() {
    _ensureAlive();
    _generation++;
    _src!.clear();
    _corpus?.clear();
  }

  void refresh(List<String> source) {
    _ensureAlive();
    _generation++;
    _src = List<String>.of(source);
    if (!_indexed) return;
    if (_inFlight == 0) _corpus?.dispose();
    _corpus = FuzzyCorpus(items: _src!);
    _freeWhenIdle = false;
  }

  List<FuzzyHit> match(String query, {int? limit, bool? ignoreCase}) {
    _ensureAlive();
    _checkLimit(limit);
    if (!ffuzzy.isInitialized) {
      throw StateError('ffuzzy 尚未初始化完成,同步方法前请先 `await ffuzzy.ensureInitialized()`');
    }
    final cfg = ignoreCase == null ? _fuzzyConfig : _config.copyWith(ignoreCase: ignoreCase)._toFuzzy();
    final hs = _src;
    if (hs == null) throw StateError('无数据源,请先 refresh(source)');
    if (_corpus != null) return _corpus!.filter(query: query, config: cfg, limit: limit);
    _warnFallback();
    return fuzzyFilter(query: query, items: hs, config: cfg, limit: limit);
  }

  Future<List<FuzzyHit>> matchAsync(String query, {int? limit, bool? ignoreCase}) async {
    _ensureAlive();
    _checkLimit(limit);
    final cfg = ignoreCase == null ? _fuzzyConfig : _config.copyWith(ignoreCase: ignoreCase)._toFuzzy();
    final gen = _generation;
    _inFlight++;
    try {
      await ffuzzy.ensureInitialized();
      final hs = _src;
      if (gen != _generation || hs == null) return const <FuzzyHit>[];
      final List<FuzzyHit> result;
      if (_corpus != null) {
        result = await _corpus!.filterAsync(query: query, config: cfg, limit: limit);
      } else {
        _warnFallback();
        result = await fuzzyFilterAsync(query: query, items: hs, config: cfg, limit: limit);
      }
      if (gen != _generation) return const <FuzzyHit>[];
      return result;
    } finally {
      _inFlight--;
      if (_inFlight == 0) _onIdle();
    }
  }

  FuzzyHit? single(String query, {bool? ignoreCase}) {
    final r = match(query, limit: 1, ignoreCase: ignoreCase);
    return r.isEmpty ? null : r.first;
  }

  Future<FuzzyHit?> singleAsync(String query, {bool? ignoreCase}) async {
    final r = await matchAsync(query, limit: 1, ignoreCase: ignoreCase);
    return r.isEmpty ? null : r.first;
  }

  void _warnFallback() {
    if (_indexed && kDebugMode && !_warnedScan) {
      _warnedScan = true;
      debugPrint('[ffuzzy lite] 提示:索引未建立,本次为慢速整表扫描。调用 buildIndices() 进入高速模式。');
    }
  }

  void _releaseCorpus() {
    _corpus?.dispose();
    _corpus = null;
  }

  void _onIdle() {
    if (_disposed) {
      _releaseCorpus();
      _src = null;
    } else if (_freeWhenIdle) {
      _releaseCorpus();
      _freeWhenIdle = false;
    }
    for (final w in _idleWaiters) {
      if (!w.isCompleted) w.complete();
    }
    _idleWaiters.clear();
  }

  void _ensureAlive() {
    if (_disposed) throw StateError('该匹配器已被 dispose,不能再使用');
  }
}

// ─────────────────── Generic typed matcher ───────────────────

/// Lite generic searcher over any type [T].
///
/// Identical to [FuzzyMatcher] from `ffuzzy.dart` except `mode` is absent.
class LiteFuzzyMatcher<T> {
  LiteFuzzyMatcher(
    List<T> items,
    String Function(T) stringOf, {
    bool indexed = true,
    LiteConfig config = kDefaultLiteConfig,
  })  : _objs = List<T>.of(items),
        _stringOf = stringOf,
        _indexed = indexed,
        _config = config;

  static LiteFuzzyMatcher<Map<String, dynamic>> key(
    List<Map<String, dynamic>> items,
    String key, {
    bool indexed = true,
    LiteConfig config = kDefaultLiteConfig,
  }) =>
      LiteFuzzyMatcher<Map<String, dynamic>>(
        items,
        (m) => (m[key] as String?) ?? '',
        indexed: indexed,
        config: config,
      );

  final bool _indexed;
  final LiteConfig _config;
  final String Function(T) _stringOf;
  List<T>? _objs;
  List<String>? _projected;

  FuzzyCorpus? _corpus;
  bool _disposed = false;
  int _inFlight = 0;
  bool _freeWhenIdle = false;
  int _generation = 0;
  final List<Completer<void>> _idleWaiters = <Completer<void>>[];
  bool _warnedScan = false;

  bool get hasIndices => _indexed && _corpus != null && !_freeWhenIdle;
  bool get isDisposed => _disposed;
  int get length => _objs?.length ?? 0;

  FuzzyConfig get _fuzzyConfig => _config._toFuzzy();

  List<String>? get _haystacks {
    final objs = _objs;
    if (objs == null) return null;
    return _projected ??= <String>[for (final o in objs) _stringOf(o)];
  }

  void buildIndices() {
    _ensureAlive();
    if (!_indexed) return;
    if (_corpus != null && !_freeWhenIdle) return;
    final hs = _haystacks;
    if (hs == null) throw StateError('无数据源,请先 refresh(source) 再 buildIndices()');
    _freeWhenIdle = false;
    _corpus = FuzzyCorpus(items: hs);
    _projected = null;
  }

  Future<void> buildIndicesAsync() async {
    _ensureAlive();
    if (!_indexed) return;
    if (_corpus != null && !_freeWhenIdle) return;
    final hs = _haystacks;
    if (hs == null) throw StateError('无数据源,请先 refresh(source) 再 buildIndicesAsync()');
    final gen = _generation;
    await ffuzzy.ensureInitialized();
    if (_disposed || gen != _generation) return;
    final corpus = await fuzzyCorpusNewAsync(items: hs);
    if (_disposed || gen != _generation) {
      corpus.dispose();
      return;
    }
    _freeWhenIdle = false;
    _corpus = corpus;
    _projected = null;
  }

  void freeIndices() {
    if (_disposed || !_indexed) return;
    if (_inFlight == 0) {
      _releaseCorpus();
    } else {
      _freeWhenIdle = true;
    }
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _generation++;
    if (_inFlight == 0) {
      _releaseCorpus();
      _objs = null;
      _projected = null;
    }
  }

  Future<void> disposeAndWait() {
    dispose();
    if (_inFlight == 0) return Future<void>.value();
    final c = Completer<void>();
    _idleWaiters.add(c);
    return c.future;
  }

  void add(T item) => addAll(<T>[item]);

  void addAll(Iterable<T> newItems) {
    _ensureAlive();
    final list = newItems.toList();
    if (list.isEmpty) return;
    final newHaystacks = <String>[for (final o in list) _stringOf(o)];
    _objs!.addAll(list);
    _projected?.addAll(newHaystacks);
    if (_corpus != null) _corpus!.add(items: newHaystacks);
  }

  void update(int index, T item) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _objs!);
    _generation++;
    _objs![index] = item;
    final s = _stringOf(item);
    if (_projected != null) _projected![index] = s;
    _corpus?.setAt(index: index, item: s);
  }

  void removeAt(int index) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _objs!);
    _generation++;
    _objs!.removeAt(index);
    _projected?.removeAt(index);
    if (_corpus != null) _corpus!.removeIndices(indices: <int>[index]);
  }

  int removeWhere(bool Function(T) test) {
    _ensureAlive();
    final idx = <int>[for (var i = 0; i < _objs!.length; i++) if (test(_objs![i])) i];
    if (idx.isEmpty) return 0;
    _generation++;
    for (var k = idx.length - 1; k >= 0; k--) {
      _objs!.removeAt(idx[k]);
      _projected?.removeAt(idx[k]);
    }
    if (_corpus != null && idx.isNotEmpty) _corpus!.removeIndices(indices: idx);
    return idx.length;
  }

  void clear() {
    _ensureAlive();
    _generation++;
    _objs!.clear();
    _projected?.clear();
    _corpus?.clear();
  }

  void refresh(List<T> source) {
    _ensureAlive();
    _generation++;
    _objs = List<T>.of(source);
    _projected = null;
    if (!_indexed) return;
    if (_inFlight == 0) _corpus?.dispose();
    _corpus = FuzzyCorpus(items: _haystacks!);
    _freeWhenIdle = false;
    _projected = null;
  }

  List<LiteOutput<T>> match(String query, {int? limit, bool? ignoreCase}) =>
      _project(_rawMatch(query, limit, ignoreCase));

  Future<List<LiteOutput<T>>> matchAsync(String query, {int? limit, bool? ignoreCase}) async =>
      _project(await _rawMatchAsync(query, limit, ignoreCase));

  LiteOutput<T>? single(String query, {bool? ignoreCase}) {
    final r = match(query, limit: 1, ignoreCase: ignoreCase);
    return r.isEmpty ? null : r.first;
  }

  Future<LiteOutput<T>?> singleAsync(String query, {bool? ignoreCase}) async {
    final r = await matchAsync(query, limit: 1, ignoreCase: ignoreCase);
    return r.isEmpty ? null : r.first;
  }

  List<FuzzyHit> _rawMatch(String query, int? limit, bool? ignoreCase) {
    _ensureAlive();
    _checkLimit(limit);
    if (!ffuzzy.isInitialized) {
      throw StateError('ffuzzy 尚未初始化完成,同步方法前请先 `await ffuzzy.ensureInitialized()`');
    }
    final cfg = ignoreCase == null ? _fuzzyConfig : _config.copyWith(ignoreCase: ignoreCase)._toFuzzy();
    final hs = _haystacks;
    if (hs == null) throw StateError('无数据源,请先 refresh(source)');
    if (_corpus != null) return _corpus!.filter(query: query, config: cfg, limit: limit);
    _warnFallback();
    return fuzzyFilter(query: query, items: hs, config: cfg, limit: limit);
  }

  Future<List<FuzzyHit>> _rawMatchAsync(String query, int? limit, bool? ignoreCase) async {
    _ensureAlive();
    _checkLimit(limit);
    final cfg = ignoreCase == null ? _fuzzyConfig : _config.copyWith(ignoreCase: ignoreCase)._toFuzzy();
    final gen = _generation;
    _inFlight++;
    try {
      await ffuzzy.ensureInitialized();
      final hs = _haystacks;
      if (gen != _generation || hs == null) return const <FuzzyHit>[];
      final List<FuzzyHit> result;
      if (_corpus != null) {
        result = await _corpus!.filterAsync(query: query, config: cfg, limit: limit);
      } else {
        _warnFallback();
        result = await fuzzyFilterAsync(query: query, items: hs, config: cfg, limit: limit);
      }
      if (gen != _generation) return const <FuzzyHit>[];
      return result;
    } finally {
      _inFlight--;
      if (_inFlight == 0) _onIdle();
    }
  }

  List<LiteOutput<T>> _project(List<FuzzyHit> hits) {
    final objs = _objs;
    if (objs == null) return const [];
    return <LiteOutput<T>>[
      for (final h in hits)
        if (h.index >= 0 && h.index < objs.length) LiteOutput<T>(objs[h.index], h.score, h.indices),
    ];
  }

  void _warnFallback() {
    if (_indexed && kDebugMode && !_warnedScan) {
      _warnedScan = true;
      debugPrint('[ffuzzy lite] 提示:索引未建立,本次为慢速整表扫描。调用 buildIndices() 进入高速模式。');
    }
  }

  void _releaseCorpus() {
    _corpus?.dispose();
    _corpus = null;
  }

  void _onIdle() {
    if (_disposed) {
      _releaseCorpus();
      _objs = null;
      _projected = null;
    } else if (_freeWhenIdle) {
      _releaseCorpus();
      _freeWhenIdle = false;
    }
    for (final w in _idleWaiters) {
      if (!w.isCompleted) w.complete();
    }
    _idleWaiters.clear();
  }

  void _ensureAlive() {
    if (_disposed) throw StateError('该匹配器已被 dispose,不能再使用');
  }
}
