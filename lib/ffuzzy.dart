/// ffuzzy —— 基于 nucleo (Rust) + flutter_rust_bridge 的高性能模糊搜索。
///
/// 入口与公开 API:
///  - [ffuzzy.ensureInitialized] 一次性初始化(懒加载、幂等)。
///  - [FuzzyMatcher] 泛型搜索器(字符串/对象),`match` 返回 [FuzzyOutput]。
///  - [FuzzyStringMatcher] 字符串搜索器,`match` 返回 [FuzzyHit]。
///  - 独立函数 [fuzzyMatch] / [fuzzyMatchIndices] / [fuzzyFilter] / [fuzzyFilterAsync]。
///  - 类型 [FuzzyConfig] / [FuzzyHit] / [FuzzyMatch] / [FuzzyOutput] / 常量 [kDefaultFuzzyConfig]。
library;

// 入口类按品牌名小写 `ffuzzy`，豁免类型大驼峰命名 lint。
// ignore_for_file: camel_case_types

import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

import 'src/rust/api/fuzzy.dart';
import 'src/rust/frb_generated.dart';

export 'src/rust/api/fuzzy.dart'
    show
        FuzzyConfig,
        FuzzyHit,
        FuzzyMatch,
        fuzzyMatch,
        fuzzyMatchIndices,
        fuzzyFilter,
        fuzzyFilterAsync;

void _checkLimit(int? limit) {
  if (limit != null && limit < 0) {
    throw ArgumentError.value(limit, 'limit', 'limit 不能为负');
  }
}

/// 默认配置:忽略大小写、Unicode 归一化、前缀优先(已规避 nucleo issue #92)。
const FuzzyConfig kDefaultFuzzyConfig = FuzzyConfig(
  ignoreCase: true,
  normalize: true,
  preferPrefix: true,
);

/// 插件入口:初始化收口。
class ffuzzy {
  ffuzzy._();

  static Future<void>? _initFuture;
  static bool _initialized = false;

  /// 初始化底层 Rust 库。**懒加载 + 幂等**:首次调用真正初始化,之后立即返回同一个 Future;
  /// 已初始化则直接跳过。可在 `main` 里 await,也可在「真正用之前」await。
  static Future<void> ensureInitialized() =>
      _initFuture ??= RustLib.init().then((_) => _initialized = true);

  /// 初始化是否已**完成**(非"已开始")。同步搜索前应为 true。
  static bool get isInitialized => _initialized;
}

/// 一条命中结果:[obj] 命中的原始对象、[score] 匹配分、[indices] 命中字符下标(用于高亮)。
class FuzzyOutput<T> {
  const FuzzyOutput(this.obj, this.score, this.indices);
  final T obj;
  final int score;
  final Uint32List indices;
}

/// 匹配器基类:统一管理索引(index)的构建/释放、在飞异步排空、dispose 等生命周期。
///
/// 设计要点:
///  - **从不自动建/重建索引**:`match`/`matchAsync` 有索引就用索引(快);无索引(未建、或已
///    [freeIndices])则**退化为整表扫描**(慢,但不分配持久索引,也绝不偷偷把内存加回来)。
///    要加速请显式 [buildIndices](已存在则跳过)或 [refresh]。
///  - **[freeIndices] 只释放 Rust 侧索引**:Dart 侧的源/投影始终保留(它相对对象很小、且可秒级重建),
///    `buildIndices` 即用它在 Rust 侧重建。
///  - **[dispose] 两侧全销毁**:释放 Rust 索引 + 丢弃 Dart 侧数据引用,实例不可再用,需重建。
abstract class _IndexedMatcher {
  _IndexedMatcher(this.indexed, this.config);

  /// 是否启用 Rust 侧常驻索引;false 则每次把整表传入 Rust。
  final bool indexed;

  /// 匹配配置。
  final FuzzyConfig config;

  FuzzyCorpus? _corpus;
  bool _disposed = false;
  int _inFlight = 0;
  bool _freeWhenIdle = false;
  // 结构性变更(refresh/dispose)自增,用于丢弃过期的在飞异步结果。
  int _generation = 0;
  final List<Completer<void>> _idleWaiters = <Completer<void>>[];

  // —— 子类钩子 ——
  /// 当前数据源对应的可搜索字符串(无数据返回 null)。始终保留,仅 dispose 时清。
  List<String>? get _haystacks;

  /// dispose 时清理 Dart 侧全部数据引用。
  void _disposeData();

  /// 索引是否已建立(高速模式)。
  bool get hasIndices => indexed && _corpus != null && !_freeWhenIdle;

  /// 是否已 dispose。
  bool get isDisposed => _disposed;

  /// 显式建立索引。索引已存在则直接跳过(幂等)。非索引模式为空操作。
  /// 要求已 `await ffuzzy.ensureInitialized()`。
  void buildIndices() {
    _ensureAlive();
    if (!indexed) return;
    if (_corpus != null && !_freeWhenIdle) return; // 已存在 -> 跳过
    final hs = _haystacks;
    if (hs == null) {
      throw StateError('无数据源,请先 refresh(source) 再 buildIndices()');
    }
    _freeWhenIdle = false;
    _corpus = FuzzyCorpus(items: hs);
  }

  /// 只释放 Rust 侧索引(Dart 侧源/投影保留,`buildIndices` 可秒级重建)。
  /// 释放后 `match` 退化为整表扫描。有在飞异步搜索时,推迟到其排空后再释放。
  /// 幂等。仅 `indexed=true` 生效。要连 Dart 侧数据一起释放请用 [dispose]。
  void freeIndices() {
    if (_disposed || !indexed) return;
    if (_inFlight == 0) {
      _releaseCorpus();
    } else {
      _freeWhenIdle = true;
    }
  }

  /// 彻底销毁:释放 Rust 索引 + 丢弃 Dart 侧数据。实例不可再用,需重建。
  /// 有在飞搜索则排空后再释放。幂等。
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _generation++; // 丢弃在飞搜索结果
    if (_inFlight == 0) {
      _releaseCorpus();
      _disposeData();
    }
  }

  /// 同 [dispose],但 Future 在「在飞搜索排空且资源已释放」后才完成。
  Future<void> disposeAndWait() {
    dispose();
    if (_inFlight == 0) return Future<void>.value();
    final completer = Completer<void>();
    _idleWaiters.add(completer);
    return completer.future;
  }

  bool _warnedScan = false;

  /// indexed 模式下走了退化扫描(忘了 buildIndices / 已 free),debug 下每实例提醒一次。
  void _warnScanFallback() {
    if (indexed && kDebugMode && !_warnedScan) {
      _warnedScan = true;
      debugPrint('[ffuzzy] 提示:索引未建立,本次为慢速整表扫描。'
          '调用 buildIndices() 进入高速模式。');
    }
  }

  List<FuzzyHit> _rawMatch(String query, int? limit) {
    _ensureAlive();
    _checkLimit(limit);
    if (!ffuzzy.isInitialized) {
      throw StateError('ffuzzy 尚未初始化完成,同步方法前请先 `await ffuzzy.ensureInitialized()`');
    }
    final hs = _haystacks;
    if (hs == null) {
      throw StateError('无数据源,请先 refresh(source)');
    }
    // 有索引走索引(快);无索引(未建/已 freeIndices/indexed=false)退化为整表扫描
    // (慢,但不分配持久索引、绝不自动重建)。
    if (_corpus != null) {
      return _corpus!.filter(query: query, config: config, limit: limit);
    }
    _warnScanFallback();
    return fuzzyFilter(query: query, items: hs, config: config, limit: limit);
  }

  Future<List<FuzzyHit>> _rawMatchAsync(String query, int? limit) async {
    _ensureAlive();
    _checkLimit(limit);
    final gen = _generation; // 发起时的版本
    _inFlight++; // 同步占位，确保紧随的 dispose/free 能感知到在飞搜索
    try {
      await ffuzzy.ensureInitialized();
      final hs = _haystacks;
      // 期间发生 refresh/dispose(版本变化或数据已清)→ 丢弃,等同终止旧任务。
      if (gen != _generation || hs == null) return const <FuzzyHit>[];
      final List<FuzzyHit> result;
      if (_corpus != null) {
        result = await _corpus!.filterAsync(query: query, config: config, limit: limit);
      } else {
        _warnScanFallback();
        result = await fuzzyFilterAsync(
          query: query,
          items: hs,
          config: config,
          limit: limit,
        );
      }
      // 搜索期间若 refresh/dispose,结果已过期 → 丢弃,避免错位/越界/崩溃。
      if (gen != _generation) return const <FuzzyHit>[];
      return result;
    } finally {
      _inFlight--;
      if (_inFlight == 0) _onIdle();
    }
  }

  /// refresh 用:替换数据源后重建索引。
  void _rebuildCorpus() {
    _generation++; // 数据已换,丢弃在飞旧结果
    if (!indexed) return;
    final hs = _haystacks;
    if (hs == null) return;
    if (_inFlight == 0) {
      _corpus?.dispose(); // 无在飞搜索才显式释放旧索引
    }
    // 有在飞搜索时,旧 corpus 由 Rust Arc 持有至其结束、之后 GC 回收;这里直接换新引用。
    _corpus = FuzzyCorpus(items: hs);
    _freeWhenIdle = false;
  }

  void _onIdle() {
    if (_disposed) {
      _releaseCorpus();
      _disposeData();
    } else if (_freeWhenIdle) {
      _releaseCorpus();
      _freeWhenIdle = false;
    }
    for (final w in _idleWaiters) {
      if (!w.isCompleted) w.complete();
    }
    _idleWaiters.clear();
  }

  /// 把新投影串增量追加到已建索引(无索引时为空操作,数据已在 Dart 源里,下次 build 生效)。
  void _appendToIndex(List<String> haystacks) {
    if (haystacks.isEmpty) return;
    if (_corpus != null) _corpus!.add(items: haystacks);
  }

  void _corpusClear() => _corpus?.clear();
  void _corpusSetAt(int index, String haystack) =>
      _corpus?.setAt(index: index, item: haystack);
  void _corpusRemoveIndices(List<int> indices) {
    if (_corpus != null && indices.isNotEmpty) {
      _corpus!.removeIndices(indices: indices);
    }
  }

  void _releaseCorpus() {
    _corpus?.dispose();
    _corpus = null;
  }

  void _ensureAlive() {
    if (_disposed) {
      throw StateError('该匹配器已被 dispose,不能再使用');
    }
  }
}

/// 字符串模糊搜索器:面向 `List<String>`,`match` 返回带原列表下标的 [FuzzyHit]。
class FuzzyStringMatcher extends _IndexedMatcher {
  /// [indexed] 是否启用常驻索引(默认 true);false 则每次把整表传入 Rust。
  FuzzyStringMatcher(
    List<String> items, {
    bool indexed = true,
    FuzzyConfig config = kDefaultFuzzyConfig,
  })  : _src = List<String>.of(items), // 可增长,支持 add
        super(indexed, config);

  List<String>? _src;

  @override
  List<String>? get _haystacks => _src;

  @override
  void _disposeData() => _src = null;

  /// 候选集(只读视图)。
  List<String> get items =>
      _src == null ? const <String>[] : UnmodifiableListView(_src!);

  /// 增量追加一条;若已建索引,直接追加到 Rust 索引(不重建)。
  void add(String item) => addAll(<String>[item]);

  /// 增量追加多条。已 dispose 抛错。
  void addAll(Iterable<String> items) {
    _ensureAlive();
    final list = items.toList();
    if (list.isEmpty) return;
    _src!.addAll(list);
    _appendToIndex(list);
  }

  /// 改:替换下标 [index] 处的候选。会丢弃在飞结果(内容已变)。
  void update(int index, String item) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _src!);
    _generation++;
    _src![index] = item;
    _corpusSetAt(index, item);
  }

  /// 删:移除下标 [index] 处的候选(后续下标前移)。会丢弃在飞结果。
  void removeAt(int index) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _src!);
    _generation++;
    _src!.removeAt(index);
    _corpusRemoveIndices(<int>[index]);
  }

  /// 删:按条件批量移除。会丢弃在飞结果。返回移除条数。
  int removeWhere(bool Function(String item) test) {
    _ensureAlive();
    final idx = <int>[for (var i = 0; i < _src!.length; i++) if (test(_src![i])) i];
    if (idx.isEmpty) return 0;
    _generation++;
    for (var k = idx.length - 1; k >= 0; k--) {
      _src!.removeAt(idx[k]);
    }
    _corpusRemoveIndices(idx);
    return idx.length;
  }

  /// 清空全部候选(实例保留,可继续 add/refresh)。
  void clear() {
    _ensureAlive();
    _generation++;
    _src!.clear();
    _corpusClear();
  }

  /// 候选数量。
  int get length => _src?.length ?? 0;

  /// 同步搜索。无索引时退化为整表扫描(慢);用 [buildIndices] 加速。
  List<FuzzyHit> match(String query, {int? limit}) => _rawMatch(query, limit);

  /// 异步搜索:后台线程执行,不阻塞 UI。
  Future<List<FuzzyHit>> matchAsync(String query, {int? limit}) =>
      _rawMatchAsync(query, limit);

  /// 取最佳一条(与 [match] 元素类型一致):无命中返回 `null`。
  FuzzyHit? single(String query) {
    final r = match(query, limit: 1);
    return r.isEmpty ? null : r.first;
  }

  /// [single] 的异步版本。
  Future<FuzzyHit?> singleAsync(String query) async {
    final r = await matchAsync(query, limit: 1);
    return r.isEmpty ? null : r.first;
  }

  /// 替换数据源并**自动重建索引**(适合「先占位空列表、数据回来后再喂入」)。
  void refresh(List<String> source) {
    _ensureAlive();
    _src = List<String>.unmodifiable(source);
    _rebuildCorpus();
  }
}

/// 泛型模糊搜索器:对任意类型 [T] 建索引,`match` 直接返回命中的**对象**([FuzzyOutput])。
class FuzzyMatcher<T> extends _IndexedMatcher {
  /// [stringOf] 把每个元素投影成「用于索引的可搜索串」(搜字符串列表用 `(s) => s`,
  /// 多字段用 `(g) => '${g.a} ${g.b}'`);[indexed] 是否启用常驻索引(默认 true);[config] 匹配配置。
  FuzzyMatcher(
    List<T> items,
    String Function(T) stringOf, {
    bool indexed = true,
    FuzzyConfig config = kDefaultFuzzyConfig,
  })  : _objs = List<T>.of(items), // 可增长,支持 add
        _stringOf = stringOf,
        super(indexed, config);

  /// 便捷构造:候选为 `Map` 且按字段名 [key] 搜索(如 `'gameName'`)。
  static FuzzyMatcher<Map<String, dynamic>> key(
    List<Map<String, dynamic>> items,
    String key, {
    bool indexed = true,
    FuzzyConfig config = kDefaultFuzzyConfig,
  }) =>
      FuzzyMatcher<Map<String, dynamic>>(
        items,
        (m) => (m[key] as String?) ?? '',
        indexed: indexed,
        config: config,
      );

  List<T>? _objs;
  final String Function(T) _stringOf;
  List<String>? _projected; // Dart 侧派生索引缓存

  @override
  List<String>? get _haystacks {
    final objs = _objs;
    if (objs == null) return null;
    return _projected ??= <String>[for (final o in objs) _stringOf(o)];
  }

  // 投影缓存随实例存亡(相对对象很小);dispose 时连对象一起丢。
  @override
  void _disposeData() {
    _objs = null;
    _projected = null;
  }

  /// 候选数量。
  int get length => _objs?.length ?? 0;

  /// 增量追加一条对象;若已建索引,投影后直接追加到 Rust 索引(不重建)。
  void add(T item) => addAll(<T>[item]);

  /// 增量追加多条对象。已 dispose 抛错。
  void addAll(Iterable<T> items) {
    _ensureAlive();
    final list = items.toList();
    if (list.isEmpty) return;
    final newHaystacks = <String>[for (final o in list) _stringOf(o)];
    _objs!.addAll(list);
    _projected?.addAll(newHaystacks); // 保持投影缓存与对象同步
    _appendToIndex(newHaystacks);
  }

  /// 改:替换下标 [index] 处的对象(重新投影该条)。会丢弃在飞结果。
  void update(int index, T item) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _objs!);
    _generation++;
    _objs![index] = item;
    final s = _stringOf(item);
    if (_projected != null) _projected![index] = s;
    _corpusSetAt(index, s);
  }

  /// 删:移除下标 [index] 处的对象(后续下标前移)。会丢弃在飞结果。
  void removeAt(int index) {
    _ensureAlive();
    RangeError.checkValidIndex(index, _objs!);
    _generation++;
    _objs!.removeAt(index);
    _projected?.removeAt(index);
    _corpusRemoveIndices(<int>[index]);
  }

  /// 删:按条件批量移除。会丢弃在飞结果。返回移除条数。
  int removeWhere(bool Function(T item) test) {
    _ensureAlive();
    final idx = <int>[for (var i = 0; i < _objs!.length; i++) if (test(_objs![i])) i];
    if (idx.isEmpty) return 0;
    _generation++;
    for (var k = idx.length - 1; k >= 0; k--) {
      _objs!.removeAt(idx[k]);
      _projected?.removeAt(idx[k]);
    }
    _corpusRemoveIndices(idx);
    return idx.length;
  }

  /// 清空全部候选(实例保留,可继续 add/refresh)。
  void clear() {
    _ensureAlive();
    _generation++;
    _objs!.clear();
    _projected?.clear();
    _corpusClear();
  }

  /// 同步搜索,返回命中对象。无索引时退化为整表扫描(慢);用 [buildIndices] 加速。
  List<FuzzyOutput<T>> match(String query, {int? limit}) =>
      _project(_rawMatch(query, limit));

  /// 异步搜索:后台线程执行,不阻塞 UI。
  /// 若搜索期间发生 [refresh]/[dispose],本次结果会被丢弃(返回空),由调用方按新状态重查。
  Future<List<FuzzyOutput<T>>> matchAsync(String query, {int? limit}) async =>
      _project(await _rawMatchAsync(query, limit));

  /// 取最佳一条(与 [match] 元素类型一致,`FuzzyOutput<T>`):无命中返回 `null`。
  /// 命中对象用 `single(q)?.obj` 取。
  FuzzyOutput<T>? single(String query) {
    final r = match(query, limit: 1);
    return r.isEmpty ? null : r.first;
  }

  /// [single] 的异步版本。
  Future<FuzzyOutput<T>?> singleAsync(String query) async {
    final r = await matchAsync(query, limit: 1);
    return r.isEmpty ? null : r.first;
  }

  /// 替换数据源并**自动重建索引**(适合「先占位空列表、数据回来后再喂入」)。
  void refresh(List<T> source) {
    _ensureAlive();
    _objs = List<T>.unmodifiable(source);
    _projected = null;
    _rebuildCorpus();
  }

  List<FuzzyOutput<T>> _project(List<FuzzyHit> hits) {
    final objs = _objs;
    if (objs == null) return const []; // 已 dispose,兜底
    return <FuzzyOutput<T>>[
      for (final h in hits)
        if (h.index >= 0 && h.index < objs.length)
          FuzzyOutput<T>(objs[h.index], h.score, h.indices),
    ];
  }
}

/// 在默认/现有配置基础上改少量字段,避免每次写全三个字段。
extension FuzzyConfigCopyWith on FuzzyConfig {
  FuzzyConfig copyWith({bool? ignoreCase, bool? normalize, bool? preferPrefix}) =>
      FuzzyConfig(
        ignoreCase: ignoreCase ?? this.ignoreCase,
        normalize: normalize ?? this.normalize,
        preferPrefix: preferPrefix ?? this.preferPrefix,
      );
}
