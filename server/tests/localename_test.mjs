// localename_test.mjs — glibc region names (sdk/localename.js, #53): what the installer and Settings
// show and store, and what they send to the root side, for every UTF-8 region this machine's glibc lists.
// The root side (server/src/localename.h) accepts a listed name with the codeset inserted before the
// modifier; install-plan-test checks that half.
//
//   node tests/localename_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { regionOf, localeOf, regionCountry, languageOfLocale, localeLanguage, regionLanguageCode, regionLabel } = await import(path.join(here, '../../sdk/localename.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

eq('region with codeset', regionOf('en_GB.UTF-8'), 'en_GB');
eq('region without codeset', regionOf('en_IN'), 'en_IN');
eq('region utf8 spelling', regionOf('es_AR.utf8'), 'es_AR');
eq('region with modifier', regionOf('ca_ES.UTF-8@valencia'), 'ca_ES@valencia');
eq('region with modifier, no codeset', regionOf('ca_ES@valencia'), 'ca_ES@valencia');
eq('locale', localeOf('en_AU'), 'en_AU.UTF-8');
eq('locale codeset-less region', localeOf('en_IN'), 'en_IN.UTF-8');
eq('locale modifier', localeOf('sr_RS@latin'), 'sr_RS.UTF-8@latin');
eq('locale is idempotent', localeOf('en_GB.UTF-8'), 'en_GB.UTF-8');
eq('locale is idempotent with modifier', localeOf('ca_ES.UTF-8@valencia'), 'ca_ES.UTF-8@valencia');
eq('country', regionCountry('en_AU'), 'AU');
eq('country three-letter language', regionCountry('ast_ES'), 'ES');
eq('country modifier', regionCountry('ca_ES@valencia'), 'ES');
eq('no country', regionCountry('eo'), '');

// the interface language for a locale (the login screen and a first session without a saved choice)
const IDS = ['en', 'es', 'pt', 'ja', 'zh-Hans', 'zh-Hant', 'sr-Latn'];
eq('language of pt_BR', languageOfLocale('pt_BR.UTF-8', IDS), 'pt');
eq('language without country', languageOfLocale('ja', IDS), 'ja');
eq('Chinese of China', languageOfLocale('zh_CN.UTF-8', IDS), 'zh-Hans');
eq('Chinese of Singapore', languageOfLocale('zh_SG.UTF-8', IDS), 'zh-Hans');
eq('Chinese of Taiwan', languageOfLocale('zh_TW.UTF-8', IDS), 'zh-Hant');
eq('Chinese of Hong Kong', languageOfLocale('zh_HK.UTF-8', IDS), 'zh-Hant');
eq('Chinese of Macau', languageOfLocale('zh_MO', IDS), 'zh-Hant');
eq('Chinese with only one script', languageOfLocale('zh_TW.UTF-8', ['en', 'zh-Hans']), 'zh-Hans');
eq('the only variant of a language', languageOfLocale('sr_RS.UTF-8@latin', IDS), 'sr-Latn');
eq('a language LordOS lacks', languageOfLocale('de_DE.UTF-8', IDS), '');
eq('C locale', languageOfLocale('C.UTF-8', IDS), '');
eq('empty locale', languageOfLocale('', IDS), '');
eq('no ids', languageOfLocale('es_AR.UTF-8', null), '');
eq('glibc language of zh-Hant', localeLanguage('zh-Hant'), 'zh');
eq('glibc language of pt', localeLanguage('pt'), 'pt');

// #239: a region's row names its language, not only its country (Basque eu_FR was picked for French)
const catalog = (lang) => {
  const read = (ns) => JSON.parse(fs.readFileSync(path.join(here, `../../assets/locales/${lang}/${ns}.json`), 'utf8'));
  const cats = { languages: read('languages'), countries: read('countries') };
  return (key) => { const [ns, k] = key.split('.'); return cats[ns]?.[k] ?? null; };
};
const en = catalog('en'), es = catalog('es'), fr = catalog('fr');
eq('language code', regionLanguageCode('eu_FR.UTF-8'), 'eu');
eq('language code three letters', regionLanguageCode('ast_ES'), 'ast');
eq('language code with modifier', regionLanguageCode('ca_ES@valencia'), 'ca');
eq('language code of C', regionLanguageCode('C.UTF-8'), '');
eq('Basque in France', regionLabel('eu_FR', en), 'Basque · France (eu_FR)');
eq('Breton in France', regionLabel('br_FR.UTF-8', en), 'Breton · France (br_FR)');
eq('Occitan in France', regionLabel('oc_FR', en), 'Occitan · France (oc_FR)');
eq('Catalan in France', regionLabel('ca_FR', en), 'Catalan · France (ca_FR)');
eq('French in France', regionLabel('fr_FR', en), 'French · France (fr_FR)');
const french = ['br_FR', 'ca_FR', 'eu_FR', 'fr_FR', 'oc_FR'].map((r) => regionLabel(r, en).split(' (')[0]);
eq('the French-country regions read differently', new Set(french).size, 5);
eq('in the interface language', regionLabel('eu_FR', es), 'Euskera · Francia (eu_FR)');
eq('in French', regionLabel('eu_ES', fr), 'Basque · Espagne (eu_ES)');
eq('modifier kept', regionLabel('ca_ES@valencia', en), 'Catalan · Spain (ca_ES@valencia)');
eq('language without a name', regionLabel('agr_PE', en), 'Peru (agr_PE)');
eq('no names at all', regionLabel('eu_FR', () => null), 'eu_FR');
eq('no lookup', regionLabel('eu_FR'), 'eu_FR');
eq('a failing lookup', regionLabel('eu_FR', () => { throw new Error('x'); }), 'eu_FR');
eq('search by language finds it', `eu_FR ${regionLabel('eu_FR', en)}`.toLowerCase().includes('basque'), true);
// every catalog names the same languages as English, in that language
const enLangs = Object.keys(JSON.parse(fs.readFileSync(path.join(here, '../../assets/locales/en/languages.json'), 'utf8')));
eq('the catalog names the common languages', ['eu', 'br', 'oc', 'ca', 'fr', 'ast', 'en'].every((c) => enLangs.includes(c)), true);
for (const lang of fs.readdirSync(path.join(here, '../../assets/locales'))) {
  const f = path.join(here, `../../assets/locales/${lang}/languages.json`);
  if (!fs.existsSync(path.join(here, `../../assets/locales/${lang}/meta.json`))) continue;
  const keys = fs.existsSync(f) ? Object.keys(JSON.parse(fs.readFileSync(f, 'utf8'))) : [];
  eq(`${lang} names every language`, keys.join(','), enLangs.join(','));
}

// Every UTF-8 region of this machine: shown once, sent in the form the root side accepts
// (the codeset inserted before any modifier, the same rule as localename.h's locale_utf8_form).
const utf8Form = (loc) => {
  const at = loc.indexOf('@'), base = at < 0 ? loc : loc.slice(0, at);
  return base.includes('.') ? loc : `${base}.UTF-8${at < 0 ? '' : loc.slice(at)}`;
};
let supported = null;
try { supported = fs.readFileSync('/usr/share/i18n/SUPPORTED', 'utf8'); } catch { /* no locales package */ }
if (supported) {
  const names = supported.split('\n').map((l) => l.trim().split(/\s+/)).filter((p) => p.length === 2 && p[1] === 'UTF-8').map((p) => p[0]);
  let wrong = 0;
  for (const name of names) {
    const region = regionOf(name);
    if (region.includes('.') || localeOf(region) !== utf8Form(name)) { wrong++; if (wrong < 10) console.log(`✗ ${name} → ${region} → ${localeOf(region)}`); }
  }
  eq(`all ${names.length} UTF-8 regions round-trip`, wrong, 0);
  eq('the regions are many', names.length > 100, true);
  const enRegions = names.map(regionOf).filter((r) => r.startsWith('en_'));
  for (const r of ['en_GB', 'en_AU', 'en_IN', 'en_US']) eq(`English offers ${r}`, enRegions.includes(r), true);
  const regions = [...new Set(names.map(regionOf))];
  const unnamed = regions.filter((r) => regionLanguageCode(r) && enLangs.includes(regionLanguageCode(r)) === false);
  eq('almost every region names its language', unnamed.length < 30, true);
} else console.log('localename: skipped the SUPPORTED regions: needs /usr/share/i18n/SUPPORTED (locales)');

console.log(bad ? `localename: ${bad} of ${n} failed` : `localename: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
