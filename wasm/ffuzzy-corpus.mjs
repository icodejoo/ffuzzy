// High-level FuzzyCorpus over the ffuzzy WASM engine. Naming mirrors ffuzzy.dart.
// ES6-compatible: private state uses _underscore convention (no ES2022 # fields),
// so this file can be processed by any modern bundler including OXC / Rolldown.
//
//   import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy';
//   await ffuzzyInitialize();
//   const corpus = FuzzyCorpus.strings(['src/main.rs', 'README.md']);
//   const hits = corpus.fuzzy('src');
//   corpus.dispose();

// --- internal WASM module singleton -----------------------------------------
let _M = null;

export async function ffuzzyInitialize(opts) {
  if (_M) return;
  const factory =
    (typeof ffuzzyModule !== 'undefined') ? ffuzzyModule
    : (typeof ffuzzyModuleLite !== 'undefined') ? ffuzzyModuleLite
    : null;
  if (!factory) throw new Error('ffuzzy: WASM module factory not found in bundle');
  _M = await factory(opts);
}

export function ffuzzyReady() { return _M !== null; }

function _mod() {
  if (!_M) throw new Error('ffuzzy not initialized — call `await ffuzzyInitialize()` once before using FuzzyCorpus');
  return _M;
}

export const FuzzyCase    = Object.freeze({ respect: 0, ignore: 1, smart: 2 });
export const FuzzyNorm    = Object.freeze({ never: 0, smart: 1 });
export const FuzzyMode    = Object.freeze({ fuzzy: 0, substring: 1, prefix: 2, postfix: 3, exact: 4 });
export const FuzzyScoring = Object.freeze({ fast: 0, off: 1, nucleo: 2 });
export const FuzzyKeyKind = Object.freeze({ original: 0, pinyin: 1, initials: 2, romaji: 3, custom: 100 });

export class FuzzyKey {
  constructor(text, kind) {
    this.text = text;
    this.kind = kind === undefined ? FuzzyKeyKind.pinyin : kind;
  }
  static kind(text, kind) { return new FuzzyKey(text, kind); }
}

export class FuzzyOptions {
  constructor(init) {
    const o = init || {};
    this.scoring       = o.scoring       !== undefined ? o.scoring       : FuzzyScoring.fast;
    this.caseMatching  = o.caseMatching  !== undefined ? o.caseMatching  : FuzzyCase.smart;
    this.normalization = o.normalization !== undefined ? o.normalization : FuzzyNorm.smart;
    this.parallel      = o.parallel      !== undefined ? o.parallel      : false;
    this.threads       = o.threads       !== undefined ? o.threads       : 0;
    this.limit         = o.limit         !== undefined ? o.limit         : 0;
    this.highlight     = o.highlight     !== undefined ? o.highlight     : false;
  }
}

// Read a (possibly dotted) path from an object, e.g. 'platform.id'.
// Returns '' for any missing key or null/undefined value — never throws.
function _get(obj, path) {
  if (obj == null) return '';
  if (path.indexOf('.') === -1) { const v = obj[path]; return v == null ? '' : String(v); }
  let cur = obj;
  const parts = path.split('.');
  for (let i = 0; i < parts.length; i++) {
    if (cur == null) return '';
    cur = cur[parts[i]];
  }
  return cur == null ? '' : String(cur);
}

function _utf8(M, s) {
  const n = M.lengthBytesUTF8(s);
  const p = M._malloc(n + 1);
  M.stringToUTF8(s, p, n + 1);
  return [p, n];
}

const SCRATCH_INIT = 256;

export class FuzzyCorpus {
  constructor(items, init) {
    const opts = init || {};
    const stringOf    = opts.stringOf    || String;
    const options     = opts.options;
    const matchPaths  = opts.matchPaths  || false;
    const preferPrefix = opts.preferPrefix || false;

    const M = _mod();
    this._M         = M;
    this._stringOf  = stringOf;
    this._opts      = new FuzzyOptions(options);
    this._items     = [];
    this._keys      = [];
    this._disposed  = false;
    this._scratch   = 0;
    this._scratch4  = 0;
    this._scratchCap = 0;

    const sc = this._opts.scoring;
    this._ptr = M._ffz_ffi_new_cfg2
      ? M._ffz_ffi_new_cfg2(matchPaths ? 1 : 0, preferPrefix ? 1 : 0, sc)
      : M._ffz_ffi_new_cfg(matchPaths ? 1 : 0, preferPrefix ? 1 : 0);
    if (!this._ptr) throw new Error('FuzzyCorpus: native allocation failed (out of memory)');
    this._allocScratch(SCRATCH_INIT);
    if (items) this.addAll(items);
  }

  _allocScratch(cap) {
    const M = this._M;
    if (this._scratchCap) { M._free(this._scratch); M._free(this._scratch4); }
    this._scratch   = M._malloc(cap * 4);
    this._scratch4  = M._malloc(cap * 4 * 4);
    this._scratchCap = cap;
  }

  _ensureScratch(n) {
    if (n > this._scratchCap) this._allocScratch(Math.max(n, this._scratchCap * 2));
  }

  static strings(items, opts) {
    return new FuzzyCorpus(items || [], Object.assign({}, opts, { stringOf: String }));
  }

  static byKey(maps, field, opts) {
    return new FuzzyCorpus(maps || [], Object.assign({}, opts, { stringOf: function(m) { return _get(m, field); } }));
  }

  static byKeys(maps, fields, opts) {
    if (!fields || fields.length === 0) throw new Error('byKeys: fields must not be empty');
    const corpus = new FuzzyCorpus([], Object.assign({}, opts, { stringOf: function(m) { return _get(m, fields[0]); } }));
    const arr = maps || [];
    for (let i = 0; i < arr.length; i++) {
      const item = arr[i];
      if (fields.length === 1) {
        corpus.add(item);
      } else {
        corpus.addKey(item, fields.slice(1).map(function(f) { return new FuzzyKey(_get(item, f), FuzzyKeyKind.custom); }));
      }
    }
    return corpus;
  }

  get length() { this._alive(); return this._items.length; }

  add(item) {
    this._alive();
    this._nativeAdd(item, null);
    this._items.push(item);
    this._keys.push(null);
  }

  addAll(items) { for (let i = 0; i < items.length; i++) this.add(items[i]); }

  addKey(item, keys) {
    this._alive();
    const ks = keys && keys.length ? keys : null;
    this._nativeAdd(item, ks);
    this._items.push(item);
    this._keys.push(ks);
  }

  update(index, item) {
    this._alive(); this._bounds(index);
    this._items[index] = item;
    this._keys[index]  = null;
    this._rebuild();
  }

  removeAt(index) {
    this._alive(); this._bounds(index);
    this._items.splice(index, 1);
    this._keys.splice(index, 1);
    this._rebuild();
  }

  removeWhere(test) {
    this._alive();
    let removed = 0;
    for (let i = this._items.length - 1; i >= 0; i--) {
      if (test(this._items[i])) {
        this._items.splice(i, 1);
        this._keys.splice(i, 1);
        removed++;
      }
    }
    if (removed) this._rebuild();
    return removed;
  }

  refresh(source) {
    this._alive();
    if (source) {
      this._items = Array.from(source);
      this._keys  = this._items.map(function() { return null; });
    }
    this._rebuild();
  }

  clear() {
    this._alive();
    this._M._ffz_ffi_clear(this._ptr);
    this._items.length = 0;
    this._keys.length  = 0;
  }

  /** Fuzzy (subsequence) search. Query supports `!`/`^`/`'`/`$` operators. */
  fuzzy(query, opts)     { return this._search(0, query, opts || {}); }

  /** Fuzzy search — raw items only, no FuzzyHit wrapper. */
  fuzzyRaws(query, opts) { return this._searchRaws(0, query, opts || {}); }

  dispose() {
    if (this._disposed) return;
    this._disposed = true;
    const M = this._M;
    M._ffz_ffi_free(this._ptr);
    if (this._scratchCap) { M._free(this._scratch); M._free(this._scratch4); }
  }

  // ── internals ───────────────────────────────────────────────────────────────
  _nativeAdd(item, keys) {
    const M = this._M;
    if (!keys) {
      const pair = _utf8(M, this._stringOf(item));
      M._ffz_ffi_add(this._ptr, pair[0], pair[1]);
      M._free(pair[0]);
      return;
    }
    const nk = keys.length;
    const ip  = _utf8(M, this._stringOf(item));
    const tP  = M._malloc(4 * nk);
    const lP  = M._malloc(4 * nk);
    const kP  = M._malloc(4 * nk);
    const kPtrs = [];
    try {
      for (let i = 0; i < nk; i++) {
        const kp = _utf8(M, keys[i].text);
        kPtrs.push(kp[0]);
        M.HEAPU32[(tP >> 2) + i] = kp[0];
        M.HEAPU32[(lP >> 2) + i] = kp[1];
        M.HEAP32 [(kP >> 2) + i] = keys[i].kind;
      }
      M._ffz_ffi_add_keyed(this._ptr, ip[0], ip[1], tP, lP, kP, nk);
    } finally {
      for (let i = 0; i < kPtrs.length; i++) M._free(kPtrs[i]);
      M._free(tP); M._free(lP); M._free(kP); M._free(ip[0]);
    }
  }

  _rebuild() {
    this._M._ffz_ffi_clear(this._ptr);
    for (let i = 0; i < this._items.length; i++) this._nativeAdd(this._items[i], this._keys[i]);
  }

  _filter(mode, query, o) {
    const M = this._M;
    const qp = _utf8(M, query);
    let res;
    if (o.highlight) {
      res = M._ffz_ffi_filter_ex2
        ? M._ffz_ffi_filter_ex2(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0, o.scoring || 0)
        : M._ffz_ffi_filter_ex(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0);
    } else {
      res = M._ffz_ffi_filter_raws
        ? M._ffz_ffi_filter_raws(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0, o.scoring || 0)
        : M._ffz_ffi_filter_ex2
          ? M._ffz_ffi_filter_ex2(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0, o.scoring || 0)
          : M._ffz_ffi_filter_ex(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0);
    }
    M._free(qp[0]);
    if (!res) throw new Error('FuzzyCorpus: filter failed (out of memory or invalid parameters)');
    return res;
  }

  _search(mode, query, overrides) {
    this._alive();
    const M  = this._M;
    const o  = Object.assign({}, this._opts, overrides);
    const res = this._filter(mode, query, o);
    const len = M._ffz_ffi_results_len(res);
    if (len === 0) { M._ffz_ffi_results_free(res); return []; }

    const hasBulk = typeof M._ffz_ffi_results_bulk === 'function';
    const hits = new Array(len);

    if (hasBulk && !o.highlight) {
      this._ensureScratch(len);
      const sp        = this._scratch4 >> 2;
      const itemsOff  = sp;
      const scoresOff = sp + len;
      const kindsOff  = sp + len * 2;
      const keysOff   = sp + len * 3;
      M._ffz_ffi_results_bulk(res,
        this._scratch4,
        this._scratch4 + len * 4,
        this._scratch4 + len * 8,
        this._scratch4 + len * 12,
        len);
      const H = M.HEAPU32;
      const I = M.HEAP32;
      for (let i = 0; i < len; i++) {
        const idx = H[itemsOff + i];
        hits[i] = { raw: this._items[idx], index: idx, score: I[scoresOff + i], matchedKind: I[kindsOff + i], matchedKey: H[keysOff + i], indices: [] };
      }
    } else {
      const canIdx = o.highlight && typeof M._ffz_ffi_results_nindices === 'function';
      for (let i = 0; i < len; i++) {
        const idx = M._ffz_ffi_results_item(res, i);
        let indices = [];
        if (canIdx) {
          const ni = M._ffz_ffi_results_nindices(res, i);
          indices = new Array(ni);
          for (let j = 0; j < ni; j++) indices[j] = M._ffz_ffi_results_index(res, i, j);
        }
        hits[i] = { raw: this._items[idx], index: idx, score: M._ffz_ffi_results_score(res, i), matchedKind: M._ffz_ffi_results_kind(res, i), matchedKey: M._ffz_ffi_results_key(res, i), indices };
      }
    }
    M._ffz_ffi_results_free(res);
    return hits;
  }

  _searchRaws(mode, query, overrides) {
    this._alive();
    const M = this._M;
    const o = Object.assign({}, this._opts, overrides);
    let res;
    if (typeof M._ffz_ffi_filter_raws === 'function') {
      const qp = _utf8(M, query);
      res = M._ffz_ffi_filter_raws(this._ptr, qp[0], qp[1], mode, o.caseMatching, o.normalization, o.parallel ? 1 : 0, o.threads || 0, o.limit || 0, o.scoring || 0);
      M._free(qp[0]);
      if (!res) throw new Error('FuzzyCorpus: filter_raws failed (out of memory or invalid parameters)');
    } else {
      res = this._filter(mode, query, o);
    }
    const len = M._ffz_ffi_results_len(res);
    if (len === 0) { M._ffz_ffi_results_free(res); return []; }

    let out;
    if (typeof M._ffz_ffi_results_items_bulk === 'function') {
      this._ensureScratch(len);
      M._ffz_ffi_results_items_bulk(res, this._scratch, len);
      const H    = M.HEAPU32;
      const base = this._scratch >> 2;
      out = new Array(len);
      for (let i = 0; i < len; i++) out[i] = this._items[H[base + i]];
    } else {
      out = [];
      for (let i = 0; i < len; i++) out.push(this._items[M._ffz_ffi_results_item(res, i)]);
    }
    M._ffz_ffi_results_free(res);
    return out;
  }

  _alive() {
    if (this._disposed) throw new Error('FuzzyCorpus used after dispose()');
  }

  _bounds(index) {
    if (index < 0 || index >= this._items.length)
      throw new RangeError('index ' + index + ' out of range [0, ' + this._items.length + ')');
  }
}

// ES6 Symbol.dispose support (optional chaining not needed — check first)
if (typeof Symbol !== 'undefined' && Symbol.dispose) {
  FuzzyCorpus.prototype[Symbol.dispose] = function() { this.dispose(); };
}

export function fuzzyCodepointToUtf16(text, codepointIndices) {
  if (!codepointIndices.length) return [];
  const offsets = [];
  let u16 = 0;
  const chars = Array.from(text);
  for (let i = 0; i < chars.length; i++) {
    offsets.push(u16);
    u16 += chars[i].codePointAt(0) > 0xFFFF ? 2 : 1;
  }
  return codepointIndices.map(function(c) { return (c >= 0 && c < offsets.length) ? offsets[c] : u16; });
}

function _esc(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export function highlightHtml(text, indices, opts) {
  const tag = (opts && opts.tag) ? opts.tag : 'mark';
  if (!indices || indices.length === 0) return _esc(text);
  const set = new Set(indices);
  const codepoints = Array.from(text);
  let out = '', open = false;
  for (let i = 0; i < codepoints.length; i++) {
    const matched = set.has(i);
    if (matched && !open)  { out += '<' + tag + '>'; open = true; }
    if (!matched && open)  { out += '</' + tag + '>'; open = false; }
    out += _esc(codepoints[i]);
  }
  if (open) out += '</' + tag + '>';
  return out;
}
