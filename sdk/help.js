// help.js — articles of the Help app (assets/help/<lang>/<id>.md) and the welcome tour.
//
//   help.list()            → [{ id, title, app, section, order, keywords }] in the interface language
//   help.read(id)          → { id, title, app, section, keywords, body } or null
//   help.open(id)          → opens Help on that article
//   help.search(text)      → [{ id, title, score, snippet }]
//   help.startTour()       → opens "Welcome to LordOS"
//
// English is the default language: every article exists in assets/help/en, and es/pt (or any other
// language) are translations. The list is the English one plus any extra articles of the interface
// language; each article is read in the interface language and falls back to English when that
// language does not have it.
//
// Each article is markdown with a header:
//   ---
//   title: Clock
//   app: clock              (the app it explains, for the "Open" button)
//   section: apps           (sidebar group: start, desktop, apps, system, settings)
//   order: 10
//   keywords: alarm, timer
//   ---
// Special links in the text: [Open Settings → Network](settings:network), [Files](app:files)
// and [another article](help:spotlight). Search indexes the articles (registerSpotlightSource).
// They are read by lord.help (server/src/ui/js_help.c), only from LordOS's assets/help folder.
import { locale, t } from './i18n.js';
import { settings } from './settings.js';
import { wm, getApp, isLiveSystem } from './windows.js';
import { registerSpotlightSource } from './spotlight.js';
import { compareNames } from './collate.js';

const DEFAULT_LANG = 'en';
const has = () => typeof lord.help !== 'undefined';
const langOf = () => String(locale.get().lang || DEFAULT_LANG).split('-')[0];
const lists = new Map();                    // 'es' → raw list of that language
const bodies = new Map();                   // 'es/clock' → parsed article (with the fallback applied)

function parse(txt) {
  const out = { body: txt, meta: {} };
  const m = /^---\n([\s\S]*?)\n---\n?/.exec(txt);
  if (!m) return out;
  for (const line of m[1].split('\n')) {
    const i = line.indexOf(':');
    if (i > 0) out.meta[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  }
  out.body = txt.slice(m[0].length);
  return out;
}

function rawList(lang) {
  if (lists.has(lang)) return lists.get(lang);
  let list = [];
  try { list = has() ? lord.help.list(lang) : []; } catch (e) { list = []; }
  lists.set(lang, list);
  return list;
}

const byOrder = (a, b) => (a.order || 0) - (b.order || 0) || compareNames(a.title, b.title);

// English articles, replaced by the translated ones where they exist, plus the articles only the
// interface language has.
function mergedList(lang) {
  const base = rawList(DEFAULT_LANG);
  if (lang === DEFAULT_LANG) return base.slice().sort(byOrder);
  const own = new Map(rawList(lang).map((a) => [a.id, a]));
  const out = base.map((a) => own.get(a.id) || a);
  for (const a of own.values()) if (!base.some((b) => b.id === a.id)) out.push(a);
  return out.sort(byOrder);
}

function readRaw(lang, id) {
  try { return has() ? lord.help.read(lang, id) : null; } catch (e) { return null; }
}

const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');

let listCache = null;                       // { lang, list }

export const help = {
  available: has,
  list() {
    const lang = langOf();
    if (!listCache || listCache.lang !== lang) listCache = { lang, list: mergedList(lang) };
    return listCache.list;
  },
  read(id) {
    const lang = langOf();
    const key = `${lang}/${id}`;
    if (bodies.has(key)) return bodies.get(key);
    let txt = readRaw(lang, id);
    if (txt == null && lang !== DEFAULT_LANG) txt = readRaw(DEFAULT_LANG, id);   // not translated yet
    if (txt == null) return null;
    const p = parse(txt);
    const a = { id, title: p.meta.title || id, app: p.meta.app || '', section: p.meta.section || '', keywords: p.meta.keywords || '', body: p.body };
    bodies.set(key, a);
    return a;
  },
  open(id) { wm.open('help', { args: { article: id } }); },
  startTour() { wm.open('help', { args: { tour: true } }); },
  search(query) {
    const q = fold(query).trim();
    if (q.length < 2) return [];
    const out = [];
    for (const it of help.list()) {
      const a = help.read(it.id);
      if (!a) continue;
      const title = fold(a.title), kw = fold(a.keywords), body = fold(a.body);
      let score = title.startsWith(q) ? 90 : title.includes(q) ? 70 : kw.includes(q) ? 55 : 0;
      const at = body.indexOf(q);
      if (!score && at >= 0) score = 30;
      if (!score) continue;
      let snippet = '';
      if (at >= 0) {
        const raw = a.body.replace(/[#*`>[\]()_]/g, '').replace(/\s+/g, ' ');
        const k = fold(raw).indexOf(q);
        let from = Math.max(0, k - 60);
        while (from > 0 && from < k && raw[from - 1] !== ' ') from++;     // start at a whole word
        snippet = `${from > 0 ? '…' : ''}${raw.slice(from, k + 140).trim()}`;
      }
      out.push({ id: a.id, title: a.title, score, snippet: snippet || a.body.replace(/^#.*$/m, '').replace(/\s+/g, ' ').trim().slice(0, 180) });
    }
    return out.sort((x, y) => y.score - x.score);
  },
  // What to do with a link in the text. Returns true if it handled it.
  link(url) {
    const u = String(url || '');
    let m;
    if ((m = /^settings:([a-z]+)$/.exec(u))) { wm.open('settings', { args: { section: m[1] } }); return true; }
    if ((m = /^app:([a-z0-9-]+)$/.exec(u))) { if (getApp(m[1])) wm.open(m[1]); return true; }
    if ((m = /^help:([a-z0-9-]+)$/.exec(u))) { help.open(m[1]); return true; }
    return false;
  },
};

// Search: the Help articles
registerSpotlightSource('help', (q, query) => {
  if (!getApp('help') || q.length < 2) return [];
  return help.search(query).slice(0, 4).map((r) => ({
    key: `help:${r.id}`, score: r.score * 0.6, title: r.title, subtitle: t('shell.spotlight.inHelp'), glyph: '?',
    preview: { kind: 'text', text: r.snippet }, run: () => help.open(r.id),
  }));
});

// First login: "Welcome to LordOS", only once (settings.welcomeShown). On the live system the
// installer opens instead.
export function maybeStartTour() {
  if (settings.get().welcomeShown || isLiveSystem() || !getApp('help') || !has()) return;
  settings.set({ welcomeShown: true });
  setTimeout(() => help.startTour(), 1200);
}

// when the language changes, everything is read again
locale.subscribe(() => { lists.clear(); bodies.clear(); listCache = null; });
