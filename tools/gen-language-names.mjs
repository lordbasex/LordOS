// gen-language-names.mjs — writes assets/locales/<lang>/languages.json: the name of every language glibc has a
// region for (/usr/share/i18n/SUPPORTED), in each interface language, from Unicode CLDR through Node.js's ICU
// (Intl.DisplayNames). Settings → Language & Region → Region names each row's language with it ("Basque · France
// (eu_FR)", #239; sdk/localename.js regionLabel). A name CLDR lacks in a language falls back to the English one; a
// code CLDR doesn't know at all is left out (the row then shows only its country and code).
//
//   node tools/gen-language-names.mjs [path/to/SUPPORTED]
//
// Without SUPPORTED (a machine that isn't Debian) it keeps the codes the English catalog already has.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const dir = path.join(root, 'assets/locales');
const supported = process.argv[2] || '/usr/share/i18n/SUPPORTED';
const enFile = path.join(dir, 'en', 'languages.json');

let codes;
if (fs.existsSync(supported)) {
  codes = fs.readFileSync(supported, 'utf8').split('\n').map((l) => (/^([a-z]{2,3})(?=[_.@ ])/.exec(l) || [])[1]).filter(Boolean);
} else if (fs.existsSync(enFile)) {
  codes = Object.keys(JSON.parse(fs.readFileSync(enFile, 'utf8')));
} else {
  console.error(`gen-language-names: no ${supported} and no ${enFile}`);
  process.exit(1);
}
codes = [...new Set(codes)].sort();

const nameIn = (lang, code) => {
  try {
    const v = new Intl.DisplayNames([lang], { type: 'language', fallback: 'none' }).of(code);
    return v && v !== code ? v : '';
  } catch (e) { return ''; }
};
const english = Object.fromEntries(codes.map((c) => [c, nameIn('en', c)]).filter(([, v]) => v));
// a list row starts with it: capitalised as the language writes the start of a sentence
const cap = (lang, s) => (s ? s[0].toLocaleUpperCase(lang) + s.slice(1) : s);

for (const lang of fs.readdirSync(dir).sort()) {
  if (!fs.existsSync(path.join(dir, lang, 'meta.json'))) continue;
  const out = {};
  for (const c of Object.keys(english)) out[c] = cap(lang, nameIn(lang, c) || english[c]);
  fs.writeFileSync(path.join(dir, lang, 'languages.json'), `${JSON.stringify(out, null, 2)}\n`);
}
console.log(`gen-language-names: ${Object.keys(english).length} languages × ${fs.readdirSync(dir).length} catalogs`);
