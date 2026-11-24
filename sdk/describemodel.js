// describemodel.js — what a program or an app is (docs/MONITOR.md, "Descriptions"): the order of the sources, the
// language chosen and the fallback to English, with lord-server's lookups and the app registry given (pure: no `lord`).
// sdk/describe.js runs it in the session; server/tests/describe_model_test.mjs checks it with made-up lookups.
import DESC from './programdesc.json' with { type: 'json' };

const DELETED = ' (deleted)';
const exePath = (exe) => { const s = String(exe || ''); return s.endsWith(DELETED) ? s.slice(0, -DELETED.length) : s; };
const base = (exe) => exePath(exe).split('/').pop();
// root-owned system folders only: never /app (a Flatpak's own tree, as the app sees it), /snap, /opt or a home folder,
// whose files an app or the person's other programs could name to borrow a trusted description
const SYSTEM_DIRS = ['/usr/', '/bin/', '/sbin/', '/lib/', '/lib64/'];
const inSystem = (exe) => SYSTEM_DIRS.some((d) => exePath(exe).startsWith(d));
// LordOS's own programs live only here (the package's fixed paths)
const LORDOS_EXE = /^\/usr\/(lib\/lordos\/bin|bin)\/lord-[a-z0-9-]+$/;
// a process inside an app's sandbox sees its own file tree: its executable's path is the app's to choose
const sandboxed = (p) => /^(flatpak|lpk):/.test(String(p.app || ''));
const KEYS = Object.keys(DESC.kernel).sort((a, b) => b.length - a.length);

// A kernel thread's description: the exact name, else the longest key it starts with where the next character isn't a
// lower-case letter ("kswapd0", "card1-crtc0") or the key ends in / _ - : ("kworker/3:2-events"); never for a process with a program (a program can
// take a kernel thread's name: Sentinela's imitation rule).
export function kernelText(name) {
  const n = String(name || '');
  if (DESC.kernel[n]) return DESC.kernel[n];
  // a key ending in / _ - : is a prefix of anything ("kworker/u80:2-events", "rcu_tasks_kthread")
  const k = KEYS.find((key) => n.startsWith(key) && (/[/_:-]$/.test(key) || !/[a-z]/.test(n.charAt(key.length))));
  return k ? DESC.kernel[k] : null;
}

// LordOS's own programs and the well-known ones: by the executable's file name, for a program in a system folder (or
// LordOS's development build), never by a process's name alone
export function curatedText(p) {
  const exe = exePath(p.exe);
  const b = base(exe) || '';
  if (/^lord-/.test(b) && DESC.lordos[b] && !sandboxed(p) && LORDOS_EXE.test(exe)) return { src: 'lordos', text: DESC.lordos[b] };
  // another user's process whose program can't be read: its name, for LordOS's services (the root ones)
  if (!exe && DESC.lordos[p.name] && p.prot) return { src: 'lordos', text: DESC.lordos[p.name] };
  if (b && DESC.programs[b] && !sandboxed(p) && inSystem(exe)) return { src: 'program', text: DESC.programs[b] };
  // another user's (root's) process whose program can't be read: the well-known ones by their name
  if (!exe && p.name && DESC.programs[p.name] && p.prot !== 'kernel') return { src: 'program', text: DESC.programs[p.name] };
  return null;
}
// the key a description is translated and kept by: where it came from and what it describes
export const translationKey = (d) => (d && d.src && d.of ? `${d.src}:${d.of}` : null);
export const lordosHelpers = () => Object.keys(DESC.lordos);
export const curatedPrograms = () => Object.keys(DESC.programs);

// ---- Lumo's translations, kept per language ----

// deps: { native(kind, arg, lang) → object | false | null, getApp(id), webAppEntry(id), readCache(lang) → text,
// writeCache(lang, text) }: lord-server's lookups, the app registry and the translation cache (sdk/describe.js gives the
// real ones; server/tests/describe_model_test.mjs made-up ones)
export function createDescriber(deps) {
const { native, getApp, webAppEntry } = deps;
const lumoCache = new Map();             // lang → { key: text }
function lumoTranslation(key, lang) {
  if (!lumoCache.has(lang)) {
    let m = {};
    try { m = JSON.parse(deps.readCache(lang)) || {}; } catch (e) { m = {}; }
    lumoCache.set(lang, m && typeof m === 'object' && !Array.isArray(m) ? m : {});
  }
  const v = lumoCache.get(lang)[key];
  return typeof v === 'string' && v ? v : null;
}
function saveLumoTranslation(key, lang, text) {
  lumoTranslation(key, lang);
  const m = lumoCache.get(lang);
  m[key] = String(text).slice(0, 600);
  const keys = Object.keys(m);
  if (keys.length > 2000) delete m[keys[0]];
  try { deps.writeCache(lang, `${JSON.stringify(m)}\n`); } catch (e) { /* kept in memory */ }
}

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


// p: { name, exe, app, prot, pkg (the owner, if known: lord.monitor.exeOwners), unit (from Get Info's cgroup) }
// apps: the launcher's entries (useLauncherApps), to find a .desktop or Flatpak app's Comment.
// The sources in order: LordOS's own programs; the app (a LordOS app's manifest, a web app, a .desktop or Flatpak
// Comment); the well-known programs (programdesc.json); the manual page; the service's Description=; the package. The
// first one in the person's language wins; with none in it, the first one (English, marked). more: some lookup is still
// running (ask again a moment later).
function describeProgram(p, { lang = 'en', apps = [] } = {}) {
  if (!p) return { unknown: true };
  if (p.prot === 'kernel') {
    const k = kernelText(p.name);
    return k ? finish({ src: 'kernel', of: String(p.name).replace(/\d+/g, '#'), text: k, lang: 'en' }, lang) : { unknown: true };
  }
  const pkg = p.pkg || null;
  const english = !String(lang || 'en').startsWith('en');
  const found = [];
  let waiting = false;
  const ask = (kind, arg) => {
    const r = native(kind, arg, lang);
    if (r === null) waiting = true;
    return r && r.text ? r : null;
  };
  const c = curatedText(p);
  if (c && c.src === 'lordos') return finish({ ...c, of: base(p.exe) || p.name, lang: 'en' }, lang);
  const app = String(p.app || '');
  let m;
  if ((m = /^lpk:(.+)$/.exec(app))) {
    const a = getApp(m[1]);
    if (a && a.description) found.push({ src: 'app', of: m[1], text: a.description, lang: 'en' });
  }
  if ((m = /^desktop:lordos-web-(.+)$/.exec(app))) {
    const e = webAppEntry(m[1]);
    if (e && (e.description || e.name)) found.push({ src: 'web', of: m[1], text: e.description || e.name, lang: e.description ? 'en' : lang });
  }
  const fp = /^flatpak:(.+)$/.exec(app)?.[1];
  const desk = /^desktop:(.+)$/.exec(app)?.[1];
  if (fp || desk) {
    const it = (apps || []).find((x) => (fp && x.flatpakId === fp) || (desk && (x.desktopId === desk || x.desktopId === `${desk}.desktop`)));
    if (it && it.comment) found.push({ src: fp ? 'flatpak' : 'desktop', of: fp || desk, text: it.comment, lang: it.commentLocal ? lang : 'en' });
  }
  if (c) found.push({ ...c, of: base(p.exe) || p.name, lang: 'en' });
  const b = base(p.exe);
  if (b && !sandboxed(p) && inSystem(p.exe)) { const r = ask('man', b); if (r) found.push({ src: 'man', of: b, text: r.text, lang: r.lang }); }
  if (p.unit) { const r = ask('unit', p.unit); if (r) found.push({ src: 'unit', of: p.unit, text: r.text, lang: r.lang }); }
  let pkgDesc = null;
  if (pkg) { const r = ask('pkg', pkg); if (r) found.push((pkgDesc = { src: 'pkg', of: pkg, pkg, text: r.text, long: r.long || '', lang: r.lang })); }
  if (!found.length) return waiting ? { pending: true } : { unknown: true };
  const pick = (english && found.find((x) => x.lang !== 'en')) || found[0];
  const out = pick.src !== 'pkg' && pkg ? { ...pick, pkg, pkgText: pkgDesc ? pkgDesc.text : null } : pick;
  return { ...finish(out, lang), more: waiting };
}

// An app of the launcher or the Store: a LordOS app's manifest, a Linux or Flatpak app's Comment, a web app's site.
function describeApp(item, { lang = 'en' } = {}) {
  const it = typeof item === 'string' ? { id: item } : item || {};
  const a = it.kind === 'lordos' || !it.kind ? getApp(it.id) : null;
  if (a && a.description) return finish({ src: 'app', of: a.id, text: a.description, lang: 'en' }, lang);
  if (it.comment) return finish({ src: it.kind === 'flatpak' ? 'flatpak' : it.kind === 'web' ? 'web' : 'desktop', of: it.id, text: it.comment, lang: it.kind === 'web' || it.commentLocal ? lang : 'en' }, lang);
  return { unknown: true };
}
return { describeProgram, describeApp, lumoTranslation, saveLumoTranslation };
}
