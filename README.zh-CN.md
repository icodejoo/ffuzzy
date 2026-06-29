# ffuzzy

[English](README.md) | 中文

为 Flutter 提供的高性能模糊搜索,由紧凑的 **C** 引擎经 `dart:ffi` 驱动。

`ffuzzy` 是 [`nucleo`](https://github.com/helix-editor/nucleo)(Helix 编辑器背后的
matcher)的纯 C 逐字节复刻:无需 Rust 工具链、无代码生成,引擎就是几个 C 源文件,
由各平台 SDK 自行编译。原生库 **strip 后约 32 KB**。

- **快** —— 媲美或超过 Rust 的 `nucleo`:所有多线程档全面更快、`substring` 全档更快,
  CJK 与单线程 `fuzzy` 持平。10 万条语料一次过滤约 1.4 ms。
- **小** —— 原生 `.so`(arm64)约 32 KB,纯 C,零第三方依赖。
- **全平台** —— Android / iOS / macOS / Linux / Windows。源码随各平台构建打包,使用者
  无需额外工具链。*(不支持 web —— web 上没有 `dart:ffi`。)*
- **可搜任意对象** —— `FuzzyCorpus<T>` 搜索 `List<T>`,命中携带原对象(`hit.raw`)。
- **匹配模式即方法** —— `fuzzy`(fzf 风格,支持 `! ^ ' $` 操作符)、`substring`、`prefix`、
  `postfix`、`exact`,各带 `…Async` 异步孪生。
- **多线程**与**异步**扫描,大语料也不卡 UI。
- **命中高亮**,带正确的 Unicode(码点 → UTF-16)下标转换。
- **Unicode / CJK** —— 变音符 + 完整简单大小写折叠;CJK 直接逐码点匹配。
- **多键搜索** —— 挂载宿主算好的拼音 / 罗马音 / 首字母,让 CJK 项也能用拉丁键入找到。

## 安装

```yaml
dependencies:
  ffuzzy: ^0.3.1
```

> **无需额外平台配置** — C 源码由各平台 SDK 在 `flutter build` 时自动编译打包，
> 使用者不需要配置 NDK、Xcode 标志等任何工具链。

## 快速上手

```dart
import 'package:ffuzzy/ffuzzy.dart';

// 纯字符串:
final corpus = FuzzyCorpus.strings(['src/main.dart', 'lib/widget.dart', '中文搜索']);
for (final h in corpus.fuzzy('srcmn', parallel: true, limit: 50)) {
  print('${h.raw}  score=${h.score}');   // h.raw 就是命中的字符串
}
corpus.dispose();                          // 或交给 NativeFinalizer 回收

// 任意对象 —— 给一个 stringOf 提取器;命中携带原对象:
final files = FuzzyCorpus<File>(myFiles, stringOf: (f) => f.path);
final hit = files.prefix('lib/').firstOrNull;   // hit.raw 是 File
```

> `FuzzyCorpus` 持有原生内存,且只能在创建它的 isolate 上使用。模式方法在调用 isolate
> 上同步执行 —— 大语料请用 `…Async` 孪生(后台 isolate),或把 corpus 放到后台 isolate,
> 避免卡 UI。

## 适用场景

文件路径、命令面板、联系人/歌曲列表、日志行,或任何内存中的列表 —— 凡是想要 fzf 级别
排序又要原生速度的地方,尤其是大列表(数万条)和 CJK 内容。

---

# API

全部从 `package:ffuzzy/ffuzzy.dart` 导出。

## `FuzzyCorpus<T>`

一份常驻的 `T` 语料,建一次、反复搜。

### 构造

```dart
FuzzyCorpus<T>(
  Iterable<T> items, {
  required String Function(T) stringOf, // 每个 item 的可搜索文本
  FuzzyOptions options = const FuzzyOptions(), // 默认搜索选项
  bool matchPaths = false,   // 针对路径文本调整分隔符
  bool preferPrefix = false, // 让靠前的命中加分
  String? libraryPath,       // 指定原生库文件(测试/非打包用)
})

// 纯字符串便捷构造(item 即其搜索文本):
static FuzzyCorpus<String> FuzzyCorpus.strings(Iterable<String> items, {…})

// List<Map> 按单个字段搜索；hit.raw 是整张 map:
static FuzzyCorpus<Map<String, dynamic>> FuzzyCorpus.byKey(
    Iterable<Map<String, dynamic>> items, String field, {…})

// List<Map> 跨多个字段搜索；hit.matchedKey 是命中的字段下标:
static FuzzyCorpus<Map<String, dynamic>> FuzzyCorpus.byKeys(
    Iterable<Map<String, dynamic>> items, List<String> fields, {…})

// 在后台 isolate 上插入、构建(大)语料,不卡 UI:
static Future<FuzzyCorpus<T>> FuzzyCorpus.buildAsync<T>(
    Iterable<T> items, {required String Function(T) stringOf, …})
```

加载原生库失败时抛 [`FuzzyException`](#fuzzyexception)。

> `strings`/`byKey`/`byKeys`/`buildAsync` 是 **static 方法**(而非 `factory` 构造),因为它们要
> 把元素类型定死(`FuzzyCorpus<String>` / `<Map>`),而泛型类上的 factory 构造做不到这点。
> 调用写法与性能都和构造函数完全一致 —— 它们只是转发给 `FuzzyCorpus(...)`。

### 增 / 删 / 改

| 成员 | 说明 |
|---|---|
| `void add(T item)` | 追加一条。 |
| `void addAll(Iterable<T> items)` | 追加多条(插入顺序即各命中的 `index`)。 |
| `Future<void> addAllAsync(Iterable<T> items)` | 在**后台 isolate** 上插入(不卡 UI)。运行期间独占。 |
| `void addKey(T item, List<FuzzyKey> keys)` | 追加 `item` 并带[备用搜索键](#多键--cjk-转写)。原文本(`stringOf(item)`)自动加入。 |
| `void update(int index, T item)` | 替换 `index` 处的项(其备用键被丢弃)。 |
| `void removeAt(int index)` | 删除 `index` 处的项。 |
| `int removeWhere(bool Function(T) test)` | 删除所有匹配项;返回删除条数。 |
| `void refresh([Iterable<T>? source])` | 无参:重新加入当前项(当其 `stringOf` 文本变了)。传 `source`:整体替换数据集。 |
| `void clear()` | 清空**全部**项与原生数据;corpus 对象仍可用(重新 `add`/`addAll` 即可)。 |
| `int get length` | 当前项数。 |

> **没有"单独的索引"要构建** —— 原生 corpus 本身就是数据,`add`/`addAll`/`addAllAsync`
> 在插入时逐条建好。`clear()` 全部清空;"重建"就是重新 add(或 `refresh`)。由于原生
> corpus 是 append-only,`update` / `removeAt` / `removeWhere` / `refresh` 会 O(n) 重建
> —— 偶尔编辑很便宜;频繁变更请批量处理。

### 搜索模式

每个匹配模式都是一个返回 `List<FuzzyHit<T>>` 的方法,并各带一个返回
`Future<List<FuzzyHit<T>>>`、在后台 isolate 运行的 `…Async` 孪生:

```dart
List<FuzzyHit<T>> fuzzy(String query, {…覆盖项});      Future<…> fuzzyAsync(…);
List<FuzzyHit<T>> substring(String query, {…覆盖项});  Future<…> substringAsync(…);
List<FuzzyHit<T>> prefix(String query, {…覆盖项});     Future<…> prefixAsync(…);
List<FuzzyHit<T>> postfix(String query, {…覆盖项});    Future<…> postfixAsync(…);
List<FuzzyHit<T>> exact(String query, {…覆盖项});      Future<…> exactAsync(…);
```

- **`fuzzy`** 会把查询解析成空格分隔的多个词项 + fzf 操作符(`!` 取反、`^` 前缀、`'` 子串、
  `$` 后缀)—— 所以 `'lib parse'` 是两个词项的 AND。其他模式把整个查询当作一个字面原子。
- **覆盖项**(`{FuzzyCase? caseMatching, FuzzyNorm? normalization, bool? parallel,
  int? threads, int? limit, bool? highlight, FuzzyScoring? scoring}`):每个非 null
  实参仅对**该次调用**覆盖 corpus 的 [`FuzzyOptions`](#fuzzyoptions) 对应字段，如
  `corpus.fuzzy(q, limit: 50)` 或 `corpus.fuzzy(q, highlight: true)`。
- **原始对象快捷方式** —— 只需要命中 item、不需要 score/indices 等元数据时，`*Raws`
  系列方法跳过 `FuzzyHit` 包装、速度更快：`fuzzyRaws`、`substringRaws`、`prefixRaws`、
  `postfixRaws`、`suffixRaws`、`exactRaws`（各带 `…Async` 孪生）。`corpus.one`
  也新增 `fuzzyRaw`、`prefixRaw`… 系列，返回 `T?`。
- **取最佳单条**:`corpus.one` 是一个视图,暴露同样的 5 个模式,但各返回 `FuzzyHit<T>?`
  (top-1 或 null)而非列表 —— `corpus.one.fuzzy(q)`、`corpus.one.prefix(q)`、…(+ `…Async`)。
  它跑的是与 `fuzzy(q, limit: 1)` **完全相同**的原生扫描,无额外开销。

`…Async` 调用可安全并发(各自独立的原生 matcher)。其中任一在飞期间,任何变更
(`add`/`update`/`removeAt`/`clear`/…)或 `dispose` 都会抛 [`StateError`](#错误)(否则是原生
use-after-free)。

### 生命周期

| 成员 | 说明 |
|---|---|
| `void dispose()` | 任何时候调用均安全；若异步任务正在执行，等待其完成后再释放原生内存。幂等。 |
| `Future<void> disposeAndWait()` | 类似 `dispose`,但先 await 在飞的异步搜索/构建,因此不会抛异常。 |

若你忘了 `dispose`,`NativeFinalizer` 会在 GC 时自动释放;但仍推荐显式
`dispose`/`disposeAndWait` 以便及时回收。

**在 Flutter `StatefulWidget` 中：**

```dart
@override
void dispose() {
  // unawaited 是安全的：NativeFinalizer 作为兜底，corpus 会在
  // 所有进行中的异步搜索完成后自动释放。
  unawaited(_corpus.disposeAndWait());
  super.dispose();
}
```

## `FuzzyOptions`

打包每次搜索的设置。在构造函数上设 corpus 级默认;模式方法用命名参数逐字段覆盖。可选 ——
每个字段都有默认值,所以 `const FuzzyOptions()` 是常见起点。

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `caseMatching` | `FuzzyCase` | `smart` | 大小写处理 |
| `normalization` | `FuzzyNorm` | `smart` | 变音符归一 |
| `parallel` | `bool` | `false` | 多线程打分 |
| `threads` | `int` | `0` | `0`=自动(CPU 一半,上限 8;硬上限 cpu-1;<512 项恒串行) |
| `limit` | `int` | `0` | 最多返回数(`0`=全部) |
| `highlight` | `bool` | `false` | `true` 触发 Pass 2，填充 `FuzzyHit.indices`（用于高亮）；`false`（默认）跳过以提速。 |
| `scoring` | `FuzzyScoring` | `FuzzyScoring.fast` | 打分算法：`fast`（滚动 DP，默认）、`off`（不排名，按插入顺序）、`nucleo`（全矩阵 DP，精度最高，CPU 约 2×）。 |

`FuzzyOptions` 还有 `copyWith(...)`。示例:

```dart
final corpus = FuzzyCorpus.strings(items,
    options: const FuzzyOptions(parallel: true, limit: 50));
corpus.fuzzy('foo');               // 用 parallel + limit 50
corpus.fuzzy('bar', limit: 10);    // 同样默认,但本次 limit 覆盖为 10
```

## `FuzzyHit<T>`

一条搜索结果。

| 字段 | 类型 | 说明 |
|---|---|---|
| `raw` | `T` | 命中的原始 item。 |
| `index` | `int` | 该 item 在语料中的插入序号。 |
| `score` | `int` | 匹配分(越高越好)。 |
| `matchedKind` | `FuzzyKeyKind` | 命中的键种类(original / pinyin / …)。 |
| `matchedKindCode` | `int` | 命中键的原始整数 kind 值（如 `100`、`101`）。内置 kind 与 `matchedKind.code` 相同；对通过 `addKey`/`byKeys` 添加的宿主自定义键，此字段保留原始数值，可区分 `matchedKind` 均显示 `custom` 的多种自定义键类型。 |
| `matchedKey` | `int` | 命中的是该 item 的哪个键(`0`==原键)。 |
| `indices` | `List<int>` | 命中键内的**码点**下标。**仅 `highlight: true` 时有值**，否则为空。用于 Dart `String` 前先经 [`fuzzyCodepointToUtf16`](#高亮) 转换。 |

## 枚举

### `FuzzyCase` —— 大小写处理

| 值 | 含义 |
|---|---|
| `respect` | 大小写敏感;`A` ≠ `a`。 |
| `ignore` | 大小写不敏感;`A` == `a`。 |
| `smart` | 默认不敏感,但**查询里含大写字母时**转为敏感(默认)。 |

### `FuzzyNorm` —— Unicode 归一(变音符)

| 值 | 含义 |
|---|---|
| `never` | 不折叠;`café` ≠ `cafe`。 |
| `smart` | 折叠变音符,除非查询本身带变音符;`cafe` 命中 `café`(默认)。 |

### `FuzzyKeyKind` —— 命中来自哪种键

| 值 | `.code` | 含义 |
|---|---|---|
| `original` | `0` | item 自身文本(`stringOf`)。 |
| `pinyin` | `1` | 拼音备用键。 |
| `initials` | `2` | 首字母备用键。 |
| `romaji` | `3` | 罗马音备用键。 |
| `custom` | `100` | 任意宿主自定义种类(`>= 100`)。 |

`FuzzyKeyKindCode` 扩展提供 `int get code`(构造 [`FuzzyKey`](#fuzzykey) 时用);
`FuzzyKey.kind(...)` 会替你设好。

## `FuzzyKey`

通过 [`FuzzyCorpus.addKey`](#增--删--改) 挂到某个 item 上的备用搜索键。

| 成员 | 说明 |
|---|---|
| `final String text` | 备用键的可搜索文本。 |
| `final int kind` | 该键的 [`FuzzyKeyKind`](#fuzzykeykind--命中来自哪种键) code(或任意宿主值 `>= 100`)。 |
| `const FuzzyKey(String text, {int kind = 1})` | `kind` 默认 `1`(拼音)。 |
| `FuzzyKey.kind(String text, FuzzyKeyKind kind)` | 用枚举设 `kind`(推荐)。 |

用法见[多键 / CJK 转写](#多键--cjk-转写)。

## 高亮

```dart
List<int> fuzzyCodepointToUtf16(String text, List<int> codepointIndices)
```

搜索时传 `highlight: true` 才会填充 `FuzzyHit.indices`（默认 `false` 以节省 Pass 2
开销）。`indices` 是码点位置；Dart 字符串是 UTF-16，构建 `TextSpan` 前先转换，
以免 emoji / 星平面字符错位：

```dart
final hit = corpus.fuzzy('src', highlight: true).first;
final text = hit.raw as String;
final marks = fuzzyCodepointToUtf16(text, hit.indices).toSet();
final spans = [
  for (var i = 0; i < text.length; i++)
    TextSpan(text: text[i], style: marks.contains(i) ? boldStyle : null),
];
```

## 多键 / CJK 转写

matcher 不内置拼音/罗马音词典 —— 你在宿主侧算好备用键并挂上(见 [`FuzzyKey`](#fuzzykey)),
让 CJK 项也能用拉丁键入找到。

```dart
corpus.addKey(zhangsan, [
  FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zs', FuzzyKeyKind.initials),
]);

final h = corpus.fuzzy('zs').first;
// h.matchedKind == FuzzyKeyKind.initials, h.matchedKey == 2
```

#### 大列表 + 拼音批量建索引

对于万级联系人，请在后台 Isolate 中构建语料库以避免 UI 卡顿：

```dart
final corpus = await Isolate.run(() async {
  final c = FuzzyCorpus<Contact>(
    contacts,
    stringOf: (c) => c.name,
    options: const FuzzyOptions(scoring: FuzzyScoring.fast),
  );
  for (int i = 0; i < contacts.length; i++) {
    c.addKey(contacts[i], [
      FuzzyKey(contacts[i].pinyin, kind: FuzzyKeyKind.pinyin),
      FuzzyKey(contacts[i].initials, kind: FuzzyKeyKind.initials),
    ]);
  }
  return c;
});
```

> **注意**：`FuzzyCorpus` 不能跨 Isolate 传递——在 Isolate 内部完整构建后直接使用，
> 或将数据传回主 Isolate 重建。

## 错误

- **可恢复**错误可被捕获:库/符号加载失败、内存不足会以 `FuzzyException` 抛出;误用
  (dispose 后使用、异步在飞时变更)抛 `StateError`。引擎被加固为"降级而非崩溃"
  (分配失败即丢弃、scratch 有界、无递归、非法 UTF-8 → U+FFFD)。
- **硬性原生崩溃**(段错误/abort)无法变成 Dart 异常 —— 见 [`FuzzyCrash`](#fuzzycrash)。

### `FuzzyException`

```dart
class FuzzyException implements Exception { final String message; }
```

## `FuzzyCrash`

可选、需显式开启的最后兜底处理器,用于**不可恢复**的原生崩溃。它在进程死亡前向 stderr
(Android 为 logcat)打印栈回溯,并在你给了 `breadcrumbPath` 时把同样的报告写入文件,
以便下次启动展示"上次崩溃"。在启动时调用一次。

```dart
final report = FuzzyCrash.lastReport();        // 上一次运行的崩溃(若有)
if (report != null) log('ffuzzy 上次崩溃:\n$report');
FuzzyCrash.install(breadcrumbPath: '${dir.path}/ffuzzy_crash.log');
```

| 成员 | 签名 | 说明 |
|---|---|---|
| `install` | `static bool install({String? breadcrumbPath, String? libraryPath})` | 注册处理器。库缺该符号(如 strip 的 release 省略了它)时返回 `false`。 |
| `lastReport` | `static String? lastReport({String? breadcrumbPath})` | 读取并清除上次运行留下的崩溃报告,无则 `null`。 |

栈回溯的可读性随构建自动变化:debug/profile 保留符号(Windows 显示 `file:line`);
strip 的 release 打印偏移,需用随包的 `.debug` / `.pdb` / `.dSYM` 离线符号化。详见
[`doc/INTERNALS.md`](doc/INTERNALS.md) 的 debug/release 分档。

---

## 高频 & 大语料搜索

**构建大语料** —— `add`/`addAll` 在调用 isolate 上跑,插入数十万条会卡 UI。用
[`FuzzyCorpus.buildAsync`](#构造)(或 `addAllAsync`)把原生插入放到后台 isolate。构建是
*独占*的:其间对该 corpus 搜索或变更会抛 [`StateError`](#错误)。

**数据竞争** —— 原生 corpus 允许并发**读**、但需独占**写**,绑定层强制了这点,你无法触发竞争:

- 同步 `fuzzy`/`substring`/… 完全在调用 isolate 上跑 —— 无并发、无竞争。
- `…Async` 搜索从 worker isolate 读取 corpus;多个可安全重叠(各自独立的原生 matcher
  scratch —— 读不改共享状态)。
- 搜索在飞期间的任何变更(`add`/`update`/`removeAt`/`clear`/…)、`addAllAsync` 或 `dispose`
  会抛 `StateError`;反之异步构建在写时发起搜索也会抛。先 await(或 [`disposeAndWait`](#生命周期))。

**内存 / CPU** —— 常驻语料为每个 item 的文本存一份原生副本(这就是"索引");Dart 侧还保留
你的 `List<T>` 以还原 `hit.raw`,所以大致按"文本存两份 + 你的对象"估算。搜索只分配一次性的
结果缓冲(用完即释放)—— 反复搜索**不会**增长内存。注意 `…Async` 每次会启一个短命 isolate,
所以每键入一字符就发一次会造成无谓开销 —— 见下。

**让"最新查询"的结果生效(随键即搜)** —— 库不会自动取消被取代的搜索(原生扫描总会跑完),
所以由**你**决定谁生效:

- **小/中语料(≲10 万):直接同步搜。** 10 万条同步 `fuzzy(q)` 约 1.4 ms —— 远低于一帧 ——
  且天然"最新优先"(你 `setState` 的就是最新一次键入的结果)。
- **大语料 / 重查询:用 `…Async` + generation 守卫**,丢弃旧键入乱序返回的结果,并可加
  debounce 避免每字符都 fan-out 一个 isolate:

```dart
int _gen = 0;
Future<void> onQueryChanged(String q) async {
  final gen = ++_gen;                       // 最新查询胜出
  final hits = await corpus.fuzzyAsync(q, limit: 50);
  if (gen != _gen) return;                  // 已被更新的键入取代
  setState(() => _hits = hits);
}
```

(示例 app 就用了这个模式。)

## 平台 & 原生库如何打包

`ffuzzy` 是 FFI 插件:C 源码随各平台编译打包(Android NDK / CMake,iOS 与 macOS 经 podspec
静态链接,Linux 与 Windows 用 CMake)。使用者**无需** Rust、无需额外工具链,只要标准平台 SDK。
Dart 侧通过 `ffz.dll` / `libffz.so` 加载,或在 Apple 上经 `DynamicLibrary.process()` 解析
静态链接的符号。

## 性能

真机对比(Flutter Windows,profile 模式,10 万条,C 引擎 vs Rust `nucleo` 引擎):

| | C(ffuzzy) | Rust(nucleo) |
|---|---|---|
| 常驻语料内存 | 15.25 MB | 16.54 MB |
| 过滤(fuzzy, top-50) | 1.36 ms | 1.65 ms |

完整方法学、差分测试保证(6210/6210 与 nucleo 逐字节一致)、Unicode 覆盖、体积、引擎设计,
都在 [`doc/INTERNALS.md`](doc/INTERNALS.md)。

## 许可

MIT —— 见 [LICENSE](LICENSE)。
