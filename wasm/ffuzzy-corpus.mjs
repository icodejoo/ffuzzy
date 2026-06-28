// High-level FuzzyCorpus wrapping the raw ffuzzy WASM module instance.
// Compatible with both ffuzzy.js (full) and ffuzzy-lite.js (lite).
//
// This file is appended verbatim to each .js by the WASM build —
// import directly from the combined bundle. The WASM module is held internally;
// call `ffuzzyInitialize()` once at startup, then use FuzzyCorpus exactly like
// the Dart API (synchronous, no module handle to pass around):
//
//   import { ffuzzyInitialize, FuzzyCorpus } from './ffuzzy.js';
//
//   await ffuzzyInitialize();                            // once, anywhere at startup
//   const corpus = FuzzyCorpus.strings(['src/main.rs', 'README.md']);
//   const hits = corpus.fuzzy('src');   // [{ obj, index, score, matchedKind, matchedKey, indices }]
//   corpus.dispose();

// --- internal WASM module singleton -----------------------------------------
let _M = null;

/// Initialize the WASM engine. Must be awaited once before constructing any
/// FuzzyCorpus (WASM instantiation is inherently async on the main thread).
/// Idempotent — extra calls resolve immediately. `opts` is forwarded to the
/// Emscripten module factory (e.g. { locateFile }), rarely needed.
export async function ffuzzyInitialize(opts) {
  if (_M) return;
  // The factory's exported name differs per bundle (ffuzzyModule /
  // ffuzzyModuleLite); pick whichever this bundle defines. `typeof` on an
  // undeclared identifier is safe (returns 'undefined' without throwing).
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
    throw new Error(
      'ffuzzy not initialized — call `await ffuzzyInitialize()` once before using FuzzyCorpus');
  }
  return _M;
}

export const FuzzyCase = Object.freeze({ respect: 0, ignore: 1, smart: 2 });
export const FuzzyNorm = Object.freeze({ never: 0, smart: 1 });
export const FuzzyMode = Object.freeze({ fuzzy: 0, substring: 1, prefix: 2, postfix: 3, exact: 4 });
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
    caseMatching = FuzzyCase.smart,
    normalization = FuzzyNorm.smart,
    parallel = false,
    threads = 0,
    limit = 0,
    highlight = true,
  } = {}) {
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
  #items;
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
    this.#ptr = M._ffz_ffi_new_cfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    this.#items = [];
    this.addAll(items);
  }

  static strings(items, opts = {}) {
    return new FuzzyCorpus(items, { stringOf: String, ...opts });
  }

  static keyed(maps, field, opts = {}) {
    return new FuzzyCorpus(maps, {
      stringOf: m => String(m[field] ?? ''),
      ...opts,
    });
  }

  get length() { this.#alive(); return this.#items.length; }

  add(item) {
    this.#alive();
    const [p, n] = _utf8(this.#M, this.#stringOf(item));
    this.#M._ffz_ffi_add(this.#ptr, p, n);
    this.#M._free(p);
    this.#items.push(item);
  }

  addAll(items) { for (const it of items) this.add(it); }

  addKeyed(item, keys) {
    this.#alive();
    const M = this.#M;
    const nk = keys.length;
    if (nk === 0) { this.add(item); return; }
    const [ip, ilen] = _utf8(M, this.#stringOf(item));
    // parallel C arrays: const char *[], size_t[], int[]  (WASM32: each entry 4 bytes)
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
      kPtrs.forEach(p => M._free(p));
      M._free(tP); M._free(lP); M._free(kP); M._free(ip);
    }
    this.#items.push(item);
  }

  clear() { this.#alive(); this.#M._ffz_ffi_clear(this.#ptr); this.#items.length = 0; }

  fuzzy    (query, opts = {}) { return this.#search(0, query, opts); }
  substring(query, opts = {}) { return this.#search(1, query, opts); }
  prefix   (query, opts = {}) { return this.#search(2, query, opts); }
  postfix  (query, opts = {}) { return this.#search(3, query, opts); }
  exact    (query, opts = {}) { return this.#search(4, query, opts); }

  #search(mode, query, overrides) {
    this.#alive();
    const M = this.#M;
    const o = { ...this.#opts, ...overrides };
    const [qp, qn] = _utf8(M, query);
    const res = M._ffz_ffi_filter_ex(
      this.#ptr, qp, qn, mode,
      o.caseMatching, o.normalization,
      o.parallel ? 1 : 0, o.threads ?? 0, o.limit ?? 0,
    );
    M._free(qp);
    if (!res) throw new Error('FuzzyCorpus: filter failed (out of memory)');
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
        matchedKey:  M._ffz_ffi_results_key(res, i),
        indices,
      });
    }
    M._ffz_ffi_results_free(res);
    return hits;
  }

  dispose() {
    if (this.#disposed) return;
    this.#disposed = true;
    this.#M._ffz_ffi_free(this.#ptr);
  }

  [Symbol.dispose]() { this.dispose(); }

  #alive() {
    if (this.#disposed) throw new Error('FuzzyCorpus used after dispose()');
  }
}

/**
 * Convert codepoint indices (as in FuzzyHit.indices) to UTF-16 code-unit
 * offsets into `text`, suitable for DOM range / highlight APIs.
 * (JS strings are UTF-16; emoji/astral chars occupy two code units.)
 */
export function fuzzyCodepointToUtf16(text, codepointIndices) {
  if (!codepointIndices.length) return [];
  const offsets = [];
  let u16 = 0;
  for (const ch of text) {            // for…of iterates code points
    offsets.push(u16);
    u16 += ch.codePointAt(0) > 0xFFFF ? 2 : 1;
  }
  return codepointIndices.map(c => c >= 0 && c < offsets.length ? offsets[c] : u16);
}
