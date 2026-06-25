//! 基于 `nucleo-matcher` 的模糊匹配 API，通过 flutter_rust_bridge 暴露给 Dart。
//!
//! 设计说明：使用底层同步的 `Pattern`/`Matcher`（而非高层带后台线程的 `nucleo` crate），
//! 一次性调用即可拿到分数与命中字符下标，适合跨 FFI 桥接。
//!
//! 匹配模式（[MatchMode]）：
//! - `Fuzzy`（默认）：nucleo 子序列模糊 + 打分 + 排序。**两趟**：先 score 全扫并排序，
//!   再只对返回的 top-N 回溯高亮下标，避免给所有命中都白算/分配下标。
//! - `Substring`/`Prefix`/`Word`：字面（包含/前缀/整串相等），原序、不排序、命中即截断。
//!
//! 大小写：`ignore_case` 是**按查询**的参数。简单模式/子序列在「原样」或「折叠（小写）」字符串上匹配——
//! 默认在原样上：`ignore_case=false` 直接比（快），`ignore_case=true` 则把候选临时折叠后比（慢，少数路径）；
//! 若构造时开启 `ignore_case_indices`，则常驻一份折叠副本让 `ignore_case=true` 也走快路径。
//! Fuzzy 的大小写由 nucleo 在匹配时折叠，无需折叠副本。
//! （注：极少数大小写折叠会改变字符数的 Unicode 字符，折叠路径下高亮下标可能与原串轻微错位；ASCII 无此问题。）

use flutter_rust_bridge::frb;
use nucleo_matcher::pattern::{CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32Str, Utf32String};

/// 匹配模式。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MatchMode {
    /// nucleo 子序列模糊匹配 + 打分 + 排序（默认）。
    Fuzzy,
    /// 精确子串：`haystack` 包含 `query`。
    Substring,
    /// 前缀：`haystack` 以 `query` 开头。
    Prefix,
    /// 全词：`haystack` 与 `query` 完全相等。
    Word,
}

/// 模糊匹配配置。`ignore_case` 按查询传，其余仅 `Fuzzy` 相关。
pub struct FuzzyConfig {
    /// 忽略大小写。
    pub ignore_case: bool,
    /// Unicode 归一化（仅 `Fuzzy` 生效；简单模式忽略）。
    pub normalize: bool,
    /// 前缀优先（仅 `Fuzzy` 的排序生效）。
    pub prefer_prefix: bool,
    /// 匹配模式。
    pub mode: MatchMode,
}

impl Default for FuzzyConfig {
    fn default() -> Self {
        Self {
            ignore_case: true,
            normalize: true,
            prefer_prefix: true,
            mode: MatchMode::Fuzzy,
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

fn make_matcher() -> Matcher {
    // 不启用 nucleo 的 Config.prefer_prefix（见 upstream issue #92）；前缀优先在排序阶段用
    // 「命中下标是否从 0 开始」实现。
    Matcher::new(Config::DEFAULT)
}

fn make_pattern(query: &str, cfg: &FuzzyConfig) -> Pattern {
    Pattern::parse(query, case_matching(cfg), normalization(cfg))
}

// ───────────────────────── 非 Fuzzy（简单 + 子序列）匹配 ─────────────────────────

/// 在「已按 ignore_case 折好大小写」的 `q`/`hay` 上做非 Fuzzy 匹配。
/// 命中返回字符（rune）下标，不命中返回 `None`。
fn nonfuzzy_match_indices(mode: MatchMode, q: &str, hay: &str) -> Option<Vec<u32>> {
    match mode {
        MatchMode::Word => (hay == q).then(|| (0..hay.chars().count() as u32).collect()),
        MatchMode::Prefix => hay
            .starts_with(q)
            .then(|| (0..q.chars().count() as u32).collect()),
        MatchMode::Substring => {
            // 字节长度守卫：候选比查询短，不可能包含。
            if hay.len() < q.len() {
                return None;
            }
            hay.find(q).map(|byte_pos| {
                let start = hay[..byte_pos].chars().count() as u32;
                let len = q.chars().count() as u32;
                (start..start + len).collect()
            })
        }
        MatchMode::Fuzzy => None, // 调用方已分流
    }
}

/// 非 Fuzzy 列表过滤：在 `source`（已折好大小写）上按**原序**匹配，命中满 `limit` 即停。
fn nonfuzzy_filter(source: &[String], q: &str, mode: MatchMode, limit: Option<u32>) -> Vec<FuzzyHit> {
    let lim = limit.map(|l| l as usize);
    let mut hits = Vec::new();
    for (i, hay) in source.iter().enumerate() {
        if let Some(indices) = nonfuzzy_match_indices(mode, q, hay) {
            hits.push(FuzzyHit {
                index: i as u32,
                score: 0,
                indices,
            });
            if let Some(l) = lim {
                if hits.len() >= l {
                    break;
                }
            }
        }
    }
    hits
}

// ───────────────────────── Fuzzy 两趟（score 扫描 → top-N 补下标） ─────────────────────────

struct Scored {
    index: u32,
    score: u32,
    is_prefix: bool,
}

fn rank_scored(mut v: Vec<Scored>, prefer_prefix: bool, limit: Option<u32>) -> Vec<Scored> {
    v.sort_by(|a, b| {
        let pref = if prefer_prefix {
            b.is_prefix.cmp(&a.is_prefix)
        } else {
            std::cmp::Ordering::Equal
        };
        pref.then(b.score.cmp(&a.score)).then(a.index.cmp(&b.index))
    });
    if let Some(l) = limit {
        v.truncate(l as usize);
    }
    v
}

/// 第一趟：对一条候选打分。`prefer_prefix` 时顺带用下标判断是否「从 0 开始」（前缀），
/// 但**不保留/克隆下标**（复用 `buf`）。
fn scan_one(
    pattern: &Pattern,
    matcher: &mut Matcher,
    hay: Utf32Str,
    prefer_prefix: bool,
    buf: &mut Vec<u32>,
) -> Option<(u32, bool)> {
    if prefer_prefix {
        buf.clear();
        pattern
            .indices(hay, matcher, buf)
            .map(|score| (score, buf.first() == Some(&0)))
    } else {
        pattern.score(hay, matcher).map(|score| (score, false))
    }
}

/// 第二趟：对最终 top-N 回溯高亮下标。
fn indices_one(pattern: &Pattern, matcher: &mut Matcher, hay: Utf32Str, buf: &mut Vec<u32>) -> Vec<u32> {
    buf.clear();
    pattern.indices(hay, matcher, buf);
    buf.sort_unstable();
    buf.dedup();
    buf.clone()
}

/// 在一组已转好的 Utf32 候选上做 Fuzzy 两趟过滤。
fn fuzzy_rank_haystacks(
    haystacks: &[Utf32String],
    query: &str,
    cfg: &FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    let mut matcher = make_matcher();
    let pattern = make_pattern(query, cfg);
    let mut buf = Vec::new();
    let mut scored = Vec::new();
    for (i, hay) in haystacks.iter().enumerate() {
        if let Some((score, is_prefix)) =
            scan_one(&pattern, &mut matcher, hay.slice(..), cfg.prefer_prefix, &mut buf)
        {
            scored.push(Scored {
                index: i as u32,
                score,
                is_prefix,
            });
        }
    }
    let top = rank_scored(scored, cfg.prefer_prefix, limit);
    top.into_iter()
        .map(|s| {
            let indices = indices_one(&pattern, &mut matcher, haystacks[s.index as usize].slice(..), &mut buf);
            FuzzyHit {
                index: s.index,
                score: s.score,
                indices,
            }
        })
        .collect()
}

/// 在源字符串上做 Fuzzy 两趟过滤（无常驻 Utf32 时用，临时转换）。
fn fuzzy_rank_items(
    items: &[String],
    query: &str,
    cfg: &FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    let mut matcher = make_matcher();
    let pattern = make_pattern(query, cfg);
    let mut conv = Vec::new();
    let mut buf = Vec::new();
    let mut scored = Vec::new();
    for (i, item) in items.iter().enumerate() {
        conv.clear();
        let hay = Utf32Str::new(item, &mut conv);
        if let Some((score, is_prefix)) =
            scan_one(&pattern, &mut matcher, hay, cfg.prefer_prefix, &mut buf)
        {
            scored.push(Scored {
                index: i as u32,
                score,
                is_prefix,
            });
        }
    }
    let top = rank_scored(scored, cfg.prefer_prefix, limit);
    top.into_iter()
        .map(|s| {
            conv.clear();
            let hay = Utf32Str::new(&items[s.index as usize], &mut conv);
            let indices = indices_one(&pattern, &mut matcher, hay, &mut buf);
            FuzzyHit {
                index: s.index,
                score: s.score,
                indices,
            }
        })
        .collect()
}

// ───────────────────────── 单条匹配（保持原 API） ─────────────────────────

/// 对单个字符串打分；不匹配返回 `None`。简单/子序列模式命中返回 `Some(0)`。
#[frb(sync)]
pub fn fuzzy_match(query: String, haystack: String, config: FuzzyConfig) -> Option<u32> {
    if config.mode != MatchMode::Fuzzy {
        let (q, hay) = fold_pair(&query, &haystack, config.ignore_case);
        return nonfuzzy_match_indices(config.mode, &q, &hay).map(|_| 0);
    }
    let mut matcher = make_matcher();
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
    if config.mode != MatchMode::Fuzzy {
        let (q, hay) = fold_pair(&query, &haystack, config.ignore_case);
        return nonfuzzy_match_indices(config.mode, &q, &hay)
            .map(|indices| FuzzyMatch { score: 0, indices });
    }
    let mut matcher = make_matcher();
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

/// 折叠一对字符串（用于单条匹配的 ignore_case）。
fn fold_pair(q: &str, hay: &str, ignore_case: bool) -> (String, String) {
    if ignore_case {
        (q.to_lowercase(), hay.to_lowercase())
    } else {
        (q.to_string(), hay.to_string())
    }
}

// ───────────────────────── 无状态列表过滤 ─────────────────────────

/// 对列表做筛选。`Fuzzy` 按分数降序，其余按原序（命中满 `limit` 即停）。
#[frb(sync)]
pub fn fuzzy_filter(
    query: String,
    items: Vec<String>,
    config: FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    if config.mode != MatchMode::Fuzzy {
        let q = if config.ignore_case {
            query.to_lowercase()
        } else {
            query
        };
        let owned_folded;
        let source: &[String] = if config.ignore_case {
            owned_folded = items.iter().map(|s| s.to_lowercase()).collect::<Vec<_>>();
            &owned_folded
        } else {
            &items
        };
        return nonfuzzy_filter(source, &q, config.mode, limit);
    }
    fuzzy_rank_items(&items, &query, &config, limit)
}

/// 异步版本：在 frb worker 线程执行，不阻塞 Dart UI 线程。
pub fn fuzzy_filter_async(
    query: String,
    items: Vec<String>,
    config: FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    fuzzy_filter(query, items, config, limit)
}

// ───────────────────────── 缓存语料 ─────────────────────────

/// 缓存语料的模糊匹配器：候选常驻 Rust 侧，调用只跨 FFI 传查询串。
/// `Fuzzy` 用预转换的 Utf32 索引；简单/子序列模式用源字符串（可选常驻折叠副本加速 ignore_case）。
#[frb(opaque)]
pub struct FuzzyCorpus {
    /// 源字符串（原样）。
    items: Vec<String>,
    /// 预转换的 Utf32 索引（`Fuzzy` 用）；`free` 后为 `None`。
    haystacks: Option<Vec<Utf32String>>,
    /// 可选的折叠（小写）副本：构造时 `ignore_case_indices=true` 才建，让简单/子序列模式在
    /// `ignore_case=true` 时走快路径；`free` 后为 `None`。
    folded: Option<Vec<String>>,
    /// 是否启用折叠副本（决定 rehydrate 时是否重建）。
    keep_folded: bool,
}

fn build_haystacks(items: &[String]) -> Vec<Utf32String> {
    items.iter().map(|s| Utf32String::from(s.as_str())).collect()
}

fn build_folded(items: &[String]) -> Vec<String> {
    items.iter().map(|s| s.to_lowercase()).collect()
}

/// 异步构建语料：在 frb worker 线程执行 Utf32 转换/折叠（大数据时较重），**不阻塞 Dart UI 线程**。
/// 与 [FuzzyCorpus::new] 等价，只是不标 `#[frb(sync)]`（Dart 侧返回 `Future<FuzzyCorpus>`）。
/// 注：候选列表的跨 FFI 编组同样在 worker 线程；但 Dart 侧的投影（stringOf）仍在调用线程算。
pub fn fuzzy_corpus_new_async(items: Vec<String>, ignore_case_indices: bool) -> FuzzyCorpus {
    FuzzyCorpus::build(items, ignore_case_indices)
}

impl FuzzyCorpus {
    /// 实际构建逻辑（sync `new` 与 async `fuzzy_corpus_new_async` 共用）。
    fn build(items: Vec<String>, ignore_case_indices: bool) -> FuzzyCorpus {
        let haystacks = Some(build_haystacks(&items));
        let folded = if ignore_case_indices {
            Some(build_folded(&items))
        } else {
            None
        };
        FuzzyCorpus {
            items,
            haystacks,
            folded,
            keep_folded: ignore_case_indices,
        }
    }

    /// 用一组候选项构建语料。`ignore_case_indices=true` 时额外常驻一份折叠副本。
    #[frb(sync)]
    pub fn new(items: Vec<String>, ignore_case_indices: bool) -> FuzzyCorpus {
        Self::build(items, ignore_case_indices)
    }

    /// 末尾追加（不重建）。同步维护 Utf32 索引与折叠副本。
    #[frb(sync)]
    pub fn add(&mut self, items: Vec<String>) {
        if let Some(hs) = self.haystacks.as_mut() {
            hs.extend(items.iter().map(|s| Utf32String::from(s.as_str())));
        }
        if let Some(fd) = self.folded.as_mut() {
            fd.extend(items.iter().map(|s| s.to_lowercase()));
        }
        self.items.extend(items);
    }

    /// 替换指定下标（越界忽略）。
    #[frb(sync)]
    pub fn set_at(&mut self, index: u32, item: String) {
        let i = index as usize;
        if i >= self.items.len() {
            return;
        }
        if let Some(hs) = self.haystacks.as_mut() {
            hs[i] = Utf32String::from(item.as_str());
        }
        if let Some(fd) = self.folded.as_mut() {
            fd[i] = item.to_lowercase();
        }
        self.items[i] = item;
    }

    /// 批量按下标删除（内部降序去重）。
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
                if let Some(fd) = self.folded.as_mut() {
                    fd.remove(i);
                }
            }
        }
    }

    /// 清空全部候选（保留实例与驻留状态）。
    #[frb(sync)]
    pub fn clear(&mut self) {
        self.items.clear();
        if let Some(hs) = self.haystacks.as_mut() {
            hs.clear();
        }
        if let Some(fd) = self.folded.as_mut() {
            fd.clear();
        }
    }

    #[frb(sync)]
    pub fn len(&self) -> u32 {
        self.items.len() as u32
    }

    #[frb(sync)]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    #[frb(sync)]
    pub fn is_hydrated(&self) -> bool {
        self.haystacks.is_some()
    }

    /// 释放占内存大头的 Utf32 索引（与折叠副本），保留源字符串便于 `rehydrate`。幂等。
    #[frb(sync)]
    pub fn free(&mut self) {
        self.haystacks = None;
        self.folded = None;
    }

    /// 从源字符串重建 Utf32 索引（及折叠副本，若启用）。无跨 FFI 编组开销。幂等。
    #[frb(sync)]
    pub fn rehydrate(&mut self) {
        if self.haystacks.is_none() {
            self.haystacks = Some(build_haystacks(&self.items));
            if self.keep_folded {
                self.folded = Some(build_folded(&self.items));
            }
        }
    }

    /// 非 Fuzzy 过滤：选好（原样/折叠）源后做字面/子序列匹配。
    fn filter_nonfuzzy(&self, query: &str, cfg: &FuzzyConfig, limit: Option<u32>) -> Vec<FuzzyHit> {
        let q = if cfg.ignore_case {
            query.to_lowercase()
        } else {
            query.to_string()
        };
        let owned_folded;
        let source: &[String] = match (cfg.ignore_case, &self.folded) {
            // ignore_case + 有常驻折叠副本：快路径。
            (true, Some(folded)) => folded,
            // ignore_case + 无折叠副本：临时折叠（慢，少数路径）。
            (true, None) => {
                owned_folded = build_folded(&self.items);
                &owned_folded
            }
            // 区分大小写：用原样源。
            (false, _) => &self.items,
        };
        nonfuzzy_filter(source, &q, cfg.mode, limit)
    }

    /// 过滤已缓存的语料。`Fuzzy` 按分数降序；其余按原序。
    #[frb(sync)]
    pub fn filter(&self, query: String, config: FuzzyConfig, limit: Option<u32>) -> Vec<FuzzyHit> {
        if config.mode != MatchMode::Fuzzy {
            return self.filter_nonfuzzy(&query, &config, limit);
        }
        match &self.haystacks {
            Some(haystacks) => fuzzy_rank_haystacks(haystacks, &query, &config, limit),
            None => fuzzy_rank_items(&self.items, &query, &config, limit),
        }
    }

    /// `filter` 的异步版本：frb worker 线程执行，不阻塞 UI。
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
    use super::*;

    fn cfg() -> FuzzyConfig {
        FuzzyConfig::default()
    }

    fn cfg_mode(mode: MatchMode) -> FuzzyConfig {
        FuzzyConfig {
            mode,
            ..FuzzyConfig::default()
        }
    }

    #[test]
    fn match_subsequence_and_miss() {
        assert!(fuzzy_match("fb".into(), "flutter_rust_bridge".into(), cfg()).is_some());
        assert!(fuzzy_match("zzz".into(), "flutter_rust_bridge".into(), cfg()).is_none());
    }

    #[test]
    fn indices_map_to_query_chars() {
        let r = fuzzy_match_indices("fzd".into(), "fuzzy.dart".into(), cfg()).unwrap();
        let mut sorted = r.indices.clone();
        sorted.sort_unstable();
        assert_eq!(r.indices, sorted);
        let runes: Vec<char> = "fuzzy.dart".chars().collect();
        let matched: String = r.indices.iter().map(|&i| runes[i as usize]).collect();
        assert_eq!(matched, "fzd");
    }

    #[test]
    fn fuzzy_two_pass_sorts_and_indexes() {
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
            assert!(!h.indices.is_empty(), "top-N 应带高亮下标");
        }
        for w in hits.windows(2) {
            assert!(w[0].score >= w[1].score || w[0].score == w[1].score);
        }
    }

    #[test]
    fn fuzzy_limit_truncates() {
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
        assert!(limited.iter().all(|h| !h.indices.is_empty()));
    }

    #[test]
    fn prefer_prefix_fixes_issue_92() {
        let items = vec![
            "lsp_code_lens".to_string(),
            "code_lens".to_string(),
            "lens_factory".to_string(),
        ];
        let hits = fuzzy_filter("lens".into(), items.clone(), FuzzyConfig::default(), None);
        let name = |h: &FuzzyHit| items[h.index as usize].as_str();
        assert_eq!(name(&hits[0]), "lens_factory");
        let score_of = |n: &str| hits.iter().find(|h| name(h) == n).map(|h| h.score).unwrap();
        assert_eq!(score_of("lsp_code_lens"), score_of("code_lens"));
    }

    #[test]
    fn ignore_case_per_query() {
        // case-sensitive：rust 不该命中 RUST。
        let cs = FuzzyConfig {
            ignore_case: false,
            ..FuzzyConfig::default()
        };
        assert!(fuzzy_match("rust".into(), "RUST".into(), cs).is_none());
        assert!(fuzzy_match("rust".into(), "RUST".into(), cfg()).is_some());
    }

    // ── 简单 / 子序列模式 ──

    #[test]
    fn substring_mode() {
        let items = vec![
            "Super Gems 1000".to_string(),
            "Dragon Gem".to_string(),
            "Fortune Coin".to_string(),
        ];
        let hits = fuzzy_filter("gem".into(), items.clone(), cfg_mode(MatchMode::Substring), None);
        assert_eq!(hits.len(), 2);
        for h in &hits {
            let runes: Vec<char> = items[h.index as usize].chars().collect();
            let matched: String = h.indices.iter().map(|&i| runes[i as usize]).collect();
            assert_eq!(matched.to_lowercase(), "gem");
        }
    }

    #[test]
    fn prefix_mode() {
        let items = vec![
            "Super Gems".to_string(),
            "supersonic".to_string(),
            "A Super Thing".to_string(),
        ];
        let hits = fuzzy_filter("super".into(), items, cfg_mode(MatchMode::Prefix), None);
        assert_eq!(hits.len(), 2);
        assert_eq!(hits[0].index, 0);
        assert_eq!(hits[1].index, 1);
    }

    #[test]
    fn word_mode_full_equality() {
        let items = vec![
            "gem".to_string(),
            "GEM".to_string(),
            "gems".to_string(),
            "a gem".to_string(),
        ];
        let hits = fuzzy_filter("gem".into(), items.clone(), cfg_mode(MatchMode::Word), None);
        assert_eq!(hits.len(), 2);
        assert!(hits.iter().all(|h| items[h.index as usize].to_lowercase() == "gem"));
    }

    #[test]
    fn simple_mode_original_order_and_limit() {
        let items = vec![
            "ax".to_string(),
            "bx".to_string(),
            "ay".to_string(),
            "az".to_string(),
        ];
        let hits = fuzzy_filter("a".into(), items, cfg_mode(MatchMode::Substring), Some(2));
        assert_eq!(hits.len(), 2);
        assert_eq!(hits[0].index, 0);
        assert_eq!(hits[1].index, 2);
    }

    #[test]
    fn simple_case_sensitive_per_query() {
        let items = vec!["gem".to_string(), "GEM".to_string()];
        let cs = FuzzyConfig {
            ignore_case: false,
            mode: MatchMode::Word,
            ..FuzzyConfig::default()
        };
        let hits = fuzzy_filter("gem".into(), items, cs, None);
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].index, 0);
    }

    // ── 缓存语料 ──

    #[test]
    fn corpus_matches_stateless_all_modes() {
        let items = vec![
            "Super Gems".to_string(),
            "Dragon Gem".to_string(),
            "Fortune".to_string(),
        ];
        for ici in [false, true] {
            let corpus = FuzzyCorpus::new(items.clone(), ici);
            for mode in [
                MatchMode::Fuzzy,
                MatchMode::Substring,
                MatchMode::Prefix,
                MatchMode::Word,
            ] {
                let cached = corpus.filter("gem".into(), cfg_mode(mode), None);
                let stateless = fuzzy_filter("gem".into(), items.clone(), cfg_mode(mode), None);
                assert_eq!(cached.len(), stateless.len(), "ici={ici} mode={mode:?}");
                for (a, b) in cached.iter().zip(stateless.iter()) {
                    assert_eq!(a.index, b.index, "ici={ici} mode={mode:?}");
                    assert_eq!(a.indices, b.indices, "ici={ici} mode={mode:?}");
                }
            }
        }
    }

    #[test]
    fn corpus_free_rehydrate() {
        let items = vec![
            "service42".to_string(),
            "widget7".to_string(),
            "controller9".to_string(),
        ];
        let mut corpus = FuzzyCorpus::new(items.clone(), true);
        let before = corpus.filter("srvc".into(), cfg(), None);
        corpus.free();
        assert!(!corpus.is_hydrated());
        let freed = corpus.filter("srvc".into(), cfg(), None);
        assert_eq!(before.len(), freed.len());
        corpus.rehydrate();
        assert!(corpus.is_hydrated());
        // 折叠副本应随 rehydrate 恢复（keep_folded=true）。
        let icase = corpus.filter("GEM".into(), cfg_mode(MatchMode::Substring), None);
        let _ = icase; // 不 panic 即可
    }

    #[test]
    fn async_matches_sync() {
        let items = vec!["service42".to_string(), "widget7".to_string()];
        let a = fuzzy_filter_async("srvc".into(), items.clone(), cfg(), None);
        let b = fuzzy_filter("srvc".into(), items, cfg(), None);
        assert_eq!(a.len(), b.len());
    }

    #[test]
    fn empty_query_fuzzy_matches_all_zero_score() {
        let items = vec!["a".to_string(), "b".to_string(), "c".to_string()];
        let hits = fuzzy_filter("".into(), items.clone(), cfg(), None);
        assert_eq!(hits.len(), items.len());
        assert!(hits.iter().all(|h| h.score == 0));
    }
}
