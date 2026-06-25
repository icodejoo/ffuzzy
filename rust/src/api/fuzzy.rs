//! 基于 `nucleo-matcher` 的模糊匹配 API，通过 flutter_rust_bridge 暴露给 Dart。
//!
//! 设计说明：使用底层同步的 `Pattern`/`Matcher`（而非高层带后台线程的 `nucleo` crate），
//! 一次性调用即可拿到分数与命中字符下标，适合跨 FFI 桥接。
//! `Pattern::parse` 的 `CaseMatching`/`Normalization` 会在匹配时覆盖 matcher 的对应配置，
//! 而 `prefer_prefix` 需要设置在 `Matcher` 的 `Config` 上。

use flutter_rust_bridge::frb;
use nucleo_matcher::pattern::{CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32Str, Utf32String};

/// 模糊匹配配置。
pub struct FuzzyConfig {
    /// 忽略大小写。true => `CaseMatching::Ignore`，false => `CaseMatching::Respect`。
    pub ignore_case: bool,
    /// Unicode 归一化（影响带变音符号字符的匹配）。true => `Normalization::Smart`，false => `Never`。
    pub normalize: bool,
    /// 是否给「以查询开头」的结果额外加分，使前缀匹配排名更靠前。
    pub prefer_prefix: bool,
}

impl Default for FuzzyConfig {
    fn default() -> Self {
        // 与 Dart 侧 kDefaultFuzzyConfig 保持一致(prefer_prefix=true)。
        Self {
            ignore_case: true,
            normalize: true,
            prefer_prefix: true,
        }
    }
}

/// 便捷构造默认配置，供 Dart 侧直接调用。
#[frb(sync)]
pub fn default_fuzzy_config() -> FuzzyConfig {
    FuzzyConfig::default()
}

/// 单次匹配结果：分数 + 命中的字符下标（升序去重，用于 UI 高亮）。
pub struct FuzzyMatch {
    pub score: u32,
    pub indices: Vec<u32>,
}

/// 列表筛选命中项：`index` 指回原列表，`score` 为匹配分，`indices` 为命中字符下标。
pub struct FuzzyHit {
    pub index: u32,
    pub score: u32,
    pub indices: Vec<u32>,
}

fn case_matching(cfg: &FuzzyConfig) -> CaseMatching {
    if cfg.ignore_case {
        CaseMatching::Ignore
    } else {
        CaseMatching::Respect
    }
}

fn normalization(cfg: &FuzzyConfig) -> Normalization {
    if cfg.normalize {
        Normalization::Smart
    } else {
        Normalization::Never
    }
}

fn make_matcher(_cfg: &FuzzyConfig) -> Matcher {
    // 不启用 nucleo 的 Config.prefer_prefix（见 upstream issue #92：它会给「恰好以查询
    // 首字符开头、但匹配实际不在开头」的候选错误加分）。前缀优先改由我们在排序阶段正确实现，
    // 见 rank_and_truncate。
    Matcher::new(Config::DEFAULT)
}

fn make_pattern(query: &str, cfg: &FuzzyConfig) -> Pattern {
    Pattern::parse(query, case_matching(cfg), normalization(cfg))
}

/// 统一的排序 + 截断。
///
/// `prefer_prefix=true` 时，把「真正以查询开头」（命中下标从 0 开始）的项排在最前，作为
/// 主排序键，再按分数降序、最后按原序稳定排列。这样既实现了前缀优先，又规避了 nucleo
/// issue #92 —— 我们用「命中下标是否从 0 开始」这个明确信号判断前缀，而不是 nucleo 那个
/// 会被候选串首字符误导的内部加分。
fn rank_and_truncate(
    mut hits: Vec<FuzzyHit>,
    cfg: &FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    if cfg.prefer_prefix {
        hits.sort_by(|a, b| {
            let a_prefix = a.indices.first() == Some(&0);
            let b_prefix = b.indices.first() == Some(&0);
            b_prefix
                .cmp(&a_prefix)
                .then(b.score.cmp(&a.score))
                .then(a.index.cmp(&b.index))
        });
    } else {
        hits.sort_by(|a, b| b.score.cmp(&a.score).then(a.index.cmp(&b.index)));
    }
    if let Some(limit) = limit {
        hits.truncate(limit as usize);
    }
    hits
}

/// 对单个字符串打分；不匹配返回 `None`（Dart 侧为 `int?`）。
#[frb(sync)]
pub fn fuzzy_match(query: String, haystack: String, config: FuzzyConfig) -> Option<u32> {
    let mut matcher = make_matcher(&config);
    let pattern = make_pattern(&query, &config);
    let mut buf = Vec::new();
    pattern.score(Utf32Str::new(&haystack, &mut buf), &mut matcher)
}

/// 对单个字符串打分并返回命中字符下标；不匹配返回 `None`。
#[frb(sync)]
pub fn fuzzy_match_indices(
    query: String,
    haystack: String,
    config: FuzzyConfig,
) -> Option<FuzzyMatch> {
    let mut matcher = make_matcher(&config);
    let pattern = make_pattern(&query, &config);
    let mut buf = Vec::new();
    let mut indices = Vec::new();
    pattern
        .indices(Utf32Str::new(&haystack, &mut buf), &mut matcher, &mut indices)
        .map(|score| {
            indices.sort_unstable();
            indices.dedup();
            FuzzyMatch { score, indices }
        })
}

/// 对列表做模糊筛选，按分数降序（同分保持原顺序）返回命中项；
/// `limit` 为可选的结果数量上限（Dart 侧为 `int?`）。
#[frb(sync)]
pub fn fuzzy_filter(
    query: String,
    items: Vec<String>,
    config: FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    let mut matcher = make_matcher(&config);
    let pattern = make_pattern(&query, &config);
    let mut hits: Vec<FuzzyHit> = Vec::with_capacity(items.len());
    let mut buf = Vec::new();
    let mut indices = Vec::new();
    for (i, item) in items.iter().enumerate() {
        buf.clear();
        indices.clear();
        let haystack = Utf32Str::new(item, &mut buf);
        if let Some(score) = pattern.indices(haystack, &mut matcher, &mut indices) {
            indices.sort_unstable();
            indices.dedup();
            hits.push(FuzzyHit {
                index: i as u32,
                score,
                indices: indices.clone(),
            });
        }
    }
    rank_and_truncate(hits, &config, limit)
}

/// 异步版本：不标记 `#[frb(sync)]`，frb 会在独立 worker 线程上执行，**不阻塞 Dart UI 线程**。
/// 适合超大数据集或不希望卡顿的场景（Dart 侧返回 `Future<List<FuzzyHit>>`）。
/// 比 Dart 的 `compute` 更省：无需把数据拷进新 isolate，直接在 Rust 线程算。
pub fn fuzzy_filter_async(
    query: String,
    items: Vec<String>,
    config: FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    fuzzy_filter(query, items, config, limit)
}

/// 缓存语料的模糊匹配器：候选列表常驻 Rust 侧，调用时只跨 FFI 传查询串，
/// 避免每次把整份列表编组传入（适合交互式逐字搜索）。构建时预转换为 Utf32 进一步提速。
#[frb(opaque)]
pub struct FuzzyCorpus {
    /// 源字符串：rehydrate 的依据，占用较小（约 1 字节/ASCII 字符）。
    items: Vec<String>,
    /// 预转换的 Utf32 索引：占内存大头（约 4 字节/字符）；`free` 后为 `None`。
    haystacks: Option<Vec<Utf32String>>,
}

fn build_haystacks(items: &[String]) -> Vec<Utf32String> {
    items.iter().map(|s| Utf32String::from(s.as_str())).collect()
}

impl FuzzyCorpus {
    /// 用一组候选项构建语料（一次性，类似建索引）。
    #[frb(sync)]
    pub fn new(items: Vec<String>) -> FuzzyCorpus {
        let haystacks = Some(build_haystacks(&items));
        FuzzyCorpus { items, haystacks }
    }

    /// 增量追加候选项到末尾(不重建索引)。已驻留则同步追加 Utf32 索引;
    /// 未驻留(已 free)只追加源,下次 rehydrate 一并生效。O(追加量)。
    #[frb(sync)]
    pub fn add(&mut self, items: Vec<String>) {
        if let Some(hs) = self.haystacks.as_mut() {
            hs.extend(items.iter().map(|s| Utf32String::from(s.as_str())));
        }
        self.items.extend(items);
    }

    /// 替换指定下标的候选(越界忽略)。O(1)。
    #[frb(sync)]
    pub fn set_at(&mut self, index: u32, item: String) {
        let i = index as usize;
        if i >= self.items.len() {
            return;
        }
        if let Some(hs) = self.haystacks.as_mut() {
            hs[i] = Utf32String::from(item.as_str());
        }
        self.items[i] = item;
    }

    /// 批量按下标删除(内部降序去重删除,避免位移错乱)。
    #[frb(sync)]
    pub fn remove_indices(&mut self, indices: Vec<u32>) {
        let mut idx: Vec<usize> = indices.into_iter().map(|x| x as usize).collect();
        idx.sort_unstable_by(|a, b| b.cmp(a));
        idx.dedup();
        for i in idx {
            if i < self.items.len() {
                self.items.remove(i);
                if let Some(hs) = self.haystacks.as_mut() {
                    hs.remove(i);
                }
            }
        }
    }

    /// 清空全部候选(保留实例与驻留状态)。
    #[frb(sync)]
    pub fn clear(&mut self) {
        self.items.clear();
        if let Some(hs) = self.haystacks.as_mut() {
            hs.clear();
        }
    }

    /// 当前语料条目数。
    #[frb(sync)]
    pub fn len(&self) -> u32 {
        self.items.len() as u32
    }

    /// 是否为空。
    #[frb(sync)]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// 是否已驻留 Utf32 索引（free 后为 false）。
    #[frb(sync)]
    pub fn is_hydrated(&self) -> bool {
        self.haystacks.is_some()
    }

    /// 释放占内存大头的 Utf32 索引，但**保留实例与源字符串**，便于低成本 `rehydrate`。
    /// free 后内存回落到约「源字符串」大小（ASCII 下约为驻留态的 1/5）。
    /// 幂等。free 状态下 `filter` 仍可用（每次从源临时转换，较慢，但无需跨 FFI 重传列表）。
    #[frb(sync)]
    pub fn free(&mut self) {
        self.haystacks = None;
    }

    /// 从驻留的源字符串重建 Utf32 索引，恢复快速搜索。**无跨 FFI 编组开销**（源已在 Rust 侧）。
    /// 已驻留时为空操作。
    #[frb(sync)]
    pub fn rehydrate(&mut self) {
        if self.haystacks.is_none() {
            self.haystacks = Some(build_haystacks(&self.items));
        }
    }

    /// 仅传入查询串即可过滤已缓存的语料，按分数降序（同分保持原序）返回命中项。
    /// 若已 `free`，则从源字符串临时转换匹配（仍无需跨 FFI 重传列表）。
    #[frb(sync)]
    pub fn filter(&self, query: String, config: FuzzyConfig, limit: Option<u32>) -> Vec<FuzzyHit> {
        let mut matcher = make_matcher(&config);
        let pattern = make_pattern(&query, &config);
        let mut hits: Vec<FuzzyHit> = Vec::new();
        let mut indices = Vec::new();
        match &self.haystacks {
            // 已驻留：直接用预转换的 Utf32 索引（最快路径）。
            Some(haystacks) => {
                for (i, hay) in haystacks.iter().enumerate() {
                    indices.clear();
                    if let Some(score) =
                        pattern.indices(hay.slice(..), &mut matcher, &mut indices)
                    {
                        indices.sort_unstable();
                        indices.dedup();
                        hits.push(FuzzyHit {
                            index: i as u32,
                            score,
                            indices: indices.clone(),
                        });
                    }
                }
            }
            // 已 free：从源字符串临时转换（较慢，但不必跨 FFI 重传）。
            None => {
                let mut buf = Vec::new();
                for (i, item) in self.items.iter().enumerate() {
                    buf.clear();
                    indices.clear();
                    let hay = Utf32Str::new(item, &mut buf);
                    if let Some(score) = pattern.indices(hay, &mut matcher, &mut indices) {
                        indices.sort_unstable();
                        indices.dedup();
                        hits.push(FuzzyHit {
                            index: i as u32,
                            score,
                            indices: indices.clone(),
                        });
                    }
                }
            }
        }
        rank_and_truncate(hits, &config, limit)
    }

    /// `filter` 的异步版本：在 frb worker 线程执行，不阻塞 UI（Dart 侧返回 `Future`）。
    /// 语料仍常驻 Rust 侧，调用只跨 FFI 传查询串。
    pub fn filter_async(
        &self,
        query: String,
        config: FuzzyConfig,
        limit: Option<u32>,
    ) -> Vec<FuzzyHit> {
        self.filter(query, config, limit)
    }
}

#[cfg(test)]
mod tests {
    //! 主机端验证：与 `integration_test/app_test.dart` 中的断言保持一致，
    //! 用于在没有真机时先确认 nucleo 匹配逻辑本身正确。
    use super::*;

    fn cfg() -> FuzzyConfig {
        FuzzyConfig::default()
    }

    #[test]
    fn match_subsequence_and_miss() {
        assert!(fuzzy_match("fb".into(), "flutter_rust_bridge".into(), cfg()).is_some());
        assert!(fuzzy_match("zzz".into(), "flutter_rust_bridge".into(), cfg()).is_none());
    }

    #[test]
    fn indices_map_to_query_chars() {
        let r = fuzzy_match_indices("fzd".into(), "fuzzy.dart".into(), cfg()).unwrap();
        // 升序去重。
        let mut sorted = r.indices.clone();
        sorted.sort_unstable();
        assert_eq!(r.indices, sorted);
        // 命中字符按序拼接应等于查询串。
        let runes: Vec<char> = "fuzzy.dart".chars().collect();
        let matched: String = r.indices.iter().map(|&i| runes[i as usize]).collect();
        assert_eq!(matched, "fzd");
    }

    #[test]
    fn filter_sorts_desc_and_indexes_back() {
        let items = vec![
            "pubspec.yaml".to_string(),
            "lib/main.dart".to_string(),
            "rust/Cargo.toml".to_string(),
            "pub/something.txt".to_string(),
            "README.md".to_string(),
        ];
        let hits = fuzzy_filter("pub".into(), items.clone(), cfg(), None);
        assert!(!hits.is_empty());
        for h in &hits {
            assert!(items[h.index as usize].to_lowercase().contains("pub"));
        }
        for w in hits.windows(2) {
            assert!(w[0].score >= w[1].score, "应按分数降序");
        }
    }

    #[test]
    fn filter_limit_truncates() {
        let items = vec![
            "ab".to_string(),
            "abc".to_string(),
            "abcd".to_string(),
            "xabcy".to_string(),
            "a_b_c".to_string(),
        ];
        let all = fuzzy_filter("abc".into(), items.clone(), cfg(), None);
        let limited = fuzzy_filter("abc".into(), items, cfg(), Some(2));
        assert!(all.len() > 2);
        assert_eq!(limited.len(), 2);
    }

    #[test]
    fn ignore_case_config() {
        let respect = FuzzyConfig {
            ignore_case: false,
            normalize: true,
            prefer_prefix: false,
        };
        assert!(fuzzy_match("rust".into(), "RUST".into(), respect).is_none());
        assert!(fuzzy_match("rust".into(), "RUST".into(), cfg()).is_some());
    }

    #[test]
    fn cached_corpus_matches_stateless_filter() {
        let items = vec![
            "service42".to_string(),
            "widget7".to_string(),
            "controller9".to_string(),
            "database3".to_string(),
        ];
        let corpus = FuzzyCorpus::new(items.clone());
        assert_eq!(corpus.len(), 4);
        assert!(!corpus.is_empty());

        let cached = corpus.filter("srvc".into(), cfg(), None);
        let stateless = fuzzy_filter("srvc".into(), items, cfg(), None);
        assert_eq!(cached.len(), stateless.len());
        for (a, b) in cached.iter().zip(stateless.iter()) {
            assert_eq!(a.index, b.index);
            assert_eq!(a.score, b.score);
            assert_eq!(a.indices, b.indices);
        }
    }

    #[test]
    fn prefer_prefix_fixes_issue_92() {
        let items = vec![
            "lsp_code_lens".to_string(), // 0：'l' 开头但匹配在末尾
            "code_lens".to_string(),     // 1：匹配在末尾
            "lens_factory".to_string(),  // 2：真正以 "lens" 开头
        ];
        let cfg = FuzzyConfig {
            ignore_case: true,
            normalize: true,
            prefer_prefix: true,
        };
        let hits = fuzzy_filter("lens".into(), items.clone(), cfg, None);
        let name = |h: &FuzzyHit| items[h.index as usize].as_str();

        // 真正以查询开头的项排第一。
        assert_eq!(name(&hits[0]), "lens_factory");

        // issue #92 的核心：非前缀命中不应仅因候选串首字符是 'l' 就被抬高分数。
        // 修复后，两个「末尾匹配」项的分数应相等（不再有伪前缀加分）。
        let score_of = |n: &str| {
            hits.iter()
                .find(|h| name(h) == n)
                .map(|h| h.score)
                .unwrap()
        };
        assert_eq!(
            score_of("lsp_code_lens"),
            score_of("code_lens"),
            "issue #92: 伪前缀加分应已消除，两者分数应相等"
        );
    }

    #[test]
    fn free_rehydrate_keep_results() {
        let items = vec![
            "service42".to_string(),
            "widget7".to_string(),
            "controller9".to_string(),
            "database3".to_string(),
        ];
        let mut corpus = FuzzyCorpus::new(items.clone());
        assert!(corpus.is_hydrated());
        let hydrated = corpus.filter("srvc".into(), cfg(), None);

        // free 后实例仍可搜索（从源临时转换），结果一致。
        corpus.free();
        assert!(!corpus.is_hydrated());
        let freed = corpus.filter("srvc".into(), cfg(), None);
        assert_eq!(hydrated.len(), freed.len());
        for (a, b) in hydrated.iter().zip(freed.iter()) {
            assert_eq!(a.index, b.index);
            assert_eq!(a.score, b.score);
            assert_eq!(a.indices, b.indices);
        }

        // rehydrate 后恢复驻留，结果仍一致。
        corpus.rehydrate();
        assert!(corpus.is_hydrated());
        let rehydrated = corpus.filter("srvc".into(), cfg(), None);
        assert_eq!(hydrated.len(), rehydrated.len());
    }

    #[test]
    fn async_matches_sync() {
        let items = vec![
            "service42".to_string(),
            "widget7".to_string(),
            "controller9".to_string(),
        ];
        let a = fuzzy_filter_async("srvc".into(), items.clone(), cfg(), None);
        let b = fuzzy_filter("srvc".into(), items, cfg(), None);
        assert_eq!(a.len(), b.len());
        for (x, y) in a.iter().zip(b.iter()) {
            assert_eq!(x.index, y.index);
            assert_eq!(x.score, y.score);
        }
    }

    #[test]
    fn empty_query_matches_all_with_zero_score() {
        let items = vec!["a".to_string(), "b".to_string(), "c".to_string()];
        let hits = fuzzy_filter("".into(), items.clone(), cfg(), None);
        assert_eq!(hits.len(), items.len());
        assert!(hits.iter().all(|h| h.score == 0));
    }
}
