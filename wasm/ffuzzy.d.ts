/**
 * ffuzzy.js — full build (Latin/Cyrillic/Greek case-fold + accent-strip).
 *
 * Exports two layers of API from the same file:
 *
 * **High-level** (recommended) — naming mirrors `ffuzzy.dart`. The WASM module
 * is held internally; call `ffuzzyInitialize()` once, then use `FuzzyCorpus`
 * synchronously (no module handle to pass):
 * ```ts
 * import { ffuzzyInitialize, FuzzyCorpus, FuzzyCase } from './ffuzzy.js';
 * await ffuzzyInitialize();                              // once at startup
 * const corpus = FuzzyCorpus.strings(['src/main.rs', 'README.md']);
 * const hits = corpus.fuzzy('src');  // FuzzyHit<string>[]
 * corpus.dispose();
 * ```
 *
 * **Low-level** — direct WASM calls via the raw factory + `FfuzzyModuleInstance`:
 * ```ts
 * import ffuzzyModule from './ffuzzy.js';
 * const M = await ffuzzyModule();
 * const c = M._ffz_ffi_new_cfg(0, 0);
 * ```
 *
 * See `ffuzzy-lite.d.ts` / `ffuzzy-lite.js` for the smaller ASCII+CJK-only build.
 */

/**
 * Initialize the WASM engine. **Await once** before constructing any
 * `FuzzyCorpus` — WASM instantiation is inherently async on the main thread.
 * Idempotent. `opts` is forwarded to the Emscripten module factory (rarely
 * needed, e.g. `{ locateFile }`).
 */
export declare function ffuzzyInitialize(opts?: Record<string, unknown>): Promise<void>;

/** True once {@link ffuzzyInitialize} has completed. */
export declare function ffuzzyReady(): boolean;

// ═══════════════════════════════════════════════════════════════════════════
// High-level API — naming mirrors ffuzzy.dart
// ═══════════════════════════════════════════════════════════════════════════

// ---------------------------------------------------------------------------
// FuzzyCase — mirrors Dart's `FuzzyCase` enum
// ---------------------------------------------------------------------------
export declare const FuzzyCase: {
  readonly respect: 0;
  readonly ignore:  1;
  readonly smart:   2;
};
/** `0` respect · `1` ignore · `2` smart (default) */
export type FuzzyCase = 0 | 1 | 2;

// ---------------------------------------------------------------------------
// FuzzyNorm — mirrors Dart's `FuzzyNorm` enum
// ---------------------------------------------------------------------------
export declare const FuzzyNorm: {
  readonly never: 0;
  readonly smart: 1;
};
/** `0` never · `1` smart (default) */
export type FuzzyNorm = 0 | 1;

// ---------------------------------------------------------------------------
// FuzzyMode — mirrors Dart's five search methods
// ---------------------------------------------------------------------------
export declare const FuzzyMode: {
  readonly fuzzy:     0;
  readonly substring: 1;
  readonly prefix:    2;
  readonly postfix:   3;
  readonly exact:     4;
};
/** `0` fuzzy · `1` substring · `2` prefix · `3` postfix · `4` exact */
export type FuzzyMode = 0 | 1 | 2 | 3 | 4;

// ---------------------------------------------------------------------------
// FuzzyScoring — mirrors Dart's `FuzzyScoring` enum
// ---------------------------------------------------------------------------
export declare const FuzzyScoring: {
  readonly fast:   0;
  readonly off:    1;
  readonly nucleo: 2;
};
/** `0` fast (rolling DP) · `1` off (no ranking, insertion order) · `2` nucleo
 *  (full-matrix DP, highest fidelity). */
export type FuzzyScoring = 0 | 1 | 2;

// ---------------------------------------------------------------------------
// FuzzyKeyKind — mirrors Dart's `FuzzyKeyKind` enum
// ---------------------------------------------------------------------------
export declare const FuzzyKeyKind: {
  readonly original: 0;
  readonly pinyin:   1;
  readonly initials: 2;
  readonly romaji:   3;
  readonly custom:   100;
};
/** `0` original · `1` pinyin · `2` initials · `3` romaji · `100` custom */
export type FuzzyKeyKind = 0 | 1 | 2 | 3 | 100;

// ---------------------------------------------------------------------------
// FuzzyKey — mirrors Dart's `FuzzyKey`
// ---------------------------------------------------------------------------
/** An alternate search key for an item (e.g. host-computed pinyin / romaji). */
export declare class FuzzyKey {
  readonly text: string;
  readonly kind: number;
  constructor(text: string, kind?: number);
  static kind(text: string, kind: FuzzyKeyKind): FuzzyKey;
}

// ---------------------------------------------------------------------------
// FuzzyOptions — mirrors Dart's `FuzzyOptions`
// ---------------------------------------------------------------------------
/** Default search options for a corpus; individual fields can be overridden per call. */
export declare class FuzzyOptions {
  readonly scoring: FuzzyScoring;
  readonly caseMatching: FuzzyCase;
  readonly normalization: FuzzyNorm;
  readonly parallel: boolean;
  readonly threads: number;
  readonly limit: number;
  /** When `true`, `FuzzyHit.indices` is populated (Pass 2 runs). Default `false`. */
  readonly highlight: boolean;
  constructor(init?: {
    scoring?: FuzzyScoring;
    caseMatching?: FuzzyCase;
    normalization?: FuzzyNorm;
    parallel?: boolean;
    threads?: number;
    limit?: number;
    highlight?: boolean;
  });
}

// ---------------------------------------------------------------------------
// FuzzyHit<T> — mirrors Dart's `FuzzyHit<T>`
// ---------------------------------------------------------------------------
/** One result from a corpus search. */
export interface FuzzyHit<T = unknown> {
  /** The original item that matched. */
  raw: T;
  /** Insertion index in the corpus. */
  index: number;
  /** Match score — higher is better; only comparable within one query. */
  score: number;
  /** Which key matched — `FuzzyKeyKind.original` (0) for the item's own text. */
  matchedKind: number;
  /** Index of the matched key within this item's key list. */
  matchedKey: number;
  /**
   * Matched codepoint positions for highlighting. Populated only when the
   * search was called with `highlight: true`; empty array otherwise.
   * Pass to {@link highlightHtml} or use directly for custom rendering.
   */
  indices: number[];
}

// ---------------------------------------------------------------------------
// FuzzyCorpus<T> — mirrors Dart's `FuzzyCorpus<T>`
// ---------------------------------------------------------------------------

/**
 * Dot-notation field paths for `T` (up to two levels deep).
 *
 * Provides IDE autocomplete for both `'gameName'` and `'platform.id'`.
 * Accepts any `string` as a fallback so deeper paths still compile.
 *
 * @example
 * ```ts
 * type K = FieldPath<{ gameName: string; platform: { id: number } }>;
 * // → 'gameName' | 'platform' | 'platform.id'
 * ```
 */
export type FieldPath<T extends Record<string, any>> = {
  [K in keyof T & string]: T[K] extends Record<string, any>
    ? K | `${K}.${keyof T[K] & string}`
    : K;
}[keyof T & string];

/** Constructor options for `FuzzyCorpus`. */
export interface FuzzyCorpusInit<T> {
  /** Extract the searchable text from each item. Default: `String`. */
  stringOf?: (item: T) => string;
  /** Default search options (overridable per call). */
  options?: Partial<FuzzyOptions> | FuzzyOptions;
  /** Path-oriented delimiters (treats `/` specially). Default: `false`. */
  matchPaths?: boolean;
  /** Bias scoring toward matches near the start. Default: `false`. */
  preferPrefix?: boolean;
}

/**
 * A resident corpus of `T` items, searchable by five modes.
 * Build once, search many times; release native memory with `dispose()`.
 *
 * Mirrors `FuzzyCorpus<T>` from `ffuzzy.dart`.
 *
 * @example
 * ```ts
 * // Plain strings (after `await ffuzzyInitialize()`)
 * const corpus = FuzzyCorpus.strings(files.map(f => f.path));
 * corpus.fuzzy('src').forEach(h => console.log(h.raw, h.score));
 *
 * // Typed objects — hit.raw is inferred as Game
 * const corpus = new FuzzyCorpus(games, { stringOf: g => g.gameName });
 * const hit = corpus.prefix('Super')[0];
 * hit.raw.gameName;  // ← typed as Game
 *
 * // byKey / byKeys — T inferred from items, field autocomplete via FieldPath
 * const c = FuzzyCorpus.byKeys(games, ['gameName', 'gameId']);
 * c.exact('101024')[0].raw.gameId;  // typed as Game
 * ```
 */
export declare class FuzzyCorpus<T = string> {
  constructor(items?: Iterable<T>, init?: FuzzyCorpusInit<T>);

  /** Plain-string corpus — item is its own search text. */
  static strings(
    items?: Iterable<string>,
    opts?: Omit<FuzzyCorpusInit<string>, 'stringOf'>,
  ): FuzzyCorpus<string>;

  /**
   * Corpus searched by a single field of `T`.
   * `field` supports dot-notation (`'platform.id'`); missing keys return `''`.
   *
   * @example
   * ```ts
   * const c = FuzzyCorpus.byKey(games, 'gameName');
   * c.fuzzy('gems')[0].raw.gameId; // typed as Game
   * ```
   */
  static byKey<T extends Record<string, any>>(
    maps: Iterable<T> | undefined,
    field: FieldPath<T> | (string & {}),
    opts?: Omit<FuzzyCorpusInit<T>, 'stringOf'>,
  ): FuzzyCorpus<T>;

  /**
   * Corpus searched across multiple fields of `T`.
   * The first field is the primary key; the rest become alternate keys —
   * `hit.matchedKey` is the index into `fields` that produced the hit.
   * Supports dot-notation; missing keys return `''`.
   *
   * @example
   * ```ts
   * const c = FuzzyCorpus.byKeys(games, ['gameName', 'gameId']);
   * c.exact('101024')[0].raw; // typed as Game; matchedKey === 1 (gameId)
   * ```
   */
  static byKeys<T extends Record<string, any>>(
    maps: Iterable<T> | undefined,
    fields: (FieldPath<T> | (string & {}))[],
    opts?: Omit<FuzzyCorpusInit<T>, 'stringOf'>,
  ): FuzzyCorpus<T>;

  readonly length: number;

  add(item: T): void;
  addAll(items: Iterable<T>): void;
  /** Append [item] with explicit alternate search keys (pinyin/romaji/…). */
  addKey(item: T, keys: FuzzyKey[]): void;
  /** Replace the item at [index] (alternate keys dropped). O(n) rebuild. */
  update(index: number, item: T): void;
  /** Remove the item at [index]. O(n) rebuild. */
  removeAt(index: number): void;
  /** Remove every item matching [test]; returns how many were removed. */
  removeWhere(test: (item: T) => boolean): number;
  /** Re-add current items, or replace the whole data set when [source] is given. */
  refresh(source?: Iterable<T>): void;
  clear(): void;

  /**
   * Fuzzy (subsequence) search. Query supports fzf-style operators:
   * `!term` negate · `^term` prefix · `'term` substring · `term$` postfix.
   * Returns ranked `FuzzyHit<T>[]`; `hit.raw` is `T`.
   *
   * For exact / prefix / postfix / substring lookups prefer native
   * `Array.filter` + `===` / `startsWith` / `endsWith` — they are faster
   * for typical browser dataset sizes.
   */
  fuzzy(query: string, opts?: Partial<FuzzyOptions>): FuzzyHit<T>[];

  /** Fuzzy search — raw `T[]` only, no `FuzzyHit` wrapper. Faster: skips highlight-index computation. */
  fuzzyRaws(query: string, opts?: Partial<FuzzyOptions>): T[];

  /** Release native memory. Idempotent. */
  dispose(): void;
  /** `using` statement support. */
  [Symbol.dispose](): void;
}

/**
 * Wrap matched characters in an HTML tag for search-result highlighting.
 * Adjacent matched codepoints are merged: `[0,1,2]` on `'src/main.dart'`
 * → `'<mark>src</mark>/main.dart'`. Non-matched text is HTML-escaped (XSS-safe).
 * Requires `highlight: true` on the search call that produced the hit.
 */
export declare function highlightHtml(
  text: string,
  indices: number[],
  opts?: { tag?: string },
): string;

// ═══════════════════════════════════════════════════════════════════════════
// Low-level API — raw Emscripten module instance
// ═══════════════════════════════════════════════════════════════════════════

/** Opaque linear-memory address. Do not dereference except via `HEAP*` arrays. */
export type Ptr = number;
/** Handle to a native `ffz_corpus *`. */
export type FuzzyCorpusPtr = Ptr;
/** Handle to a native `ffz_results *`; free with `_ffz_ffi_results_free`. */
export type FuzzyResultsPtr = Ptr;

export interface FfuzzyModuleInstance {
  // ── memory ──────────────────────────────────────────────────────────────────
  _malloc(byteCount: number): Ptr;
  _free(ptr: Ptr): void;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  HEAP32: Int32Array;

  // ── UTF-8 helpers ────────────────────────────────────────────────────────────
  lengthBytesUTF8(str: string): number;
  stringToUTF8(str: string, outPtr: Ptr, maxBytesIncNul: number): void;
  UTF8ToString(ptr: Ptr, maxBytesToRead?: number): string;

  // ── Emscripten interop ───────────────────────────────────────────────────────
  cwrap(
    ident: string,
    returnType: 'number' | 'boolean' | 'string' | null,
    argTypes: Array<'number' | 'boolean' | 'string'>,
  ): (...args: unknown[]) => unknown;
  getValue(ptr: Ptr, type: 'i8' | 'i16' | 'i32' | 'i64' | 'float' | 'double' | '*'): number;
  setValue(ptr: Ptr, value: number, type: 'i8' | 'i16' | 'i32' | 'i64' | 'float' | 'double' | '*'): void;

  // ── corpus lifecycle ──────────────────────────────────────────────────────────
  _ffz_ffi_new(): FuzzyCorpusPtr;
  _ffz_ffi_new_cfg(paths: 0 | 1, preferPrefix: 0 | 1): FuzzyCorpusPtr;
  _ffz_ffi_add(corpus: FuzzyCorpusPtr, strPtr: Ptr, byteLen: number): void;
  _ffz_ffi_add_keyed(corpus: FuzzyCorpusPtr, strPtr: Ptr, byteLen: number, textsPtr: Ptr, lensPtr: Ptr, kindsPtr: Ptr, nkeys: number): void;
  _ffz_ffi_len(corpus: FuzzyCorpusPtr): number;
  _ffz_ffi_clear(corpus: FuzzyCorpusPtr): void;
  _ffz_ffi_free(corpus: FuzzyCorpusPtr): void;

  // ── filter ────────────────────────────────────────────────────────────────────
  _ffz_ffi_filter(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, parallel: 0 | 1, threads: number, limit: number): FuzzyResultsPtr;
  _ffz_ffi_filter_ex(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, cm: FuzzyCase, nm: FuzzyNorm, parallel: 0 | 1, threads: number, limit: number): FuzzyResultsPtr;
  /** Like `_ffz_ffi_filter_ex2` but skips Pass 2 (index computation). `result.indices` will be empty. */
  _ffz_ffi_filter_raws(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, cm: FuzzyCase, nm: FuzzyNorm, parallel: 0 | 1, threads: number, limit: number, scoring: FuzzyScoring): FuzzyResultsPtr;

  // ── result accessors ──────────────────────────────────────────────────────────
  _ffz_ffi_results_len(results: FuzzyResultsPtr): number;
  _ffz_ffi_results_item(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_score(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_kind(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_key(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_nindices(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_index(results: FuzzyResultsPtr, i: number, j: number): number;
  _ffz_ffi_results_free(results: FuzzyResultsPtr): void;
}

// ---------------------------------------------------------------------------
// Module factory
// ---------------------------------------------------------------------------
declare function ffuzzyModule(moduleArg?: Record<string, unknown>): Promise<FfuzzyModuleInstance>;
export default ffuzzyModule;
