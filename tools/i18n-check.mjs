// i18n-check.mjs — checks the LordOS language catalogs.
//
//   node tools/i18n-check.mjs            every language in assets/locales
//   node tools/i18n-check.mjs pt         just one (or a pack: path to lordos-lang-xx.json)
//
// Reports: keys used in the code (t('namespace.key')) that are not in the reference
// catalog (en), keys each language is missing or has in excess compared to en, and
// {x} variables that do not match. Exits with code 1 if something is missing. A partial language
// (meta.json "partial": true, docs/LANGUAGES.md) may miss keys: those fall back to English, and the
// check prints its coverage instead of failing (and of the core set, tools/i18n-core.json); its variables
// must still match, and each plural object must have exactly the forms of the language's plural rule
// (sdk/plurals.js).
// With every language it also runs tools/err-check.mjs: error/status codes used by the server
// without an English string, and prose (or Spanish) in ERR lines and JS exceptions, and
// tools/brand-check.mjs: third-party brand names in the catalogs, Help, README and CHANGELOG.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkErrors } from './err-check.mjs';
import { checkBrands } from './brand-check.mjs';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const { PLURALS, pluralCategories } = await import(path.join(root, 'sdk/plurals.js'));   // the rules sdk/i18n.js uses
const dir = path.join(root, 'assets/locales');
const REF = 'en';

function loadLang(id) {
  if (id.endsWith('.json') && fs.existsSync(id)) {
    const pack = JSON.parse(fs.readFileSync(id, 'utf8'));
    const { format, ...cats } = pack.catalogs;      // format.json is not texts
    return { id: pack.meta.id, cats, partial: pack.meta.partial === true, plural: pack.meta.plural };
  }
  const cats = {};
  for (const f of fs.readdirSync(path.join(dir, id))) {
    if (f.endsWith('.json') && f !== 'meta.json' && f !== 'format.json') cats[f.slice(0, -5)] = JSON.parse(fs.readFileSync(path.join(dir, id, f), 'utf8'));
  }
  let meta = {};
  try { meta = JSON.parse(fs.readFileSync(path.join(dir, id, 'meta.json'), 'utf8')); } catch (e) { /* en */ }
  return { id, cats, partial: meta.partial === true, plural: meta.plural || 'one-other' };
}

const flat = (cats) => Object.fromEntries(Object.entries(cats).flatMap(([ns, o]) => Object.entries(o).map(([k, v]) => [`${ns}.${k}`, v])));
const vars = (v) => [...new Set((typeof v === 'object' ? Object.values(v).join(' ') : String(v)).match(/\{\w+\}/g) || [])].sort().join(',');

// keys used in the code
function usedKeys() {
  const out = new Map();
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) { if (!['node_modules', 'assets'].includes(e.name)) walk(p); continue; }
      if (!/\.(jsx?|mjs)$/.test(e.name) || e.name.endsWith('.bundle.js')) continue;
      const src = fs.readFileSync(p, 'utf8').replace(/^\s*\/\/.*$/gm, '');
      for (const m of src.matchAll(/\b(?:t|T|tr)\(\s*'([a-z][\w-]*\.[\w.-]+)'/g)) out.set(m[1], path.relative(root, p));
    }
  };
  for (const d of ['apps', 'themes', 'sdk']) walk(path.join(root, d));
  return out;
}

const ref = flat(loadLang(REF).cats);
let bad = 0;
for (const [k, file] of usedKeys()) if (!(k in ref)) { console.log(`✗ ${k} (used in ${file}) is not in ${REF}`); bad++; }

// The core set (tools/i18n-core.json): the English keys a new language translates first.
const CORE = JSON.parse(fs.readFileSync(path.join(root, 'tools/i18n-core.json'), 'utf8')).namespaces;
const inCore = (k) => {
  const dot = k.indexOf('.'), sel = CORE[k.slice(0, dot)], rest = k.slice(dot + 1);
  return sel === '*' || (Array.isArray(sel) && sel.some((p) => (p.endsWith('.') ? rest.startsWith(p) : rest === p)));
};
const coreKeys = Object.keys(ref).filter(inCore);
for (const [ns, sel] of Object.entries(CORE)) {
  const sels = sel === '*' ? [''] : sel;
  for (const p of sels) if (!coreKeys.some((k) => k.startsWith(`${ns}.${p}`))) { console.log(`✗ tools/i18n-core.json: ${ns} ${p || '*'} matches no English key`); bad++; }
}

const langs = process.argv[2] ? [process.argv[2]] : fs.readdirSync(dir).filter((d) => d !== REF && fs.existsSync(path.join(dir, d, 'meta.json')));
for (const id of langs) {
  const l = loadLang(id);
  const cur = flat(l.cats);
  const missing = Object.keys(ref).filter((k) => !(k in cur));
  const extra = Object.keys(cur).filter((k) => !(k in ref));
  const varDiff = Object.keys(ref).filter((k) => k in cur && vars(ref[k]) !== vars(cur[k]));
  const have = Object.keys(ref).length - missing.length;
  // plurals: the language's rule must exist, and each translated plural object has exactly its forms
  // ("few" and "many" in Russian, the six Arabic ones, only "other" in Japanese)
  const forms = pluralCategories(l.plural || 'one-other');
  if (!forms) { console.log(`✗ ${l.id}: unknown plural rule "${l.plural}" (one of: ${Object.keys(PLURALS).join(', ')})`); bad++; }
  const pluralBad = forms ? Object.entries(cur).filter(([, v]) => v && typeof v === 'object').map(([k, v]) => {
    const has = Object.keys(v), lack = forms.filter((c) => !has.includes(c)), more = has.filter((c) => c !== 'other' && !forms.includes(c));   // "other" is always allowed (CLDR's form for fractions)
    return lack.length || more.length ? `  plural  ${k}: ${lack.length ? `needs ${lack.join(', ')}` : ''}${lack.length && more.length ? '; ' : ''}${more.length ? `${l.plural} has no ${more.join(', ')}` : ''}` : null;
  }).filter(Boolean) : [];
  console.log(`${l.id}: ${Object.keys(cur).length}/${Object.keys(ref).length} keys` +
    `${l.partial ? ` (partial: ${(100 * have / Object.keys(ref).length).toFixed(1)}%, the rest in English)` : ''}` +
    `${missing.length && !l.partial ? `, ${missing.length} missing` : ''}${extra.length ? `, ${extra.length} extra` : ''}${varDiff.length ? `, different variables in ${varDiff.length}` : ''}`);
  if (l.partial) {
    // per namespace: complete ones by name, the started ones with their share
    const byNs = {};
    for (const k of Object.keys(ref)) { const ns = k.slice(0, k.indexOf('.')); (byNs[ns] ||= [0, 0])[1]++; if (k in cur) byNs[ns][0]++; }
    const done = Object.keys(byNs).filter((ns) => byNs[ns][0] === byNs[ns][1]);
    const started = Object.keys(byNs).filter((ns) => byNs[ns][0] && byNs[ns][0] < byNs[ns][1]);
    const coreHave = coreKeys.filter((k) => k in cur).length;
    console.log(`  core set (tools/i18n-core.json): ${coreHave}/${coreKeys.length}`);
    console.log(`  complete: ${done.join(', ') || '—'}`);
    if (started.length) console.log(`  started: ${started.map((ns) => `${ns} ${byNs[ns][0]}/${byNs[ns][1]}`).join(', ')}`);
  } else for (const k of missing) console.log(`  missing ${k}`);
  for (const k of extra) console.log(`  extra   ${k}`);
  for (const k of varDiff) console.log(`  vars    ${k}: ${vars(ref[k])} ≠ ${vars(cur[k])}`);
  for (const line of pluralBad) console.log(line);
  bad += (l.partial ? 0 : missing.length) + varDiff.length + pluralBad.length;
}
if (!process.argv[2]) bad += checkErrors().problems.length + checkBrands().problems.length;
process.exit(bad ? 1 : 0);
