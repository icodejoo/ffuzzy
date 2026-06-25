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
