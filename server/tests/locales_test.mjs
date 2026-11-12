// locales_test.mjs — the plural rules (sdk/plurals.js) against CLDR 45's integer examples, and every
// bundled language's meta.json and format.json (assets/locales/<id>/): a known rule, a region, the
// direction, 12 months and 7 days, date patterns with only LordOS's placeholders, native digits ("native").
//
//   node tests/locales_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { PLURALS, pluralCategories } = await import(path.join(here, '../../sdk/plurals.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// CLDR 45 plurals.xml, integer samples of each category
const CASES = {
  'one-other': { one: [1], other: [0, 2, 3, 11, 21, 100, 1000] },
  'zero-one-other': { one: [0, 1], other: [2, 3, 11, 21, 100] },
  none: { other: [0, 1, 2, 5, 11, 100] },
  'east-slavic': { one: [1, 21, 31, 101, 1001], few: [2, 3, 4, 22, 24, 102, 1002], many: [0, 5, 11, 12, 13, 14, 19, 20, 100, 111, 112] },
  polish: { one: [1], few: [2, 3, 4, 22, 23, 24, 102], many: [0, 5, 11, 12, 13, 14, 21, 25, 100, 112] },
  'czech-slovak': { one: [1], few: [2, 3, 4], other: [0, 5, 11, 100] },
  lithuanian: { one: [1, 21, 31, 101], few: [2, 9, 22, 29, 102], other: [0, 10, 11, 15, 19, 20, 30, 111, 112] },
  romanian: { one: [1], few: [0, 2, 16, 19, 101, 102, 119, 1001], other: [20, 21, 35, 100, 120, 1000] },
  arabic: { zero: [0], one: [1], two: [2], few: [3, 10, 103, 110, 1003], many: [11, 26, 99, 111, 1011], other: [100, 101, 102, 200, 1000] },
};
for (const [rule, cats] of Object.entries(CASES)) {
  for (const [cat, nums] of Object.entries(cats)) for (const x of nums) eq(`${rule}(${x})`, PLURALS[rule](x), cat);
  eq(`${rule} categories`, pluralCategories(rule), ['zero', 'one', 'two', 'few', 'many', 'other'].filter((c) => c in cats));
}
eq('every rule is tested', Object.keys(PLURALS).sort(), Object.keys(CASES).sort());
eq('unknown rule', pluralCategories('klingon'), null);

// the bundled languages
const dir = path.join(here, '../../assets/locales');
const PH = new Set(['weekday', 'wd', 'day', 'dd', 'month', 'mon', 'mm', 'yyyy']);
const RTL = ['ar', 'fa', 'ur', 'he'];
const ids = fs.readdirSync(dir).filter((d) => fs.existsSync(path.join(dir, d, 'meta.json')));
eq('the languages are many', ids.length >= 32, true);
for (const id of ids) {
  const meta = JSON.parse(fs.readFileSync(path.join(dir, id, 'meta.json'), 'utf8'));
  eq(`${id}: meta id`, meta.id, id);
  eq(`${id}: plural rule exists`, meta.plural in PLURALS, true);
  eq(`${id}: names`, typeof meta.name === 'string' && meta.name.length > 1 && typeof meta.english === 'string' && meta.english.length > 1, true);
  eq(`${id}: region of the language`, new RegExp(`^${id.split('-')[0]}_[A-Z]{2}$`).test(meta.region || ''), true);
  eq(`${id}: direction`, meta.direction, RTL.includes(id.split('-')[0]) ? 'rtl' : 'ltr');
  const fp = path.join(dir, id, 'format.json');
  eq(`${id}: has format.json`, fs.existsSync(fp), true);
  if (!fs.existsSync(fp)) continue;
  const f = JSON.parse(fs.readFileSync(fp, 'utf8'));
  eq(`${id}: 12 months`, [f.months?.length, f.monthsShort?.length], [12, 12]);
  eq(`${id}: 7 days`, [f.weekdays?.length, f.weekdaysShort?.length], [7, 7]);
  eq(`${id}: names are trimmed and set`, [...f.months, ...f.monthsShort, ...f.weekdays, ...f.weekdaysShort].every((s) => typeof s === 'string' && s && s === s.trim()), true);
  for (const style of ['short', 'long', 'dayMonth', 'numeric']) {
    const p = f.date?.[style];
    eq(`${id}: date.${style} placeholders`, typeof p === 'string' && [...p.matchAll(/\{(\w+)\}/g)].every((m) => PH.has(m[1])), true);
  }
  eq(`${id}: numeric date has day, month, year`, /\{dd\}|\{day\}/.test(f.date.numeric) && /\{mm\}/.test(f.date.numeric) && /\{yyyy\}/.test(f.date.numeric), true);
  eq(`${id}: separators`, typeof f.decimal === 'string' && f.decimal.length === 1 && typeof f.group === 'string' && f.group !== f.decimal, true);
  eq(`${id}: clock`, typeof f.hour12, 'boolean');
  if (f.hour12) eq(`${id}: AM and PM words`, !!(f.am && f.pm), true);
  const native = (v, where) => {
    if (v === undefined || v === null) return;
    eq(`${where}: ten native digits`, typeof v.digits === 'string' && [...v.digits].length === 10 && !/[0-9]/.test(v.digits), true);
    for (const k of ['decimal', 'group']) if (v[k] !== undefined) eq(`${where}: native ${k}`, typeof v[k] === 'string' && v[k].length === 1, true);
  };
  native(f.native, id);
  for (const [r, v] of Object.entries(f.regions || {})) {
    eq(`${id}: region ${r} is of the language`, r.startsWith(`${id.split('-')[0]}_`), true);
    native(v.native, `${id} ${r}`);
  }
}

console.log(bad ? `locales: ${bad} of ${n} failed` : `locales: ${n}/${n} ok (${ids.length} languages)`);
process.exit(bad ? 1 : 0);
