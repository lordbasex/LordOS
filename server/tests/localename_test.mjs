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
const { regionOf, localeOf, regionCountry, languageOfLocale, localeLanguage } = await import(path.join(here, '../../sdk/localename.js'));

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
  const en = names.map(regionOf).filter((r) => r.startsWith('en_'));
  for (const r of ['en_GB', 'en_AU', 'en_IN', 'en_US']) eq(`English offers ${r}`, en.includes(r), true);
} else console.log('localename: skipped the SUPPORTED regions: needs /usr/share/i18n/SUPPORTED (locales)');

console.log(bad ? `localename: ${bad} of ${n} failed` : `localename: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
