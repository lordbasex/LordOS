// store_syspkg_test.mjs — Store → Updates' names for LordOS's data packages (apps/store/syspkg.js):
// which packages get a label, and lordos-hwdata versions read as dates.
//
//   node tests/store_syspkg_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { sysPkgKey, snapshotDate } = await import(path.join(here, '../../apps/store/syspkg.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ymd = (d) => (d ? [d.getFullYear(), d.getMonth() + 1, d.getDate()] : null);

eq('hwdata key', sysPkgKey('lordos-hwdata'), 'store.updates.hwdata');
for (const other of ['lordos', 'lordos-hwdata2', 'LORDOS-HWDATA', 'firefox-esr', '', 'toString', '__proto__', 'constructor', null, 42, undefined])
  eq(`no key for ${JSON.stringify(other)}`, sysPkgKey(other), null);

eq('plain date', ymd(snapshotDate('2026.10.27')), [2026, 10, 27]);
eq('serial', ymd(snapshotDate('2026.10.27.1')), [2026, 10, 27]);
eq('serial 99', ymd(snapshotDate('2026.01.05.99')), [2026, 1, 5]);
for (const v of ['2026.10.27.0', '2026.10.27.100', '2026.10.27.', '2026.13.01', '2026.02.30', '2026.00.10', '2026-10-27',
  '26.10.27', '2026.1.27', '1:2026.10.27', '2026.10.27~rc1', '', null, undefined, 20261027])
  eq(`not a date ${JSON.stringify(v)}`, snapshotDate(v), null);

// every language has both strings, and the summary takes the date
for (const lang of ['en', 'es', 'pt', 'fr', 'it']) {
  const cat = JSON.parse(fs.readFileSync(path.join(here, `../../assets/locales/${lang}/store.json`), 'utf8'));
  eq(`${lang} name`, typeof cat['updates.hwdata.name'] === 'string' && cat['updates.hwdata.name'].length > 0, true);
  eq(`${lang} summary has {date}`, /\{date\}/.test(cat['updates.hwdata.summary'] || ''), true);
}

console.log(`store-syspkg-test: ${n - bad}/${n} checks passed`);
process.exit(bad ? 1 : 0);
