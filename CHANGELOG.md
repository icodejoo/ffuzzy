## 0.2.0

新增多种匹配模式与性能开关。**破坏性**:`FuzzyConfig` 新增必填字段(`mode`/`parallel`/`incremental`),
直接 `FuzzyConfig(...)` 构造需补齐;推荐改用 `kDefaultFuzzyConfig.copyWith(...)`。

- **匹配模式 `MatchMode`**:除默认 `fuzzy` 外新增 `substring`(子串)/`prefix`(前缀)/`word`(整串相等),
  字面模式按原序返回、命中即截断,显著快于 fuzzy(实测 host 10x 数据 substring ~5×、prefix ~20×、word ~40×)。
- **按查询覆盖**:`match`/`matchAsync`/`single` 新增 `ignoreCase`/`mode` 参数,同一 matcher 不同查询可切模式/大小写。
- **`buildIndicesAsync()`**:大数据集建索引移到后台线程,不阻塞 UI。
- **多核并行**(`FuzzyConfig.parallel`,默认开):大数据 Fuzzy 搜索 + 建索引按候选数自动分块多核;
  结果与单线程完全一致;简单模式/小数据/web 单线程。
- **增量缓存**(`FuzzyConfig.incremental`,默认关):逐字输入(查询为上次前缀扩展)时只在上次命中集内重扫;
  仅 `fuzzy` + `FuzzyCorpus` 生效,任何增删改/free 自动失效。
- **构造参数 `ignoreCaseIndices`**:可选常驻一份小写折叠索引,让 `ignoreCase=true` 的简单模式走快路径。
- 内部:Fuzzy 改两趟(score 全扫排序 → 仅 top-N 回溯高亮下标);修复非 Fuzzy 模式 `limit=0` 误返回 1 条。

## 0.1.2

- 修复 Android release 构建失败:插件 `android/build.gradle` 的 `compileSdkVersion` 从 33 提升到 35
  （新版 Flutter 的 androidx 传递依赖要求 ≥34，否则 `checkReleaseAarMetadata` 报错）。已在 arm64 真机验证。
- 不影响原生库的 crate-hash，沿用 0.1.1 的预编译二进制。

## 0.1.1

- 原生库切换到「extreme」体积档(`panic=abort` + nightly build-std + immediate-abort),原生库体积约 265–320KB。
- 该版本对应的预编译二进制(crate-hash `86d0ef06`)已签名上传到 GitHub Release,使用者构建时直接下载,**无需安装 Rust 工具链**。
- 公开 API 无变化。

## 0.1.0

首个版本。

- 基于 `nucleo-matcher`(Rust)+ `flutter_rust_bridge` 的高性能模糊搜索。
- `FuzzyMatcher<T>`(对象搜索)/ `FuzzyStringMatcher`(字符串搜索),返回命中 + 分数 + 高亮下标。
- 同步 `match` 与异步 `matchAsync`(后台线程,不阻塞 UI)。
- 可控索引:`buildIndices` / `freeIndices`,以及 `add` / `addAll` / `update` / `removeAt` / `removeWhere` / `clear` / `refresh` 增量维护。
- `single` / `singleAsync` 取最佳一条;`fuzzyMatch` / `fuzzyMatchIndices` / `fuzzyFilter` / `fuzzyFilterAsync` 独立函数。
- 可配置忽略大小写、Unicode 归一化、前缀优先(已规避 nucleo issue #92)。
- 支持 Android / iOS / macOS / Windows / Linux 原生交叉编译。
