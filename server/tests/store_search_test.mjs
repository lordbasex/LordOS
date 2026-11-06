// store_search_test.mjs — the Store's search (apps/store/search.js, #37): words in any order, case and
// accents ignored, words joined or apart ("vs code" / "vscode"), the catalog's keywords, ranking (the
// exact name first, every word before some of them), and the real apt/catalog.json.
//
//   node tests/store_search_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const S = await import(path.join(here, '../../apps/store/search.js'));
const catalog = JSON.parse(fs.readFileSync(path.join(here, '../../apt/catalog.json'), 'utf8'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- the pieces ----
eq('fold case and accents', S.fold('Ação Ñandú ÉCOLE Łódź Straße'), 'acao nandu ecole lodz strase');
eq('fold decomposed', S.fold('Café'), 'cafe');
eq('fold keeps other scripts', S.fold('日本語 Кириллица'), '日本語 кириллица');
eq('compact', S.compact('VS Code (OSS)'), 'vscodeoss');
eq('query words', S.parseQuery('  VS   Code '), { words: ['vs', 'code'], joined: 'vscode' });
eq('punctuation splits', S.parseQuery('brave-browser').words, ['brave', 'browser']);
eq('empty query', S.parseQuery('   ').words, []);
eq('at most 8 words', S.parseQuery('a b c d e f g h i j').words.length, 8);
eq('level whole', S.level('code', 'code'), 4);
eq('level start', S.level('codium', 'cod'), 3);
eq('level word start', S.level('node-vscode', 'vscode'), 2);
eq('level inside a word', S.level('libvscode', 'vscode'), 1);
eq('level short word inside a word', S.level('libvs', 'vs'), 0);
eq('level across words', S.level('vs code', 'vscode', 'vscode'), 1);

// ---- items ----
const items = [
  { name: 'Visual Studio Code', id: 'vscode', package: 'code', keywords: ['vscode', 'vs code'], summary: { en: "A code editor" } },
  { name: 'VSCodium', id: 'vscodium', package: 'codium', keywords: ['codium', 'vscode', 'vs code'], summary: { en: 'The editor without telemetry' } },
  { name: 'Cursor', id: 'cursor', package: 'cursor', summary: { en: 'Code editor built around AI agents, based on VS Code' } },
  { name: 'Brave', id: 'brave-browser', package: 'brave-browser', keywords: ['web browser'], summary: { en: 'A web browser that blocks ads', es: 'Un navegador web que bloquea anuncios' } },
  { name: 'Cámara', id: 'camera', package: 'cheese', summary: { en: 'Take photos' } },
  { name: 'Code Aster', id: 'code-aster', package: 'code-aster', summary: { en: 'Finite elements' } },
  { name: 'Rust sockets', id: 'vsock', package: 'librust-vsock-dev', summary: { en: 'Virtio socket support for Rust - Rust source code' } },
];
const names = (q, lang = 'en') => S.search(items, q, (a) => S.catalogFields(a, lang)).map((a) => a.id);

const vs = names('vs code');
eq('"vs code" finds VS Code and VSCodium first', vs.slice(0, 2), ['vscode', 'vscodium']);
eq('"vs code" finds Cursor (VS Code in its summary)', vs.includes('cursor'), true);
eq('"vs code": the joined/keyword matches come before the partial ones', vs.indexOf('vscodium') < vs.indexOf('code-aster'), true);
eq('"vs code": a package with "vs" inside a word and "code" in its summary still matches, after the names', vs.indexOf('vsock') > vs.indexOf('cursor'), true);
eq('"vscode" finds both editors', names('vscode').slice(0, 2), ['vscode', 'vscodium']);
eq('"VSCODE" in capitals', names('VSCODE').slice(0, 2), ['vscode', 'vscodium']);
eq('"visual studio code"', names('visual studio code')[0], 'vscode');
eq('"code studio" (any order)', names('code studio')[0], 'vscode');
eq('"Visual" alone', names('Visual'), ['vscode']);
eq('"codium" (package name)', names('codium')[0], 'vscodium');
eq('"code": the package named code first', names('code')[0], 'vscode');
eq('"brave"', names('brave'), ['brave-browser']);
eq('"browser" (id and keyword)', names('browser'), ['brave-browser']);
eq('"web browser"', names('web browser'), ['brave-browser']);
eq('"webbrowser" (joined)', names('webbrowser'), ['brave-browser']);
eq('"navegador" in Spanish', names('navegador', 'es'), ['brave-browser']);
eq('"navegador" in English finds nothing', names('navegador', 'en'), []);
eq('"camara" without the accent', names('camara'), ['camera']);
eq('"CÁMARA"', names('CÁMARA'), ['camera']);
eq('"zz yy"', names('zz yy'), []);
eq('an empty query keeps the list', names('').length, items.length);
eq('a word only in a summary is no partial match', names('xyz telemetry').length, 0);
eq('one word starting a name is a partial match', names('xyz brave'), ['brave-browser']);
eq('matches()', [S.matches('vs code', ['VSCodium', 'codium'], []), S.matches('nope', ['VSCodium'], []), S.matches('', ['x'], [])], [true, false, true]);

// ---- the real catalog ----
const cat = (q) => S.search(catalog.apps, q, (a) => S.catalogFields(a, 'en')).map((a) => a.id);
eq('catalog: "vs code" brings Visual Studio Code first', cat('vs code')[0], 'vscode');
eq('catalog: "vscode" brings Visual Studio Code first', cat('vscode')[0], 'vscode');
eq('catalog: "chrome"', cat('chrome')[0], 'google-chrome');
eq('catalog: "speech recognition"', cat('speech recognition')[0], 'whisper.cpp');
eq('catalog: "ntfs"', cat('ntfs')[0], 'disk-formats');
for (const a of catalog.apps) {
  eq(`catalog: ${a.id} is found by its own name`, cat(a.name)[0], a.id);
  const k = a.keywords || [];
  eq(`catalog: ${a.id} keywords are short strings`, Array.isArray(k) && k.length <= 12 && k.every((w) => typeof w === 'string' && w.length >= 2 && w.length <= 40), true);
}

// ---- fast enough: 5,000 items, 20 queries ----
const many = Array.from({ length: 5000 }, (_, i) => ({ name: `App number ${i}`, id: `app-${i}`, package: `pkg${i}`, summary: { en: `Summary ${i} for a tool` } }));
const t0 = Date.now();
for (let i = 0; i < 20; i++) S.search(many, i % 2 ? 'tool 42' : 'number 4999', (a) => S.catalogFields(a, 'en'));
const ms = (Date.now() - t0) / 20;
eq(`5,000 items searched in under 60 ms (${ms.toFixed(1)} ms)`, ms < 60, true);

console.log(`store-search-test: ${n - bad}/${n} checks passed`);
process.exit(bad ? 1 : 0);
