# @codejoo/ffuzzy

Fast fuzzy search for the web — a WASM port of the [ffuzzy](https://github.com/icodejoo/ffuzzy) C engine.

Five search modes · TypeScript · browser + Node · ~57 KB full / ~43 KB lite

## Install

```sh
npm install @codejoo/ffuzzy
```

## Quick start

The WASM module is managed internally — call `ffuzzyInitialize()` once at
startup, then use `FuzzyCorpus` synchronously, exactly like the Dart API (no
module handle to pass around).

```ts
import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy';

await ffuzzyInitialize();   // once at startup (WASM instantiation is async)

// Plain strings
const corpus = FuzzyCorpus.strings(['src/main.ts', 'README.md', 'package.json']);
corpus.fuzzy('src').forEach(h => console.log(h.raw, h.score));
corpus.dispose();

// Generic objects — hits carry the original object
const files = new FuzzyCorpus(myFiles, { stringOf: f => f.path });
const hit = files.prefix('src/')[0];
hit.raw;  // original object
files.dispose();
```

> Why the one `await`? WASM is instantiated asynchronously on the main thread
> (browsers forbid synchronous compilation of modules >4 KB), so the engine must
> be readied once. After that, every call is synchronous.

## Lite build

~14 KB smaller; covers ASCII + CJK. No Cyrillic/Greek case-fold or accent-strip.

```ts
import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy/lite';

await ffuzzyInitialize();
```

## Search modes

All modes accept an optional second argument to override corpus defaults.

```ts
corpus.fuzzy    ('src m')   // subsequence — supports ! ^ ' $ operators
corpus.substring('src/')    // contiguous substring
corpus.prefix   ('src/')    // must start with query
corpus.postfix  ('.ts')     // must end with query
corpus.exact    ('main.ts') // whole-string match
```

## Options

```ts
import { FuzzyCorpus, FuzzyCase, FuzzyNorm } from '@codejoo/ffuzzy';

const corpus = new FuzzyCorpus(items, {
  stringOf: item => item.name,
  options: {
    caseMatching: FuzzyCase.smart,    // 0 respect · 1 ignore · 2 smart (default)
    normalization: FuzzyNorm.smart,   // 0 never · 1 smart/accent-strip (default)
    limit: 50,                        // max results (0 = unlimited)
    highlight: true,                  // populate FuzzyHit.indices
  },
  matchPaths: false,   // treat '/' as path separator
  preferPrefix: false, // bias toward prefix matches
});
```

Per-call overrides:

```ts
corpus.fuzzy('query', { limit: 10, caseMatching: FuzzyCase.respect });
```

## Multi-key search (pinyin / romaji)

```ts
import { FuzzyCorpus, FuzzyKey, FuzzyKeyKind } from '@codejoo/ffuzzy';

corpus.addKey(item, [
  FuzzyKey.kind('zhongguo', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zg',       FuzzyKeyKind.initials),
]);
```

> Map corpora: `FuzzyCorpus.byKey(rows, 'name')` (one field) or
> `FuzzyCorpus.byKeys(rows, ['name', 'email'])` (multi-field; `hit.matchedKey` is
> the field index). Mutation: `add` / `addAll` / `addKey` / `update` / `removeAt`
> / `removeWhere` / `refresh` / `clear`. Naming mirrors `ffuzzy.dart`.

## Hit highlighting

Pass `{ highlight: true }` on the search call — `FuzzyHit.indices` is empty by
default (`highlight: false`) for speed.

**Option A — `highlightHtml`** (convenience, XSS-safe):

```ts
import { highlightHtml } from '@codejoo/ffuzzy';

const [hit] = corpus.fuzzy('src', { highlight: true });
element.innerHTML = highlightHtml(hit.raw, hit.indices);
// → '<mark>src</mark>/main.dart'
// Custom tag: highlightHtml(hit.raw, hit.indices, { tag: 'b' })
```

**Option B — raw codepoint positions** (for Flutter / custom rendering):

```ts
import { fuzzyCodepointToUtf16 } from '@codejoo/ffuzzy';

const [hit] = corpus.fuzzy('src', { highlight: true });
const u16 = fuzzyCodepointToUtf16(hit.raw, hit.indices);
// apply u16 offsets to DOM Range / TextSpan / highlight API
```

## `using` statement

```ts
using corpus = FuzzyCorpus.strings(items); // auto-disposed at scope exit
```

## FuzzyHit shape

```ts
interface FuzzyHit<T> {
  raw:         T;        // original item
  index:       number;   // insertion index in corpus
  score:       number;   // higher = better; only comparable within one query
  matchedKind: number;   // FuzzyKeyKind of the matched key
  matchedKey:  number;   // key index within the item
  indices:     number[]; // matched codepoint positions — populated only when highlight:true
}
```

## Build from source

```sh
# Dart types live in *.d.ts.src — edit those, then build to regenerate *.d.ts:
cd wasm && npm run build        # appends wrapper → ffuzzy.js / ffuzzy-lite.js + regenerates *.d.ts

# Rebuild the WASM engine (requires Emscripten ≥3.x):
npm run build:engine            # emcc compiles src/*.c → *.engine.mjs, then npm run build
```

## Related

- [ffuzzy on pub.dev](https://pub.dev/packages/ffuzzy) — Flutter / Dart package
- [ffuzzy on GitHub](https://github.com/icodejoo/ffuzzy)

## License

MIT
