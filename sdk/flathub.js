// flathub.js — Flathub's jobs and state, for the Store and Settings → Flatpak (docs/FLATPAK.md, phase 1 step 5). Everything goes through
// lord.flatpak (server/src/ui/js_flatpak.c): the closed list of `flatpak --user` commands, one at a time, each in
// lordos-net-flathub.scope; the catalog read by the sandboxed lord-appstream. Turning Flathub on installs the `flatpak`
// Debian package with the Store's own package job (debian.js, without Recommends), then adds the remote from the file
// the lordos package ships and fetches the catalog. The pages are flathubpages.js; the plain logic sdk/flathubmodel.js.
import { createStore } from './store.js';
import { settings } from './settings.js';
import { locale, t } from './i18n.js';
import { refreshApps } from './apps.js';
import * as M from './flathubmodel.js';
import { firewall, firewallState } from './monitor.js';

const api = () => (typeof lord !== 'undefined' && lord.flatpak ? lord.flatpak : null);
// this PC can have Flathub (phase 1: x86-64); flatpak itself is installed
export const supported = () => { const a = api(); return !!a && (typeof a.supported !== 'function' || a.supported()); };
export const available = () => { const a = api(); return !!a && a.available(); };
export const prefs = () => M.prefsOf(settings.get().flathub);
export const setPrefs = (p) => settings.set((s) => ({ ...s, flathub: { ...M.prefsOf(s.flathub), ...p } }));

// catalog: the last read (lord.flatpak.catalog()); installed: `flatpak list` ({ id, branch, version }); listed: that
// list was read at least once (before, its 0 means "not known yet"); job: the
// running install, removal or update ({ kind, id, name, pct, line }); busy: the catalog or setup step running ('' when
// none); msg: the last outcome ({ ok, text }).
export const state = createStore({ catalog: [], loaded: false, installed: [], listed: false, job: null, busy: '', msg: null });
const patch = (p) => state.set((s) => ({ ...s, ...p }));

// ---- the job pump: lord.flatpak.take() while something runs ----
const waiting = new Map();
let timer = 0;
function pump() {
  const a = api();
  if (a) {
    for (const ev of a.take()) {
      const w = waiting.get(ev.id);
      if (!w) continue;
      if (ev.done) { waiting.delete(ev.id); w.resolve({ code: ev.code, lines: w.lines }); }
      else { w.lines.push(ev.line); if (w.lines.length > 4000) w.lines.shift(); if (w.onLine) w.onLine(ev.line); }
    }
  }
  if (!waiting.size && timer) { clearInterval(timer); timer = 0; }
}
// One command → { code, lines } (code -1: refused or not available).
function run(name, ref, onLine) {
  const a = api();
  if (!a) return Promise.resolve({ code: -1, lines: [] });
  const id = name === 'catalog' ? a.loadCatalog(ref) : ref ? a.start(name, ref) : a.start(name);
  if (!(id > 0)) return Promise.resolve({ code: -1, lines: [] });
  return new Promise((resolve) => {
    waiting.set(id, { resolve, lines: [], onLine });
    if (!timer) timer = setInterval(pump, 200);
  });
}
// Flatpak's own last words, for a failure (its "error: …" line when it wrote one).
const lastWords = (lines) => {
  const l = (lines || []).filter((x) => x.trim());
  return (l.find((x) => /^error:/i.test(x.trim())) || l[l.length - 1] || '').trim().slice(0, 300);
};
// Flathub's downloads are the firewall component lordos:flathub. When one fails because of the Firewall, Flatpak only
// says it found nothing ("No such ref …"), so the Store says what happened and where to change it: a block on the
// whole component, or a connection a rule denied since the job started.
const FW_APP = 'lordos:flathub';
function firewallWords(since) {
  if (!firewall.available()) return null;
  if (firewall.appRule(FW_APP) === 'deny' || firewall.blocks(FW_APP, 'no-internet') || firewall.blocks(FW_APP, 'no-network')) {
    return t('flathub.msg.fwBlocked');
  }
  firewall.refresh();
  const e = (firewallState.get().events || []).filter((x) => x.app === FW_APP && x.v === 'deny' && x.dir === 'out' && (x.at || 0) >= since).pop();
  return e ? t('flathub.msg.fwDenied', { host: (e.hosts && e.hosts[0]) || e.addr || '?' }) : null;
}
const failed = (r, since = 0) => ({ ok: false, text: r.code === -1 ? t('flathub.msg.refused')
  : firewallWords(since) || t('flathub.msg.failed', { reason: lastWords(r.lines) || r.code }) });

// ---- the catalog ----
let loading = null;
// Reads the catalog (fetching it first when it is a day old, or never was); a fetch that fails keeps the last one.
export function loadCatalog({ fetch = false } = {}) {
  if (loading) return loading;
  loading = (async () => {
    const since = Date.now();
    let fetched = false;
    const doFetch = async () => {
      patch({ busy: 'appstream' });
      const r = await run('appstream');
      fetched = true;
      if (r.code === 0) setPrefs({ appstreamAt: Date.now() });
      return r;
    };
    let fr = null;
    if (fetch || M.appstreamStale(prefs().appstreamAt, Date.now())) fr = await doFetch();
    patch({ busy: 'catalog' });
    let r = await run('catalog', M.catalogLang(locale.get().lang));
    if (r.code === 2 && !fetched) { fr = await doFetch(); patch({ busy: 'catalog' }); r = await run('catalog', M.catalogLang(locale.get().lang)); }
    const a = api();
    // the last catalog stays when a fetch fails, but the person is told it couldn't be updated, and why
    const stale = fr && fr.code !== 0 ? { ok: false, text: firewallWords(since) || t('flathub.msg.notRefreshed', { reason: lastWords(fr.lines) || fr.code }) } : null;
    if (r.code === 0 && a) patch({ catalog: a.catalog(), loaded: true, busy: '', ...(stale ? { msg: stale } : {}) });
    else patch({ busy: '', loaded: state.get().loaded, msg: fr && fr.code !== 0 ? failed(fr, since) : { ok: false, text: firewallWords(since) || t('flathub.msg.noCatalog') } });
    loading = null;
  })();
  return loading;
}
locale.subscribe(() => { if (state.get().loaded) loadCatalog(); });

export async function refreshInstalled() {
  const r = await run('list');
  if (r.code === 0) patch({ installed: M.parseList(r.lines), listed: true });
  return state.get().installed;
}

// While a page shows what's installed (Settings → Flatpak, the Store's Installed): an app installed or removed outside
// the Store (a terminal's `flatpak install --user`, a .flatpakref, another program) is seen within a few seconds. Every
// 3 s it compares the exported apps' ids (lord.apps.flatpak: the server reads its folders again only when they changed)
// and lists the installation again only when they did. Returns the function that stops it.
const exportedIds = () => { try { return lord.apps.flatpak('en').map((a) => a.id).sort().join(' '); } catch (e) { return ''; } };
export function watchInstalled() {
  if (!available() || typeof lord.apps?.flatpak !== 'function') return () => {};
  let last = exportedIds();
  const id = setInterval(() => {
    const now = exportedIds();
    if (now === last) return;
    last = now;
    refreshApps();
    refreshInstalled();
  }, 3000);
  return () => clearInterval(id);
}

// Turning Flathub on, once flatpak is installed: the remote (if it isn't there), the catalog, what is installed.
export async function turnOn() {
  patch({ busy: 'remote', msg: null });
  const since = Date.now();
  const r = await run('remoteAdd');
  if (r.code !== 0) { patch({ busy: '', msg: failed(r, since) }); return false; }
  setPrefs({ on: true });
  await loadCatalog({ fetch: true });
  await refreshInstalled();
  return state.get().loaded;
}
// Off: the section hides; Flatpak, its apps and the remote stay (Installed still lists and removes the apps).
export const turnOff = () => setPrefs({ on: false });

// ---- installing, removing, updating ----
async function job(kind, app, steps) {
  if (state.get().job) return false;
  const since = Date.now();
  patch({ job: { kind, id: app ? app.id : '', name: app ? app.name : '', pct: null, line: '' }, msg: null });
  // flatpak prints many progress lines a second: they are gathered and the state changes at most every 250 ms (each
  // change redraws the Store's Flathub page; one per line froze the desktop while a big app downloaded)
  let pend = null, timer = 0;
  const flush = () => {
    timer = 0;
    const p = pend;
    pend = null;
    if (p) state.set((s) => (s.job ? { ...s, job: { ...s.job, line: p.line || s.job.line, pct: p.pct == null ? s.job.pct : p.pct } } : s));
  };
  const onLine = (line) => {
    const pct = M.progressOf(line);
    pend = { line: line.trim().slice(0, 200) || pend?.line || '', pct: pct == null ? pend?.pct ?? null : pct };
    if (!timer) timer = setTimeout(flush, 250);
  };
  let last = { code: 0, lines: [] };
  for (const [name, ref] of steps) {
    last = await run(name, ref, onLine);
    if (last.code !== 0) break;
  }
  if (timer) { clearTimeout(timer); timer = 0; pend = null; }
  await refreshInstalled();
  refreshApps();
  const ok = last.code === 0;
  const name = app ? app.name : '';
  patch({ job: null, msg: { ...(ok ? { ok: true, text: t(`flathub.msg.${kind}Done`, { name }) } : failed(last, since)), id: app ? app.id : '' } });
  return ok;
}

export const install = (app) => job('install', app, [['install', M.refOf(app.id, app.branch)]]);
// removing also removes the runtimes no app uses any more
export const uninstall = (app) => job('uninstall', app, [['uninstall', M.refOf(app.id, app.branch)], ['uninstallUnused']]);
export const update = (app) => job('update', app, [['update', M.refOf(app.id, app.branch)], ['uninstallUnused']]);
export const updateAll = () => job('updateAll', null, [['update'], ['uninstallUnused']]);
// Settings → Flatpak, "Remove Flatpak completely" (the Flathub part): every Flathub app, the runtimes left unused and the
// Flathub remote. data: also each removed app's own folder ~/.var/app/<id>, to the Trash (it can be restored). The
// flatpak package itself is removed afterwards by the package engine, with its plan shown (Settings). → true when done.
export async function removeAll({ data = false } = {}) {
  await refreshInstalled();
  const apps = state.get().installed.slice();
  const steps = apps.map((a) => ['uninstall', M.refOf(a.id, a.branch)]).filter(([, ref]) => ref);
  steps.push(['uninstallUnused']);
  const ok = await job('removeAll', null, steps);
  if (!ok) return false;
  const rd = await run('remoteDelete');       // gone already (never added, or removed by hand) is not a failure
  if (rd.code !== 0 && !rd.lines.some((l) => /not found|no remote/i.test(l))) { patch({ msg: failed(rd) }); return false; }
  setPrefs({ on: false, appstreamAt: 0 });
  patch({ catalog: [], loaded: false, installed: [] });
  if (data) {
    // every app folder in ~/.var/app (only Flatpak uses it), also of apps removed earlier
    const home = typeof lord !== 'undefined' && lord.fs ? lord.fs.home() : '';
    let ids = apps.map((a) => a.id);
    try { ids = [...new Set([...ids, ...lord.fs.list(`${home}/.var/app`).map((e) => e.name)])]; } catch (e) { /* no folder */ }
    for (const id of ids) {
      if (!home || !M.idOk(id)) continue;
      try { lord.fs.trash(`${home}/.var/app/${id}`); } catch (e) { /* not there, or not movable: left as it is */ }
    }
  }
  refreshApps();
  return true;
}

export const cancelJob = () => {
  const a = api();
  for (const id of waiting.keys()) if (a) a.cancel(id);
};

// ---- an app's metadata (its declared permissions), once per ref ----
const infos = new Map();
export function info(ref) {
  if (!ref) return Promise.resolve(null);
  if (!infos.has(ref)) {
    infos.set(ref, run('info', ref).then((r) => {
      if (r.code !== 0) { infos.delete(ref); return { error: lastWords(r.lines) || String(r.code) }; }
      return M.parseMetadata(r.lines);
    }));
  }
  return infos.get(ref);
}

export const catalogApp = (id) => state.get().catalog.find((a) => a.id === id) || null;
// An app's description from the last catalog read, in its language ('' when it has none): asked for one app at a time,
// so the catalog itself stays small (lord.flatpak.description, checked again in lord-server).
export function description(id) {
  const a = api();
  if (!a || typeof a.description !== 'function' || !M.idOk(id)) return '';
  try { return String(a.description(id) || ''); } catch (e) { return ''; }
}
export const installedApp = (id) => state.get().installed.find((a) => a.id === id) || null;
