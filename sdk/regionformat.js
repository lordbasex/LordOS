// regionformat.js — a region's formats from glibc's own locale data (#45).
//
// lord.locale.formats('lt_LT') gives glibc's LC_TIME and LC_NUMERIC fields for a generated region:
//   { dfmt: '%Y-%m-%d', tfmt: '%T', am: '', pm: '', decimal: ',', group: '.' }
// fromGlibc() turns them into the part of a format.json that comes from the region (sdk/i18n.js):
//   { date: { numeric: '{yyyy}-{mm}-{dd}' }, hour12: false, decimal: ',', group: '.' }
// so a region whose language LordOS has no format.json for (lt_LT, de_DE, pl_PL…) still gets its own
// 24-hour clock, date order and separators instead of the interface language's. Pure: no lord.*;
// tests in server/tests/regionformat_test.mjs.
import { regionOf, regionCountry } from './localename.js';

// strftime conversions inside a date pattern, as our {placeholders}. A two-digit year (%y) is shown
// with four digits, like the bundled regions. Other conversions (era, week number…) are dropped.
const DATE = {
  d: '{dd}', e: '{day}', m: '{mm}', Y: '{yyyy}', y: '{yyyy}', C: '', b: '{mon}', h: '{mon}', B: '{month}',
  a: '{wd}', A: '{weekday}', D: '{mm}/{dd}/{yyyy}', F: '{yyyy}-{mm}-{dd}', n: ' ', t: ' ', '%': '%',
};

function datePattern(fmt) {
  let out = '';
  for (let i = 0; i < fmt.length; i++) {
    const ch = fmt[i];
    if (ch !== '%') { out += ch; continue; }
    let k = fmt[++i], unpadded = false;
    while (k && '-_0^#EO'.includes(k)) { if (k === '-') unpadded = true; k = fmt[++i]; }   // flags
    if (k === undefined) break;
    out += k === 'd' && unpadded ? '{day}' : DATE[k] ?? '';
  }
  out = out.trim();
  // only a pattern with the day, the month and the year is a numeric date
  return /\{(dd|day)\}/.test(out) && /\{(mm|mon|month)\}/.test(out) && out.includes('{yyyy}') ? out : null;
}

// 12-hour clock: the time format uses the 12-hour hour (%I, %l) or %r, and the region has AM/PM words.
const twelveHour = (tfmt, am) => /%[-_0^#]?[Ilr]/.test(tfmt) && !!am;

export function fromGlibc(g) {
  if (!g || typeof g !== 'object') return null;
  const str = (v) => (typeof v === 'string' ? v : '');
  const out = {};
  const numeric = datePattern(str(g.dfmt));
  if (numeric) out.date = { numeric };
  if (str(g.tfmt)) out.hour12 = twelveHour(str(g.tfmt), str(g.am));
  if (out.hour12) { out.am = str(g.am); out.pm = str(g.pm); }
  if (str(g.decimal)) out.decimal = str(g.decimal);
  if (typeof g.group === 'string') out.group = g.group;
  return Object.keys(out).length ? out : null;
}

// The region a session runs in when none was chosen in Settings: the user's LC_TIME or LANG
// (~/.config/locale.conf, which the installer and the first run write), else the computer's
// (/etc/default/locale; the login screen has only that one) when it is of the interface's language
// (lang; any when not given): Debian's en_US.UTF-8 is not a choice made for a Spanish desktop.
// '' when it names no country (C.UTF-8).
//   regionFromEnv({ user: {}, system: { LANG: 'lt_LT.UTF-8' } }) → 'lt_LT'
//   regionFromEnv({ user: {}, system: { LANG: 'en_US.UTF-8' } }, 'es') → ''
export function regionFromEnv(env, lang) {
  const u = (env && env.user) || {}, s = (env && env.system) || {};
  const mine = regionOf(u.LC_TIME || u.LANG || '');
  if (regionCountry(mine)) return mine;
  const sys = regionOf(s.LANG || '');
  if (!regionCountry(sys)) return '';
  return !lang || sys.split('_')[0] === String(lang).split(/[-_]/)[0] ? sys : '';
}
