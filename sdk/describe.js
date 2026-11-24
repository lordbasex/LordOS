// describe.js — what a program or an app is, in one place (docs/MONITOR.md, "Descriptions"): Monitor's Get Info shows it,
// and the Store, Settings → Apps and the Dock's and launcher's tooltips can ask the same function.
//
//   describeProgram(p, { lang })   p: a process of lord.monitor.processes ({ name, exe, app, prot, pkg?, unit? })
//   describeApp(item, { lang })    item: a launcher entry (useLauncherApps) or an app id
// Both return { text, long, lang, src, pkg, pkgText, english } | { pending: true } | { unknown: true }:
//   text     one line; long: a package's longer text (paragraphs), or '';
//   lang     the language the text is in ('en' when no translation exists for free);
//   src      where it comes from: 'kernel' | 'lordos' | 'program' (sdk/programdesc.json, English) | 'app' (a LordOS app's
//            manifest) | 'desktop' (a .desktop Comment, localized) | 'flatpak' (its exported entry's Comment, localized)
//            | 'web' | 'man' (its manual page's NAME, a translated page first) | 'unit' (a systemd unit's Description=)
//            | 'pkg' (the installed package's description, Debian's translation when apt has it) | 'lumo' (translated
//            by Sentinela's local model, kept in ~/.cache/lordos/descriptions/<lang>.json);
//   pkg, pkgText: the package that owns the program and its one line, when the main text is another one's;
//   english  the text is English while the interface isn't (offer "Translate with Lumo").
// pending: lord-server's worker thread is still reading (lord.monitor.describe; ask again a moment later). Nothing is
// run, nothing goes to the network: dpkg's database, apt's translation lists, manual pages, units and .desktop entries.
import DESC from './programdesc.json';
import { getApp } from './windows.js';
import { webAppEntry } from './webapps.js';
import * as fs from './fs.js';

const DELETED = ' (deleted)';
const exePath = (exe) => { const s = String(exe || ''); return s.endsWith(DELETED) ? s.slice(0, -DELETED.length) : s; };
const base = (exe) => exePath(exe).split('/').pop();
const SYSTEM_DIRS = ['/usr/', '/bin/', '/sbin/', '/lib/', '/lib64/', '/opt/', '/snap/', '/app/'];
const inSystem = (exe) => SYSTEM_DIRS.some((d) => exePath(exe).startsWith(d));
const KEYS = Object.keys(DESC.kernel).sort((a, b) => b.length - a.length);

// A kernel thread's description: the exact name, else the longest key it starts with where the next character isn't a
// lower-case letter ("kswapd0", "card1-crtc0", "kworker/3:2-events"); never for a process with a program (a program can
// take a kernel thread's name: Sentinela's imitation rule).
export function kernelText(name) {
  const n = String(name || '');
  if (DESC.kernel[n]) return DESC.kernel[n];
  const k = KEYS.find((key) => n.startsWith(key) && !/[a-z]/.test(n.charAt(key.length)));
  return k ? DESC.kernel[k] : null;
}

// LordOS's own programs and the well-known ones: by the executable's file name, for a program in a system folder (or
// LordOS's development build), never by a process's name alone
export function curatedText(p) {
  const exe = exePath(p.exe);
  const b = base(exe) || '';
  if (/^lord-/.test(b) && DESC.lordos[b] && (inSystem(exe) || /\/server\/build\/lord-[a-z0-9-]+$/.test(exe))) return { src: 'lordos', text: DESC.lordos[b] };
  // another user's process whose program can't be read: its name, for LordOS's services (the root ones)
  if (!exe && DESC.lordos[p.name] && p.prot) return { src: 'lordos', text: DESC.lordos[p.name] };
  if (b && DESC.programs[b] && inSystem(exe)) return { src: 'program', text: DESC.programs[b] };
  if (!exe && (p.name === '(sd-pam)' || p.name === 'systemd') && p.prot) return { src: 'program', text: DESC.programs[p.name] };
  return null;
}
export const lordosHelpers = () => Object.keys(DESC.lordos);
export const curatedPrograms = () => Object.keys(DESC.programs);

// ---- what lord-server reads (its worker thread) ----

const native = (kind, arg, lang) => {
  try {
    if (typeof lord === 'undefined' || typeof lord.monitor?.describe !== 'function' || !arg) return false;
    return lord.monitor.describe(kind, String(arg), lang);       // object, false (nothing), null (being read)
  } catch (e) { return false; }
};

// ---- Lumo's translations, kept per language ----

const CACHE_DIR = '.cache/lordos/descriptions';
const lumoCache = new Map();             // lang → { key: text }
export function lumoTranslation(key, lang) {
  if (!lumoCache.has(lang)) {
    let m = {};
    try { m = JSON.parse(fs.read(`${CACHE_DIR}/${lang}.json`)) || {}; } catch (e) { m = {}; }
    lumoCache.set(lang, m && typeof m === 'object' && !Array.isArray(m) ? m : {});
  }
  const v = lumoCache.get(lang)[key];
  return typeof v === 'string' && v ? v : null;
}
export function saveLumoTranslation(key, lang, text) {
  lumoTranslation(key, lang);
  const m = lumoCache.get(lang);
  m[key] = String(text).slice(0, 600);
  const keys = Object.keys(m);
  if (keys.length > 2000) delete m[keys[0]];
  try {
    for (const d of ['.cache', '.cache/lordos', CACHE_DIR]) { try { fs.mkdir(d); } catch (e) { /* there */ } }
    fs.write(`${CACHE_DIR}/${lang}.json`, `${JSON.stringify(m)}\n`);
  } catch (e) { /* kept in memory */ }
}
// the key a description is translated and kept by: where it came from and what it describes
export const translationKey = (d) => (d && d.src && d.of ? `${d.src}:${d.of}` : null);

function finish(d, lang) {
  if (!d || d.pending || d.unknown) return d;
  const english = d.lang === 'en' && !String(lang || 'en').startsWith('en');
  const out = { long: '', pkg: null, pkgText: null, ...d, english };
  if (english) {
    const k = translationKey(out);
    const tr = k ? lumoTranslation(k, lang) : null;
    if (tr) return { ...out, text: tr, english: false, lang, src: 'lumo', from: out.src, original: out.text };
  }
  return out;
}

// the package's own line, as a second line under another source's text
function withPackage(d, pkg, lang) {
  if (!pkg || d.src === 'pkg') return d;
  const r = native('pkg', pkg, lang);
  return r && r.text ? { ...d, pkg, pkgText: r.text } : { ...d, pkg };
}

// p: { name, exe, app, prot, pkg (the owner, if known: lord.monitor.exeOwners), unit (from Get Info's cgroup) }
// apps: the launcher's entries (useLauncherApps), to find a .desktop or Flatpak app's Comment
export function describeProgram(p, { lang = 'en', apps = [] } = {}) {
  if (!p) return { unknown: true };
  if (p.prot === 'kernel') {
    const k = kernelText(p.name);
    return k ? finish({ src: 'kernel', of: String(p.name).replace(/\d+/g, '#'), text: k, lang: 'en' }, lang) : { unknown: true };
  }
  const c = curatedText(p);
  const pkg = p.pkg || null;
  if (c) return finish(withPackage({ ...c, of: base(p.exe) || p.name, lang: 'en' }, c.src === 'lordos' ? null : pkg, lang), lang);
  const app = String(p.app || '');
  let m;
  // a LordOS app (isolated: its scope lpk-<id>)
  if ((m = /^lpk:(.+)$/.exec(app))) {
    const a = getApp(m[1]);
    if (a && a.description) return finish({ src: 'app', of: m[1], text: a.description, lang: 'en' }, lang);
  }
  // a web app
  if ((m = /^desktop:lordos-web-(.+)$/.exec(app))) {
    const e = webAppEntry(m[1]);
    if (e && (e.description || e.name)) return finish({ src: 'web', of: m[1], text: e.description || e.name, lang: e.description ? 'en' : lang }, lang);
  }
  // a Linux app with a .desktop entry, or a Flatpak app: its Comment, in the person's language when it has one
  const fp = /^flatpak:(.+)$/.exec(app)?.[1];
  const desk = /^desktop:(.+)$/.exec(app)?.[1];
  if (fp || desk) {
    const it = (apps || []).find((x) => (fp && x.flatpakId === fp) || (desk && (x.desktopId === desk || x.desktopId === `${desk}.desktop`)));
    if (it && it.comment) return finish(withPackage({ src: fp ? 'flatpak' : 'desktop', of: fp || desk, text: it.comment, lang: it.commentLocal ? lang : 'en' }, pkg, lang), lang);
  }
  const b = base(p.exe);
  let waiting = false;
  // its manual page's one line (the program's own, more precise than its package's)
  if (b && inSystem(p.exe)) {
    const r = native('man', b, lang);
    if (r === null) waiting = true;
    else if (r && r.text) return finish(withPackage({ src: 'man', of: b, text: r.text, lang: r.lang }, pkg, lang), lang);
  }
  if (p.unit) {
    const r = native('unit', p.unit, lang);
    if (r === null) waiting = true;
    else if (r && r.text) return finish(withPackage({ src: 'unit', of: p.unit, text: r.text, lang: r.lang }, pkg, lang), lang);
  }
  if (pkg) {
    const r = native('pkg', pkg, lang);
    if (r === null) waiting = true;
    else if (r && r.text) return finish({ src: 'pkg', of: pkg, pkg, text: r.text, long: r.long || '', lang: r.lang }, lang);
  }
  return waiting ? { pending: true } : { unknown: true };
}

// An app of the launcher or the Store: a LordOS app's manifest, a Linux or Flatpak app's Comment, a web app's site.
export function describeApp(item, { lang = 'en' } = {}) {
  const it = typeof item === 'string' ? { id: item } : item || {};
  const a = it.kind === 'lordos' || !it.kind ? getApp(it.id) : null;
  if (a && a.description) return finish({ src: 'app', of: a.id, text: a.description, lang: 'en' }, lang);
  if (it.comment) return finish({ src: it.kind === 'flatpak' ? 'flatpak' : it.kind === 'web' ? 'web' : 'desktop', of: it.id, text: it.comment, lang: it.kind === 'web' || it.commentLocal ? lang : 'en' }, lang);
  return { unknown: true };
}
