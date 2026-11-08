// regionformat_test.mjs — a region's formats from glibc's locale data (sdk/regionformat.js, #45): the
// 24-hour clock, date order and separators of a region whose language LordOS has no format.json for,
// and the region a session runs in when none was chosen (the login screen's too).
//
//   node tests/regionformat_test.mjs        (from server/; run_suites.sh runs it when node exists)
//
// Besides fixed cases, every UTF-8 locale generated on this machine is read with `locale -k` (the same
// fields lord.locale.formats takes with nl_langinfo_l) and, when its language is bundled, checked
// against that language's format.json: glibc and our own tables must agree on the clock.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { fromGlibc, regionFromEnv } = await import(path.join(here, '../../sdk/regionformat.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// lt_LT as glibc 2.41 has it (/usr/share/i18n/locales/lt_LT): the reporter's region
eq('lt_LT', fromGlibc({ dfmt: '%Y-%m-%d', tfmt: '%T', am: '', pm: '', decimal: ',', group: '.' }),
  { date: { numeric: '{yyyy}-{mm}-{dd}' }, hour12: false, decimal: ',', group: '.' });
eq('en_US', fromGlibc({ dfmt: '%m/%d/%Y', tfmt: '%r', am: 'AM', pm: 'PM', decimal: '.', group: ',' }),
  { date: { numeric: '{mm}/{dd}/{yyyy}' }, hour12: true, am: 'AM', pm: 'PM', decimal: '.', group: ',' });
eq('de_DE', fromGlibc({ dfmt: '%d.%m.%Y', tfmt: '%T', am: '', pm: '', decimal: ',', group: '.' }),
  { date: { numeric: '{dd}.{mm}.{yyyy}' }, hour12: false, decimal: ',', group: '.' });
eq('two-digit year shown with four', fromGlibc({ dfmt: '%d/%m/%y', tfmt: '%T' }).date, { numeric: '{dd}/{mm}/{yyyy}' });
eq('%D and %F', [fromGlibc({ dfmt: '%D' }).date.numeric, fromGlibc({ dfmt: '%F' }).date.numeric],
  ['{mm}/{dd}/{yyyy}', '{yyyy}-{mm}-{dd}']);
eq('unpadded day and flags', fromGlibc({ dfmt: '%-d.%-m.%Y', tfmt: '%H:%M' }).date, { numeric: '{day}.{mm}.{yyyy}' });
eq('%e is the unpadded day', fromGlibc({ dfmt: '%e/%m/%Y' }).date, { numeric: '{day}/{mm}/{yyyy}' });
eq('12-hour with %I', fromGlibc({ tfmt: '%I:%M:%S %p', am: 'AM', pm: 'PM' }).hour12, true);
eq('12-hour with %l', fromGlibc({ tfmt: '%l:%M %p', am: 'a.m.', pm: 'p.m.' }), { hour12: true, am: 'a.m.', pm: 'p.m.' });
eq('%I without AM/PM words is 24-hour', fromGlibc({ tfmt: '%I:%M', am: '', pm: '' }).hour12, false);
eq('a narrow no-break space group is kept', fromGlibc({ decimal: ',', group: ' ' }), { decimal: ',', group: ' ' });
eq('no grouping', fromGlibc({ decimal: '.', group: '' }).group, '');
eq('a date without the year is not numeric', fromGlibc({ dfmt: '%d.%m', tfmt: '%T' }).date, undefined);
eq('nothing', fromGlibc(null), null);
eq('not an object', fromGlibc('lt_LT'), null);
eq('empty fields', fromGlibc({}), null);

eq('the user\'s LANG', regionFromEnv({ user: { LANG: 'lt_LT.UTF-8' }, system: { LANG: 'en_US.UTF-8' } }), 'lt_LT');
eq('LC_TIME first', regionFromEnv({ user: { LANG: 'en_US.UTF-8', LC_TIME: 'en_GB.UTF-8' }, system: {} }), 'en_GB');
eq('the computer\'s (login screen)', regionFromEnv({ user: {}, system: { LANG: 'lt_LT.UTF-8' } }), 'lt_LT');
eq('modifier kept', regionFromEnv({ system: { LANG: 'sr_RS.UTF-8@latin' } }), 'sr_RS@latin');
eq('the computer\'s of another language is not taken', regionFromEnv({ user: {}, system: { LANG: 'en_US.UTF-8' } }, 'es'), '');
eq('the computer\'s of the same language is', regionFromEnv({ user: {}, system: { LANG: 'es_AR.UTF-8' } }, 'es'), 'es_AR');
eq('the user\'s is taken whatever the language', regionFromEnv({ user: { LANG: 'lt_LT.UTF-8' }, system: { LANG: 'en_US.UTF-8' } }, 'en'), 'lt_LT');
eq('C.UTF-8 names no region', regionFromEnv({ user: {}, system: { LANG: 'C.UTF-8' } }), '');
eq('nothing at all', regionFromEnv(null), '');

// every generated UTF-8 locale on this machine, read as lord.locale.formats reads it
let locales = [];
try { locales = execFileSync('locale', ['-a'], { encoding: 'utf8' }).split('\n').filter((l) => /^[a-z]{2,3}_[A-Z]{2}\.utf-?8$/i.test(l)); }
catch (e) { console.log('skipped: needs locale (glibc) for the generated-locale cases'); }
const formatOf = (lang) => { try { return JSON.parse(fs.readFileSync(path.join(here, '../../assets/locales', lang, 'format.json'), 'utf8')); } catch (e) { return null; } };
for (const loc of locales) {
  let out;
  try { out = execFileSync('locale', ['-k', 'd_fmt', 't_fmt', 'am_pm', 'decimal_point', 'thousands_sep'], { encoding: 'utf8', env: { ...process.env, LC_ALL: loc } }); }
  catch (e) { continue; }
  const kv = Object.fromEntries([...out.matchAll(/^(\w+)="(.*)"$/gm)].map((m) => [m[1], m[2]]));
  const [am = '', pm = ''] = (kv.am_pm || '').split(';');
  const f = fromGlibc({ dfmt: kv.d_fmt, tfmt: kv.t_fmt, am, pm, decimal: kv.decimal_point, group: kv.thousands_sep });
  const region = loc.split('.')[0];
  n++;
  if (!f || typeof f.hour12 !== 'boolean' || !f.decimal || !f.date?.numeric) { bad++; console.log(`✗ ${loc}: incomplete ${JSON.stringify(f)}`); continue; }
  const bundled = formatOf(region.split('_')[0]);
  if (bundled) {
    const r = { ...bundled, ...(bundled.regions?.[region] || {}) };
    eq(`${loc}: clock agrees with format.json`, f.hour12, r.hour12);
  }
}

console.log(`regionformat: ${n - bad}/${n} ok (${locales.length} generated locales)`);
process.exit(bad ? 1 : 0);
