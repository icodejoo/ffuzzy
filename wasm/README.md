# @codejoo/ffuzzy

Ranked fuzzy search for the web — a WASM port of the [ffuzzy](https://github.com/icodejoo/ffuzzy) C engine.

Fuzzy search only · TypeScript · browser + Node · ~57 KB full / ~43 KB lite

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
const hit = files.fuzzy('src')[0];
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

## Search

The high-level API exposes **fuzzy search only** — the one mode where WASM
genuinely outperforms native JS (8-55× faster than fuse.js).

For exact / prefix / postfix / substring lookups use native JS directly —
`Array.filter` + `===` / `startsWith` / `endsWith` is faster at typical
browser dataset sizes (< 100k items):

```ts
// exact
items.filter(g => g.gameId === '101024')

// prefix / postfix
items.filter(g => g.gameName.startsWith('Super'))
items.filter(g => g.gameName.endsWith('1000'))

// fuzzy — corpus is indispensable here
corpus.fuzzy('gems', { limit: 50 })   // ranked, scored, multi-key
```

`fuzzy` supports fzf-style operators: `!term` negate · `^term` prefix-force ·
`'term` substring-force · `term$` postfix-force.

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

## Typed object search — `byKey` / `byKeys`

`T` is inferred from the items array, so `hit.raw` is fully typed:

```ts
interface Game { gameId: string; gameName: string; platform: { id: string } }

// Single field — hit.raw is Game
const byName = FuzzyCorpus.byKey(games, 'gameName');
byName.fuzzy('gems')[0].raw.gameId;   // ✓ typed as string

// Multiple fields — matchedKey tells you which field matched
const corpus = FuzzyCorpus.byKeys(games, ['gameName', 'gameId']);
const hit = corpus.fuzzy('gems')[0];
hit.raw.gameName;    // ✓ Game
hit.matchedKey;      // 0 = gameName matched, 1 = gameId matched

// Dot-notation for nested fields (IDE autocomplete included)
const byPlatform = FuzzyCorpus.byKey(games, 'platform.id');
byPlatform.fuzzy('226')[0]?.raw.gameId;  // ✓
```

Missing or null fields are silently treated as `''` — no runtime errors.

## Multi-key search (pinyin / romaji)

```ts
import { FuzzyCorpus, FuzzyKey, FuzzyKeyKind } from '@codejoo/ffuzzy';

corpus.addKey(item, [
  FuzzyKey.kind('zhongguo', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zg',       FuzzyKeyKind.initials),
]);
```

Mutation: `add` / `addAll` / `addKey` / `update` / `removeAt` / `removeWhere` / `refresh` / `clear`.

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
