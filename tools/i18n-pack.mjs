// i18n-pack.mjs — builds a LordOS language pack (a single JSON file).
//
//   node tools/i18n-pack.mjs new fr "Français" French one-other fr_FR
//        → lordos-lang-fr.json with every key in English, to translate
//   node tools/i18n-pack.mjs export pt
//        → lordos-lang-pt.json with the pt language as it is
//
// The pack is installed in LordOS by copying it to ~/Downloads and using Settings → Language and
// region → "Add"; it ends up in ~/.local/share/lordos/locales/<id>/. Available plural
// rules: one-other, zero-one-other, none, east-slavic, polish, czech-slovak, lithuanian, arabic
// (see sdk/i18n.js). The direction ("rtl" for Arabic, Hebrew, Persian, Urdu…) comes from the id.
// Check it with: node tools/i18n-check.mjs lordos-lang-fr.json
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const dir = path.join(root, 'assets/locales');
const read = (id) => {
  const cats = {};
  let meta = {};
  for (const f of fs.readdirSync(path.join(dir, id))) {
    const obj = JSON.parse(fs.readFileSync(path.join(dir, id, f), 'utf8'));
    if (f === 'meta.json') meta = obj; else cats[f.slice(0, -5)] = obj;
  }
  return { meta, catalogs: cats };
};

const [cmd, id, name, english, plural = 'one-other', region = ''] = process.argv.slice(2);
if (!/^[a-z]{2,3}(-[A-Za-z]{2,4})?$/.test(id || '')) {
  console.error('usage: i18n-pack.mjs new <id> "<name>" <English name> [plural] [region]  |  export <id>');
  process.exit(2);
}
let pack;
if (cmd === 'export') pack = read(id);
else if (cmd === 'new') {
  const en = read('en');
  // Languages written right to left: the whole interface is mirrored.
  const RTL = ['ar', 'he', 'fa', 'ur', 'yi', 'ps', 'dv', 'sd', 'ug', 'ckb', 'syr'];
  const direction = RTL.includes(id.split('-')[0]) ? 'rtl' : 'ltr';
  pack = { meta: { id, name: name || id, english: english || id, plural, region, direction }, catalogs: en.catalogs };
} else { console.error('unknown command'); process.exit(2); }
const out = `lordos-lang-${id}.json`;
fs.writeFileSync(out, `${JSON.stringify(pack, null, 2)}\n`);
console.log(`${out}: ${Object.values(pack.catalogs).reduce((n, c) => n + Object.keys(c).length, 0)} strings`);
