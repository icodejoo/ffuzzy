# Scoring Mode (FAST / OFF / NUCLEO) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a three-level scoring mode to the ffuzzy C library and Dart binding so callers can trade ranking quality for speed: FAST (default, 2-row rolling DP), OFF (prefilter only, score=0), or NUCLEO (current full-matrix DP).

**Architecture:** `ffz_scoring_mode` is stored in `ffz_config` as a corpus-level default; each `ffz_corpus_filter` call receives an explicit resolved scoring value (Dart layer merges corpus default with per-call override). Inside the C library, `ffz_match()` dispatches to the appropriate algorithm via a `switch (cfg->scoring_mode)` inside the FUZZY branch; non-FUZZY modes only suppress the score to 0 for OFF. The corpus layer skips `qsort` when scoring is OFF to preserve original insertion order.

**Tech Stack:** C11, POSIX pthreads / Win32 threads, dart:ffi, Dart 3.

## Global Constraints

- All existing tests (`make test`) must pass after every task.
- New C tests use the same no-framework pattern already in `tests/test_ffz.c`: `CHECK(cond, msg)` macro, `g_fail` / `g_total` counters.
- `ffz_scoring_mode` enum values: `FFZ_SCORE_FAST=0`, `FFZ_SCORE_OFF=1`, `FFZ_SCORE_NUCLEO=2` — the integer values are part of the ABI (Dart passes `.index`).
- All existing `ffz_corpus_filter` call sites must be updated to pass `FFZ_SCORE_FAST` as the new `scoring` argument; the function signature change is not optional.
- Do not alter `ffz_fuzzy_optimal()`, `ffz_calculate_score()`, or `ffz_fuzzy_greedy()` — only add new code and branch on the new mode.

---

### Task 1: Add `ffz_scoring_mode` enum + `ffz_config` field + `ffz_fast_bonus` helper

**Files:**
- Modify: `include/ffz.h`
- Modify: `src/ffz_string.c` (updates `ffz_config_default` and `ffz_config_match_paths`)
- Modify: `src/ffz_chars.c` (adds `ffz_fast_bonus`)
- Modify: `src/ffz_internal.h` (declares `ffz_fast_bonus`)

**Interfaces:**
- Produces: `ffz_scoring_mode` enum; `ffz_config.scoring_mode` field; `uint8_t ffz_fast_bonus(ffz_char_class prev, ffz_char_class cls)` in `ffz_chars.c`

- [ ] **Step 1: Add enum and field to `include/ffz.h`**

  Find the block just before `ffz_config` typedef (after `ffz_char_class` enum). Insert:

  ```c
  // ---------------------------------------------------------------------------
  // Scoring mode — controls the algorithm used in ffz_match / ffz_corpus_filter.
  // ---------------------------------------------------------------------------
  typedef enum {
      FFZ_SCORE_FAST   = 0,  // default: 2-row rolling DP, simplified bonuses
      FFZ_SCORE_OFF    = 1,  // prefilter only; score=0, original insertion order
      FFZ_SCORE_NUCLEO = 2,  // nucleo-compatible full-matrix DP (legacy behaviour)
  } ffz_scoring_mode;
  ```

  Then inside `ffz_config`, add as the **last** field before the closing `}`:

  ```c
      ffz_scoring_mode scoring_mode;  // algorithm for ffz_match / corpus_filter
  ```

- [ ] **Step 2: Set default in `src/ffz_string.c`**

  In `ffz_config_default()`, after `fill_ascii_class(&c);` and before `return c;`, add:

  ```c
      c.scoring_mode = FFZ_SCORE_FAST;
  ```

  In `ffz_config_match_paths()`, after `fill_ascii_class(&c);` and before `return c;`, add:

  ```c
      c.scoring_mode = FFZ_SCORE_FAST;
  ```

- [ ] **Step 3: Add `ffz_fast_bonus` to `src/ffz_internal.h`**

  At the end of the `// --- char ops` section (after the existing declarations), add:

  ```c
  // Simplified 4-bonus model for FFZ_SCORE_FAST: BOUNDARY=8, CAMEL=7, else 0.
  // Ignores whitespace/delimiter distinction (no 10/9/8 tiers).
  uint8_t ffz_fast_bonus(ffz_char_class prev, ffz_char_class cls);
  ```

- [ ] **Step 4: Implement `ffz_fast_bonus` in `src/ffz_chars.c`**

  Add at the end of `src/ffz_chars.c`, after `ffz_bonus_for`:

  ```c
  #define FFZ_FAST_BONUS_BOUNDARY 8
  #define FFZ_FAST_BONUS_CAMEL    7

  uint8_t ffz_fast_bonus(ffz_char_class prev, ffz_char_class cls) {
      // After any non-word / separator: word-boundary bonus.
      if (prev <= FFZ_CLASS_DELIMITER && cls > FFZ_CLASS_DELIMITER)
          return FFZ_FAST_BONUS_BOUNDARY;
      // camelCase or letter→digit transition.
      if ((prev == FFZ_CLASS_LOWER && cls == FFZ_CLASS_UPPER) ||
          (prev != FFZ_CLASS_NUMBER && cls == FFZ_CLASS_NUMBER))
          return FFZ_FAST_BONUS_CAMEL;
      return 0;
  }
  ```

- [ ] **Step 5: Build to verify compilation**

  ```
  make test
  ```

  Expected: all existing tests pass (no behavioural change yet).

- [ ] **Step 6: Commit**

  ```
  git add include/ffz.h src/ffz_string.c src/ffz_chars.c src/ffz_internal.h
  git commit -m "feat: add ffz_scoring_mode enum, config field, and ffz_fast_bonus helper"
  ```

---

### Task 2: Add rolling-row scratch to `ffz_matcher` + implement `ffz_fuzzy_rolling()`

**Files:**
- Modify: `src/ffz_internal.h` (adds `roll` field to `ffz_matcher` struct; declares `ffz_fuzzy_rolling`)
- Modify: `src/ffz_match.c` (updates `ffz_matcher_reserve` and `ffz_matcher_free`)
- Modify: `src/ffz_fuzzy.c` (implements `ffz_fuzzy_rolling`)
- Modify: `tests/test_ffz.c` (adds rolling-DP tests)

**Interfaces:**
- Consumes: `ffz_fast_bonus()`, `ffz_matcher_reserve()`, `m->hay`, `m->bonus`, `m->roll`, `ffz_at()`, `ffz_class_and_normalize()`, `FFZ_SCORE_MATCH`, `FFZ_PENALTY_GAP_START`, `FFZ_PENALTY_GAP_EXTENSION`, `FFZ_BONUS_CONSECUTIVE`, `FFZ_BONUS_FIRST_CHAR_MULTIPLIER`
- Produces: `int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle, size_t start, size_t end)` — returns best score ≥ 1 or -1 on no match; never fills indices.

- [ ] **Step 1: Write failing test**

  Add a new test function to `tests/test_ffz.c` (before `int main`):

  ```c
  static void test_rolling_dp(void) {
      ffz_config cfg = ffz_config_default();
      cfg.scoring_mode = FFZ_SCORE_FAST;
      ffz_matcher *m = ffz_matcher_new(cfg);

      // FAST mode: fuzzy match returns a positive score.
      int32_t s = score(m, "cfg", "ffz_config", NULL, FFZ_FUZZY);
      CHECK(s > 0, "rolling: fuzzy match gives positive score");

      // No match returns -1.
      int32_t s2 = score(m, "xyz", "abcdef", NULL, FFZ_FUZZY);
      CHECK(s2 < 0, "rolling: no-match gives -1");

      // Boundary match scores higher than interior (cfg at start vs middle).
      int32_t sb = score(m, "cfg", "cfg_helper", NULL, FFZ_FUZZY);
      int32_t si = score(m, "cfg", "my_cfg_val", NULL, FFZ_FUZZY);
      CHECK(sb > si, "rolling: boundary match scores higher than interior");

      ffz_matcher_free(m);
  }
  ```

  Also call it from `main()` alongside the other test functions.

- [ ] **Step 2: Run test to confirm it fails**

  ```
  make test
  ```

  Expected: FAIL — `ffz_fuzzy_rolling` not declared; `score()` with FAST config calls `ffz_fuzzy_optimal` (returns positive but boundary check may differ — compile error is the first failure).

- [ ] **Step 3: Add `roll` field to `struct ffz_matcher` in `src/ffz_internal.h`**

  In the `struct ffz_matcher` block, add `roll` after `pmat`:

  ```c
  struct ffz_matcher {
      ffz_config cfg;
      uint32_t *hay;      // normalized haystack window           [cap_hay]
      uint8_t  *bonus;    // precomputed bonus per column          [cap_hay]
      ffz_mcell *mgrid;   // full M grid (needle_len x width)       [cap_grid]
      uint8_t  *pmat;     // full P-origin bits (needle_len x width)[cap_grid]
      uint16_t *roll;     // 2 rolling rows for FAST DP             [2 * cap_hay]
      size_t cap_hay, cap_grid;
  };
  ```

  Also add the declaration after the existing `ffz_fuzzy_greedy` / `ffz_fuzzy_optimal` declarations:

  ```c
  // 2-row rolling Smith-Waterman DP (FAST mode, score-only, no backtracking).
  int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle,
                            size_t start, size_t end);
  ```

- [ ] **Step 4: Update `ffz_matcher_reserve` and `ffz_matcher_free` in `src/ffz_match.c`**

  In `ffz_matcher_reserve`, inside the `if (width > m->cap_hay)` block, after the `m->bonus` realloc succeeds (and before updating `m->cap_hay`), add:

  ```c
          uint16_t *rl = (uint16_t *)realloc(m->roll, nc * 2 * sizeof(uint16_t));
          if (!rl) return false;
          m->roll = rl;
  ```

  In `ffz_matcher_free`, add `free(m->roll);` alongside the other frees:

  ```c
  void ffz_matcher_free(ffz_matcher *m) {
      if (!m) return;
      free(m->hay);
      free(m->bonus);
      free(m->mgrid);
      free(m->pmat);
      free(m->roll);   // add this line
      free(m);
  }
  ```

- [ ] **Step 5: Implement `ffz_fuzzy_rolling` in `src/ffz_fuzzy.c`**

  Add at the end of `src/ffz_fuzzy.c`:

  ```c
  // --- 2-row rolling DP (FAST scoring mode) --------------------------------
  // Uses simplified bonuses (ffz_fast_bonus) and O(W) scratch instead of the
  // full needle_len×W matrix. Score-only: never fills indices.
  //
  // Recurrences (H = match track, C = gap/skip track, lagged by 1 column):
  //   H[k][i] = max(H[k-1][i-1] + max(b, CONSECUTIVE),   // consecutive
  //                 C[k][i-1]   + b)                       // after gap
  //             + SCORE_MATCH   (when hay[i] == needle[k])
  //   C[k][i] = max(H[k-1][i-1] - GAP_START,
  //                 C[k][i-1]   - GAP_EXTENSION)
  // C[k][i] is computed during the scan and feeds column i+1 via pprev_c.
  int32_t ffz_fuzzy_rolling(ffz_matcher *m, ffz_str hay, ffz_str needle,
                            size_t start, size_t end) {
      const ffz_config *cfg = &m->cfg;
      size_t W = end - start;
      size_t nl = needle.len;

      if (!ffz_matcher_reserve(m, W, 1)) return -1;  // OOM

      uint16_t *H_prev = m->roll;                 // row k-1
      uint16_t *H_curr = m->roll + m->cap_hay;    // row k

      // Normalize window and precompute fast bonuses.
      ffz_char_class prev_cls =
          start > 0 ? ffz_char_class_of(ffz_at(hay, start - 1), cfg)
                    : cfg->initial_char_class;
      for (size_t i = 0; i < W; i++) {
          uint32_t c;
          ffz_char_class cls =
              ffz_class_and_normalize(ffz_at(hay, start + i), cfg, &c);
          m->hay[i]   = c;
          m->bonus[i] = ffz_fast_bonus(prev_cls, cls);
          prev_cls    = cls;
      }

      // Row 0: match needle[0].
      uint32_t nd0 = ffz_at(needle, 0);
      for (size_t i = 0; i < W; i++) {
          H_prev[i] = (m->hay[i] == nd0)
              ? (uint16_t)(m->bonus[i] * FFZ_BONUS_FIRST_CHAR_MULTIPLIER +
                           FFZ_SCORE_MATCH)
              : 0;
      }

      // Rows 1 .. nl-1.
      for (size_t k = 1; k < nl; k++) {
          uint32_t ndk = ffz_at(needle, k);
          uint16_t pprev_c = 0;  // C[k][i-1] (gap score from the previous column)

          for (size_t i = 0; i < W; i++) {
              uint16_t new_h = 0;
              if (m->hay[i] == ndk) {
                  uint8_t b = m->bonus[i];
                  // Consecutive path: from H[k-1][i-1].
                  if (i > 0 && H_prev[i - 1]) {
                      uint8_t cb = b > FFZ_BONUS_CONSECUTIVE
                                       ? b
                                       : (uint8_t)FFZ_BONUS_CONSECUTIVE;
                      new_h = (uint16_t)(H_prev[i - 1] + cb + FFZ_SCORE_MATCH);
                  }
                  // Gap path: from C[k][i-1] = pprev_c.
                  if (pprev_c) {
                      uint16_t g = (uint16_t)(pprev_c + b + FFZ_SCORE_MATCH);
                      if (g > new_h) new_h = g;
                  }
              }
              H_curr[i] = new_h;

              // Compute C[k][i] (for use at column i+1 as pprev_c).
              uint16_t new_c = 0;
              if (i > 0 && H_prev[i - 1] > FFZ_PENALTY_GAP_START)
                  new_c = (uint16_t)(H_prev[i - 1] - FFZ_PENALTY_GAP_START);
              if (pprev_c > FFZ_PENALTY_GAP_EXTENSION) {
                  uint16_t ext =
                      (uint16_t)(pprev_c - FFZ_PENALTY_GAP_EXTENSION);
                  if (ext > new_c) new_c = ext;
              }
              pprev_c = new_c;
          }
          // Rotate rows.
          uint16_t *tmp = H_prev;
          H_prev = H_curr;
          H_curr = tmp;
      }

      // Find best score in the final row.
      uint16_t best = 0;
      for (size_t i = 0; i < W; i++)
          if (H_prev[i] > best) best = H_prev[i];
      return best ? (int32_t)best : -1;
  }
  ```

- [ ] **Step 6: Run tests**

  ```
  make test
  ```

  Expected: all tests pass including the new `test_rolling_dp`.

- [ ] **Step 7: Commit**

  ```
  git add include/ffz_internal.h src/ffz_match.c src/ffz_fuzzy.c tests/test_ffz.c
  git commit -m "feat: add ffz_fuzzy_rolling (2-row rolling DP for FAST mode)"
  ```

---

### Task 3: Wire scoring mode into `ffz_match()` dispatch

**Files:**
- Modify: `src/ffz_match.c`
- Modify: `tests/test_ffz.c` (adds FAST and OFF matcher-level tests)

**Interfaces:**
- Consumes: `ffz_fuzzy_rolling()`, `ffz_fuzzy_greedy()`, `ffz_fuzzy_optimal()`, `ffz_calculate_score()`, `ffz_prefilter()`
- Produces: `ffz_match()` now branches on `cfg->scoring_mode` for FUZZY; non-FUZZY modes with OFF return 0 instead of computed score.

- [ ] **Step 1: Write failing tests**

  Add to `tests/test_ffz.c`:

  ```c
  static void test_scoring_modes(void) {
      // --- FAST mode ---
      ffz_config cfgf = ffz_config_default();
      cfgf.scoring_mode = FFZ_SCORE_FAST;
      ffz_matcher *mf = ffz_matcher_new(cfgf);

      // FAST: fuzzy match returns positive score.
      CHECK(score(mf, "cfg", "ffz_config", NULL, FFZ_FUZZY) > 0,
            "FAST fuzzy: positive score on match");
      // FAST: no match still -1.
      CHECK(score(mf, "xyz", "abcdef", NULL, FFZ_FUZZY) < 0,
            "FAST fuzzy: -1 on no match");
      // FAST: produces indices.
      ffz_indices ix = {0};
      int32_t sf = score(mf, "ab", "abcdef", &ix, FFZ_FUZZY);
      CHECK(sf > 0, "FAST fuzzy: positive score with indices");
      CHECK(ix.len == 2, "FAST fuzzy: index count == needle length");
      ffz_indices_free(&ix);
      // FAST: exact/prefix modes still work (no DP involved).
      CHECK(score(mf, "abc", "abcdef", NULL, FFZ_EXACT) < 0,
            "FAST exact: whole-string mismatch -> -1");
      CHECK(score(mf, "abc", "abc", NULL, FFZ_EXACT) > 0,
            "FAST exact: exact whole-string match -> positive");

      ffz_matcher_free(mf);

      // --- OFF mode ---
      ffz_config cfgo = ffz_config_default();
      cfgo.scoring_mode = FFZ_SCORE_OFF;
      ffz_matcher *mo = ffz_matcher_new(cfgo);

      // OFF fuzzy: match -> 0.
      CHECK(score(mo, "cfg", "ffz_config", NULL, FFZ_FUZZY) == 0,
            "OFF fuzzy: score is 0 on match");
      // OFF fuzzy: no match -> -1.
      CHECK(score(mo, "xyz", "abcdef", NULL, FFZ_FUZZY) < 0,
            "OFF fuzzy: -1 on no match");
      // OFF fuzzy: indices still populated.
      ffz_indices ix2 = {0};
      int32_t so = score(mo, "ab", "abcdef", &ix2, FFZ_FUZZY);
      CHECK(so == 0, "OFF fuzzy: score is 0 when indices requested");
      CHECK(ix2.len == 2, "OFF fuzzy: index count == needle length");
      ffz_indices_free(&ix2);
      // OFF exact: score is 0 on match.
      CHECK(score(mo, "abc", "abc", NULL, FFZ_EXACT) == 0,
            "OFF exact: score is 0");
      // OFF exact: -1 on mismatch (match logic still runs).
      CHECK(score(mo, "abc", "abcd", NULL, FFZ_EXACT) < 0,
            "OFF exact: -1 on length mismatch");

      ffz_matcher_free(mo);

      // --- NUCLEO mode (current behaviour unchanged) ---
      ffz_config cfgn = ffz_config_default();
      cfgn.scoring_mode = FFZ_SCORE_NUCLEO;
      ffz_matcher *mn = ffz_matcher_new(cfgn);
      CHECK(score(mn, "cfg", "ffz_config", NULL, FFZ_FUZZY) > 0,
            "NUCLEO fuzzy: positive score");
      ffz_matcher_free(mn);
  }
  ```

  Add call to `test_scoring_modes()` in `main()`.

- [ ] **Step 2: Run to confirm failure**

  ```
  make test
  ```

  Expected: `test_scoring_modes` FAILs — OFF fuzzy currently returns a non-zero score; FAST fuzzy goes to the optimal DP (not rolling).

- [ ] **Step 3: Refactor `ffz_match()` in `src/ffz_match.c`**

  The existing `ffz_match` function ends with a big `switch (mode)`. Rename the inner implementation to a static helper and add dispatching:

  Replace the existing `ffz_match` body with:

  ```c
  // Internal implementation; called by ffz_match after early-exit guards.
  static int32_t ffz_match_impl(ffz_matcher *m, ffz_str haystack, ffz_str needle,
                                ffz_mode mode, ffz_indices *out) {
      const ffz_config *cfg = &m->cfg;
      size_t hn = haystack.len, nl = needle.len;

      switch (mode) {
          case FFZ_EXACT: {
              size_t lead = ffz_str_ws(needle, 0, cfg) ? 0 : leading_ws(haystack, cfg);
              size_t trail =
                  ffz_str_ws(needle, nl - 1, cfg) ? 0 : trailing_ws(haystack, cfg);
              if (trail == hn) return -1;
              return exact_impl(m, haystack, needle, lead, hn - trail, out);
          }
          case FFZ_PREFIX: {
              size_t lead = ffz_str_ws(needle, 0, cfg) ? 0 : leading_ws(haystack, cfg);
              if (hn - lead < nl) return -1;
              return exact_impl(m, haystack, needle, lead, nl + lead, out);
          }
          case FFZ_POSTFIX: {
              size_t trail =
                  ffz_str_ws(needle, nl - 1, cfg) ? 0 : trailing_ws(haystack, cfg);
              if (hn - trail < nl) return -1;
              return exact_impl(m, haystack, needle, hn - nl - trail, hn - trail, out);
          }
          case FFZ_SUBSTRING:
              return substring_match(m, haystack, needle, out);

          case FFZ_FUZZY:
          default: {
              if (nl == hn)
                  return exact_impl(m, haystack, needle, 0, hn, out);
              if (nl == 1) {
                  long pos = substring_best(m, haystack, needle);
                  if (pos < 0) return -1;
                  return (int32_t)ffz_calculate_score(m, haystack, needle,
                                                      (size_t)pos, (size_t)pos + 1, out);
              }
              size_t start, greedy_end, end;
              if (!ffz_prefilter(cfg, haystack, needle, false, &start, &greedy_end,
                                 &end))
                  return -1;
              if (nl == end - start)
                  return (int32_t)ffz_calculate_score(m, haystack, needle, start,
                                                      start + nl, out);

              switch (cfg->scoring_mode) {
                  case FFZ_SCORE_OFF:
                      // Prefilter already confirmed subsequence; fill indices greedily.
                      if (out)
                          ffz_calculate_score(m, haystack, needle, start, greedy_end,
                                             out);
                      return 0;
                  case FFZ_SCORE_FAST:
                      if (!out)
                          return ffz_fuzzy_rolling(m, haystack, needle, start, end);
                      // Pass 2 (indices requested): greedy linear path.
                      return ffz_fuzzy_greedy(m, haystack, needle, start, greedy_end,
                                             out);
                  default: /* FFZ_SCORE_NUCLEO */
                      return ffz_fuzzy_optimal(m, haystack, needle, start, greedy_end,
                                              end, out);
              }
          }
      }
  }

  int32_t ffz_match(ffz_matcher *m, ffz_str haystack, ffz_str needle,
                    ffz_mode mode, ffz_indices *out) {
      const ffz_config *cfg = &m->cfg;
      size_t hn = haystack.len, nl = needle.len;

      if (nl == 0) return 0;
      if (haystack.b && !needle.b) return -1;
      if (nl > hn) return -1;

      int32_t s = ffz_match_impl(m, haystack, needle, mode, out);

      // OFF mode: suppress score for non-FUZZY modes (FUZZY already returns 0).
      if (s > 0 && cfg->scoring_mode == FFZ_SCORE_OFF) return 0;
      return s;
  }
  ```

- [ ] **Step 4: Run tests**

  ```
  make test
  ```

  Expected: all tests pass including `test_scoring_modes`.

- [ ] **Step 5: Commit**

  ```
  git add src/ffz_match.c tests/test_ffz.c
  git commit -m "feat: wire scoring mode into ffz_match (FAST rolling DP, OFF prefilter-only)"
  ```

---

### Task 4: Update `ffz_corpus_filter` signature + thread scoring through corpus internals

**Files:**
- Modify: `include/ffz_corpus.h`
- Modify: `src/ffz_corpus.c`
- Modify: `tests/test_ffz.c` (update 20 call sites + add corpus-level tests)
- Modify: `tests/test_leak.c` (update 6 call sites)
- Modify: `ffi/ffz_ffi.c` (update 1 call site, temporary until Task 5)

**Interfaces:**
- Consumes: `ffz_scoring_mode`
- Produces: `ffz_corpus_filter(..., ffz_scoring_mode scoring, ...)` — new signature

- [ ] **Step 1: Write failing corpus tests**

  Add to `tests/test_ffz.c` (before `int main`):

  ```c
  static void test_corpus_scoring_modes(void) {
      // --- OFF: original order, score=0, limit respected ---
      ffz_corpus *c = ffz_corpus_new(ffz_config_default());
      ffz_corpus_add(c, "configure", 9);
      ffz_corpus_add(c, "cfg_helper", 10);
      ffz_corpus_add(c, "my_cfg", 6);
      ffz_corpus_add(c, "ffz_config", 10);
      ffz_corpus_add(c, "no_match_xyz", 12);

      ffz_results r = {0};
      ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                        ffz_parallel_off(), 2, FFZ_SCORE_OFF, &r);
      CHECK(r.len == 2, "OFF corpus: respects limit=2");
      // First two matches in insertion order: configure(0), cfg_helper(1).
      CHECK(r.hits[0].item_index == 0, "OFF corpus: first hit is insertion-order first");
      CHECK(r.hits[1].item_index == 1, "OFF corpus: second hit is insertion-order second");
      CHECK(r.hits[0].score == 0, "OFF corpus: score is 0");
      CHECK(r.hits[1].score == 0, "OFF corpus: score is 0");
      ffz_results_free(&r);

      // --- FAST: ranked by rolling DP score ---
      ffz_results r2 = {0};
      ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                        ffz_parallel_off(), 3, FFZ_SCORE_FAST, &r2);
      CHECK(r2.len == 3, "FAST corpus: returns 3 matches");
      // cfg_helper should score higher (cfg at start) than my_cfg (cfg in middle).
      bool cfg_helper_first = r2.hits[0].item_index == 1;  // cfg_helper is item 1
      CHECK(cfg_helper_first, "FAST corpus: boundary match ranked first");
      CHECK(r2.hits[0].score > 0, "FAST corpus: score is positive");
      ffz_results_free(&r2);

      // --- NUCLEO: existing behaviour ---
      ffz_results r3 = {0};
      ffz_corpus_filter(c, "cfg", 3, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                        ffz_parallel_off(), 3, FFZ_SCORE_NUCLEO, &r3);
      CHECK(r3.len == 3, "NUCLEO corpus: returns 3 matches");
      CHECK(r3.hits[0].score > 0, "NUCLEO corpus: positive score");
      ffz_results_free(&r3);

      ffz_corpus_free(c);
  }
  ```

  Add call to `test_corpus_scoring_modes()` in `main()`.

- [ ] **Step 2: Update `ffz_corpus_filter` signature in `include/ffz_corpus.h`**

  Replace:
  ```c
  void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                         ffz_case_matching cm, ffz_normalization nm,
                         ffz_mode mode, ffz_parallel par, size_t limit,
                         ffz_results *out);
  ```
  With:
  ```c
  void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                         ffz_case_matching cm, ffz_normalization nm,
                         ffz_mode mode, ffz_parallel par, size_t limit,
                         ffz_scoring_mode scoring,
                         ffz_results *out);
  ```

- [ ] **Step 3: Thread `scoring` through `src/ffz_corpus.c`**

  **3a.** Add `scoring` field to `scan_job`:
  ```c
  typedef struct {
      ffz_corpus *c;
      const ffz_pattern *pat;
      size_t lo, hi;
      scored *out;
      size_t cap;
      bool bounded;
      ffz_scoring_mode scoring;   // new
      size_t n;
  } scan_job;
  ```

  **3b.** In `scan_job_run`, after `ffz_matcher *m = ffz_matcher_new(j->c->cfg);`, add:
  ```c
      if (m) m->cfg.scoring_mode = j->scoring;
  ```

  **3c.** In `ffz_corpus_filter`, update the function signature to match the header:
  ```c
  void ffz_corpus_filter(ffz_corpus *c, const char *query, size_t query_len,
                         ffz_case_matching cm, ffz_normalization nm,
                         ffz_mode mode, ffz_parallel par, size_t limit,
                         ffz_scoring_mode scoring,
                         ffz_results *out)
  ```

  **3d.** In `ffz_corpus_filter`, in the serial (single-thread) path, set scoring on `fm` after creating it:
  ```c
      ffz_matcher *fm = ffz_matcher_new(c->cfg);
      if (!pat || !fm) { ... }
      fm->cfg.scoring_mode = scoring;
  ```

  **3e.** In the multi-thread job setup, set `jobs[t].scoring = scoring;` when constructing each job.

  **3f.** Add OFF-mode order preservation — after the merge of per-thread results and before the sort/topk block, add:

  ```c
      // OFF mode: results are in corpus order (chunks already ordered); skip sort.
      if (scoring == FFZ_SCORE_OFF) {
          // Items arrived in chunk order (thread 0 first, etc.), which preserves
          // corpus insertion order because chunks are contiguous and non-overlapping.
          // Just truncate to limit.
          keep = (limit && limit < ns) ? limit : ns;
          // sc[0..keep) is already in insertion order.
          goto pass2;
      }
  ```

  Place the `pass2:` label immediately before the pass-2 loop. If `goto` is undesirable in the codebase style, wrap the sort+topk in an `else` block instead:

  ```c
      size_t keep;
      if (scoring == FFZ_SCORE_OFF) {
          keep = (limit && limit < ns) ? limit : ns;
      } else if (top) {
          scored_topk(sc, ns, limit, top);
          free(sc);
          sc = top;
          keep = limit;
      } else {
          if (ns) qsort(sc, ns, sizeof(scored), cmp_scored);
          keep = ns;
      }
  ```

  (The existing `scored *top = ...` allocation for topk should move inside the `else if` branch.)

- [ ] **Step 4: Update all 20 call sites in `tests/test_ffz.c`**

  Every call to `ffz_corpus_filter` in this file currently ends with `..., limit, &r)`. Change all of them to `..., limit, FFZ_SCORE_FAST, &r)`.

  Quick verify: `grep -c "ffz_corpus_filter" tests/test_ffz.c` should output `22` after adding the 2 new calls from Step 1 (20 old + 2 new).

  All old calls look like:
  ```c
  ffz_corpus_filter(c, "al", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                    ffz_parallel_off(), 0, &r);
  ```
  Become:
  ```c
  ffz_corpus_filter(c, "al", 2, FFZ_CASE_SMART, FFZ_NORM_SMART, FFZ_FUZZY,
                    ffz_parallel_off(), 0, FFZ_SCORE_FAST, &r);
  ```

- [ ] **Step 5: Update 6 call sites in `tests/test_leak.c`**

  Same transformation: add `FFZ_SCORE_FAST` before `&r` in every `ffz_corpus_filter` call.

- [ ] **Step 6: Update 1 call site in `ffi/ffz_ffi.c`**

  Find:
  ```c
      ffz_corpus_filter(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                        (ffz_mode)mode, par, limit, r);
  ```
  Replace with (temporary — uses corpus's own scoring_mode as a default until Task 5):
  ```c
      ffz_corpus_filter(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                        (ffz_mode)mode, par, limit, c->cfg.scoring_mode, r);
  ```

  Note: `ffz_corpus` doesn't expose its config publicly. Add a getter in `src/ffz_corpus.c` and declare it in `include/ffz_corpus.h`:
  ```c
  // include/ffz_corpus.h
  ffz_scoring_mode ffz_corpus_scoring(const ffz_corpus *c);

  // src/ffz_corpus.c
  ffz_scoring_mode ffz_corpus_scoring(const ffz_corpus *c) {
      return c->cfg.scoring_mode;
  }
  ```

  Then `ffi/ffz_ffi.c` uses `ffz_corpus_scoring(c)`.

- [ ] **Step 7: Run tests**

  ```
  make test
  ```

  Expected: all tests pass including `test_corpus_scoring_modes`.

- [ ] **Step 8: Commit**

  ```
  git add include/ffz_corpus.h src/ffz_corpus.c tests/test_ffz.c tests/test_leak.c ffi/ffz_ffi.c
  git commit -m "feat: add scoring param to ffz_corpus_filter; thread scoring through corpus/jobs"
  ```

---

### Task 5: FFI shim — add `ffz_ffi_new_cfg2` and `ffz_ffi_filter_ex2`

**Files:**
- Modify: `ffi/ffz_ffi.c`

**Interfaces:**
- Produces: `ffz_ffi_new_cfg2(int paths, int prefer_prefix, int scoring)` and `ffz_ffi_filter_ex2(corpus, q, qn, mode, cm, nm, parallel, threads, limit, scoring)` — new exported symbols

- [ ] **Step 1: Add `ffz_ffi_new_cfg2` to `ffi/ffz_ffi.c`**

  After the existing `ffz_ffi_new_cfg` function, add:

  ```c
  // ffz_ffi_new_cfg2: like ffz_ffi_new_cfg but sets the scoring mode.
  // scoring: 0=FAST (default), 1=OFF, 2=NUCLEO.
  FFZ_API ffz_corpus *ffz_ffi_new_cfg2(int paths, int prefer_prefix, int scoring) {
      ffz_config cfg = paths ? ffz_config_match_paths() : ffz_config_default();
      cfg.prefer_prefix = prefer_prefix != 0;
      cfg.scoring_mode  = (ffz_scoring_mode)scoring;
      return ffz_corpus_new(cfg);
  }
  ```

- [ ] **Step 2: Add `ffz_ffi_filter_ex2` to `ffi/ffz_ffi.c`**

  After the existing `ffz_ffi_filter_ex` function, add:

  ```c
  // ffz_ffi_filter_ex2: like ffz_ffi_filter_ex but takes an explicit scoring mode.
  // The Dart layer passes the already-resolved effective scoring (corpus default
  // merged with per-call override). scoring: 0=FAST, 1=OFF, 2=NUCLEO.
  FFZ_API ffz_results *ffz_ffi_filter_ex2(ffz_corpus *c, const char *q, size_t qn,
                                          int mode, int cm, int nm,
                                          int parallel, int threads,
                                          size_t limit, int scoring) {
      ffz_results *r = (ffz_results *)calloc(1, sizeof(ffz_results));
      if (!r) return NULL;
      ffz_parallel par;
      par.parallel = parallel != 0;
      par.threads  = threads;
      ffz_corpus_filter(c, q, qn, (ffz_case_matching)cm, (ffz_normalization)nm,
                        (ffz_mode)mode, par, limit,
                        (ffz_scoring_mode)scoring, r);
      return r;
  }
  ```

- [ ] **Step 3: Update the existing `ffz_ffi_filter_ex` to no longer use `ffz_corpus_scoring`**

  Now that `ffz_ffi_filter_ex2` is the proper path, revert the temporary fix from Task 4 Step 6: the old `ffz_ffi_filter_ex` should keep using `FFZ_SCORE_FAST` as a safe backward-compat default (not the corpus's runtime setting):

  ```c
  FFZ_API ffz_results *ffz_ffi_filter_ex(ffz_corpus *c, const char *q, size_t qn,
                                         int mode, int cm, int nm, int parallel,
                                         int threads, size_t limit) {
      // Old callers get FAST (the new default) for backward compatibility.
      return ffz_ffi_filter_ex2(c, q, qn, mode, cm, nm, parallel, threads, limit,
                                FFZ_SCORE_FAST);
  }
  ```

  This means `ffz_corpus_scoring` (added in Task 4) is no longer needed in `ffi/ffz_ffi.c`. Remove the `#include` if it was added, but keep the `ffz_corpus_scoring` function in corpus.c (it's a legitimate public accessor).

- [ ] **Step 4: Add symbols to `ffz.map` (Linux export map)**

  Open `ffz.map`. Add `ffz_ffi_new_cfg2;` and `ffz_ffi_filter_ex2;` alongside the existing FFI exports.

- [ ] **Step 5: Build**

  ```
  make test
  ```

  Expected: all tests pass.

- [ ] **Step 6: Commit**

  ```
  git add ffi/ffz_ffi.c ffz.map
  git commit -m "feat: add ffz_ffi_new_cfg2 and ffz_ffi_filter_ex2 for Dart scoring-mode support"
  ```

---

### Task 6: Dart — `FuzzyScoring` enum + `FuzzyOptions` + method params

**Files:**
- Modify: `lib/ffuzzy.dart`

**Interfaces:**
- Consumes: `ffz_ffi_new_cfg2` (tolerant lookup), `ffz_ffi_filter_ex2` (tolerant lookup)
- Produces: `enum FuzzyScoring { fast, off, nucleo }`; `FuzzyOptions.scoring`; `scoring` named param on all 10 search methods (5 sync + 5 async)

- [ ] **Step 1: Add `FuzzyScoring` enum**

  After the `FuzzyNorm` enum declaration, add:

  ```dart
  /// Scoring model for corpus queries.
  ///
  /// Set the corpus-wide default in [FuzzyOptions]; override per call via the
  /// `scoring` named parameter on [FuzzyCorpus.fuzzy], [FuzzyCorpus.exact], etc.
  enum FuzzyScoring {
    /// 2-row rolling DP with simplified bonuses (default). Faster than [nucleo]
    /// in single-threaded scenarios; good ranking quality for typical use.
    fast,

    /// No DP. Prefilter only; all matching items get [FuzzyHit.score] == 0 and
    /// are returned in corpus insertion order. Use for programmatic
    /// exact/ID matching where ranking is irrelevant.
    off,

    /// Full-matrix DP, nucleo 0.3.1 compatible. Highest ranking accuracy.
    nucleo,
  }
  ```

- [ ] **Step 2: Add `scoring` to `FuzzyOptions`**

  In the `FuzzyOptions` class:

  **a.** Add field:
  ```dart
  final FuzzyScoring scoring;
  ```

  **b.** Add to constructor with default:
  ```dart
  const FuzzyOptions({
    this.scoring = FuzzyScoring.fast,   // new field, first
    this.caseMatching = FuzzyCase.smart,
    this.normalization = FuzzyNorm.smart,
    this.parallel = false,
    this.threads = 0,
    this.limit = 0,
    this.highlight = true,
  });
  ```

  **c.** Add to `copyWith`:
  ```dart
  FuzzyOptions copyWith({
    FuzzyScoring? scoring,
    FuzzyCase? caseMatching,
    FuzzyNorm? normalization,
    bool? parallel,
    int? threads,
    int? limit,
    bool? highlight,
  }) =>
      FuzzyOptions(
        scoring: scoring ?? this.scoring,
        caseMatching: caseMatching ?? this.caseMatching,
        normalization: normalization ?? this.normalization,
        parallel: parallel ?? this.parallel,
        threads: threads ?? this.threads,
        limit: limit ?? this.limit,
        highlight: highlight ?? this.highlight,
      );
  ```

- [ ] **Step 3: Add tolerant FFI lookups for the two new symbols**

  In `class _Lib`, add two new function fields after `filterEx`:

  ```dart
  final Pointer<Void> Function(int, int, int)? newCfg2;
  final Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? filterEx2;
  ```

  In the `_Lib` constructor, look them up tolerantly (same pattern as `installCrash`):

  ```dart
  newCfg2 = _lookupNewCfg2(lib),
  filterEx2 = _lookupFilterEx2(lib),
  ```

  Add the two static helpers:

  ```dart
  static Pointer<Void> Function(int, int, int)? _lookupNewCfg2(
      DynamicLibrary lib) {
    try {
      return lib.lookupFunction<
          Pointer<Void> Function(Int32, Int32, Int32),
          Pointer<Void> Function(int, int, int)>('ffz_ffi_new_cfg2');
    } catch (_) {
      return null;
    }
  }

  static Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
      int, int, int, int, int)? _lookupFilterEx2(DynamicLibrary lib) {
    try {
      return lib.lookupFunction<
          Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, Size, Int32,
              Int32, Int32, Int32, Int32, Size, Int32),
          Pointer<Void> Function(Pointer<Void>, Pointer<Utf8>, int, int, int,
              int, int, int, int, int)>('ffz_ffi_filter_ex2');
    } catch (_) {
      return null;
    }
  }
  ```

  Update the native type aliases at the top of `_Lib` if needed (add `_NewCfg2N` and `_FilterEx2N`).

- [ ] **Step 4: Use `ffz_ffi_new_cfg2` in the `FuzzyCorpus` constructor**

  Find where `newCfg` is called (in `FuzzyCorpus._init` or constructor body). Replace:

  ```dart
  _ptr = lib.newCfg(paths ? 1 : 0, preferPrefix ? 1 : 0);
  ```

  With:

  ```dart
  final sc = options.scoring.index;
  _ptr = (lib.newCfg2 != null)
      ? lib.newCfg2!(paths ? 1 : 0, preferPrefix ? 1 : 0, sc)
      : lib.newCfg(paths ? 1 : 0, preferPrefix ? 1 : 0);
  ```

- [ ] **Step 5: Update `_eff()` helper to accept and merge `scoring`**

  Replace:
  ```dart
  FuzzyOptions _eff(FuzzyCase? cm, FuzzyNorm? nm, bool? par, int? th, int? lim,
          bool? hl) =>
      options.copyWith(
          caseMatching: cm, normalization: nm, parallel: par,
          threads: th, limit: lim, highlight: hl);
  ```

  With:
  ```dart
  FuzzyOptions _eff(FuzzyCase? cm, FuzzyNorm? nm, bool? par, int? th, int? lim,
          bool? hl, FuzzyScoring? sc) =>
      options.copyWith(
          scoring: sc, caseMatching: cm, normalization: nm,
          parallel: par, threads: th, limit: lim, highlight: hl);
  ```

- [ ] **Step 6: Add `FuzzyScoring? scoring` parameter to all 10 search methods**

  For each of `fuzzy`, `substring`, `prefix`, `postfix`, `exact`, and their `…Async` twins, add `FuzzyScoring? scoring` as a named parameter and pass it to `_eff`:

  Example for `fuzzy`:
  ```dart
  List<FuzzyHit<T>> fuzzy(String query,
          {FuzzyCase? caseMatching,
          FuzzyNorm? normalization,
          bool? parallel,
          int? threads,
          int? limit,
          bool? highlight,
          FuzzyScoring? scoring}) =>           // new
      _search(
          _mFuzzy,
          query,
          _eff(caseMatching, normalization, parallel, threads, limit,
              highlight, scoring));             // pass scoring
  ```

  Apply identically to `substring`, `prefix`, `postfix`, `exact`, `fuzzyAsync`, `substringAsync`, `prefixAsync`, `postfixAsync`, `exactAsync`.

- [ ] **Step 7: Use `ffz_ffi_filter_ex2` in `_rawFilter`**

  Find `_rawFilter` (the static method that calls `lib.filterEx`). Replace the `filterEx` call:

  ```dart
  // Before:
  final r = lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching.index,
      o.normalization.index, o.parallel ? 1 : 0, o.threads, o.limit);

  // After:
  final r = (lib.filterEx2 != null)
      ? lib.filterEx2!(ptr, u.ptr, u.len, mode, o.caseMatching.index,
            o.normalization.index, o.parallel ? 1 : 0, o.threads, o.limit,
            o.scoring.index)
      : lib.filterEx(ptr, u.ptr, u.len, mode, o.caseMatching.index,
            o.normalization.index, o.parallel ? 1 : 0, o.threads, o.limit);
  ```

- [ ] **Step 8: Verify Dart analysis (if Flutter SDK is available)**

  ```
  flutter analyze lib/ffuzzy.dart
  ```

  Or if only Dart SDK:
  ```
  dart analyze lib/ffuzzy.dart
  ```

  Expected: no errors, no warnings about unused parameters.

- [ ] **Step 9: Commit**

  ```
  git add lib/ffuzzy.dart
  git commit -m "feat: add FuzzyScoring enum and scoring param to FuzzyOptions and search methods"
  ```

---

## Self-Review Checklist

**Spec coverage:**

| Spec requirement | Covered by |
|---|---|
| `FFZ_SCORE_FAST=0` default in `ffz_config_default` | Task 1 Step 2 |
| `ffz_fuzzy_rolling()` 2-row rolling DP + simplified bonuses | Task 2 Step 5 |
| FUZZY+FAST: rolling DP for pass 1, greedy for pass 2 | Task 3 Step 3 |
| FUZZY+OFF: prefilter only, score=0 | Task 3 Step 3 |
| Non-FUZZY+OFF: score overridden to 0 | Task 3 Step 3 |
| OFF corpus: insertion order, limit respected, skip sort | Task 4 Step 3f |
| `ffz_corpus_filter` gets `scoring` param | Task 4 Step 2+3 |
| `scan_job` gets `scoring`; `scan_job_run` sets `m->cfg.scoring_mode` | Task 4 Step 3a+3b |
| `ffz_ffi_new_cfg2` | Task 5 Step 1 |
| `ffz_ffi_filter_ex2` | Task 5 Step 2 |
| Old `ffz_ffi_filter_ex` backward-compat default = FAST | Task 5 Step 3 |
| `FuzzyScoring` enum with `.fast`, `.off`, `.nucleo` | Task 6 Step 1 |
| `FuzzyOptions.scoring` field, default `FuzzyScoring.fast` | Task 6 Step 2 |
| Constructor uses `ffz_ffi_new_cfg2` tolerantly | Task 6 Step 4 |
| `scoring` named param on all 10 search methods | Task 6 Step 6 |
| `_rawFilter` uses `ffz_ffi_filter_ex2` tolerantly | Task 6 Step 7 |
| `score=0` in hit when OFF (not omitted) | Guaranteed by C returning 0; Dart stores `r.score` |

**No placeholders detected.**

**Type consistency:** `ffz_scoring_mode` used as `int` at the FFI boundary (`scoring.index` in Dart → `int scoring` in C shim → cast to `(ffz_scoring_mode)`). Consistent across Tasks 4–6.
