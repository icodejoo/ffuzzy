---
name: ffuzzy-plugin
description: Use when developing, building, testing, or publishing the ffuzzy Flutter fuzzy-search plugin. The published `ffuzzy` package is now the pure-C engine AT THE REPO ROOT (lib/ffuzzy.dart + src/ ffi/ include/ + platform dirs). The old Rust + flutter_rust_bridge engine is DEPRECATED, lives under benchmark/, and is kept only for performance comparison. Covers project layout, the public Dart API, the C build/test workflow, environment gotchas on this machine, and pub.dev publishing.
---

# ffuzzy 插件开发指南

> ⚠️ **大重构(2026-06-27):C 引擎已提到仓库根并改名为 `ffuzzy`。**
> - **当前发布/开发对象 = 仓库根的纯 C FFI 插件**,pub 包名 **`ffuzzy`**(已发版 0.3.0,
>   顶替 pub.dev 上原 Rust 版 `ffuzzy` 0.1.2)。入口 `lib/ffuzzy.dart`,C 源在
>   `src/ ffi/ include/`,平台目录 `android/ios/macos/linux/windows/`,示例 `example/`,
>   对比/差分工具 `difftest/ perf/`,引擎内幕文档 `doc/INTERNALS.md`。
> - **原生库名 `libffz`/`ffz.dll`、C 符号 `ffz_*`、FFI 查找名 `ffz_ffi_*`、C 编译宏 `FFZ_*` 保持不变**。
>   **Dart 公开 API 全部 `Fuzzy*` 前缀**(2026-06-27 从 `Ffz*` 改名 + 重设计,对齐 Rust 版能力):
>   - `FuzzyCorpus<T>`(泛型对象搜索,构造传 `stringOf` 提取器;`FuzzyCorpus.strings(...)` 便捷构造纯字符串)。
>   - **模式是方法不是 flag**:`fuzzy`/`substring`/`prefix`/`postfix`/`exact`,各带 `…Async` 孪生(后台 isolate)。`FuzzyMode` 枚举已删,改为内部 int 常量。
>   - `FuzzyOptions`(可选、含默认值,聚合 `caseMatching`/`normalization`/`parallel`/`threads`/`limit`/`highlight`):构造函数设 corpus 级默认,方法上用可空命名参数逐字段覆盖(`copyWith` 合并)。
>   - 增删改:`add`/`addAll`/`addKeyed`/`update`/`removeAt`/`removeWhere`/`refresh`/`clear`。**原生 corpus 是 append-only,逐项删改在 Dart 侧 clear+重 add 重建(O(n));内部维护 `List<T> _items` + `List<List<FuzzyKey>?> _keys` 与原生下标 1:1**。
>   - `FuzzyHit<T>`(带 `.obj` 原对象 + `index/score/matchedKind/matchedKey/indices`)、`FuzzyKey`/`FuzzyKeyKind`/`FuzzyCase`/`FuzzyNorm`、顶层 `fuzzyCodepointToUtf16`、`FuzzyException`、`FuzzyCrash`。
>   - 销毁/回收内存:`FuzzyCorpus.dispose()`(显式幂等)+ `NativeFinalizer` 兜底;异步在飞时 mutate/dispose 抛 `StateError`。
> - **Rust + frb 引擎已废弃**,移到 `benchmark/`(包名 `ffuzzy_rust_bench`,`publish_to: none`),
>   **仅用于 C-vs-Rust 性能对比**;`.pubignore` 已把 `benchmark/` 排除出发布包。
> - 下面**关于 Rust/frb/cargokit/预编译/wasm 的所有章节都只适用于 `benchmark/` 里的遗留 Rust 包**;
>   日常 ffuzzy 任务以仓库根的 C 代码为准。CI 是根的 `.github/workflows/ci.yml`(C 测试全套)。
> - C(FFI)在 **web 上不可用**;web 曾是 Rust+wasm 的卖点,已随 Rust 一并搁置。
> - 详见 memory `ffuzzy-c-port-clang`(注:其中 `clang/` 路径前缀现已等于仓库根)。

以下 `ffuzzy`(遗留 Rust 包)章节描述的是 **`benchmark/` 里的对比基准**:Rust [`nucleo-matcher`](https://crates.io/crates/nucleo)
经 [`flutter_rust_bridge`](https://pub.dev/packages/flutter_rust_bridge)(frb 2.12.0)+ cargokit
暴露给 Dart。**不再用于发布**。

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

## 改代码后怎么重新编译(速查)

先按"改了什么"分流——大多数情况**不需要**全量重编:

**A. 只改了 Dart(`lib/ffuzzy.dart` 或测试)** —— 不碰 FFI 边界,直接:
```bash
flutter analyze && flutter test          # 复用现有 rust/target/release/rust_lib_ffuzzy.dll
```

**B. 改了 `rust/src/api/*.rs` 的函数签名/新增导出**(FFI 边界变了)——必须先 codegen 再编译:
```bash
export PATH="$HOME/.cargo/bin:$PATH"
flutter_rust_bridge_codegen generate     # 重生成 lib/src/rust/* + rust/src/frb_generated.rs(勿手改)
(cd rust && cargo build --release)        # 重新编宿主 dll(flutter test 靠它)
flutter test && (cd rust && cargo test)   # Dart 测试 + Rust 单测
```

**C. 只改了 `rust/` 的函数体(签名没变)** —— 跳过 codegen,只重编:
```bash
export PATH="$HOME/.cargo/bin:$PATH"
(cd rust && cargo build --release) && flutter test
```

**判断要不要 codegen**:动了 `#[frb]` 暴露的函数签名、参数/返回类型、新增/删除导出 → 要 B;否则 C。
codegen 后务必 `git diff lib/src/rust rust/src/frb_generated.rs` 确认生成物变化合理。

**真机/桌面端验证**(走 cargokit 完整交叉编译):
```bash
cd example && flutter run -d <device>            # 或 flutter build windows --release
```

> ⚠️ **改了 `rust/`(含 `Cargo.lock`)= crate-hash 变**:发布前**必须重跑 precompile CI**(见下文),
> 否则使用者算出的哈希在 Release 里找不到二进制,会退回源码编译(又要 Rust)。纯 Dart 改动不影响哈希。

## 本机环境踩坑点(zh-CN 网络 + Windows)

- **Rust**:rustup(msvc host),已装 Android target(aarch64/armv7/x86_64/i686-linux-android)。
- **crate 镜像**:`~/.cargo/config.toml` 用 rsproxy.cn 源(官方源 TLS 被干扰)。
- **cargokit + Gradle 9**:`cargokit/gradle/plugin.gradle` 已把 `Project.exec()` 改成注入式
  `ExecOperations`(Gradle 9 移除了 `exec()`)。重新 integrate/升级 frb 后这个补丁会被覆盖,需重打。
- **NDK**:example 钉 `ndkVersion = "28.2.13676358"`(插件要求该版本)。
- **Android 构建(仅本机网络需要,勿提交进发布包)**:JDK 信任库不认网络 TLS 拦截代理 → 需
  `example/android/gradle.properties` 加 `systemProp.javax.net.ssl.trustStoreType=Windows-ROOT`;
  Gradle 发行版用本地 `file://`(官方源大文件被 RST);Maven 仓库加阿里云镜像。这些是**本机环境配置**,
  不应写进要发布的插件,验证时临时加。
- **JDK**:`flutter config --jdk-dir "C:\sdk\jdk\openjdk-21.0.5+11"`(曾指向不存在的旧路径)。

## CI / 推送踩坑(已踩过,务必记住)

- **shell 脚本的可执行位(高频坑)**:在 Windows 上 git 默认不带执行位,提交后 `cargokit/run_build_tool.sh`
  和 `cargokit/build_pod.sh` 会变成 `100644`。Linux/macOS runner checkout 后**不可执行**,Android/iOS
  交叉编译时 Rust 拿 `run_build_tool.sh` 当 linker 包装器去 exec → `could not exec the linker ...
  Permission denied (os error 13)`。**修法**:`git update-index --chmod=+x cargokit/run_build_tool.sh
  cargokit/build_pod.sh`。**自检**:`git ls-files -s '*.sh'` 必须是 `100755`。新加任何 `.sh` 都要打执行位。
- **预编译 CI**:`.github/workflows/precompile_binaries.yml` 手动触发(workflow_dispatch),串行 matrix。
  Android 没有独立 runner,**挂在 ubuntu 上用 NDK 交叉编译**(`--android-sdk-location=$ANDROID_SDK_ROOT
  --android-ndk-version=...`);iOS 必须 macОС、Windows 必须 windows。需先在仓库 Settings 配好 Secret `PRIVATE_KEY`。
- **Linux ARM64 交叉编译**:cargokit 在 Linux 上默认只编宿主架构(`buildableTargets` 不跨架构)。
  要补 `aarch64-unknown-linux-gnu`,**传 `--glibc-version` 触发 `cargo zigbuild`**(zig 当交叉链接器)。
  cargokit 会自动 `cargo install cargo-zigbuild`,但 **zig 本身要在工作流里单独装**(`mlugg/setup-zig@v2`)。
  glibc 取 **2.17**(manylinux2014 基线,可移植性最好;arm64 自 glibc 2.17 起支持)。
  产物追加到**同一个** `precompiled_<hash>` Release(hash 只由源码定,不受 `--target`/`--glibc-version` 影响)。
  RISC-V(`riscv64gc-unknown-linux-gnu`)曾考虑过但**按需求去掉**(太小众;若要补需 glibc≥2.27)。
  注:现有 `x86_64-unknown-linux-gnu` 是在 ubuntu runner 原生编的(glibc 较高);如需统一低 glibc,
  删掉该 Release 里的 x86_64 资产后用 zigbuild 重编即可。
- **本机网络会 RST 大块 git 上传**:`git push` 整包(几百 KB 一次)会在 `send-pack: unexpected disconnect
  while reading sideband packet` 处断开,但**放行小推送**。解法:把改动拆成多个小提交逐个推,且**用远程 sha
  与本地 HEAD 比对确认真·成功**(`git ls-remote origin -h refs/heads/main`),不要只看命令退出码——曾误报成功。
- **读 CI 日志**:本机没装 `gh`,GitHub Actions 日志是 JS 渲染 + 日志 API 403,WebFetch 抓不到正文。
  最快是让用户贴失败步骤最后 30~50 行;run 页面只能拿到 "exit code 1"。

## 发布到 pub.dev

`flutter pub publish --dry-run` 现已 **0 warnings**(已采用自包含单包结构)。仓库已推送到
`github.com/icodejoo/ffuzzy`(`repository`/`homepage` 已是真实地址)。

- **pubspec SDK 下限别低于 3.3.0**:`lib/src/rust/frb_generated.web.dart` 用了 `extension type`(inline-class,
  需 Dart ≥3.3.0)。下限设到 3.0.0 会让 `dart analyze` 在 dry-run 里报 `undefined_class RustLibWasmModule` +
  `experiment_not_enabled` 直接 fail。当前 `sdk: ">=3.3.0 <4.0.0"`。
- **Android `compileSdk` 坑(0.1.2 修)**:Flutter 插件模板默认 `android/build.gradle` 的 `compileSdkVersion 33`,
  在新版 Flutter(3.44+)+ 新 androidx 传递依赖下,使用者 Android release 构建会在 `:ffuzzy:checkReleaseAarMetadata`
  报 "requires compileSdk >= 34" 失败。已提到 **35**(真机 arm64 验证通过)。该文件**不在 `rust/` 内 → 不影响 crate-hash**,
  改它只需发新版本号,不必重跑预编译 CI。

### 真实流程验证(在第三方 app 里测,已跑通)
模拟「普通用户(无 Rust)」拿预编译包:本机装了 rustup 时 cargokit **默认本地编译**(见 `options.dart`
`defaultUsePrecompiledBinaries() => Rustup.executablePath()==null`),要强制走下载+验签,在 app 根目录放
`cargokit_options.yaml` → `use_precompiled_binaries: true`(+`verbose_logging: true` 看日志)。
- 单独验证原生库获取(免 VS/NDK 全量构建):直接驱动 cargokit `build-cmake`,喂 `CARGOKIT_CONFIGURATION=release`、
  `CARGOKIT_TARGET_PLATFORM`、`CARGOKIT_{MANIFEST,OUTPUT,TARGET_TEMP}_DIR`、`CARGOKIT_ROOT_PROJECT_DIR`(放上面的
  options 文件),manifest 指向 pub cache 里 `ffuzzy-<ver>/rust`。成功会下载 `<target>_<lib>` + `.sig` 验签后落到 OUTPUT_DIR。
  注:**不能在 pub cache 目录里 `dart run`**(Cannot operate on packages inside the cache),用仓库内的 `cargokit/build_tool` 跑。
- 真机整链:`flutter build apk --release --target-platform android-arm64` → 日志出现 `Found precompiled artifacts for
  aarch64-linux-android`(无 cargo 编译)→ `flutter install` + `adb shell am start` → logcat 看到自打的 smoke 行即通。
  release 模式 `debugPrint` 仍进 logcat(tag `flutter`)。

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

## 体积(打进 App 的原生库)

三档实测(host dll,x86_64-pc-windows-msvc):基线 514KB →(+panic=abort)373KB →
(+nightly build-std)329KB →(+RUSTFLAGS immediate-abort)**264KB**(−49%)。关 nucleo Unicode
feature 这条路走不通(0.3.1 在 `default-features=false` 下编不过)。

发布产物(CI:build-std + immediate-abort;arm64-linux 仅 build-std)预计:

| 架构 | 优化前 | 预计(待 CI 实测) |
|---|---:|---:|
| android arm64-v8a | ~618 KB | ~320 KB |
| android armeabi-v7a | ~448 KB | ~230 KB |
| android x86_64 | ~666 KB | ~345 KB |
| windows-x64 | ~514 KB | ~265 KB |

体积取决于三层叠加:① `rust/Cargo.toml [profile.release]`(opt-level="z"+lto+codegen-units=1+strip+
**panic="abort"**);② `rust/cargokit.yaml` 的 `cargo.release`(nightly + `-Zbuild-std=std,panic_abort`);
③ workflow 里 desktop/android job 的 `RUSTFLAGS=-Zunstable-options -Cpanic=immediate-abort`(cross-linux 不设,
见上文 zigbuild 说明)。本机 `flutter test` 走 stable 直编,只吃到 ①(~373KB),不影响发布产物。
Android 发布建议 `--split-per-abi`,每个 APK 只带一个架构的 .so。

### 两档可切换(extreme / safe)

怕极致压缩(nightly build-std/immediate-abort)出问题,保留了一键回退到原始 ~600KB 配置:

```bash
bash scripts/size-profile.sh extreme   # 极致压缩(默认,~265–320KB)
bash scripts/size-profile.sh safe      # 稳妥(原始 ~600KB,纯 stable,无 nightly/build-std)
bash scripts/size-profile.sh status    # 看当前档
```

机制:差异只在 ①②(`Cargo.toml` 的 panic 行、`cargokit.yaml` 的 `cargo` 段,都带 `size-profile:` 标记),
脚本注释/反注释这两处即可;③ 的 workflow **自动探测** `cargokit.yaml` 是否有生效的 build-std 来决定加不加
RUSTFLAGS(单一事实来源 = 提交里的配置,脚本与 workflow 不会失配)。

**两种切法**:
- 本地:`bash scripts/size-profile.sh safe`(改文件)→ 自己 commit/push → 重跑工作流。
- **CI 下拉框(省事,推荐)**:Actions → Run workflow → `profile` 选 `extreme`/`safe`/`keep`。`prepare` job
  会在 runner 上跑切换脚本、把改动**提交回仓库**(github-actions[bot]),再让编译阶段 checkout 那个 commit 来编。
  即"选档位 + 点运行"一步到位,本地记得 `git pull`。`keep` = 用仓库当前配置不改。

⚠️ **为什么不能做成两个独立 workflow 各编一套**:crate-hash 把 `Cargo.toml`+`cargokit.yaml`+`src/*.rs`+
`Cargo.lock`+`build.rs` 全算进去(`crate_hash.dart`),使用者按 hash 下载。所以**哪套二进制能被用,取决于提交里的
配置**——两套配置无法对同一发布版本同时生效。切档 = hash 变,切完必须重跑 CI + **发新版本号**。极致档发布后若出问题,
走「workflow 选 safe → 跑完 → 发 0.x+1」。
