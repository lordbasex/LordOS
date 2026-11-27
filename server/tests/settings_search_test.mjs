// settings_search_test.mjs — Settings' search (apps/settings/searchmodel.js) against the real catalogs of every UI
// language: the AI section is found by its functions and capabilities as each language names them (BUG-4 of the
// 0.8.9.6.2 QA: "Escritura" found nothing), by its keywords, and accents don't count.
//   node tests/settings_search_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const { foldSearch, sectionWords } = await import(path.join(root, 'apps/settings/searchmodel.js'));
const { SECTION_KEYS } = await import(path.join(root, 'apps/settings/searchkeys.js'));
import { spawnSync } from 'node:child_process';
let n = 0, bad = 0;
const ok = (name, v) => { n++; if (!v) { bad++; console.log(`✗ ${name}`); } };
const dir = path.join(root, 'assets/locales');
const langs = fs.readdirSync(dir).filter((l) => fs.existsSync(path.join(dir, l, 'settings.json')));
for (const l of langs) {
  const cat = {};
  for (const ns of fs.readdirSync(path.join(dir, l)).filter((f) => f.endsWith('.json')).map((f) => f.slice(0, -5))) {
    const f = path.join(dir, l, `${ns}.json`);
    const d = fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : {};
    for (const [k, v] of Object.entries(d)) cat[`${ns}.${k}`] = v;
  }
  const tr = (k) => (k in cat ? cat[k] : k);
  ok(`${l}: settings.keywords.ai exists`, typeof cat['settings.keywords.ai'] === 'string' && cat['settings.keywords.ai'].length > 10);
  const words = foldSearch(sectionWords('ai', tr));
  for (const k of ['writing.name', 'sentinela.name', 'hands.name', 'settings.aiStore.cap.voice', 'settings.aiStore.cap.brain']) {
    ok(`${l}: AI found by "${cat[k]}"`, words.includes(foldSearch(cat[k])));
  }
  ok(`${l}: AI found by "lumo"`, words.includes('lumo'));
  // (ws170 QA BUG-18) every row label, choice and short note of every page finds its section by its first word
  for (const [id, keys] of Object.entries(SECTION_KEYS)) {
    const all = foldSearch(sectionWords(id, tr));
    for (const k of keys) {
      const first = foldSearch(String(tr(k)).replace(/\{[^}]*\}/g, ' ')).split(/[\s,.:;!?¿¡…()«»"“”]+/u).find((w) => w);
      if (first) ok(`${l}: ${id} found by "${first}" (${k})`, all.includes(first));
    }
  }
  ok(`${l}: another section isn't found by Writing's name`, !foldSearch(sectionWords('sound', tr)).includes(foldSearch(cat['writing.name'])));
}
ok('es: "escritura" finds AI', foldSearch(sectionWords('ai', (k) => ({ 'writing.name': 'Escritura' }[k] || k))).includes(foldSearch('escritura')));
ok('es: "acomodar" and "borde" find Desktop & Dock (settings.dock.snap.label)', (() => {
  const es = JSON.parse(fs.readFileSync(path.join(dir, 'es', 'settings.json'), 'utf8'));
  const w = foldSearch(sectionWords('dock', (k) => (k.startsWith('settings.') ? es[k.slice(9)] ?? k : k)));
  return w.includes('acomodar') && w.includes('borde');
})());
ok('apps/settings/searchkeys.js is up to date (node tools/gen-settings-search.mjs)',
  spawnSync(process.execPath, [path.join(root, 'tools/gen-settings-search.mjs'), '--check']).status === 0);
ok('accents and case do not count', foldSearch('Música Corrección') === 'musica correccion');
console.log(`settings-search-test: ${n} checks, ${bad} failures (${langs.length} languages)`);
process.exit(bad ? 1 : 0);
