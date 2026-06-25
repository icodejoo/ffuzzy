# ffuzzy

[English](README.md) | **中文**

高性能模糊搜索 Flutter 插件:基于 [`nucleo-matcher`](https://crates.io/crates/nucleo)(Rust,
Helix 编辑器同款引擎)+ [`flutter_rust_bridge`](https://pub.dev/packages/flutter_rust_bridge),
fzf 式子序列匹配。十万级数据下比常见纯 Dart 模糊库快 **45–300 倍**。

## 特性
- ⚡ **极快**:Rust nucleo 引擎 + 常驻索引,48.8 万条单次查询约 3ms(默认多核并行;串行约 18ms)。
- 🎯 **对象 / 字符串均可搜**:`FuzzyMatcher<T>` 直接返回命中对象,`FuzzyStringMatcher` 返回原列表下标。
- ✨ **命中高亮**:返回命中字符下标,直接用于高亮。
- 🧵 **同步 + 异步**:`matchAsync` 在后台线程执行,不阻塞 UI。
- 🗂️ **可控索引**:按需 `buildIndices` / `freeIndices`,内存占用自主掌控。
- 🔁 **增删改清**:`add` / `update` / `removeWhere` / `clear` / `refresh` 增量维护,无需整体重建。
- ⚙️ **可配置**:忽略大小写、Unicode 归一化、前缀优先(已规避 nucleo issue #92)。

## 支持平台
Android · iOS · macOS · Windows · Linux(均为 cargokit 原生交叉编译)。

**Web/WASM:支持,但不建议生产使用。** Rust 侧已兼容(多核自动退化为串行),可在 Linux 上经 `Web Build (WASM)` GitHub Actions 工作流(`flutter_rust_bridge_codegen build-web`)编出 example 的 web 产物。但 **wasm 体积较大**(未进一步优化约 450KB+),web 上通常更适合用轻量的纯 Dart 库;ffuzzy 的强项在原生(Android/iOS/桌面)。(build-web 必须在 Linux/CI 跑:Windows 上 frb 无法 spawn `flutter`/`dart` 的 `.bat`。)

## 安装
```yaml
dependencies:
  ffuzzy: ^0.1.0
```
执行 `flutter pub get`。入口库:`package:ffuzzy/ffuzzy.dart`(每个类都附可直接复制的示例)。

## 目录
- [初始化 ffuzzy](#初始化-ffuzzy)
- [快速上手](#快速上手)
- [命名速查](#命名速查)
- [核心概念:索引的建立与释放](#核心概念索引的建立与释放)
- [增量更新与跨模块同步](#增量更新与跨模块同步)
- [FuzzyMatcher&lt;T&gt;](#fuzzymatchert)
- [FuzzyStringMatcher](#fuzzystringmatcher)
- [独立函数](#独立函数)
- [数据类型](#数据类型)
- [Flutter 完整示例(搜索框 + 高亮)](#flutter-完整示例搜索框--高亮)
- [性能](#性能)

---

## 初始化 ffuzzy

`ffuzzy.ensureInitialized()` **懒加载且幂等**,重复调用安全。

```dart
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main() async {
  await ffuzzy.ensureInitialized(); // 也可在「真正用之前」再 await
  runApp(const MyApp());
}
```

> ⚠️ 同步方法(`match`/`buildIndices`/`fuzzyFilter` 等)要求初始化已完成。
> 异步方法(`matchAsync`)内部会自动确保初始化。

---

## 快速上手

```dart
import 'package:ffuzzy/ffuzzy.dart';

await ffuzzy.ensureInitialized();

final matcher = FuzzyMatcher<Game>(games, (g) => g.name) // 用 name 投影
  ..buildIndices();                                       // 显式建立索引

for (final out in matcher.match('drgn', limit: 20)) {
  print('${out.obj.name}  分数=${out.score}  高亮=${out.indices}');
}

final best = matcher.single('drgn'); // 最佳一条 -> FuzzyOutput<Game>?; 对象用 best?.obj
matcher.dispose();                    // 用完销毁
```

---

## 命名速查

| 你的数据 | 用哪个类 | `match` 返回 | `single` 返回 |
|---|---|---|---|
| 对象 / Map(要返回对象) | `FuzzyMatcher<T>` | `List<FuzzyOutput<T>>`(`.obj` 取对象) | `FuzzyOutput<T>?` |
| 纯字符串列表(要下标) | `FuzzyStringMatcher` | `List<FuzzyHit>`(`.index` 回指列表) | `FuzzyHit?` |
| 只查一两次,不建索引 | 独立函数 `fuzzyFilter` / `fuzzyMatchIndices` | `List<FuzzyHit>` / `FuzzyMatch?` | — |

> 结果三类型只差第一个字段:`FuzzyOutput.obj`(对象)/ `FuzzyHit.index`(下标)/ `FuzzyMatch`(单串无定位);三者都有 `score` + `indices`。
> `limit` 必须 ≥ 0(负数抛 `ArgumentError`);`limit: 0` 返回空。

---

## 核心概念:索引的建立与释放

索引是**可选的速度缓存**,由你显式控制何时占内存:

- `match`/`matchAsync` **从不自动建/重建索引**:有索引就走索引(快);**无索引则退化为整表扫描**
  (慢,但不分配持久索引、绝不偷偷把内存加回来)。要加速请显式 `buildIndices()`(已建则跳过)或 `refresh()`。
- 所以「忘了 `buildIndices()`」不会崩溃,只是搜索变慢(等同 `indexed:false` / 独立函数的速度);
  可用 `hasIndices` 自查当前是否处于高速模式。

```text
创建实例(无索引,match 此时走慢速扫描)
  └─ buildIndices()  → 建立索引,进入高速模式(已建则跳过)
       └─ match / matchAsync / single → 搜索
            ├─ freeIndices()           → 只释放 Rust 索引(Dart 侧源/投影保留);match 退化为扫描
            └─ dispose()               → 两侧全释放并销毁实例,不可再用
refresh(source) → 换数据源并【自动重建】索引(适合先占位、数据回来再喂入)
```

| 操作 | Rust 侧索引 | Dart 侧数据 | 之后 `match` |
|---|---|---|---|
| 仅创建 / `freeIndices()` | 无 | 保留 | 能(退化扫描,慢)|
| `buildIndices()` | 建立 | 保留 | 能(快)|
| `dispose()` | 释放 | 释放全部 | 否(抛 `StateError`,需重建实例)|
| `refresh(src)` | 重建 | 替换为 src | 能(快)|

> 有在飞的异步搜索时,`freeIndices`/`dispose` 会**先等其排空**再释放,绝不释放正被后台线程使用的索引。

---

## 增量更新与跨模块同步

**matcher 持有自己的数据快照,不会观察外部集合。** 也就是说:你先 `FuzzyMatcher(A)` 建好索引,之后在别处对 `A` 做 `A.add(x)`,**matcher 搜不到 `x`**——因为它不知道 A 变了。这是刻意的(否则跨模块就得让 matcher 去耦合/监听 A)。

正确做法:**谁改数据,谁把变更喂给 matcher**;matcher 提供增量接口,代价极小。

```dart
final m = FuzzyMatcher<Game>(games, (g) => g.name)..buildIndices();
// 增
m.add(newGame);                         // 追加一条,直接进 Rust 索引,不重建
m.addAll(moreGames);                    // 批量追加
// 改
m.update(0, editedGame);                // 替换下标 0 处对象
// 删
m.removeAt(2);                          // 按下标删
final n = m.removeWhere((g) => g.disabled); // 按条件删,返回删除数
// 清空
m.clear();                              // 全清(实例保留)
// 整体替换
m.refresh(reloadedGames);               // 换源 + 重建
```

> `add` 只在末尾追加,不影响在飞搜索;`update`/`remove*`/`clear` 会改变下标/内容,因此会**丢弃在飞 `matchAsync` 结果**(返回空,由调用方按新状态重查)。`FuzzyStringMatcher` 同名方法签名把 `T` 换成 `String`。

**A 与 matcher 不在同一模块时**,选一种接法(matcher 始终不依赖 A 的具体类型):

1. **仓库/Service 持有两者(推荐)**:写一个 `GameRepo`,内部既存数据又持有 matcher,`repo.add(x)` 同时更新数据与 `matcher.add(x)`。两个模块都用 repo,matcher 不外泄。
2. **可观察数据源**:让 A 暴露变更流(`Stream`/`ChangeNotifier`),在装配层 `a.changes.listen((c) => matcher.add(c.item))`。A 不认识 matcher,matcher 不认识 A,靠装配层粘合。
3. **matcher 即数据源**:不再单独维护 A,所有增删走 matcher(`matcher.add` / `matcher.items` 读),最省心,但调用方需依赖 matcher。

> 注意:`add` 是 `&mut` 操作,若此刻有**在飞的 `matchAsync`** 正在 Rust 端读索引,`add` 会等其结束(frb 用读写锁保护,不会数据竞争);大数据 + 长在飞搜索时偶有短暂阻塞。`add` 不影响已在飞结果(只在末尾追加,不改已有下标)。

---

## FuzzyMatcher&lt;T&gt;

对任意类型 `T` 建索引,`match` 直接返回命中的**对象**(`FuzzyOutput<T>`)。

### 创建

```dart
// 函数投影(最通用)
final m1 = FuzzyMatcher<Game>(games, (g) => g.name);

// 字段名投影(Map / JSON 数据)
final m2 = FuzzyMatcher.key(jsonList, 'gameName');

// 多字段一起搜
final m3 = FuzzyMatcher<Game>(games, (g) => '${g.name} ${g.id}');

// 省内存模式:不建常驻索引,每次临时处理
final m4 = FuzzyMatcher<Game>(games, (g) => g.name, indexed: false);

// 自定义配置(推荐在默认配置上 copyWith 只改要改的字段)
final m5 = FuzzyMatcher<Game>(games, (g) => g.name,
    config: kDefaultFuzzyConfig.copyWith(ignoreCase: false));
```

### 搜索

```dart
final m = FuzzyMatcher<Game>(games, (g) => g.name)..buildIndices();

// 同步:返回按分数降序的命中对象
final List<FuzzyOutput<Game>> hits = m.match('dragon', limit: 20);
for (final h in hits) {
  print(h.obj);     // 命中的原始对象 (Game)
  print(h.score);   // 匹配分
  print(h.indices); // 命中字符下标(高亮用)
}

// 异步(后台线程,不阻塞 UI,超大数据集首选)
final hitsAsync = await m.matchAsync('dragon', limit: 20);

// 取最佳一条(与 match 元素类型一致):无命中返回 null
final FuzzyOutput<Game>? best = m.single('dragon');
final Game? obj = best?.obj;
final FuzzyOutput<Game>? bestAsync = await m.singleAsync('dragon');
```

### 换源(占位 → 数据回来)

```dart
final m = FuzzyMatcher<Game>(const <Game>[], (g) => g.name); // 先占位
final games = await api.fetchGames();  // 数据慢慢回来
m.refresh(games);                      // 换源并自动重建索引
m.match('dragon');
```

### 生命周期

```dart
final m = FuzzyMatcher.key(jsonList, 'gameName');
m.hasIndices;             // false(尚未建)
m.buildIndices();         // 建立
m.match('gold');          // 快速搜索
m.freeIndices();          // 空闲:只释放 Rust 索引(Dart 侧对象/投影保留)
m.match('gold');          // 仍可搜,只是退化为慢速扫描(不会自动重建/加内存)
m.buildIndices();         // 想再快:秒级重建(用保留的投影)
m.dispose();              // 不再使用:两侧全释放
m.isDisposed;             // true
// m.match('x');          // ✗ dispose 后抛 StateError
```

### 成员

| 成员 | 签名 | 说明 |
|---|---|---|
| 构造 | `FuzzyMatcher<T>(List<T> items, String Function(T) stringOf, {bool indexed = true, FuzzyConfig config = kDefaultFuzzyConfig})` | `stringOf` 投影出可搜索串 |
| 构造 | `static FuzzyMatcher<Map<String,dynamic>> FuzzyMatcher.key(List<Map<String,dynamic>> items, String key, {bool indexed = true, FuzzyConfig config = kDefaultFuzzyConfig})` | 按字段名搜索 |
| `buildIndices` | `void buildIndices()` | 建立索引(已存在则跳过) |
| `buildIndicesAsync` | `Future<void> buildIndicesAsync()` | 建索引异步版:Utf32 转换在后台线程,**不阻塞 UI**,适合大数据集(注:`stringOf` 投影仍在调用线程) |
| `add` / `addAll` | `void add(T item)` / `void addAll(Iterable<T> items)` | 增:追加;已建索引则直接追加(不重建,O(追加量)) |
| `update` | `void update(int index, T item)` | 改:替换下标处对象(重投影该条) |
| `removeAt` / `removeWhere` | `void removeAt(int index)` / `int removeWhere(bool Function(T) test)` | 删:按下标 / 按条件;`removeWhere` 返回删除数 |
| `clear` | `void clear()` | 清空全部(实例保留,可继续 add/refresh) |
| `refresh` | `void refresh(List<T> source)` | 整体换源并自动重建 |
| `match` | `List<FuzzyOutput<T>> match(String query, {int? limit, bool? ignoreCase, MatchMode? mode})` | 同步搜索;需先建索引。`ignoreCase`/`mode` 可按本次查询覆盖配置 |
| `matchAsync` | `Future<List<FuzzyOutput<T>>> matchAsync(String query, {int? limit, bool? ignoreCase, MatchMode? mode})` | 异步搜索,不阻塞 UI;搜索期间若 `refresh`/`dispose`,本次结果丢弃返回空 |
| `single` | `FuzzyOutput<T>? single(String query, {bool? ignoreCase, MatchMode? mode})` | 最佳一条(同 `match` 元素类型),无命中返回 null;对象用 `?.obj` |
| `singleAsync` | `Future<FuzzyOutput<T>?> singleAsync(String query, {bool? ignoreCase, MatchMode? mode})` | `single` 异步版 |
| `freeIndices` | `void freeIndices()` | 只释放 Rust 索引(Dart 侧源/投影保留,可秒级重建) |
| `dispose` | `void dispose()` | 两侧全释放并销毁 |
| `disposeAndWait` | `Future<void> disposeAndWait()` | 同 `dispose`,等在飞搜索排空后完成 |
| `length` / `hasIndices` / `isDisposed` | `int` / `bool` / `bool` | 状态 |

---

## FuzzyStringMatcher

面向 `List<String>`,`match` 返回带原列表下标的 `FuzzyHit`(想要下标而非对象时用)。

```dart
await ffuzzy.ensureInitialized();
final m = FuzzyStringMatcher(['alpha', 'beta', 'alphabet'])..buildIndices();

final List<FuzzyHit> hits = m.match('alph', limit: 10);
for (final h in hits) {
  print(m.items[h.index]); // h.index 指回原列表
  print(h.score);
  print(h.indices);
}

final FuzzyHit? best = m.single('bet');       // -> FuzzyHit?; 文本用 m.items[best!.index]
final more = await m.matchAsync('alph');      // 异步

m.refresh(['gold', 'golden']);                // 换源并重建
m.freeIndices();                              // 释放索引(可 buildIndices 重建)
m.dispose();                                  // 销毁
await m.disposeAndWait();                     // 等在飞搜索排空后完成

// 省内存模式:无需 buildIndices,每次传整表
final plain = FuzzyStringMatcher(['a', 'b'], indexed: false);
plain.match('a');
```

### 成员

与 `FuzzyMatcher` 同名方法语义一致(含 `buildIndices`/`freeIndices`/`dispose`/`disposeAndWait`、
增删改清 `add`/`addAll`/`update`/`removeAt`/`removeWhere`/`clear`、`refresh`、`single`/`singleAsync`),
差异仅在类型:`match`→`List<FuzzyHit>`、`single`→`FuzzyHit?`、`refresh`/`add` 等参数为 `String`。
额外只读属性:`List<String> items`、`bool indexed`、`FuzzyConfig config`。

---

## 独立函数

不建索引、只查一两次时用。**调用前需 `await ffuzzy.ensureInitialized()`**。

```dart
await ffuzzy.ensureInitialized();
const cfg = kDefaultFuzzyConfig;

int? score = fuzzyMatch(query: 'dt', haystack: 'Dragon Treasure', config: cfg); // 不匹配为 null

FuzzyMatch? m = fuzzyMatchIndices(query: 'dt', haystack: 'Dragon Treasure', config: cfg);
print(m?.score); print(m?.indices);

final hits = fuzzyFilter(query: 'drg', items: ['Dragon', 'Golden'], config: cfg, limit: 50);
final hitsAsync = await fuzzyFilterAsync(query: 'drg', items: ['Dragon'], config: cfg);
```

| 函数 | 签名 |
|---|---|
| `fuzzyMatch` | `int? fuzzyMatch({required String query, required String haystack, required FuzzyConfig config})` |
| `fuzzyMatchIndices` | `FuzzyMatch? fuzzyMatchIndices({required String query, required String haystack, required FuzzyConfig config})` |
| `fuzzyFilter` | `List<FuzzyHit> fuzzyFilter({required String query, required List<String> items, required FuzzyConfig config, int? limit})` |
| `fuzzyFilterAsync` | `Future<List<FuzzyHit>> fuzzyFilterAsync({required String query, required List<String> items, required FuzzyConfig config, int? limit})` |

---

## 数据类型

```dart
// FuzzyMatcher.match / single 的结果
class FuzzyOutput<T> {
  final T obj;              // 命中的原始对象
  final int score;          // 匹配分
  final Uint32List indices; // 命中字符下标(高亮用)
}

// FuzzyStringMatcher.match / fuzzyFilter 的结果
class FuzzyHit {
  final int index;          // 指回原列表下标
  final int score;
  final Uint32List indices;
}

// fuzzyMatchIndices 的结果(单串)
class FuzzyMatch {
  final int score;
  final Uint32List indices;
}

// 匹配配置(6 个字段;通常不直接写全,用 kDefaultFuzzyConfig.copyWith 只改要改的)
const kDefaultFuzzyConfig = FuzzyConfig(
  ignoreCase: true,      // 忽略大小写(也可 match(q, ignoreCase: ...) 按查询覆盖)
  normalize: true,       // Unicode 归一化(仅 Fuzzy 生效)
  preferPrefix: true,    // 前缀优先(仅 Fuzzy 排序)
  mode: MatchMode.fuzzy, // 匹配模式(也可 match(q, mode: ...) 按查询覆盖)
  parallel: true,        // 大数据 Fuzzy 搜索自动多核(候选 > 2 万触发;简单模式/小数据/web 单线程)
  incremental: false,    // 增量缓存(仅 Fuzzy + FuzzyCorpus;逐字输入可开)
);

// 推荐:在默认配置上只改个别字段(扩展方法 copyWith)
final c = kDefaultFuzzyConfig.copyWith(mode: MatchMode.substring);
```

### 匹配模式 MatchMode

| 模式 | 含义 | 典型用途 |
|---|---|---|
| `fuzzy`(默认) | 子序列模糊 + 打分排序(容错、空格分词可乱序) | 命令面板、容错搜索 |
| `substring` | 子串包含(`contains`) | "包含"过滤 |
| `prefix` | 前缀(`startsWith`) | 自动补全、前缀筛选 |
| `word` | **整串完全相等**(equals,**不是**词边界匹配) | 精确匹配/查表 |

> 各模式实测提升见文末 [性能](#性能) 表。

- **`mode` / `ignoreCase` 可按查询覆盖**:`m.match(q, mode: MatchMode.prefix, ignoreCase: false)`。同一个 matcher 不同查询切模式无需重建。
- 简单模式(substring/prefix/word)按**原序**返回、不排序、命中满 `limit` 即停。
- `parallel` / `incremental` 是 matcher 级策略(构造时经 `config` 定),不在 `match` 参数里:`incremental` 依赖"连续查询是上次前缀扩展"的状态,逐字输入搜索框时开启可加速(仅 `fuzzy` + `FuzzyCorpus` 生效)。
- **大数据集首次建索引用 `await m.buildIndicesAsync()`** 避免卡 UI。
- 大小写不敏感(`ignoreCase: true`)的**简单模式**首次查询会惰性构建一份小写副本并缓存,之后复用(首查略慢、后续快;见末尾性能表)。

---

## Flutter 完整示例(搜索框 + 高亮)

可直接复制运行:输入实时模糊筛选并高亮命中字符。

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

  @override
  void dispose() {
    _matcher.dispose();
    super.dispose();
  }

  int _token = 0; // 防竞态:只接受最新一次查询的结果

  Future<void> _onChanged(String q) async {
    final token = ++_token;
    if (q.isEmpty) {
      setState(() => _hits = [
            for (int i = 0; i < _items.length; i++)
              FuzzyHit(index: i, score: 0, indices: Uint32List(0)),
          ]);
      return;
    }
    // 异步搜索:在后台线程跑,不阻塞 UI。大数据/连打字务必用 matchAsync。
    final hits = await _matcher.matchAsync(q, limit: 50);
    if (!mounted || token != _token) return; // 过期结果丢弃,避免列表闪回旧值
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
              decoration: const InputDecoration(border: OutlineInputBorder(), hintText: '输入模糊查询')),
        ),
        Expanded(
          child: ListView.builder(
            itemCount: _hits.length,
            itemBuilder: (_, i) {
              final hit = _hits[i];
              final text = _items[hit.index];
              final matched = hit.indices.toSet();
              // indices 是「字符(rune)下标」,高亮务必按 runes 切分,
              // 直接用 text[c]/UTF-16 索引会让 emoji 等非 BMP 字符错位。
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

> 上例用 `matchAsync` + `_token` 版本号,这是搜索框的推荐写法:不阻塞 UI、且丢弃过期结果避免闪烁。
> 小数据(几千条)用同步 `match` 也无妨。**高亮按 `runes` 切分**(`indices` 是字符下标,非 UTF-16 码元)。

---

## 性能

### vs 其它 Dart 模糊库(488,600 条,Windows x86_64 release)

| 库 | 每查询耗时 | 相对 ffuzzy |
|---|---:|---:|
| **ffuzzy(缓存)** | **~3 ms**(并行) | **1×** |
| string_similarity | ~755 ms | ~250× |
| fuzzy_bolt | ~1276 ms | ~420× |
| fuzzy (Fuse) | ~1585 ms | ~520× |
| fuzzywuzzy | ~5066 ms | ~1600× |

> `nucleo` 是子序列模糊(fzf 式),与编辑距离/Dice 类库解决的问题不同;上表为「同类功能、不同实现」的吞吐对比,命中集合不完全等价。

### 各参数/开关对性能与内存的影响(488,600 条,同机实测)

一次性:`buildIndices` ~120ms;常驻 Utf32 索引 ~35MB(48.8 万条)。基准 = 默认 Fuzzy + 并行,单查 ~2.9ms。

| API / 开关 | 默认 | 作用 | 实测提升 | 内存 / 代价 |
|---|---|---|---:|---|
| `parallel`(多核) | on | 大数据 Fuzzy 搜索分块多核(候选 >2万触发) | **6.3×**(18.1→2.9ms) | 仅搜索瞬时占多核;线程数 = cores−2(留 UI 余量),≤3 核自动串行 |
| `mode: prefix` | — | 前缀(对比并行 fuzzy) | **3.5×**(→0.85ms) | 无额外 |
| `mode: word` | — | 整串相等(对比并行 fuzzy) | **2.3×**(→1.30ms) | 无额外 |
| `mode: substring` | — | 子串(对比并行 fuzzy;对比**串行** fuzzy ~6×) | ~1.0×(→2.9ms) | 无额外 |
| `incremental`(**串行**/Web/低核) | off | 逐字输入只重扫上次命中集 | **2.2×**(逐字 7 次 117→53ms) | 缓存上次命中下标(≤2万 个 u32) |
| `incremental`(多核) | off | 并行已快,增量基本中性 | ~1.0×(不再变慢) | 同上 |
| `ignoreCase` 简单模式(惰性折叠缓存) | 自动 | 首查建一份小写副本、之后复用 | 首查后 **13×**(39→3ms) | 一份小写副本(≈原串大小,首查时惰性建,增删改/free 失效) |
| `buildIndicesAsync()` | — | 建索引移后台线程 | 不提速但**不卡 UI** | 与同步建索引等耗时 |

要点:
- **多核并行是 Fuzzy 大数据的最大杠杆(6×+)**;桌面/多核 Android 默认即享。
- **简单模式(prefix/word/substring)** 串行执行;大数据下相对"并行 fuzzy"优势被拉近(substring ~1×),但语义不同(前缀/相等/子串是 fuzzy 给不了的),且**小数据或串行(Web)环境优势更大**。
- **`incremental` 专为 Web/WASM/低端单核**(无多核时)的逐字输入搜索:那里并行帮不上忙,它给 ~2×;多核机上中性、不会变慢(opt-in 默认关)。
- **`ignoreCase` 的简单模式**首次查询惰性建小写副本(一次性 ~40ms@48.8万),之后稳态与原样一致;内存换"大小写不敏感不每查重折"。

> 内存吃紧:`freeIndices()` 空闲释放 Rust 索引,`buildIndices()`/`refresh()` 秒级恢复(Dart 侧源保留)。

---

## 开发

```bash
# 在插件根目录
flutter test                       # 运行 Dart 测试
cargo test --manifest-path rust/Cargo.toml   # 运行 Rust 单测

# 运行示例 App
cd example && flutter run -d <device>

# 修改 Rust 后重新生成绑定
flutter_rust_bridge_codegen generate
```

首次构建会自动通过 cargokit 交叉编译 Rust;需安装 Rust 工具链与对应平台的 target。
