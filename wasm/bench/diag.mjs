import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';
await ffuzzyInitialize();

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const mock = JSON.parse(readFileSync(mockPath, 'utf8'));
const corpus = FuzzyCorpus.byKeys(mock, ['gameName', 'gameId']);

const name = 'Super Gems 1000';
const item = mock.find(g => g.gameName === name);
const buf = Buffer.from(item.gameName, 'utf8');
console.log('bytes:', [...buf].map(b => b.toString(16).padStart(2,'0')).join(' '));
console.log('filter hits:         ', mock.filter(g => g.gameName === name).length);
console.log('corpus.exact hits:   ', corpus.exact(name).length);
console.log('corpus.substring hits:', corpus.substring(name).length);

// strings corpus sanity check
const sc = FuzzyCorpus.strings(mock.map(g => g.gameName ?? ''));
console.log('strings.exact hits:  ', sc.exact(name).length);
sc.dispose(); corpus.dispose();
