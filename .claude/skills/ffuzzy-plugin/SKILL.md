---
name: ffuzzy-plugin
description: Use when developing, building, regenerating bindings, testing, or publishing the ffuzzy Flutter plugin (high-performance fuzzy search powered by Rust nucleo-matcher via flutter_rust_bridge). Covers project layout, public API, the codegen+cargo build workflow, environment gotchas on this machine, and pub.dev publishing blockers.
---

# ffuzzy 插件开发指南

ffuzzy 是一个 Flutter 插件:Rust [`nucleo-matcher`](https://crates.io/crates/nucleo) 引擎
通过 [`flutter_rust_bridge`](https://pub.dev/packages/flutter_rust_bridge)(frb 2.12.0)+ cargokit
暴露给 Dart,做 fzf 式子序列模糊搜索。

## 工程结构(自包含单包,可直接发布)

```
ffuzzy/
├── lib/
│   ├── ffuzzy.dart            # 唯一对外入口(手写公开 API)
│   └── src/rust/              # frb 生成的 Dart 绑定(勿手改)
├── rust/                      # Rust crate(name = rust_lib_ffuzzy → 产物 librust_lib_ffuzzy.so)
│   ├── Cargo.toml             # flutter_rust_bridge + nucleo-matcher + [profile.release] 体积优化
│   └── src/api/fuzzy.rs       # 核心实现(nucleo 封装),改这里
├── cargokit/                  # cargokit 构建工具(含已打 Gradle 9 补丁的 gradle/plugin.gradle)
├── android|ios|macos|linux|windows/  # 各平台 cargokit 钩子,统一引用 ../rust 与 ../cargokit
├── example/                   # 演示 App + integration_test
├── test/                      # 自包含单元测试(宿主加载 rust/target/release/rust_lib_ffuzzy.dll)
└── flutter_rust_bridge.yaml   # codegen 配置
```

**ffuzzy 本身就是 ffiPlugin**(pubspec `flutter.plugin.platforms` 各平台 `ffiPlugin: true`),
cargokit 钩子在自己的平台目录里直接编译 `../rust`。**无 rust_builder、无 path 依赖,单包可发布。**

> 平台钩子路径规律:cargokit 的 manifest 相对 `CMAKE_CURRENT_SOURCE_DIR`/podspec 目录解析,rust 在插件内部,
> 所以各平台统一用 `../rust`(android `manifestDir`、ios/macos `build_pod.sh ../rust`、linux/windows
> `apply_cargokit(.. ../rust ..)`);linux/windows 的 `PROJECT_NAME` 与 `<plugin>_bundled_libraries` 用 `ffuzzy`,
> 但传给 cargokit 的 libname 仍是 `rust_lib_ffuzzy`(= .so 名,与 frb 加载器 stem 一致)。

## 公开 API(`package:ffuzzy/ffuzzy.dart`)

- `ffuzzy.ensureInitialized()` — 懒加载、幂等;同步方法前需 await 完成,异步方法内部自动确保。
- `FuzzyMatcher<T>(items, stringOf, {indexed, config})` / `.key(maps, fieldName)` — 对象搜索,
  `match`/`matchAsync`→`List<FuzzyOutput<T>>`(`.obj/.score/.indices`),`single`/`singleAsync`。
- `FuzzyStringMatcher(items, {indexed, config})` — 字符串搜索,返回 `FuzzyHit`(`.index/.score/.indices`)。
- 索引生命周期:`buildIndices`(显式建,幂等)/ `freeIndices`(只释放 Rust 索引)/ `dispose`/`disposeAndWait`。
- 增删改清:`add`/`addAll`(末尾追加,不重建)/`update`/`removeAt`/`removeWhere`/`clear`/`refresh`。
- 独立函数:`fuzzyMatch`/`fuzzyMatchIndices`/`fuzzyFilter`/`fuzzyFilterAsync`。
- 类型:`FuzzyConfig{ignoreCase,normalize,preferPrefix}`(+`copyWith` 扩展)、常量 `kDefaultFuzzyConfig`。

## 关键设计(改代码前必读)

- **从不自动建/重建索引**:无索引时 `match` 退化为整表扫描(慢但不崩、不偷偷占内存);要快须 `buildIndices`/`refresh`。
- **竞态用版本号丢弃**:`refresh`/`dispose`/`update`/`remove*`/`clear` 自增 `_generation`;在飞 `matchAsync` 完成后若版本变了返回空。`add` 只追加、不动下标,不丢弃在飞结果。
- **在飞排空再释放**:`freeIndices`/`dispose` 若有在飞异步搜索,等其结束再释放。
- **高亮按 rune**:`indices` 是字符(Unicode 标量)下标,Dart 高亮必须用 `text.runes`,不能用 `text[i]`(emoji 会错位)。
- **prefer_prefix 修复**:不用 nucleo 的内部 `prefer_prefix`(见 nucleo issue #92),改在排序层用"命中下标是否从 0 开始"判断。Rust `FuzzyConfig::default()` 与 Dart `kDefaultFuzzyConfig` 都为 `preferPrefix=true`。

## 开发工作流

改了 `rust/src/api/*.rs` 后:
```bash
export PATH="$HOME/.cargo/bin:$PATH"
flutter_rust_bridge_codegen generate          # 重生成 lib/src/rust + rust/src/frb_generated.rs
(cd rust && cargo build --release)            # 宿主 dll,供 flutter test 加载(rust/target/release/rust_lib_ffuzzy.dll)
flutter test                                  # 单元/竞态/CRUD/内存测试
(cd rust && cargo test)                       # Rust 单测
cd example && flutter run -d <device>         # 跑演示 App
```

## 本机环境踩坑点(zh-CN 网络 + Windows)

- **Rust**:rustup(msvc host),已装 Android target(aarch64/armv7/x86_64/i686-linux-android)。
- **crate 镜像**:`~/.cargo/config.toml` 用 rsproxy.cn 源(官方源 TLS 被干扰)。
- **cargokit + Gradle 9**:`rust_builder/cargokit/gradle/plugin.gradle` 已把 `Project.exec()` 改成注入式
  `ExecOperations`(Gradle 9 移除了 `exec()`)。重新 integrate/升级 frb 后这个补丁会被覆盖,需重打。
- **NDK**:example 钉 `ndkVersion = "28.2.13676358"`(插件要求该版本)。
- **Android 构建(仅本机网络需要,勿提交进发布包)**:JDK 信任库不认网络 TLS 拦截代理 → 需
  `example/android/gradle.properties` 加 `systemProp.javax.net.ssl.trustStoreType=Windows-ROOT`;
  Gradle 发行版用本地 `file://`(官方源大文件被 RST);Maven 仓库加阿里云镜像。这些是**本机环境配置**,
  不应写进要发布的插件,验证时临时加。
- **JDK**:`flutter config --jdk-dir "C:\sdk\jdk\openjdk-21.0.5+11"`(曾指向不存在的旧路径)。

## 发布到 pub.dev

`flutter pub publish --dry-run` 现已 **0 warnings**(已采用自包含单包结构)。仓库已推送到
`github.com/icodejoo/ffuzzy`(`repository`/`homepage` 已是真实地址)。

## 预编译二进制(让使用者免装 Rust)

机制:`rust/cargokit.yaml` 声明 `precompiled_binaries: {url_prefix, public_key}`。使用者构建时
cargokit 按 `<url_prefix><crate-hash>/<target>_<lib>` 从 GitHub Release 下载已签名二进制并验签,
成功则跳过 cargo 编译 → **无需 Rust 工具链**。crate-hash 由 `rust/` 内容决定。

- **签名密钥**:`cargokit/build_tool` 里 `dart run build_tool gen-key`。公钥写进 `rust/cargokit.yaml`;
  私钥存仓库 Secret `PRIVATE_KEY`(本地 `precompiled_signing_key.PRIVATE.txt` 已 gitignore,绝不入库)。
- **CI**:`.github/workflows/precompile_binaries.yml`(手动触发,串行 matrix)。ubuntu 编 linux+Android,
  macos 编 macOS+iOS,windows 编 win x64+arm64;用 `precompile-binaries --manifest-dir=../../rust
  --repository=icodejoo/ffuzzy`,创建 tag `precompiled_<hash>` 的 Release 上传二进制+`.sig`。
- **发布顺序**:① 定稿 `rust/` → ② 跑 precompile 工作流(等 Release 产出)→ ③ `verify-binaries` 校验 →
  ④ `flutter pub publish`。**改了 `rust/` 必须重跑 ② 再发**,否则使用者哈希对不上会退回源码编译。
- **验证**:`dart run build_tool verify-binaries --manifest-dir=../../rust`(检查各 target 是否有已签名资产)。
- 使用者想强制源码编译:在 app 平台目录放 `cargokit_options.yaml` → `use_precompiled_binaries: false`。

发布前建议先在 Android 真机/各桌面平台跑通 `example` 构建(首次经 cargokit 交叉编译 Rust)。

## 体积(打进 App 的原生库,release + size profile)

| 架构 | 大小 |
|---|---:|
| arm64-v8a | ~618 KB |
| armeabi-v7a | ~448 KB |
| x86_64 | ~666 KB |
| windows-x64 | ~514 KB |

体积只取决于 `rust/Cargo.toml` 的 `[profile.release]`(opt-level="z" + lto + codegen-units=1 + strip)。
要再压可加 `panic = "abort"`(但会破坏 frb 的 panic→异常桥接)。Android 发布建议 `--split-per-abi`,
每个 APK 只带一个架构的 .so。
