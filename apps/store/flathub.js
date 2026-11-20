// flathub.js — the Store's Flathub jobs and state (docs/FLATPAK.md, phase 1 step 5). Everything goes through
// lord.flatpak (server/src/ui/js_flatpak.c): the closed list of `flatpak --user` commands, one at a time, each in
// lordos-net-flathub.scope; the catalog read by the sandboxed lord-appstream. Turning Flathub on installs the `flatpak`
// Debian package with the Store's own package job (debian.js, without Recommends), then adds the remote from the file
// the lordos package ships and fetches the catalog. The pages are flathubpages.js; the plain logic sdk/flathubmodel.js.
import { createStore, settings, locale, refreshApps, flathubModel as M, t } from '@lordos/sdk';

const api = () => (typeof lord !== 'undefined' && lord.flatpak ? lord.flatpak : null);
// this PC can have Flathub (phase 1: x86-64); flatpak itself is installed
export const supported = () => { const a = api(); return !!a && (typeof a.supported !== 'function' || a.supported()); };
export const available = () => { const a = api(); return !!a && a.available(); };
export const prefs = () => M.prefsOf(settings.get().flathub);
export const setPrefs = (p) => settings.set((s) => ({ ...s, flathub: { ...M.prefsOf(s.flathub), ...p } }));

// catalog: the last read (lord.flatpak.catalog()); installed: `flatpak list` ({ id, branch, version }); job: the
// running install, removal or update ({ kind, id, name, pct, line }); busy: the catalog or setup step running ('' when
// none); msg: the last outcome ({ ok, text }).
export const state = createStore({ catalog: [], loaded: false, installed: [], job: null, busy: '', msg: null });
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
const failed = (r) => ({ ok: false, text: r.code === -1 ? t('flathub.msg.refused') : t('flathub.msg.failed', { reason: lastWords(r.lines) || r.code }) });

// ---- the catalog ----
let loading = null;
// Reads the catalog (fetching it first when it is a day old, or never was); a fetch that fails keeps the last one.
export function loadCatalog({ fetch = false } = {}) {
  if (loading) return loading;
  loading = (async () => {
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
    if (r.code === 0 && a) patch({ catalog: a.catalog(), loaded: true, busy: '' });
    else patch({ busy: '', loaded: state.get().loaded, msg: fr && fr.code !== 0 ? failed(fr) : { ok: false, text: t('flathub.msg.noCatalog') } });
    loading = null;
  })();
  return loading;
}
locale.subscribe(() => { if (state.get().loaded) loadCatalog(); });

export async function refreshInstalled() {
  const r = await run('list');
  if (r.code === 0) patch({ installed: M.parseList(r.lines) });
  return state.get().installed;
}

// Turning Flathub on, once flatpak is installed: the remote (if it isn't there), the catalog, what is installed.
export async function turnOn() {
  patch({ busy: 'remote', msg: null });
  const r = await run('remoteAdd');
  if (r.code !== 0) { patch({ busy: '', msg: failed(r) }); return false; }
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
  patch({ job: { kind, id: app ? app.id : '', name: app ? app.name : '', pct: null, line: '' }, msg: null });
  const onLine = (line) => {
    const pct = M.progressOf(line);
    state.set((s) => ({ ...s, job: s.job && { ...s.job, line: line.trim().slice(0, 200) || s.job.line, pct: pct == null ? s.job.pct : pct } }));
  };
  let last = { code: 0, lines: [] };
  for (const [name, ref] of steps) {
    last = await run(name, ref, onLine);
    if (last.code !== 0) break;
  }
  await refreshInstalled();
  refreshApps();
  const ok = last.code === 0;
  const name = app ? app.name : '';
  patch({ job: null, msg: { ...(ok ? { ok: true, text: t(`flathub.msg.${kind}Done`, { name }) } : failed(last)), id: app ? app.id : '' } });
  return ok;
}

export const install = (app) => job('install', app, [['install', M.refOf(app.id, app.branch)]]);
// removing also removes the runtimes no app uses any more
export const uninstall = (app) => job('uninstall', app, [['uninstall', M.refOf(app.id, app.branch)], ['uninstallUnused']]);
export const update = (app) => job('update', app, [['update', M.refOf(app.id, app.branch)], ['uninstallUnused']]);
export const updateAll = () => job('updateAll', null, [['update'], ['uninstallUnused']]);
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
export const installedApp = (id) => state.get().installed.find((a) => a.id === id) || null;
