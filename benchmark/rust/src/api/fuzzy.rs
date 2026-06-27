//! 基于 `nucleo-matcher` 的模糊匹配 API，通过 flutter_rust_bridge 暴露给 Dart。
//!
//! 设计说明：使用底层同步的 `Pattern`/`Matcher`（而非高层带后台线程的 `nucleo` crate），
//! 一次性调用即可拿到分数与命中字符下标，适合跨 FFI 桥接。
//!
//! 匹配模式（[MatchMode]）：
//! - `Fuzzy`（默认）：nucleo 子序列模糊 + 打分 + 排序。**两趟**：先 score 全扫并排序，
//!   再只对返回的 top-N 回溯高亮下标，避免给所有命中都白算/分配下标。
//! - `Substring`/`Prefix`/`Word`（仅 full 版本，需 feature = "advanced_modes"）：
//!   字面（包含/前缀/整串相等），原序、不排序、命中即截断。
//!   Lite 版本（--no-default-features）忽略这三种模式，退化为 Fuzzy。
//!
//! 大小写：`ignore_case` 是**按查询**的参数。简单模式/子序列在「原样」或「折叠（小写）」字符串上匹配——
//! 默认在原样上：`ignore_case=false` 直接比（快），`ignore_case=true` 则把候选临时折叠后比（慢，少数路径）；
//! `ignore_case=true` 的简单模式首次查询惰性构建一份折叠（小写）副本并缓存，之后复用。
//! Fuzzy 的大小写由 nucleo 在匹配时折叠，无需折叠副本。
//! （注：极少数大小写折叠会改变字符数的 Unicode 字符，折叠路径下高亮下标可能与原串轻微错位；ASCII 无此问题。）

use flutter_rust_bridge::frb;
use nucleo_matcher::pattern::{CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32Str, Utf32String};

/// 匹配模式。
///
/// Lite 版本（--no-default-features）仅有效地支持 `Fuzzy`；传入其他变体退化为 Fuzzy。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MatchMode {
    /// nucleo 子序列模糊匹配 + 打分 + 排序（默认；lite/full 均支持）。
    Fuzzy,
    /// 精确子串：`haystack` 包含 `query`（仅 full）。
    Substring,
    /// 前缀：`haystack` 以 `query` 开头（仅 full）。
    Prefix,
    /// 全词：`haystack` 与 `query` 完全相等（仅 full）。
    Word,
}

/// 模糊匹配配置。`ignore_case` 按查询传，其余仅 `Fuzzy` 相关。
///
/// Lite 版本中 `mode`/`parallel`/`incremental` 字段存在但被忽略（始终用 Fuzzy 单线程非增量）。
pub struct FuzzyConfig {
    /// 忽略大小写。
    pub ignore_case: bool,
    /// Unicode 归一化（仅 `Fuzzy` 生效；简单模式忽略）。
    pub normalize: bool,
    /// 前缀优先（仅 `Fuzzy` 的排序生效）。
    pub prefer_prefix: bool,
    /// 匹配模式（lite 版本中 non-Fuzzy 退化为 Fuzzy）。
    pub mode: MatchMode,
    /// 是否允许多核并行（仅 full 版本 + `Fuzzy` 搜索；lite 中忽略）。
    pub parallel: bool,
    /// 是否启用增量搜索缓存（仅 full 版本 + `FuzzyCorpus` + `Fuzzy` 模式生效；lite 中忽略）。
    pub incremental: bool,
}

impl Default for FuzzyConfig {
    fn default() -> Self {
        Self {
            ignore_case: true,
            normalize: true,
            prefer_prefix: true,
            mode: MatchMode::Fuzzy,
            parallel: true,
            incremental: false,
        }
    }
}

// ───────────────────────── parallel feature: 多核常量与辅助 ─────────────────────────

/// 并行阈值：候选数 >= 此值才考虑多核（小数据并行开销不划算）。仅原生用(wasm 无多核)。
#[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
const PARALLEL_THRESHOLD: usize = 20_000;

#[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
fn num_threads(len: usize) -> usize {
    let cores = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    // 留 2 个核给 UI/raster/系统;cores<=3 时结果<=1 → should_parallel/build 据此退化为串行。
    cores.saturating_sub(2).min(8).min(len.max(1))
}

#[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
fn should_parallel(cfg: &FuzzyConfig, len: usize) -> bool {
    cfg.parallel && len >= PARALLEL_THRESHOLD && num_threads(len) > 1
}

// ───────────────────────── advanced_modes feature: 增量复用上限 ─────────────────────────

/// 增量复用上限：**感知是否并行**。
/// - 并行时：全扫成本 ~ len/线程数,复用子集须小于它才划算 → 取 `len/线程数`(下限 `PARALLEL_THRESHOLD`)。
/// - 串行(含 wasm / parallel 关 / 低核):全扫成本 ~ len,任何更小子集都值得复用 → 无上限。
#[cfg(feature = "advanced_modes")]
fn incr_cap(cfg: &FuzzyConfig, len: usize) -> usize {
    let _ = (cfg, len);
    #[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
    {
        if should_parallel(cfg, len) {
            return (len / num_threads(len).max(1)).max(PARALLEL_THRESHOLD);
        }
    }
    usize::MAX
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

// ───────────────────────── advanced_modes feature: 非 Fuzzy 匹配 ─────────────────────────

/// 在「已按 ignore_case 折好大小写」的 `q`/`hay` 上做非 Fuzzy 匹配。
/// 命中返回字符（rune）下标，不命中返回 `None`。
#[cfg(feature = "advanced_modes")]
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
#[cfg(feature = "advanced_modes")]
fn nonfuzzy_filter(source: &[String], q: &str, mode: MatchMode, limit: Option<u32>) -> Vec<FuzzyHit> {
    let lim = limit.map(|l| l as usize);
    if lim == Some(0) {
        return Vec::new(); // limit=0 应返回空(否则下面"先 push 再判 >="会多返回 1 条)
    }
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

/// 第一趟扫描（串行）：对每条候选打分，返回 `Scored`。
fn scan_haystacks_serial(haystacks: &[Utf32String], query: &str, cfg: &FuzzyConfig) -> Vec<Scored> {
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
    scored
}

/// 第一趟扫描（多核）：分块、每线程独立 `Matcher`/`Pattern`，结果按全局下标合并。
/// 归并后由 `rank_scored` 用同一比较器排序，结果与串行**完全一致**（确定性）。
#[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
fn scan_haystacks_parallel(
    haystacks: &[Utf32String],
    query: &str,
    cfg: &FuzzyConfig,
) -> Vec<Scored> {
    let nthreads = num_threads(haystacks.len());
    let chunk = haystacks.len().div_ceil(nthreads).max(1); // .max(1): 防 chunks(0) panic
    std::thread::scope(|s| {
        let handles: Vec<_> = haystacks
            .chunks(chunk)
            .enumerate()
            .map(|(ci, slice)| {
                let base = ci * chunk;
                s.spawn(move || {
                    let mut matcher = make_matcher();
                    let pattern = make_pattern(query, cfg);
                    let mut buf = Vec::new();
                    let mut out = Vec::new();
                    for (j, hay) in slice.iter().enumerate() {
                        if let Some((score, is_prefix)) = scan_one(
                            &pattern,
                            &mut matcher,
                            hay.slice(..),
                            cfg.prefer_prefix,
                            &mut buf,
                        ) {
                            out.push(Scored {
                                index: (base + j) as u32,
                                score,
                                is_prefix,
                            });
                        }
                    }
                    out
                })
            })
            .collect();
        let mut all = Vec::new();
        for h in handles {
            all.extend(h.join().unwrap());
        }
        all
    })
}

/// 第一趟扫描分流：大数据 + 开启并行 → 多核；否则串行。wasm 恒走串行。
fn scan_haystacks(haystacks: &[Utf32String], query: &str, cfg: &FuzzyConfig) -> Vec<Scored> {
    #[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
    if should_parallel(cfg, haystacks.len()) {
        return scan_haystacks_parallel(haystacks, query, cfg);
    }
    scan_haystacks_serial(haystacks, query, cfg)
}

/// 增量复用：只扫上次命中集 `subset` 中的候选（串行；子集通常已较小）。
#[cfg(feature = "advanced_modes")]
fn scan_subset(
    haystacks: &[Utf32String],
    subset: &[u32],
    query: &str,
    cfg: &FuzzyConfig,
) -> Vec<Scored> {
    let mut matcher = make_matcher();
    let pattern = make_pattern(query, cfg);
    let mut buf = Vec::new();
    let mut out = Vec::new();
    for &idx in subset {
        let i = idx as usize;
        if i >= haystacks.len() {
            continue; // 理论上 mutation 已清缓存，这里再兜一层底
        }
        if let Some((score, is_prefix)) =
            scan_one(&pattern, &mut matcher, haystacks[i].slice(..), cfg.prefer_prefix, &mut buf)
        {
            out.push(Scored {
                index: idx,
                score,
                is_prefix,
            });
        }
    }
    out
}

/// 第二趟：排序 + 截断 + 只对 top-N 回溯高亮下标（量小，串行）。
fn finish_fuzzy(
    haystacks: &[Utf32String],
    scored: Vec<Scored>,
    query: &str,
    cfg: &FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    let top = rank_scored(scored, cfg.prefer_prefix, limit);
    let mut matcher = make_matcher();
    let pattern = make_pattern(query, cfg);
    let mut buf = Vec::new();
    top.into_iter()
        .map(|s| {
            let indices =
                indices_one(&pattern, &mut matcher, haystacks[s.index as usize].slice(..), &mut buf);
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

/// 对单个字符串打分；不匹配返回 `None`。
/// Lite 版本中 `config.mode` 非 Fuzzy 时退化为 Fuzzy（返回 Fuzzy 分数而非 `Some(0)`）。
#[frb(sync)]
pub fn fuzzy_match(query: String, haystack: String, config: FuzzyConfig) -> Option<u32> {
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
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

/// 折叠一对字符串（用于单条匹配的 ignore_case；仅 advanced_modes 使用）。
#[cfg(feature = "advanced_modes")]
fn fold_pair(q: &str, hay: &str, ignore_case: bool) -> (String, String) {
    if ignore_case {
        (q.to_lowercase(), hay.to_lowercase())
    } else {
        (q.to_string(), hay.to_string())
    }
}

// ───────────────────────── 无状态列表过滤 ─────────────────────────

/// 对列表做筛选。`Fuzzy` 按分数降序，其余按原序（命中满 `limit` 即停）。
/// Lite 版本中非 Fuzzy 的 `config.mode` 退化为 Fuzzy。
#[frb(sync)]
pub fn fuzzy_filter(
    query: String,
    items: Vec<String>,
    config: FuzzyConfig,
    limit: Option<u32>,
) -> Vec<FuzzyHit> {
    #[cfg(feature = "advanced_modes")]
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
/// 增量搜索缓存（仅 advanced_modes）：上次 Fuzzy 查询及其**全部命中下标**（非 top-N）。
///
/// 复用键 = `query` + `ignore_case` + `normalize`：因为 Fuzzy 的**命中集**只取决于这三者
/// （`prefer_prefix`/`mode` 只影响排序或根本不进此路径，不影响是否命中）。若将来引入影响
/// "是否命中"（而非排序）的配置，**必须**把它加进复用键，否则会静默复用错误的命中集。
#[cfg(feature = "advanced_modes")]
struct IncrCache {
    query: String,
    ignore_case: bool,
    normalize: bool,
    hits: Vec<u32>,
}

#[frb(opaque)]
pub struct FuzzyCorpus {
    /// 源字符串（原样）。
    items: Vec<String>,
    /// 预转换的 Utf32 索引（`Fuzzy` 用）；`free` 后为 `None`。
    haystacks: Option<Vec<Utf32String>>,
    /// 增量搜索缓存（仅 advanced_modes；`Mutex` 因 `filter` 是 `&self` 且异步在 worker 线程跑）。
    /// 任何增删改/free 都会清空。
    #[cfg(feature = "advanced_modes")]
    incr: std::sync::Mutex<Option<IncrCache>>,
    /// 惰性折叠（小写）副本缓存（仅 advanced_modes）：用 `ignore_case=true` 的简单模式时首次构建并缓存
    /// （`Arc` 便于短持锁克隆），之后复用，避免每查都全量降小写。任何增删改/free 失效。
    #[cfg(feature = "advanced_modes")]
    folded_cache: std::sync::Mutex<Option<std::sync::Arc<Vec<String>>>>,
}

fn build_haystacks(items: &[String]) -> Vec<Utf32String> {
    // 大数据自动多核转换（一次性、纯提速、保序）。
    #[cfg(all(not(target_arch = "wasm32"), feature = "parallel"))]
    if items.len() >= PARALLEL_THRESHOLD && num_threads(items.len()) > 1 {
        let nthreads = num_threads(items.len());
        let chunk = items.len().div_ceil(nthreads).max(1); // .max(1): 防 chunks(0) panic
        return std::thread::scope(|s| {
            let handles: Vec<_> = items
                .chunks(chunk)
                .map(|slice| {
                    s.spawn(move || {
                        slice
                            .iter()
                            .map(|x| Utf32String::from(x.as_str()))
                            .collect::<Vec<_>>()
                    })
                })
                .collect();
            let mut out = Vec::with_capacity(items.len());
            for h in handles {
                out.extend(h.join().unwrap());
            }
            out
        });
    }
    items.iter().map(|s| Utf32String::from(s.as_str())).collect()
}

#[cfg(feature = "advanced_modes")]
fn build_folded(items: &[String]) -> Vec<String> {
    items.iter().map(|s| s.to_lowercase()).collect()
}

/// 异步构建语料：在 frb worker 线程执行 Utf32 转换/折叠（大数据时较重），**不阻塞 Dart UI 线程**。
/// 与 [FuzzyCorpus::new] 等价，只是不标 `#[frb(sync)]`（Dart 侧返回 `Future<FuzzyCorpus>`）。
/// 注：候选列表的跨 FFI 编组同样在 worker 线程；但 Dart 侧的投影（stringOf）仍在调用线程算。
pub fn fuzzy_corpus_new_async(items: Vec<String>) -> FuzzyCorpus {
    FuzzyCorpus::build(items)
}

impl FuzzyCorpus {
    /// 实际构建逻辑（sync `new` 与 async `fuzzy_corpus_new_async` 共用）。
    fn build(items: Vec<String>) -> FuzzyCorpus {
        let haystacks = Some(build_haystacks(&items));
        FuzzyCorpus {
            items,
            haystacks,
            #[cfg(feature = "advanced_modes")]
            incr: std::sync::Mutex::new(None),
            #[cfg(feature = "advanced_modes")]
            folded_cache: std::sync::Mutex::new(None),
        }
    }

    /// 清空易失缓存（增量 + 惰性折叠；任何结构性变更/free 后调用）。
    /// Lite 版本中为空操作（无缓存字段）。
    fn clear_caches(&self) {
        #[cfg(feature = "advanced_modes")]
        {
            if let Ok(mut g) = self.incr.lock() {
                *g = None;
            }
            if let Ok(mut g) = self.folded_cache.lock() {
                *g = None;
            }
        }
    }

    /// 取惰性折叠副本（无则构建并缓存）。短持锁:命中直接 clone `Arc`,未命中构建后存。
    #[cfg(feature = "advanced_modes")]
    fn lazy_folded(&self) -> std::sync::Arc<Vec<String>> {
        let mut g = self.folded_cache.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(a) = g.as_ref() {
            return a.clone();
        }
        let a = std::sync::Arc::new(build_folded(&self.items));
        *g = Some(a.clone());
        a
    }

    /// 用一组候选项构建语料。
    #[frb(sync)]
    pub fn new(items: Vec<String>) -> FuzzyCorpus {
        Self::build(items)
    }

    /// 末尾追加（不重建）。同步维护 Utf32 索引。
    #[frb(sync)]
    pub fn add(&mut self, items: Vec<String>) {
        if let Some(hs) = self.haystacks.as_mut() {
            hs.extend(items.iter().map(|s| Utf32String::from(s.as_str())));
        }
        self.items.extend(items);
        self.clear_caches();
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
        self.items[i] = item;
        self.clear_caches();
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
            }
        }
        self.clear_caches();
    }

    /// 清空全部候选（保留实例与驻留状态）。
    #[frb(sync)]
    pub fn clear(&mut self) {
        self.items.clear();
        if let Some(hs) = self.haystacks.as_mut() {
            hs.clear();
        }
        self.clear_caches();
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

    /// 释放占内存大头的 Utf32 索引，保留源字符串便于 `rehydrate`。幂等。
    #[frb(sync)]
    pub fn free(&mut self) {
        self.haystacks = None;
        self.clear_caches();
    }

    /// 从源字符串重建 Utf32 索引。无跨 FFI 编组开销。幂等。
    #[frb(sync)]
    pub fn rehydrate(&mut self) {
        if self.haystacks.is_none() {
            self.haystacks = Some(build_haystacks(&self.items));
        }
    }

    /// 非 Fuzzy 过滤：`ignore_case` 时用惰性折叠副本，否则用原样源；做字面/子序列匹配。
    #[cfg(feature = "advanced_modes")]
    fn filter_nonfuzzy(&self, query: &str, cfg: &FuzzyConfig, limit: Option<u32>) -> Vec<FuzzyHit> {
        let q = if cfg.ignore_case {
            query.to_lowercase()
        } else {
            query.to_string()
        };
        let lazy_arc;
        let source: &[String] = if cfg.ignore_case {
            // 惰性折叠缓存：首查构建一次、后续复用，避免每查全量降小写。
            lazy_arc = self.lazy_folded();
            lazy_arc.as_slice()
        } else {
            &self.items
        };
        nonfuzzy_filter(source, &q, cfg.mode, limit)
    }

    /// 增量缓存路径（仅 advanced_modes；从 `fuzzy_filter_hydrated` 分离出来保持干净）。
    #[cfg(feature = "advanced_modes")]
    fn fuzzy_filter_incremental(
        &self,
        haystacks: &[Utf32String],
        query: &str,
        cfg: &FuzzyConfig,
        limit: Option<u32>,
    ) -> Vec<FuzzyHit> {
        let cap = incr_cap(cfg, haystacks.len());
        // 读缓存：能复用则取上次命中集（短暂持锁、克隆后即释放，避免阻塞并发 filterAsync）。
        let subset: Option<Vec<u32>> = {
            let g = self.incr.lock().unwrap_or_else(|e| e.into_inner());
            g.as_ref().and_then(|c| {
                let reusable = c.ignore_case == cfg.ignore_case
                    && c.normalize == cfg.normalize
                    && !c.query.is_empty()
                    && c.hits.len() <= cap // 命中集过大不复用(并行下串行子集扫 < 并行全扫)
                    && query.starts_with(&c.query);
                reusable.then(|| c.hits.clone())
            })
        };
        let scored = match subset {
            Some(hits) => scan_subset(haystacks, &hits, query, cfg),
            None => scan_haystacks(haystacks, query, cfg), // 可并行
        };
        // 写缓存：只缓存"足够小"的命中集。
        {
            let mut g = self.incr.lock().unwrap_or_else(|e| e.into_inner());
            *g = if scored.len() <= cap {
                Some(IncrCache {
                    query: query.to_string(),
                    ignore_case: cfg.ignore_case,
                    normalize: cfg.normalize,
                    hits: scored.iter().map(|s| s.index).collect(),
                })
            } else {
                None
            };
        }
        finish_fuzzy(haystacks, scored, query, cfg, limit)
    }

    /// Fuzzy 过滤（已驻留 Utf32）。
    /// advanced_modes：支持增量复用（仅 `incremental=true` 且本次是上次的追加扩展）。
    fn fuzzy_filter_hydrated(
        &self,
        haystacks: &[Utf32String],
        query: &str,
        cfg: &FuzzyConfig,
        limit: Option<u32>,
    ) -> Vec<FuzzyHit> {
        #[cfg(feature = "advanced_modes")]
        if cfg.incremental {
            return self.fuzzy_filter_incremental(haystacks, query, cfg, limit);
        }
        let scored = scan_haystacks(haystacks, query, cfg);
        finish_fuzzy(haystacks, scored, query, cfg, limit)
    }

    /// 过滤已缓存的语料。`Fuzzy` 按分数降序；其余按原序（仅 advanced_modes）。
    #[frb(sync)]
    pub fn filter(&self, query: String, config: FuzzyConfig, limit: Option<u32>) -> Vec<FuzzyHit> {
        #[cfg(feature = "advanced_modes")]
        if config.mode != MatchMode::Fuzzy {
            return self.filter_nonfuzzy(&query, &config, limit);
        }
        match &self.haystacks {
            Some(haystacks) => self.fuzzy_filter_hydrated(haystacks, &query, &config, limit),
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

    #[cfg(feature = "advanced_modes")]
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

    // ── 简单 / 子序列模式（仅 advanced_modes） ──

    #[test]
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
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
    #[cfg(feature = "advanced_modes")]
    fn corpus_matches_stateless_all_modes() {
        let items = vec![
            "Super Gems".to_string(),
            "Dragon Gem".to_string(),
            "Fortune".to_string(),
        ];
        let corpus = FuzzyCorpus::new(items.clone());
        for mode in [
            MatchMode::Fuzzy,
            MatchMode::Substring,
            MatchMode::Prefix,
            MatchMode::Word,
        ] {
            let cached = corpus.filter("gem".into(), cfg_mode(mode), None);
            let stateless = fuzzy_filter("gem".into(), items.clone(), cfg_mode(mode), None);
            assert_eq!(cached.len(), stateless.len(), "mode={mode:?}");
            for (a, b) in cached.iter().zip(stateless.iter()) {
                assert_eq!(a.index, b.index, "mode={mode:?}");
                assert_eq!(a.indices, b.indices, "mode={mode:?}");
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
        let mut corpus = FuzzyCorpus::new(items.clone());
        let before = corpus.filter("srvc".into(), cfg(), None);
        corpus.free();
        assert!(!corpus.is_hydrated());
        let freed = corpus.filter("srvc".into(), cfg(), None);
        assert_eq!(before.len(), freed.len());
        corpus.rehydrate();
        assert!(corpus.is_hydrated());
        // 折叠副本（advanced_modes）或基础 fuzzy 均不 panic 即可。
        #[cfg(feature = "advanced_modes")]
        {
            let icase = corpus.filter("GEM".into(), cfg_mode(MatchMode::Substring), None);
            let _ = icase;
        }
    }

    #[test]
    fn async_matches_sync() {
        let items = vec!["service42".to_string(), "widget7".to_string()];
        let a = fuzzy_filter_async("srvc".into(), items.clone(), cfg(), None);
        let b = fuzzy_filter("srvc".into(), items, cfg(), None);
        assert_eq!(a.len(), b.len());
    }

    #[test]
    #[cfg(feature = "parallel")]
    fn parallel_matches_serial_large() {
        // 构造 > PARALLEL_THRESHOLD 条触发多核,比对 parallel/serial 结果完全一致(确定性)。
        let items: Vec<String> =
            (0..25_000).map(|i| format!("item gem {i} dragon")).collect();
        let corpus = FuzzyCorpus::new(items);
        let par = FuzzyConfig {
            parallel: true,
            ..FuzzyConfig::default()
        };
        let ser = FuzzyConfig {
            parallel: false,
            ..FuzzyConfig::default()
        };
        let a = corpus.filter("gem".into(), par, Some(50));
        let b = corpus.filter("gem".into(), ser, Some(50));
        assert_eq!(a.len(), 50);
        assert_eq!(a.len(), b.len());
        for (x, y) in a.iter().zip(b.iter()) {
            assert_eq!(x.index, y.index);
            assert_eq!(x.score, y.score);
            assert_eq!(x.indices, y.indices);
        }
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn incremental_matches_nonincremental() {
        let items: Vec<String> = (0..3000).map(|i| format!("dragon gem {i}")).collect();
        let corpus = FuzzyCorpus::new(items);
        let incr = || FuzzyConfig {
            incremental: true,
            ..FuzzyConfig::default()
        };
        // 逐字输入,走增量缓存(每次都是上次的追加扩展)。
        corpus.filter("dg".into(), incr(), Some(20));
        corpus.filter("dge".into(), incr(), Some(20));
        let inc = corpus.filter("dgem".into(), incr(), Some(20));
        // 与非增量(冷查)对比应完全一致。
        let cold = corpus.filter("dgem".into(), FuzzyConfig::default(), Some(20));
        assert_eq!(inc.len(), cold.len());
        for (a, b) in inc.iter().zip(cold.iter()) {
            assert_eq!(a.index, b.index);
            assert_eq!(a.score, b.score);
        }
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn incremental_invalidated_on_mutation() {
        let mut corpus =
            FuzzyCorpus::new(vec!["dragon".to_string(), "drag".to_string()]);
        let incr = || FuzzyConfig {
            incremental: true,
            ..FuzzyConfig::default()
        };
        corpus.filter("dr".into(), incr(), None); // 填充缓存
        corpus.add(vec!["draco".to_string()]); // 应清缓存
        // 若缓存未清,"dra"(starts_with "dr")会只在旧命中集 {0,1} 里扫,漏掉新加的 draco(下标2)。
        let r = corpus.filter("dra".into(), incr(), None);
        assert!(r.iter().any(|h| h.index == 2), "mutation 后增量缓存应失效,能搜到新加项");
    }

    #[test]
    fn limit_zero_returns_empty_all_modes() {
        let items = vec!["alpha".to_string(), "alto".to_string(), "beta".to_string()];
        // Fuzzy with limit=0 always returns empty (always available).
        let r = fuzzy_filter("al".into(), items.clone(), cfg(), Some(0));
        assert!(r.is_empty(), "limit=0 应返回空, mode=Fuzzy");
        // Non-fuzzy modes only available in full (advanced_modes feature).
        #[cfg(feature = "advanced_modes")]
        for mode in [MatchMode::Substring, MatchMode::Prefix, MatchMode::Word] {
            let r = fuzzy_filter("al".into(), items.clone(), cfg_mode(mode), Some(0));
            assert!(r.is_empty(), "limit=0 应返回空, mode={mode:?}");
        }
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn nonfuzzy_empty_query() {
        let items = vec!["a".to_string(), "".to_string(), "bc".to_string()];
        // Substring/Prefix 空查询恒真 -> 全中。
        assert_eq!(
            fuzzy_filter("".into(), items.clone(), cfg_mode(MatchMode::Substring), None).len(),
            3
        );
        assert_eq!(
            fuzzy_filter("".into(), items.clone(), cfg_mode(MatchMode::Prefix), None).len(),
            3
        );
        // Word 空查询只命中空串。
        let w = fuzzy_filter("".into(), items, cfg_mode(MatchMode::Word), None);
        assert_eq!(w.len(), 1);
        assert_eq!(w[0].index, 1);
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn incremental_backspace_and_rewrite() {
        let items: Vec<String> = (0..500).map(|i| format!("dragon gem {i}")).collect();
        let corpus = FuzzyCorpus::new(items);
        let incr = || FuzzyConfig {
            incremental: true,
            ..FuzzyConfig::default()
        };
        corpus.filter("dg".into(), incr(), Some(20)); // 填缓存
        corpus.filter("dgem".into(), incr(), Some(20)); // 扩展
        // 退格(新查询是旧查询前缀,非扩展)-> 应回退全扫。
        let back = corpus.filter("dg".into(), incr(), Some(20));
        let cold = corpus.filter("dg".into(), FuzzyConfig::default(), Some(20));
        assert_eq!(back.len(), cold.len());
        for (a, b) in back.iter().zip(cold.iter()) {
            assert_eq!(a.index, b.index);
        }
        // 完全改写。
        let rw = corpus.filter("xyz".into(), incr(), Some(20));
        let cold_rw = corpus.filter("xyz".into(), FuzzyConfig::default(), Some(20));
        assert_eq!(rw.len(), cold_rw.len());
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn incremental_invalidated_on_ignore_case_change() {
        // ci=false "a" 只命中小写 'a'(idx 1,2);切到 ci=true "ab" 应能命中 "AB"(idx0)。
        // 若缺 ignore_case 守卫而复用旧命中集 {1,2},会漏掉 idx0。
        let items = vec!["AB".to_string(), "ab".to_string(), "Zab".to_string()];
        let corpus = FuzzyCorpus::new(items);
        let ci_off = FuzzyConfig {
            incremental: true,
            ignore_case: false,
            ..FuzzyConfig::default()
        };
        let ci_on = FuzzyConfig {
            incremental: true,
            ignore_case: true,
            ..FuzzyConfig::default()
        };
        corpus.filter("a".into(), ci_off, None); // 填缓存(区分大小写)
        let r = corpus.filter("ab".into(), ci_on, None); // 忽略大小写,缓存应失效
        assert!(
            r.iter().any(|h| h.index == 0),
            "ignore_case 变化必须使增量缓存失效,否则漏掉 AB"
        );
    }

    #[test]
    #[cfg(feature = "advanced_modes")]
    fn unicode_folding_substring_no_panic() {
        // İ(U+0130)折叠成两个 char,折叠改变字符数。验证 substring 不 panic、下标可用。
        // (已知限制:此类字符折叠路径下高亮下标可能与原串轻微错位,见模块头注释。)
        let items = vec!["İstanbul".to_string(), "info".to_string()];
        let hits = fuzzy_filter("i".into(), items, cfg_mode(MatchMode::Substring), None);
        assert!(!hits.is_empty()); // 至少 "info" 命中;不 panic 即达标
    }

    #[test]
    fn empty_query_fuzzy_matches_all_zero_score() {
        let items = vec!["a".to_string(), "b".to_string(), "c".to_string()];
        let hits = fuzzy_filter("".into(), items.clone(), cfg(), None);
        assert_eq!(hits.len(), items.len());
        assert!(hits.iter().all(|h| h.score == 0));
    }
}
