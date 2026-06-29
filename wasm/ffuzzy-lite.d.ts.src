/**
 * ffuzzy-lite.js — lite build (ASCII + CJK only, ~14 KB smaller than full).
 *
 * Same exports and API as `ffuzzy.js`; only runtime behaviour differs:
 * - `FuzzyNorm.smart`: **no effect** — accent-stripping tables compiled out.
 * - `FuzzyCase.ignore/smart` for Cyrillic / Greek: **no effect**.
 *
 * ```ts
 * import { ffuzzyInitialize, FuzzyCorpus } from './ffuzzy-lite.js';
 * await ffuzzyInitialize();                 // once at startup
 * const corpus = FuzzyCorpus.strings(items);
 * ```
 *
 * See `ffuzzy.d.ts` / `ffuzzy.js` for the full build.
 */

/**
 * Initialize the WASM engine. Await once before constructing any `FuzzyCorpus`.
 * Idempotent. `opts` is forwarded to the Emscripten module factory.
 */
export declare function ffuzzyInitialize(opts?: Record<string, unknown>): Promise<void>;

/** True once {@link ffuzzyInitialize} has completed. */
export declare function ffuzzyReady(): boolean;

// ═══════════════════════════════════════════════════════════════════════════
// High-level API — naming mirrors ffuzzy.dart
// ═══════════════════════════════════════════════════════════════════════════

export declare const FuzzyCase: {
  readonly respect: 0;
  readonly ignore:  1;
  readonly smart:   2;
};
/**
 * `0` respect · `1` ignore · `2` smart (default)
 *
 * **Lite build**: only ASCII case-folding (`a`–`z` / `A`–`Z`) is active;
 * Cyrillic and Greek uppercase are treated as opaque non-letter codepoints.
 */
export type FuzzyCase = 0 | 1 | 2;

export declare const FuzzyNorm: {
  readonly never: 0;
  readonly smart: 1;
};
/**
 * `0` never · `1` smart (default)
 *
 * **Lite build**: `FFZ_ASCII_NORM` is compiled in — this flag has no effect;
 * `'é'` and `'e'` are never considered equal regardless of the value.
 */
export type FuzzyNorm = 0 | 1;

export declare const FuzzyMode: {
  readonly fuzzy:     0;
  readonly substring: 1;
  readonly prefix:    2;
  readonly postfix:   3;
  readonly exact:     4;
};
/** `0` fuzzy · `1` substring · `2` prefix · `3` postfix · `4` exact */
export type FuzzyMode = 0 | 1 | 2 | 3 | 4;

export declare const FuzzyScoring: { readonly fast: 0; readonly off: 1; readonly nucleo: 2 };
/** `0` fast · `1` off (insertion order) · `2` nucleo (full-matrix DP). */
export type FuzzyScoring = 0 | 1 | 2;

export declare const FuzzyKeyKind: {
  readonly original: 0;
  readonly pinyin:   1;
  readonly initials: 2;
  readonly romaji:   3;
  readonly custom:   100;
};
/** `0` original · `1` pinyin · `2` initials · `3` romaji · `100` custom */
export type FuzzyKeyKind = 0 | 1 | 2 | 3 | 100;

export declare class FuzzyKey {
  readonly text: string;
  readonly kind: number;
  constructor(text: string, kind?: number);
  static kind(text: string, kind: FuzzyKeyKind): FuzzyKey;
}

export declare class FuzzyOptions {
  readonly scoring: FuzzyScoring;
  readonly caseMatching: FuzzyCase;
  readonly normalization: FuzzyNorm;
  readonly parallel: boolean;
  readonly threads: number;
  readonly limit: number;
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

export interface FuzzyHit<T = unknown> {
  raw: T;
  index: number;
  score: number;
  matchedKind: number;
  matchedKey: number;
  /** Populated only when `highlight: true`; empty array otherwise. */
  indices: number[];
}

/** Dot-notation field paths for `T` (up to two levels deep). */
export type FieldPath<T extends Record<string, any>> = {
  [K in keyof T & string]: T[K] extends Record<string, any>
    ? K | `${K}.${keyof T[K] & string}`
    : K;
}[keyof T & string];

export interface FuzzyCorpusInit<T> {
  stringOf?: (item: T) => string;
  options?: Partial<FuzzyOptions> | FuzzyOptions;
  matchPaths?: boolean;
  preferPrefix?: boolean;
}

export declare class FuzzyCorpus<T = string> {
  constructor(items?: Iterable<T>, init?: FuzzyCorpusInit<T>);
  static strings(items?: Iterable<string>, opts?: Omit<FuzzyCorpusInit<string>, 'stringOf'>): FuzzyCorpus<string>;
  /** Corpus searched by one field of `T`. Supports dot-notation; missing keys → `''`. */
  static byKey<T extends Record<string, any>>(
    maps: Iterable<T> | undefined,
    field: FieldPath<T> | (string & {}),
    opts?: Omit<FuzzyCorpusInit<T>, 'stringOf'>,
  ): FuzzyCorpus<T>;
  /** Corpus searched across multiple fields of `T`. `hit.matchedKey` is the field index. Supports dot-notation; missing keys → `''`. */
  static byKeys<T extends Record<string, any>>(
    maps: Iterable<T> | undefined,
    fields: (FieldPath<T> | (string & {}))[],
    opts?: Omit<FuzzyCorpusInit<T>, 'stringOf'>,
  ): FuzzyCorpus<T>;
  readonly length: number;
  add(item: T): void;
  addAll(items: Iterable<T>): void;
  addKey(item: T, keys: FuzzyKey[]): void;
  update(index: number, item: T): void;
  removeAt(index: number): void;
  removeWhere(test: (item: T) => boolean): number;
  refresh(source?: Iterable<T>): void;
  clear(): void;
  /** Fuzzy (subsequence) search. Returns ranked `FuzzyHit<T>[]`. */
  fuzzy(query: string, opts?: Partial<FuzzyOptions>): FuzzyHit<T>[];
  /** Fuzzy search — raw `T[]` only. */
  fuzzyRaws(query: string, opts?: Partial<FuzzyOptions>): T[];
  dispose(): void;
  [Symbol.dispose](): void;
}

export declare function fuzzyCodepointToUtf16(text: string, codepointIndices: number[]): number[];
export declare function highlightHtml(text: string, indices: number[], opts?: { tag?: string }): string;

// ═══════════════════════════════════════════════════════════════════════════
// Low-level API — raw Emscripten module instance
// ═══════════════════════════════════════════════════════════════════════════

export type Ptr = number;
export type FuzzyCorpusPtr = Ptr;
export type FuzzyResultsPtr = Ptr;

export interface FfuzzyModuleInstance {
  _malloc(byteCount: number): Ptr;
  _free(ptr: Ptr): void;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  HEAP32: Int32Array;
  lengthBytesUTF8(str: string): number;
  stringToUTF8(str: string, outPtr: Ptr, maxBytesIncNul: number): void;
  UTF8ToString(ptr: Ptr, maxBytesToRead?: number): string;
  cwrap(ident: string, returnType: 'number' | 'boolean' | 'string' | null, argTypes: Array<'number' | 'boolean' | 'string'>): (...args: unknown[]) => unknown;
  getValue(ptr: Ptr, type: 'i8' | 'i16' | 'i32' | 'i64' | 'float' | 'double' | '*'): number;
  setValue(ptr: Ptr, value: number, type: 'i8' | 'i16' | 'i32' | 'i64' | 'float' | 'double' | '*'): void;
  _ffz_ffi_new(): FuzzyCorpusPtr;
  _ffz_ffi_new_cfg(paths: 0 | 1, preferPrefix: 0 | 1): FuzzyCorpusPtr;
  _ffz_ffi_add(corpus: FuzzyCorpusPtr, strPtr: Ptr, byteLen: number): void;
  _ffz_ffi_add_keyed(corpus: FuzzyCorpusPtr, strPtr: Ptr, byteLen: number, textsPtr: Ptr, lensPtr: Ptr, kindsPtr: Ptr, nkeys: number): void;
  _ffz_ffi_len(corpus: FuzzyCorpusPtr): number;
  _ffz_ffi_clear(corpus: FuzzyCorpusPtr): void;
  _ffz_ffi_free(corpus: FuzzyCorpusPtr): void;
  _ffz_ffi_filter(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, parallel: 0 | 1, threads: number, limit: number): FuzzyResultsPtr;
  _ffz_ffi_filter_ex(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, cm: FuzzyCase, nm: FuzzyNorm, parallel: 0 | 1, threads: number, limit: number): FuzzyResultsPtr;
  _ffz_ffi_filter_raws(corpus: FuzzyCorpusPtr, queryPtr: Ptr, queryByteLen: number, mode: FuzzyMode, cm: FuzzyCase, nm: FuzzyNorm, parallel: 0 | 1, threads: number, limit: number, scoring: FuzzyScoring): FuzzyResultsPtr;
  _ffz_ffi_results_len(results: FuzzyResultsPtr): number;
  _ffz_ffi_results_item(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_score(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_kind(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_key(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_nindices(results: FuzzyResultsPtr, i: number): number;
  _ffz_ffi_results_index(results: FuzzyResultsPtr, i: number, j: number): number;
  _ffz_ffi_results_free(results: FuzzyResultsPtr): void;
}

declare function ffuzzyModuleLite(moduleArg?: Record<string, unknown>): Promise<FfuzzyModuleInstance>;
export default ffuzzyModuleLite;
