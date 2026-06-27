# Scoring Mode: Three-Level Scoring + Dart Enum Override

**Date:** 2026-06-27  
**Branch:** main  
**Status:** Approved

---

## Goal

Make the C matcher faster in all scenarios by introducing three explicit scoring modes:

| Mode | Algorithm | Use case |
|------|-----------|----------|
| `FAST` (default) | 2-row rolling DP + simplified 4-bonus model | User-facing fuzzy search |
| `OFF` | Prefilter only, score=0, original order | Programmatic exact/ID matching |
| `NUCLEO` | Current full-matrix DP, nucleo-compatible | When ranking precision matters |

The mode is set as a corpus-level default (constructor) and overridable per filter call (method parameter), matching the existing `caseMatching`/`normalization` pattern.

---

## Background

`perf/PERF.md` shows the C library is still slower than Rust (nucleo) in single-threaded scenarios:

| Scenario | C vs Rust |
|----------|-----------|
| fuzzy, 1 thread, index-on | 0.96× |
| 50K corpus, 1 thread | 0.91× |
| 1M corpus, 1 thread | 0.92× |

Root cause: pass 1 (score-only) writes a full `needle_len × W` matrix even though backtracking is never used in that pass. nucleo uses a space-optimized 2-row rolling DP for its score pass, which has lower cache pressure.

---

## Design

### 1. C Public API — `include/ffz.h`

Add `ffz_scoring_mode` enum and a field to `ffz_config`:

```c
typedef enum {
    FFZ_SCORE_FAST   = 0,  // default: 2-row rolling DP, simplified bonuses
    FFZ_SCORE_OFF    = 1,  // prefilter only; score=0, original insertion order
    FFZ_SCORE_NUCLEO = 2,  // nucleo-compatible full-matrix DP (current behaviour)
} ffz_scoring_mode;

typedef struct {
    // ... existing fields unchanged ...
    ffz_scoring_mode scoring_mode;  // added as last field
} ffz_config;
```

`ffz_config_default()` and `ffz_config_match_paths()` set `scoring_mode = FFZ_SCORE_FAST`.

`ffz_corpus_filter` gains a `ffz_scoring_mode scoring` parameter. The passed value is always the resolved effective mode (corpus default merged with per-call override in the Dart layer).

```c
// include/ffz_corpus.h — updated signature
void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                       ffz_case_matching cm, ffz_normalization nm,
                       ffz_mode mode, ffz_parallel par, size_t limit,
                       ffz_scoring_mode scoring,   // new
                       ffz_results *out);
```

### 2. Three Internal Code Paths

#### FAST — new `ffz_fuzzy_rolling()` in `src/ffz_fuzzy.c`

2-row rolling Smith-Waterman DP. Same constants as clang branch:
- `BONUS_BOUNDARY = 8`, `BONUS_CAMEL = 7`, `BONUS_CONSECUTIVE = 4`
- No whitespace/delimiter distinction (clang model, not nucleo model)
- Reuses `m->mgrid` via `ffz_matcher_reserve(m, W, 2)` — 2 rows only
- Returns `int32_t` score; no backtracking capability

```c
// Score-only; out=NULL always. Called in pass 1.
int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle,
                          size_t start, size_t end);
```

Pass 2 (indices for survivors) uses `ffz_calculate_score(m, hay, needle, start, end, out)` — the existing greedy linear scorer, which is fast and correct for highlighting.

#### OFF — `scan_range` bypass in `src/ffz_corpus.c`

- Skip `ffz_matcher_new()` in `scan_job_run` — no DP scratch allocated
- Per item: `ffz_prefilter(...)` only; push with `score = 0` if it passes
- Collector becomes a simple counter (no heap, no sort)
- Multi-thread: each chunk collects first `min(limit, chunk_hits)` in order; chunks merged in thread-index order then truncated to `limit`
- Pass 2: still compute indices for survivors when `highlight=true`

#### NUCLEO — unchanged

`ffz_fuzzy_optimal()` full-matrix DP for both pass 1 and pass 2 backtracking. Existing behaviour preserved exactly.

### 3. FFI Shim — `ffi/ffz_ffi.c`

Two new exported functions; all existing functions preserved unchanged.

```c
// Constructor with scoring default (replaces ffz_ffi_new_cfg)
// scoring: 0=fast, 1=off, 2=nucleo
FFZ_API ffz_corpus *ffz_ffi_new_cfg2(int paths, int prefer_prefix, int scoring);

// Filter with per-call scoring (replaces ffz_ffi_filter_ex)
// scoring: already-resolved effective value from Dart layer
FFZ_API ffz_results *ffz_ffi_filter_ex2(ffz_corpus *c,
                                         const char *q, size_t qn,
                                         int mode, int cm, int nm,
                                         int parallel, int threads,
                                         size_t limit, int scoring);
```

Dart uses tolerant `lookupFunction` for both new symbols; falls back to old functions if the loaded `.so` predates this change (e.g. a cached precompiled binary). Fallback behaviour: `ffz_ffi_new_cfg` → `FFZ_SCORE_FAST` default; `ffz_ffi_filter_ex` → `FFZ_SCORE_FAST`.

### 4. Dart Layer — `lib/ffuzzy.dart`

#### New enum

```dart
/// Scoring model for [FuzzyCorpus] queries.
///
/// Set the corpus-wide default in [FuzzyOptions.scoring] passed to the
/// [FuzzyCorpus] constructor; override per call via each method's `scoring`
/// named parameter.
enum FuzzyScoring {
  /// 2-row rolling DP with simplified bonuses. Faster than [nucleo] in
  /// single-threaded scenarios; good ranking quality for most use cases.
  fast,

  /// No DP. Prefilter only; all hits get score=0 and are returned in
  /// corpus insertion order. Use for programmatic exact/ID matching where
  /// ranking is irrelevant.
  off,

  /// Full-matrix DP, nucleo 0.3.1 compatible. Highest ranking accuracy;
  /// use when precise score ordering is required.
  nucleo,
}
```

#### `FuzzyOptions` — add `scoring` field

```dart
class FuzzyOptions {
  final FuzzyScoring scoring;   // new; default fast
  final FuzzyCase caseMatching;
  final FuzzyNorm normalization;
  final bool parallel;
  final int threads;
  final int limit;
  final bool highlight;

  const FuzzyOptions({
    this.scoring = FuzzyScoring.fast,   // new default (was implicitly nucleo)
    this.caseMatching = FuzzyCase.smart,
    this.normalization = FuzzyNorm.smart,
    this.parallel = false,
    this.threads = 0,
    this.limit = 0,
    this.highlight = true,
  });

  FuzzyOptions copyWith({
    FuzzyScoring? scoring,
    FuzzyCase? caseMatching,
    FuzzyNorm? normalization,
    bool? parallel,
    int? threads,
    int? limit,
    bool? highlight,
  });
}
```

#### `FuzzyCorpus` — constructor uses `ffz_ffi_new_cfg2`

Tolerant lookup: if `ffz_ffi_new_cfg2` is absent, fall back to `ffz_ffi_new_cfg` (scoring default silently becomes FAST via the new C default in `ffz_config_default`).

#### Search methods — add `FuzzyScoring? scoring` parameter

All five methods (`fuzzy`, `substring`, `prefix`, `postfix`, `exact`) and their `…Async` variants gain a `FuzzyScoring? scoring` named parameter. The effective value is resolved before the FFI call:

```dart
FuzzyScoring _eff(FuzzyScoring? override) => override ?? options.scoring;
```

The resolved integer (`.index`) is passed as the final argument to `ffz_ffi_filter_ex2`.

#### Usage examples

```dart
// Corpus default: fast (implicit)
final corpus = FuzzyCorpus<Contact>(contacts, stringOf: (c) => c.name);

// User search — uses corpus default (fast)
corpus.fuzzy('zhangsan', limit: 20);

// Programmatic ID lookup — override to off (no ranking needed)
corpus.exact('user_001', scoring: FuzzyScoring.off);

// Precise ranking for an analytics screen — override to nucleo
corpus.fuzzy('config', limit: 5, scoring: FuzzyScoring.nucleo);
```

---

## Behaviour Invariants

| Property | Guarantee |
|----------|-----------|
| Hit count | Identical across all three modes for the same query + mode (FUZZY/EXACT/…) |
| Score when OFF | Always 0; field present in `ffz_hit` and `FuzzyHit` |
| Order when OFF | Corpus insertion order; `limit` respected |
| Backward compat | Old `.so` + new Dart → FAST mode via fallback |
| New `.so` + old Dart | ffz_ffi_filter_ex called → FAST (new C default) |
| NUCLEO output | Byte-identical to current main behaviour |

---

## Files Changed

| File | Change |
|------|--------|
| `include/ffz.h` | Add `ffz_scoring_mode` enum + field to `ffz_config` |
| `include/ffz_corpus.h` | Add `scoring` param to `ffz_corpus_filter` |
| `src/ffz_fuzzy.c` | Add `ffz_fuzzy_rolling()` |
| `src/ffz_match.c` | Branch on scoring mode in `ffz_match()` |
| `src/ffz_corpus.c` | OFF fast-path in `scan_range` / `scan_job_run`; threading order preservation |
| `src/ffz_chars.c` | Add `ffz_fast_bonus()` helper (simplified 4-bonus lookup) |
| `ffi/ffz_ffi.c` | Add `ffz_ffi_new_cfg2`, `ffz_ffi_filter_ex2` |
| `lib/ffuzzy.dart` | `FuzzyScoring` enum; `FuzzyOptions.scoring`; method `scoring` params |
