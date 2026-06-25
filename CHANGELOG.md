## 0.1.0

首个版本。

- 基于 `nucleo-matcher`(Rust)+ `flutter_rust_bridge` 的高性能模糊搜索。
- `FuzzyMatcher<T>`(对象搜索)/ `FuzzyStringMatcher`(字符串搜索),返回命中 + 分数 + 高亮下标。
- 同步 `match` 与异步 `matchAsync`(后台线程,不阻塞 UI)。
- 可控索引:`buildIndices` / `freeIndices`,以及 `add` / `addAll` / `update` / `removeAt` / `removeWhere` / `clear` / `refresh` 增量维护。
- `single` / `singleAsync` 取最佳一条;`fuzzyMatch` / `fuzzyMatchIndices` / `fuzzyFilter` / `fuzzyFilterAsync` 独立函数。
- 可配置忽略大小写、Unicode 归一化、前缀优先(已规避 nucleo issue #92)。
- 支持 Android / iOS / macOS / Windows / Linux 原生交叉编译。
