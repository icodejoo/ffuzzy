---
name: ffuzzy-plugin
description: Use when developing, building, testing, or publishing the ffuzzy Flutter fuzzy-search plugin. The published `ffuzzy` package is now the pure-C engine AT THE REPO ROOT (lib/ffuzzy.dart + src/ ffi/ include/ + platform dirs). The old Rust + flutter_rust_bridge engine is DEPRECATED, lives under benchmark/, and is kept only for performance comparison. Covers project layout, the public Dart API, the C build/test workflow, environment gotchas on this machine, and pub.dev publishing.
---

# ffuzzy 插件开发指南

> ⚠️ **当前发布/开发对象 = 仓库根的纯 C FFI 插件**（pub 包名 **`ffuzzy`**）。
> 入口 `lib/ffuzzy.dart`，C 源在 `src/ ffi/ include/`，平台目录
> `android/ios/macos/linux/windows/`，示例 `example/`，引擎内幕 `doc/INTERNALS.md`。
>
> **Rust + frb 引擎已废弃**，移到 `benchmark/`（包名 `ffuzzy_rust_bench`，`publish_to: none`），
> 仅用于 C-vs-Rust 性能对比；`.pubignore` 已把 `benchmark/` 排除出发布包。
> C(FFI) 在 **web 上不可用**。

## 工程结构

```
ffuzzy/                         ← 仓库根 = 发布包根
├── lib/ffuzzy.dart             # 唯一对外入口（手写 Dart 公开 API）
├── src/*.c                     # C 引擎核心（ffz_chars/corpus/fuzzy/match/…）
├── ffi/ffz_ffi.c               # dart:ffi ABI 胶合层（FFZ_API 导出）
├── include/ffz.h               # 公共 C 头
├── include/ffz_corpus.h        # corpus/filter/results 头
├── android/CMakeLists.txt      # → ../CMakeLists.txt
├── ios/ffuzzy.podspec          # 静态链接 src/*.c + ffi/ffz_ffi.c
├── macos/ffuzzy.podspec        # 同上
├── linux/CMakeLists.txt        # → ../CMakeLists.txt
├── windows/CMakeLists.txt      # → ../CMakeLists.txt
├── CMakeLists.txt              # 根：编 libffz.so / ffz.dll（debug/release 自动切换）
├── example/                    # 演示 App
├── test/                       # Flutter 单元测试（flutter test）
├── tool/ffi_smoke.dart         # 冒烟脚本（dart run tool/ffi_smoke.dart）
└── benchmark/                  # 废弃 Rust 引擎，仅对比用
```

## 公开 Dart API（`package:ffuzzy/ffuzzy.dart`）

### FuzzyCorpus\<T\>

**构造**
```dart
FuzzyCorpus<T>(items, {required String Function(T) stringOf, FuzzyOptions options, bool matchPaths, bool preferPrefix, String? libraryPath})
FuzzyCorpus.strings(items, {…})             // T = String
FuzzyCorpus.byKey(maps, field, {…})         // T = Map<String,dynamic>，按单字段搜
FuzzyCorpus.byKeys(maps, fields, {…})       // T = Map<String,dynamic>，跨多字段；hit.matchedKey = 命中字段下标
FuzzyCorpus.buildAsync(items, stringOf:, {…}) // 后台 isolate 建库，不卡 UI
// FuzzyCorpus.keyed 已废弃 → 用 byKey
```

**增删改**
```
add / addAll / addAllAsync    # addAllAsync 独占写，期间搜索/改/dispose 抛 StateError
addKey(item, List<FuzzyKey>)  # CJK 拼音/罗马音场景：同一条目挂多种转写键（原 addKeyed）
update / removeAt / removeWhere / refresh([source]) / clear
```
> `addKey` 仅用于"存汉字但用拼音输入"场景；纯中文搜中文直接用 `add/addAll`。

**搜索模式**（每个都有 `…Async` 孪生）
```
fuzzy / substring / prefix / postfix / suffix / exact
```
- 均可传命名参数覆盖 `FuzzyOptions`（`caseMatching`/`normalization`/`parallel`/`threads`/`limit`/`highlight`/`scoring`）。
- `corpus.one.fuzzy(q)` → `FuzzyHit<T>?`（top-1 或 null，零额外开销）。

**生命周期**
```
dispose()          # 幂等；in-flight 时等待完成后释放，不抛异常（可在 State.dispose() 直接调）
disposeAndWait()   # 同上但返回 Future，可 await
NativeFinalizer    # GC 兜底，但推荐显式 dispose
```

### FuzzyOptions

| 字段 | 默认 | 说明 |
|------|------|------|
| `scoring` | `FuzzyScoring.fast` | 打分算法：`fast`（滚动DP）/ `off`（不排名，插入顺序）/ `nucleo`（全矩阵DP，精度最高约2×CPU） |
| `caseMatching` | `smart` | `respect`/`ignore`/`smart` |
| `normalization` | `smart` | `never`/`smart` |
| `parallel` | `false` | 多线程打分 |
| `threads` | `0` | 0=自动（半核，上限8；硬上限cpu-1；<512项恒串行） |
| `limit` | `0` | 最多返回数（0=全部） |
| `highlight` | `true` | false 跳过 match indices（更快） |

### FuzzyHit\<T\>

| 字段 | 说明 |
|------|------|
| `obj` | 命中的原始对象 |
| `index` | 插入顺序下标 |
| `score` | 匹配分（同一次查询内可比较） |
| `matchedKind` | `FuzzyKeyKind` 枚举（original/pinyin/initials/romaji/custom） |
| `matchedKindCode` | 原始整数 kind 值；内置 kind 与 `matchedKind.code` 相同；`addKey` 自定义 kind（100,101…）在此保留原值，`matchedKind` 均为 `custom` |
| `matchedKey` | 该条目内哪个键命中（0=original；`byKeys` 下等于 `fields` 下标） |
| `indices` | 命中字符的**码点**下标（传给 `fuzzyCodepointToUtf16` 后用于高亮） |

### FuzzyKey / FuzzyKeyKind

```dart
FuzzyKey(text, {int kind = 1})        // kind 默认 1=pinyin
FuzzyKey.kind(text, FuzzyKeyKind.xxx) // 推荐
// FuzzyKeyKind: original=0, pinyin=1, initials=2, romaji=3, custom=100
// 自定义 kind >= 100，可用 101,102,… 区分多种
```

### FuzzyScoring

```dart
FuzzyScoring.fast    // 滚动DP，默认，适合名称/路径/符号
FuzzyScoring.off     // 不打分，按插入顺序返回（ID匹配/唯一匹配场景）
FuzzyScoring.nucleo  // 全矩阵DP，精度最高，约 2× CPU 开销
```

## 并发模型

- 原生 corpus 允许**并发读**（每次 filter 各自 malloc matcher scratch，线程安全），**写需独占**。
- 同步搜索在调用 isolate 无竞争；`…Async` 搜索多个可安全重叠（各自私有 matcher）。
- `addAllAsync` 是独占写：期间任何搜索/改/dispose 抛 `StateError`。
- 搜索 in-flight 时 mutate/dispose 抛 `StateError`。
- **高频键入"最新优先"**：同步搜天然最新；异步用 generation 守卫丢弃过期结果（example 已示范）。

## 编译速查

**只改了 Dart（lib/ 或 test/）**——直接：
```bash
dart analyze lib/ test/ tool/ example/lib/
flutter test test/
```

**改了 C 源（src/ 或 ffi/）**——需重编原生库：
```bash
# Windows: 用 CMakeLists.txt 直接构建，或跑 flutter build windows 触发 CMake
# C 测试（tests/test_ffz.c）：
build_test.bat        # 或见 INTERNALS.md 的手动 gcc/clang 命令
```

**真机验证**：
```bash
cd example && flutter run -d <device>
# 或 flutter build apk --release --target-platform android-arm64
```

## 构建配置关键点

- **Android**：`android/build.gradle` 已钉 `ndkVersion '26.1.10909125'`，`abiFilters 'arm64-v8a', 'armeabi-v7a', 'x86_64'`。
- **iOS/macOS podspec**：`source_files` 只含 `'../src/*.c', '../ffi/ffz_ffi.c', '../include/*.h'`（不含 `ffz_crash.c`，crash handler 由 CMake 条件编译）。
- **CMakeLists.txt**：`file(GLOB FFZ_CORE CONFIGURE_DEPENDS …)`（新增文件自动感知）；debug/profile 保留符号，release hidden visibility + strip。

## 本机环境踩坑

- **NDK**：插件 `android/build.gradle` 已钉 `ndkVersion '26.1.10909125'`；example 的 `android/build.gradle` 钉 `28.2.13676358`（可不同）。
- **Android 构建（仅本机网络）**：JDK 信任库不认 TLS 拦截代理 → `example/android/gradle.properties` 加 `systemProp.javax.net.ssl.trustStoreType=Windows-ROOT`；Maven 加阿里云镜像。这些是**本机配置，勿提交**。
- **JDK**：`flutter config --jdk-dir "C:\sdk\jdk\openjdk-21.0.5+11"`。
- **`reachabilityFence`**：`dart:ffi` 的 `reachabilityFence` 在本机 SDK 路径下解析失败（Dart 3.12.2 + Windows），改用 `_keepAlive()`（读实例字段，等效保活语义）。

## CI / 推送踩坑

- **本机网络 RST 大块推送**：`git push` 整包（几百 KB 一次）断在 `send-pack: unexpected disconnect`，但放行小推送。解法：拆成多个小提交逐个推；**用远程 sha 比对确认真·成功**（`git ls-remote origin -h refs/heads/main`）——退出码曾误报成功。
- **CI 日志**：本机没装 `gh`，GitHub Actions 日志 403，WebFetch 抓不到正文。让用户贴失败步骤末 30~50 行。

## 发布到 pub.dev

```bash
flutter pub publish --dry-run   # 0 warnings 才发
flutter pub publish
```

- pubspec `sdk: ">=3.3.0 <4.0.0"`（`extension type` 需 Dart ≥3.3.0）。
- 纯 Dart 改动不影响任何预编译哈希；C 改动同理（C 版无预编译机制，消费方直接 SDK 编译）。

---

## wasm/（Web 包 @codejoo/ffuzzy，已发布到 npm）

同一份 C 引擎编成 WASM，作为独立 **npm 包** 发布（目录 `wasm/`，已全部纳入 git）。与 Flutter 包独立。

**目录（扁平）**：
- `ffuzzy-corpus.mjs` — 手写 wrapper（高层 API，唯一源），追加进每个 engine。
- `ffuzzy.engine.mjs` / `ffuzzy-lite.engine.mjs` — emcc 产物（SINGLE_FILE，wasm 以 `binaryDecode` 二进制串内联，**非 base64**），构建输入。
- `ffuzzy.js` / `ffuzzy-lite.js` — 发布产物 = engine + wrapper。
- `ffuzzy.d.ts` / `ffuzzy-lite.d.ts` — 手写类型。
- `lite-tables.c` — 空表 stub（lite 用，passthrough）。
- `build-engine.sh`（emcc）/ `build.mjs`（node 拼包）/ `test/smoke.test.mjs`。

**构建命令**：
```bash
cd wasm
npm run build          # 快路：engine + wrapper -> *.js（改 wrapper 后用）
npm run build:engine   # 慢路：emcc 重编 engine（需 source emsdk_env.sh）
npm test               # build + node --test（10/10）
npm publish            # publishConfig.access=public 已设
```

**关键约束**：
- engine 必须 `-sENVIRONMENT=web,worker`（**不带 node**，否则 `node:module` import 让浏览器打包器报错；web,worker 仍能在 node 跑因 wasm 内联）。
- **API 对齐 Dart**：`await ffuzzyInitialize()` 一次 → 同步用 `FuzzyCorpus.strings/byKey/byKeys`（非 keyed）、`addKey`（非 addKeyed）、`update/removeAt/removeWhere/refresh`、`FuzzyScoring`。WASM 模块藏内部不传句柄。
- lite = `lite-tables.c` 空表 + `-DFFZ_COMPACT_CLASS`（砍 unicode 表 + class 表；**无 `FFZ_ASCII_NORM` 宏**）。
- 体积：bundle `ffuzzy.js` 62KB(gzip 26) / `ffuzzy-lite.js` 48KB(gzip 19)。

## benchmark/（废弃 Rust 引擎，仅对比用）

Rust [`nucleo-matcher`](https://crates.io/crates/nucleo) 经 [`flutter_rust_bridge`](https://pub.dev/packages/flutter_rust_bridge)(frb 2.12.0) + cargokit 暴露给 Dart。**不再用于发布。**

Rust/frb/cargokit/预编译 的细节只适用于 `benchmark/` 里的遗留包（注意：WASM 现是 `wasm/` 的正式 npm 包，见上节，不再属 benchmark）。
