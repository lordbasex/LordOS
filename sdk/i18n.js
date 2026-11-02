// i18n.js — LordOS language and region for themes and apps.
//
//   t('terminal.menu.newTab')                 → "New Tab"
//   t('files.items', { count: 3 })            → "3 items" (plural according to the language)
//   t('about.version', { v: '0.5.0' })        → "Version 0.5.0"
//   formatDate(d, 'short') / formatTime(d) / formatNumber(1234.5)
//   direction() → 'rtl' | 'ltr'      textDirection('مرحبا') → 'rtl'
//
// Strings live in JSON catalogs that the server reads at runtime:
//   assets/locales/<language>/<namespace>.json            (bundled with LordOS)
//   ~/.local/share/axia/locales/<language>/<namespace>.json   (the user's language packs)
// The namespace is the first part of the key ('terminal', 'files', 'shell'…). A new
// language is a folder with those files and a meta.json; no code changes are needed.
// If a key is missing English is used and, if it isn't there either, the key itself.
//
// There is no Intl in QuickJS: plurals and date/number formats come from our own rules
// and data (each language's format.json).
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';

export const FALLBACK = 'en';

// Plural rules (CLDR, simplified to integers). A language picks its own in meta.json.
const PLURALS = {
  'one-other': (n) => (n === 1 ? 'one' : 'other'),                   // en, es, de, it, nl, sv…
  'zero-one-other': (n) => (n === 0 || n === 1 ? 'one' : 'other'),   // pt, fr
  none: () => 'other',                                               // ja, zh, ko, th, vi, id
  'east-slavic': (n) => {                                            // ru, uk, be
    const m10 = n % 10, m100 = n % 100;
    if (m10 === 1 && m100 !== 11) return 'one';
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few';
    return 'many';
  },
  polish: (n) => {
    const m10 = n % 10, m100 = n % 100;
    if (n === 1) return 'one';
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few';
    return 'many';
  },
  'czech-slovak': (n) => (n === 1 ? 'one' : n >= 2 && n <= 4 ? 'few' : 'other'),
  arabic: (n) => {
    const m100 = n % 100;
    if (n === 0) return 'zero';
    if (n === 1) return 'one';
    if (n === 2) return 'two';
    if (m100 >= 3 && m100 <= 10) return 'few';
    if (m100 >= 11) return 'many';
    return 'other';
  },
};

const cache = new Map();          // 'es/terminal' → flat object
const metas = new Map();          // 'es' → meta.json

function catalog(lang, ns) {
  const k = `${lang}/${ns}`;
  if (cache.has(k)) return cache.get(k);
  let obj = null;
  try {
    const txt = lord.locale.catalog(lang, ns);
    obj = txt ? JSON.parse(txt) : null;
  } catch (e) { console.warn(`i18n: invalid catalog ${k}:`, e.message); }
  cache.set(k, obj);
  return obj;
}

// Available languages: [{ id, name (in its own language), english, plural, user }]
export function languages() {
  const out = [];
  for (const l of lord.locale.languages()) {
    let m = {};
    try { m = JSON.parse(l.meta); } catch (e) { continue; }
    metas.set(l.id, m);
    if (!out.some((x) => x.id === l.id)) out.push({ id: l.id, name: m.name || l.id, english: m.english || '', plural: m.plural, region: m.region, user: l.user });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

function meta(lang) {
  if (!metas.has(lang)) languages();
  return metas.get(lang) || {};
}

// Initial language: the one chosen in Settings, or the one from the user's region (LANG=pt_BR.UTF-8 → pt),
// or English (FALLBACK). A saved choice always wins.
function initial() {
  const s = settings.get().language;
  if (s) return s;
  try {
    const l = lord.locale.get();
    const lang = (l.user.LANG || l.system.LANG || '').split(/[_.@]/)[0];
    if (lang && lang !== 'C' && lord.locale.catalog(lang, 'shell')) return lang;
  } catch (e) { /* no region */ }
  return FALLBACK;
}

export const locale = createStore({ lang: initial(), region: settings.get().region || '' });

// The server picks the CJK font according to the language (the same ideograph is drawn differently in
// Chinese, Japanese and Korean) and, if the language is right-to-left (meta.json "direction": "rtl"),
// mirrors the interface: rows, start alignment, menus.
function applyNative(l) {
  try {
    lord.text.setLanguage(l.lang);
    lord.text.setDirection(dirOf(l) === 'rtl');
  } catch (e) { /* server without lord.text */ }
}
applyNative(locale.get());
locale.subscribe(() => applyNative(locale.get()));

// Direction of the interface ('rtl' in Arabic, Hebrew, Persian…) and of a standalone text (that of its
// first strong character: an Arabic file name inside an English interface).
// An isolated app (runtime/app-host.js) also gets the direction from axia-server with the language
// (`dir`), so it mirrors like the desktop even if it cannot read that language's meta.json.
function dirOf(l) { return l.dir === 'rtl' || l.dir === 'ltr' ? l.dir : meta(l.lang).direction === 'rtl' ? 'rtl' : 'ltr'; }
export const direction = () => dirOf(locale.get());
export const textDirection = (s) => { try { return lord.text.direction(String(s ?? '')); } catch (e) { return 'ltr'; } };

settings.subscribe(() => {
  const s = settings.get();
  const cur = locale.get();
  if ((s.language && s.language !== cur.lang) || (s.region || '') !== cur.region)
    locale.set({ lang: s.language || cur.lang, region: s.region || '' });
});

function lookup(lang, key) {
  const dot = key.indexOf('.');
  if (dot < 0) return undefined;
  const c = catalog(lang, key.slice(0, dot));
  return c ? c[key.slice(dot + 1)] : undefined;
}

const fill = (str, vars) => String(str).replace(/\{(\w+)\}/g, (m, k) => (vars && vars[k] != null ? String(vars[k]) : m));

export function t(key, vars) {
  const { lang } = locale.get();
  let v = lookup(lang, key);
  if (v === undefined && lang !== FALLBACK) v = lookup(FALLBACK, key);
  if (v === undefined) return fill(key, vars);
  if (v && typeof v === 'object') {
    const n = Math.abs(Number(vars?.count ?? 0));
    const rule = PLURALS[meta(lang).plural] || PLURALS['one-other'];
    v = v[rule(n)] ?? v.other ?? Object.values(v)[0];
  }
  return fill(v, vars);
}

// Translation if it exists, or null (for strings with their own fallback, e.g. theme.json).
export function tOptional(key, vars) {
  const { lang } = locale.get();
  const v = lookup(lang, key) ?? lookup(FALLBACK, key);
  return v === undefined ? null : t(key, vars);
}

// Name of an app: apps.<id> in the catalog (the user's language, then English), else manifest.json's
// name (English).
export const appName = (app) => (app ? (lookup(locale.get().lang, `apps.${app.id}`) ?? lookup(FALLBACK, `apps.${app.id}`) ?? app.name) : '');

// ---- formats (date, time, numbers) ----

const clock24 = () => settings.get().clock?.hour24 || 'auto';

// Day and month names in the interface language; date order,
// separators and 12/24 h clock according to the region (es_AR, pt_BR, en_US…).
function formats() {
  const { lang, region } = locale.get();
  const names = catalog(lang, 'format') || catalog(FALLBACK, 'format') || {};
  const rlang = region ? region.split('_')[0] : lang;
  const rbase = catalog(rlang, 'format') || names;
  const r = { ...rbase, ...(rbase.regions?.[region] || {}) };
  const own = rlang === lang;
  return {
    ...names,
    date: { ...(names.date || {}), numeric: r.date?.numeric || names.date?.numeric, ...(own ? r.date : {}) },
    // Settings → Date & Time can force a 24-hour or 12-hour clock over the region's
    decimal: r.decimal, group: r.group, hour12: clock24() === '24' ? false : clock24() === '12' ? true : r.hour12,
    am: own ? r.am : names.am || r.am, pm: own ? r.pm : names.pm || r.pm,
  };
}

const pad = (n) => String(n).padStart(2, '0');

// style: 'short' (Thu Oct 15), 'long' (Thursday, October 15, 2026), 'numeric' (10/15/2026)
export function formatDate(d, style = 'short') {
  const f = formats();
  const day = d.getDate(), month = d.getMonth(), year = d.getFullYear(), wd = d.getDay();
  const pattern = f.date?.[style] || '{dd}/{mm}/{yyyy}';
  return fill(pattern, {
    weekday: f.weekdays?.[wd] ?? wd, wd: f.weekdaysShort?.[wd] ?? wd, day, dd: pad(day),
    month: f.months?.[month] ?? month + 1, mon: f.monthsShort?.[month] ?? month + 1, mm: pad(month + 1), yyyy: year,
  });
}

// Month name (0 = January) in the interface language: monthName(9) → "October", monthName(9, true) → "Oct".
export function monthName(m, short = false) {
  const f = formats();
  return (short ? f.monthsShort : f.months)?.[m] ?? String(m + 1);
}

// Weekday name (0 = Sunday) in the interface language: weekdayName(1) → "Mon".
export function weekdayName(wd, short = true) {
  const f = formats();
  return (short ? f.weekdaysShort : f.weekdays)?.[wd] ?? String(wd);
}

export function formatTime(d, { seconds = false } = {}) {
  const f = formats();
  let h = d.getHours();
  const m = pad(d.getMinutes()), s = seconds ? `:${pad(d.getSeconds())}` : '';
  if (f.hour12) {
    const ap = h < 12 ? f.am || 'AM' : f.pm || 'PM';
    h = h % 12 || 12;
    return `${h}:${m}${s} ${ap}`;
  }
  return `${pad(h)}:${m}${s}`;
}

export function formatNumber(n, digits = null) {
  const f = formats();
  const dec = f.decimal || '.', group = f.group ?? ',';
  const fixed = digits == null ? String(n) : Number(n).toFixed(digits);
  const [i, frac] = fixed.split('.');
  const neg = i.startsWith('-');
  const int = (neg ? i.slice(1) : i).replace(/\B(?=(\d{3})+(?!\d))/g, group);
  return `${neg ? '-' : ''}${int}${frac ? dec + frac : ''}`;
}

// Hook: re-renders when the language or region changes.
export function useLocale() {
  const l = useStore(locale);
  return { ...l, dir: direction(), t, formatDate, formatTime, formatNumber };
}

export const setLanguage = (lang) => settings.set({ language: lang });

// After installing or editing a language pack.
export function reloadCatalogs() {
  cache.clear();
  metas.clear();
  locale.set((l) => ({ ...l }));
}
