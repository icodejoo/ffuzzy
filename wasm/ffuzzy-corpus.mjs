// High-level FuzzyCorpus over the ffuzzy WASM engine. Naming mirrors ffuzzy.dart
// (strings / byKey / byKeys / add / addAll / addKey / update / removeAt /
// removeWhere / refresh / clear; fuzzy / substring / prefix / postfix / exact).
//
// This file is appended verbatim to each engine bundle by build.mjs. The WASM
// module is held internally; call `ffuzzyInitialize()` once at startup, then use
// FuzzyCorpus synchronously (no module handle to pass), exactly like Dart:
//
//   import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy';
//   await ffuzzyInitialize();
//   const corpus = FuzzyCorpus.strings(['src/main.rs', 'README.md']);
//   const hits = corpus.fuzzy('src');
//   corpus.dispose();

// --- internal WASM module singleton -----------------------------------------
let _M = null;

/// Initialize the WASM engine. Await once before constructing any FuzzyCorpus
/// (WASM instantiation is async on the main thread). Idempotent.
export async function ffuzzyInitialize(opts) {
  if (_M) return;
  // The factory's export name differs per bundle (full vs lite); pick whichever
  // this bundle defines. `typeof` on an undeclared identifier is safe.
  const factory =
    (typeof ffuzzyModule !== 'undefined') ? ffuzzyModule
    : (typeof ffuzzyModuleLite !== 'undefined') ? ffuzzyModuleLite
    : null;
  if (!factory) throw new Error('ffuzzy: WASM module factory not found in bundle');
  _M = await factory(opts);
}

/// True once ffuzzyInitialize() has completed.
export function ffuzzyReady() { return _M !== null; }

function _mod() {
  if (!_M) {
    throw new Error('ffuzzy not initialized — call `await ffuzzyInitialize()` once before using FuzzyCorpus');
  }
  return _M;
}

export const FuzzyCase = Object.freeze({ respect: 0, ignore: 1, smart: 2 });
export const FuzzyNorm = Object.freeze({ never: 0, smart: 1 });
export const FuzzyMode = Object.freeze({ fuzzy: 0, substring: 1, prefix: 2, postfix: 3, exact: 4 });
export const FuzzyScoring = Object.freeze({ fast: 0, off: 1, nucleo: 2 });
export const FuzzyKeyKind = Object.freeze({ original: 0, pinyin: 1, initials: 2, romaji: 3, custom: 100 });

export class FuzzyKey {
  constructor(text, kind = FuzzyKeyKind.pinyin) {
    this.text = text;
    this.kind = kind;
  }
  static kind(text, kind) { return new FuzzyKey(text, kind); }
}

export class FuzzyOptions {
  constructor({
    scoring = FuzzyScoring.fast,
    caseMatching = FuzzyCase.smart,
    normalization = FuzzyNorm.smart,
    parallel = false,
    threads = 0,
    limit = 0,
    highlight = true,
  } = {}) {
    this.scoring = scoring;
    this.caseMatching = caseMatching;
    this.normalization = normalization;
    this.parallel = parallel;
    this.threads = threads;
    this.limit = limit;
    this.highlight = highlight;
  }
}

function _utf8(M, s) {
  const n = M.lengthBytesUTF8(s);
  const p = M._malloc(n + 1);
  M.stringToUTF8(s, p, n + 1);
  return [p, n];
}

export class FuzzyCorpus {
  #M;
  #ptr;
  #items = [];
  #keys = [];
  #stringOf;
  #opts;
  #disposed = false;

  constructor(items = [], {
    stringOf = String,
    options,
    matchPaths = false,
    preferPrefix = false,
  } = {}) {
    const M = _mod();
    this.#M = M;
    this.#stringOf = stringOf;
    this.#opts = new FuzzyOptions(options);
    const sc = this.#opts.scoring;
    this.#ptr = M._ffz_ffi_new_cfg2
      ? M._ffz_ffi_new_cfg2(matchPaths ? 1 : 0, preferPrefix ? 1 : 0, sc)
      : M._ffz_ffi_new_cfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    if (!this.#ptr) throw new Error('FuzzyCorpus: native allocation failed (out of memory)');
    this.addAll(items);
  }

  /** Plain-string corpus — each item is its own search text. */
  static strings(items = [], opts = {}) {
    return new FuzzyCorpus(items, { ...opts, stringOf: String });
  }

  /** Record-map corpus searched by one string [field]. */
  static byKey(maps = [], field, opts = {}) {
    return new FuzzyCorpus(maps, { ...opts, stringOf: (m) => String(m[field] ?? '') });
  }

  /** Record-map corpus searched across multiple [fields]. The first is the
   *  primary key; the rest become alternate keys. `hit.matchedKey` is the index
   *  into [fields] that produced the hit. */
  static byKeys(maps = [], fields, opts = {}) {
    if (!fields || fields.length === 0) throw new Error('byKeys: fields must not be empty');
    const corpus = new FuzzyCorpus([], { ...opts, stringOf: (m) => String(m[fields[0]] ?? '') });
    for (const item of maps) {
      if (fields.length === 1) {
        corpus.add(item);
      } else {
        corpus.addKey(item, fields.slice(1).map((f) => new FuzzyKey(String(item[f] ?? ''), FuzzyKeyKind.custom)));
      }
    }
    return corpus;
  }

  get length() { this.#alive(); return this.#items.length; }

  add(item) {
    this.#alive();
    this.#nativeAdd(item, null);
    this.#items.push(item);
    this.#keys.push(null);
  }

  addAll(items) { for (const it of items) this.add(it); }

  /** Append [item] with explicit alternate search [keys] (pinyin/romaji/...).
   *  The original text (`stringOf(item)`) is added automatically. */
  addKey(item, keys) {
    this.#alive();
    const ks = keys && keys.length ? keys : null;
    this.#nativeAdd(item, ks);
    this.#items.push(item);
    this.#keys.push(ks);
  }

  /** Replace the item at [index] (its alternate keys are dropped). O(n) rebuild. */
  update(index, item) {
    this.#alive(); this.#bounds(index);
    this.#items[index] = item;
    this.#keys[index] = null;
    this.#rebuild();
  }

  /** Remove the item at [index]. O(n) rebuild. */
  removeAt(index) {
    this.#alive(); this.#bounds(index);
    this.#items.splice(index, 1);
    this.#keys.splice(index, 1);
    this.#rebuild();
  }

  /** Remove every item for which [test] is true; returns how many were removed. */
  removeWhere(test) {
    this.#alive();
    let removed = 0;
    for (let i = this.#items.length - 1; i >= 0; i--) {
      if (test(this.#items[i])) {
        this.#items.splice(i, 1);
        this.#keys.splice(i, 1);
        removed++;
      }
    }
    if (removed) this.#rebuild();
    return removed;
  }

  /** Re-add current items (after their text changed), or replace the whole data
   *  set when [source] is given. */
  refresh(source) {
    this.#alive();
    if (source) {
      this.#items = [...source];
      this.#keys = this.#items.map(() => null);
    }
    this.#rebuild();
  }

  /** Remove all items; the corpus stays usable. */
  clear() {
    this.#alive();
    this.#M._ffz_ffi_clear(this.#ptr);
    this.#items.length = 0;
    this.#keys.length = 0;
  }

  fuzzy    (query, opts = {}) { return this.#search(0, query, opts); }
  substring(query, opts = {}) { return this.#search(1, query, opts); }
  prefix   (query, opts = {}) { return this.#search(2, query, opts); }
  postfix  (query, opts = {}) { return this.#search(3, query, opts); }
  exact    (query, opts = {}) { return this.#search(4, query, opts); }

  dispose() {
    if (this.#disposed) return;
    this.#disposed = true;
    this.#M._ffz_ffi_free(this.#ptr);
  }

  [Symbol.dispose]() { this.dispose(); }

  // ── internals ───────────────────────────────────────────────────────────
  #nativeAdd(item, keys) {
    const M = this.#M;
    if (!keys) {
      const [p, n] = _utf8(M, this.#stringOf(item));
      M._ffz_ffi_add(this.#ptr, p, n);
      M._free(p);
      return;
    }
    const nk = keys.length;
    const [ip, ilen] = _utf8(M, this.#stringOf(item));
    const tP = M._malloc(4 * nk);
    const lP = M._malloc(4 * nk);
    const kP = M._malloc(4 * nk);
    const kPtrs = [];
    try {
      for (let i = 0; i < nk; i++) {
        const [p, len] = _utf8(M, keys[i].text);
        kPtrs.push(p);
        M.HEAPU32[(tP >> 2) + i] = p;
        M.HEAPU32[(lP >> 2) + i] = len;
        M.HEAP32 [(kP >> 2) + i] = keys[i].kind;
      }
      M._ffz_ffi_add_keyed(this.#ptr, ip, ilen, tP, lP, kP, nk);
    } finally {
      for (const p of kPtrs) M._free(p);
      M._free(tP); M._free(lP); M._free(kP); M._free(ip);
    }
  }

  #rebuild() {
    this.#M._ffz_ffi_clear(this.#ptr);
    for (let i = 0; i < this.#items.length; i++) this.#nativeAdd(this.#items[i], this.#keys[i]);
  }

  #search(mode, query, overrides) {
    this.#alive();
    const M = this.#M;
    const o = { ...this.#opts, ...overrides };
    const [qp, qn] = _utf8(M, query);
    const res = M._ffz_ffi_filter_ex2
      ? M._ffz_ffi_filter_ex2(this.#ptr, qp, qn, mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads ?? 0, o.limit ?? 0, o.scoring ?? 0)
      : M._ffz_ffi_filter_ex(this.#ptr, qp, qn, mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads ?? 0, o.limit ?? 0);
    M._free(qp);
    if (!res) throw new Error('FuzzyCorpus: filter failed (out of memory or invalid parameters)');
    const hits = [];
    const len = M._ffz_ffi_results_len(res);
    for (let i = 0; i < len; i++) {
      const index = M._ffz_ffi_results_item(res, i);
      let indices = [];
      if (o.highlight !== false) {
        const ni = M._ffz_ffi_results_nindices(res, i);
        indices = Array.from({ length: ni }, (_, j) => M._ffz_ffi_results_index(res, i, j));
      }
      hits.push({
        obj: this.#items[index],
        index,
        score: M._ffz_ffi_results_score(res, i),
        matchedKind: M._ffz_ffi_results_kind(res, i),
        matchedKey: M._ffz_ffi_results_key(res, i),
        indices,
      });
    }
    M._ffz_ffi_results_free(res);
    return hits;
  }

  #alive() {
    if (this.#disposed) throw new Error('FuzzyCorpus used after dispose()');
  }

  #bounds(index) {
    if (index < 0 || index >= this.#items.length) {
      throw new RangeError(`index ${index} out of range [0, ${this.#items.length})`);
    }
  }
}

/**
 * Convert codepoint indices (as in FuzzyHit.indices) to UTF-16 code-unit
 * offsets into `text`, suitable for DOM range / highlight APIs.
 */
export function fuzzyCodepointToUtf16(text, codepointIndices) {
  if (!codepointIndices.length) return [];
  const offsets = [];
  let u16 = 0;
  for (const ch of text) {
    offsets.push(u16);
    u16 += ch.codePointAt(0) > 0xFFFF ? 2 : 1;
  }
  return codepointIndices.map((c) => (c >= 0 && c < offsets.length ? offsets[c] : u16));
}
