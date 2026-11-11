// webappsmodel.js — web apps (docs/WEBAPPS.md) as plain data, with no runtime or React, so it can be tested with
// node (server/tests/webapps_model_test.mjs): the rules lord-server checks again (ids, addresses), the window's
// app id, what the Dock, the launcher and the switcher show for a web app, the Store's catalog and its
// "Add from an address" sheet, and the Settings page's rows.
//
// lord-server is the one that decides: these only shape what the person sees and what is asked of
// lord.webapps (server/src/ui/js_webapps.c), which validates everything again.
import { utf8Length } from './utf8.js';

export const WEB_ID_RE = /^[a-z0-9][a-z0-9-]{0,39}$/;
export const PERM_KINDS = ['notifications', 'camera', 'microphone', 'screen', 'location'];
export const PERM_VALUES = ['allow', 'ask', 'deny'];
export const WINDOW_PREFIX = 'lordos-web.';
export const DOCK_PREFIX = 'web:';
export const RUNTIME_PACKAGE = 'lordos-web';
export const URL_MAX = 2047;                 // the probe's one line on fd 3 is at most 2 KB

export const isWebId = (s) => typeof s === 'string' && WEB_ID_RE.test(s);

// The xdg_toplevel app id of a lord-web window, "lordos-web.<app>.<profile>" → { app, profile }, or null.
export function parseWindowAppId(appId) {
  if (typeof appId !== 'string' || !appId.startsWith(WINDOW_PREFIX)) return null;
  const rest = appId.slice(WINDOW_PREFIX.length).split('.');
  if (rest.length !== 2 || !isWebId(rest[0]) || !isWebId(rest[1])) return null;
  return { app: rest[0], profile: rest[1] };
}

export const windowAppId = (app, profile) => `${WINDOW_PREFIX}${app}.${profile}`;

// The launcher's and the Dock's id of a web app: "web:<app>".
export const webKey = (app) => (isWebId(app) ? `${DOCK_PREFIX}${app}` : null);
export const webAppOfKey = (key) => {
  const s = String(key ?? '');
  return s.startsWith(DOCK_PREFIX) && isWebId(s.slice(DOCK_PREFIX.length)) ? s.slice(DOCK_PREFIX.length) : null;
};

export const profileOf = (entry, id) => (entry && Array.isArray(entry.profiles) ? entry.profiles.find((p) => p.id === id) || null : null);
export const defaultProfile = (entry) => (entry && Array.isArray(entry.profiles) && entry.profiles[0]) || null;

// What a window of a web app is called: the app's name, and the profile's when the app has more than one.
// → { name, profile } (profile: null with one profile, or when the window's profile is not in the registry).
export function windowLabel(entry, profileId) {
  if (!entry) return null;
  const p = profileOf(entry, profileId);
  return { name: entry.name, profile: p && entry.profiles.length > 1 ? p.name : null };
}

// A window, its registry entry and lord-server's word on whose process it is: the window counts as that web app
// only when the app id and the process agree (any program can set any app id). owner: lord.webapps.owner(surface).
export function windowWebApp(appId, owner, entries) {
  const id = parseWindowAppId(appId);
  if (!id || !owner || owner.app !== id.app || owner.profile !== id.profile) return null;
  const entry = (entries || []).find((e) => e.id === id.app);
  return entry ? { entry, profile: id.profile } : null;
}

// The Dock's right-click menu of a web app: "New window: <profile>" for each profile but the first (the
// first is the click itself), running ones marked; [] with one profile.
export function profileMenu(entry, running = []) {
  if (!entry || !Array.isArray(entry.profiles) || entry.profiles.length < 2) return [];
  const live = new Set(running.filter((r) => r.app === entry.id).map((r) => r.profile));
  return entry.profiles.slice(1).map((p) => ({ profile: p.id, name: p.name, running: live.has(p.id) }));
}

// What the launcher lists for a web app (the shape useLauncherApps gives every app).
export function launcherItem(entry) {
  const key = webKey(entry && entry.id);
  if (!key || typeof entry.name !== 'string') return null;
  return { id: key, kind: 'web', name: entry.name, comment: hostOf(entry.origin), category: 'Network', webApp: entry.id };
}

export function hostOf(url) {
  const m = /^https?:\/\/([^/?#]+)/.exec(String(url || ''));
  return m ? m[1] : '';
}

// ---- the Store: "Add from an address" ----

// What the person typed → { url } to probe, or { error } ('empty', 'scheme', 'invalid', 'long'). A bare host gets
// https://; only https: is probed (docs/WEBAPPS.md). The host is made lowercase; nothing else is changed.
export function normalizeAddress(text) {
  let s = String(text ?? '').trim();
  if (!s) return { error: 'empty' };
  if (s.length > URL_MAX) return { error: 'long' };
  if (!/^[a-z][a-z0-9+.-]*:(?!\d)/i.test(s)) s = `https://${s}`;     // "host:8443/x" is a host and a port, not a scheme
  const m = /^([a-z][a-z0-9+.-]*):\/\/([^/?#]*)(.*)$/i.exec(s);
  if (!m) return { error: 'invalid' };
  if (m[1].toLowerCase() !== 'https') return { error: 'scheme' };
  const host = m[2].toLowerCase();
  if (!host || host.includes('@') || /[^a-z0-9.:\-[\]]/.test(host) || /^[.-]|\.\.|[.-]$/.test(host.replace(/:\d+$/, ''))) return { error: 'invalid' };
  if (/[\s\\]/.test(m[3]) || /[^\x21-\x7e]/.test(m[3])) return { error: 'invalid' };
  const url = `https://${host}${m[3] || '/'}`;
  return url.length > URL_MAX ? { error: 'long' } : { url };
}

// The sheet's state: idle → probing → found | failed → installing → installed | failed.
//   { step, address, error, probe, result, app }
export const ADD_IDLE = { step: 'idle', address: '', error: null, probe: 0, result: null, app: null };
export function addReducer(s, a) {
  switch (a.type) {
    case 'type': return s.step === 'probing' || s.step === 'installing' ? s : { ...ADD_IDLE, address: String(a.text ?? '') };
    case 'probe': {
      if (s.step === 'probing' || s.step === 'installing') return s;
      const n = normalizeAddress(s.address);
      return n.error ? { ...s, step: 'failed', error: `address.${n.error}` } : { ...s, step: 'probing', error: null, url: n.url, result: null };
    }
    case 'started': return s.step === 'probing' ? { ...s, probe: a.id } : s;
    case 'result': {
      if (s.step !== 'probing' || (s.probe && a.id !== s.probe)) return s;
      const r = a.result || {};
      return r.ok ? { ...s, step: 'found', result: r, name: r.name } : { ...s, step: 'failed', error: r.error || 'webapps.probeFailed' };
    }
    case 'rename': return s.step === 'found' ? { ...s, name: String(a.name ?? '') } : s;
    case 'install': return s.step === 'found' && nameOk(s.name) ? { ...s, step: 'installing' } : s;
    case 'installed': return s.step === 'installing' ? { ...s, step: 'installed', app: a.app } : s;
    case 'error': return { ...s, step: 'failed', error: a.error || 'webapps.probeFailed' };
    case 'reset': return ADD_IDLE;
    default: return s;
  }
}

// The entry lord.webapps.install takes for each source (lord-server reads the rest from the catalog or the probe).
export const catalogEntry = (id, profileName) => ({ source: 'catalog', id, profile: profileName });
export const addressEntry = (s, profileName) => ({ source: 'url', probe: s.probe, name: String(s.name || '').trim(), profile: profileName });

// The same name rule lord-server applies (webreg_name_ok): 1 to 64 bytes, no control or invisible characters,
// no space at either end.
const HIDDEN = /[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁠-⁩﻿]/;
export function nameOk(name) {
  if (typeof name !== 'string' || !name || name !== name.trim() || HIDDEN.test(name)) return false;
  return utf8Length(name) <= 64;
}


// The catalog (assets/webapps/catalog.json) for the Store: each entry, whether it is installed, in the catalog's
// order or filtered by a search. An entry that breaks a rule is left out.
export function catalogApps(catalog, installed = [], query = '') {
  const have = new Set(installed.map((e) => e.id));
  const q = String(query || '').trim().toLowerCase();
  return (catalog && Array.isArray(catalog.apps) ? catalog.apps : [])
    .filter((a) => catalogEntryOk(a))
    .filter((a) => !q || a.name.toLowerCase().includes(q) || (a.keywords || []).some((k) => k.includes(q)) || hostOf(a.startUrl).includes(q))
    .map((a) => ({ ...a, installed: have.has(a.id) }));
}

export function catalogEntryOk(a) {
  if (!a || !isWebId(a.id) || !nameOk(a.name) || typeof a.description !== 'string') return false;
  if (!/^[a-z0-9-]+\.png$/.test(a.icon || '')) return false;
  const origin = `https://${hostOf(a.startUrl)}`;
  return typeof a.startUrl === 'string' && a.startUrl.startsWith('https://') && a.origin === origin &&
    typeof a.scope === 'string' && a.scope.startsWith(`${origin}/`) && a.startUrl.startsWith(a.scope) &&
    (!a.themeColor || /^#[0-9a-f]{6}$/i.test(a.themeColor));
}

// Which state the Store's section is in: always the list; without lord-web nothing can be probed ("Add from an
// address" waits), and the first install brings the engine first (getReducer).
export function storeSection({ available, installed }) {
  return { view: 'list', canAdd: !!available, needsEngine: !available, count: installed.length };
}

// The catalog's first apps, for Discover's banner and shelf (the catalog's order is LordOS's choice).
export const featuredApps = (catalog, installed = [], n = 4) => catalogApps(catalog, installed).slice(0, n);
// The catalog's apps of one of the Store's categories (its category pages show them above the packages).
export const categoryApps = (catalog, installed, cat) => catalogApps(catalog, installed).filter((a) => a.category === cat);

// ---- the Store: getting a web app, the engine first when it isn't there ----

// The steps of Get / Install (the catalog sheet and a web app's page):
//   idle → app → done                                   lord-web is installed: lord.webapps.install, nothing else
//   idle → engine → engineJob → app → done              the first web app: the lordos-web package's plan is shown
//                                                       (its download and disk size), Install runs the Store's own
//                                                       package job (the admin password, the progress), and when it
//                                                       ends well the app is installed with no other click
//   … → failed                                          the plan can't be made, the job failed, or install threw
// { step, plan, error, text, app }: error is a code (errorText), text a message the package job already wrote.
export const GET_IDLE = { step: 'idle', plan: null, error: null, text: null, app: null };
export function getReducer(s, a) {
  switch (a.type) {
    case 'start':
      if (s.step !== 'idle' && s.step !== 'failed' && s.step !== 'done') return s;
      return a.available ? { ...GET_IDLE, step: 'app' } : { ...GET_IDLE, step: 'engine', plan: a.plan || null };
    case 'confirm': return s.step === 'engine' && s.plan && s.plan.ok ? { ...s, step: 'engineJob' } : s;
    case 'jobDone':
      if (s.step !== 'engineJob') return s;
      if (!a.ok) return { ...s, step: 'failed', error: null, text: a.text || null };
      // the package is in, but lord-server must find lord-web next to it before an app is installed
      return a.available ? { ...s, step: 'app' } : { ...s, step: 'failed', error: 'webapps.noRuntime' };
    case 'installed': return s.step === 'app' ? { ...s, step: 'done', app: a.app } : s;
    case 'error': return s.step === 'app' ? { ...s, step: 'failed', error: a.error || 'webapps.badEntry' } : s;
    case 'cancel': return s.step === 'engineJob' || s.step === 'app' ? s : GET_IDLE;
    default: return s;
  }
}
export const getBusy = (s) => s.step === 'engineJob' || s.step === 'app';
// What the engine step says it takes: the plan's download and the space it adds, or null without a usable plan.
export function engineSizes(plan) {
  if (!plan || !plan.ok) return null;
  const n = (v) => (Number.isFinite(v) && v > 0 ? v : 0);
  return { download: n(plan.download), disk: n(plan.installedDelta) };
}

// ---- the Store: a web app's page ----

// The catalog's data about an app that the page shows (all optional; a wrong value reads as missing):
//   publisher        who makes the service (data: a name, never in LordOS's texts)
//   verified         "YYYY-MM-DD": the day LordOS last checked it works (docs/WEBAPPS.md)
//   typicalMemoryMB  what one profile took when LordOS measured it (Pss of lord-web and WebKit's processes)
//   limitations      keys of what doesn't work in LordOS's engine (webapps.limit.<key>)
export const LIMITATIONS = ['drm', 'calls', 'push'];
export function validDate(s) {
  if (typeof s !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(s)) return false;
  const d = new Date(`${s}T00:00:00Z`);
  return !Number.isNaN(d.getTime()) && d.toISOString().slice(0, 10) === s;
}
export function catalogFacts(a) {
  const pub = a && typeof a.publisher === 'string' ? a.publisher.trim() : '';
  return {
    publisher: pub && pub.length <= 80 ? pub : null,
    verified: a && validDate(a.verified) ? a.verified : null,
    typicalMemoryMB: a && Number.isInteger(a.typicalMemoryMB) && a.typicalMemoryMB > 0 && a.typicalMemoryMB < 65536 ? a.typicalMemoryMB : null,
    limitations: a && Array.isArray(a.limitations) ? LIMITATIONS.filter((k) => a.limitations.includes(k)) : [],
  };
}
// "Tested on LordOS": the day it was verified; tested with no day recorded; or not tested yet.
export function testedState(a) {
  const f = catalogFacts(a);
  if (f.verified) return { state: 'verified', date: f.verified };
  return { state: a && a.tested === true ? 'tested' : 'untested' };
}

// What the page is about: a catalog app (installed or not) or an installed one added from an address. null when the
// id is neither. installed: the registry's entries (lord.webapps.list).
export function detailOf(id, catalog, installed = []) {
  const entry = (installed || []).find((e) => e.id === id) || null;
  const cat = catalogApps(catalog, installed).find((a) => a.id === id) || null;
  if (!entry && !cat) return null;
  const fromCatalog = entry ? entry.source === 'catalog' && !!cat : true;
  return {
    id, entry, catalog: fromCatalog ? cat : null, installed: !!entry,
    name: entry ? entry.name : cat.name,
    origin: entry ? entry.origin : cat.origin,
    source: fromCatalog ? 'catalog' : 'url',
    background: entry ? !!entry.background : !!cat.background,
  };
}

// Size on disk: each profile's data (lord.fs.du's answers by profile id) and the total; not installed: none.
export function diskUsage(entry, sizes = {}) {
  if (!entry || !Array.isArray(entry.profiles)) return { installed: false, total: 0, complete: true, profiles: [] };
  const profiles = entry.profiles.map((p) => {
    const d = sizes && sizes[p.id];
    const ok = d && Number.isFinite(d.size) && d.size >= 0;
    return { id: p.id, name: p.name, bytes: ok ? d.size : 0, complete: !ok || d.complete !== false };
  });
  return { installed: true, total: profiles.reduce((t, p) => t + p.bytes, 0), complete: profiles.every((p) => p.complete), profiles };
}

// Memory: what it takes now (lord.webapps.memory: Pss in kB, by running profile), else what LordOS measured.
export function memoryView(live, facts) {
  if (live && Number.isFinite(live.kb) && live.kb > 0) {
    return { kind: 'live', bytes: live.kb * 1024,
      profiles: (Array.isArray(live.profiles) ? live.profiles : []).filter((p) => p && Number.isFinite(p.kb)).map((p) => ({ profile: p.profile, bytes: p.kb * 1024 })) };
  }
  if (facts && facts.typicalMemoryMB) return { kind: 'typical', bytes: facts.typicalMemoryMB * 1024 * 1024, profiles: [] };
  return { kind: 'unknown', bytes: 0, profiles: [] };
}

// The upstream version in a Debian version: "2.48.7-1~deb13u1" → "2.48.7", "1:2.0+dfsg-3" → "2.0"; "" if none.
export function upstreamVersion(v) {
  const m = /^(?:\d+:)?(\d+(?:\.\d+)*)/.exec(String(v || ''));
  return m ? m[1] : '';
}
// The WebKit package lord-web links (tools/iso/mkdeb-web.sh), whose version the page names.
export const WEBKIT_PACKAGE = 'libwpewebkit-2.0-1';

// ---- Settings → Web apps ----

// A permission row per kind, in the order the page shows them; an unknown value reads as 'ask'.
export function permissionRows(entry) {
  const p = (entry && entry.permissions) || {};
  return PERM_KINDS.map((kind) => ({ kind, value: PERM_VALUES.includes(p[kind]) ? p[kind] : kind === 'location' ? 'deny' : 'ask' }));
}

export const canRemoveProfile = (entry) => !!entry && Array.isArray(entry.profiles) && entry.profiles.length > 1;
export const canAddProfile = (entry) => !!entry && Array.isArray(entry.profiles) && entry.profiles.length < 16;

// A name for a new profile the person has not typed yet: base, then "base 2", "base 3"… not used by another profile.
export function nextProfileName(entry, base) {
  const used = new Set(((entry && entry.profiles) || []).map((p) => p.name.toLowerCase()));
  if (!used.has(base.toLowerCase())) return base;
  for (let i = 2; i < 100; i++) if (!used.has(`${base} ${i}`.toLowerCase())) return `${base} ${i}`;
  return base;
}

// The data folder of a profile, relative to the home folder (read-only from JS: lord.fs.du measures it).
export const dataDir = (app, profile) => (isWebId(app) && (!profile || isWebId(profile)) ? `.local/share/lordos/web/${app}${profile ? `/${profile}` : ''}` : null);

// The answer the permission dialog sends: remembered (allow / deny, kept in the registry) or this time only.
export const askAnswer = (allow, remember) => (allow ? (remember ? 'allow' : 'once') : remember ? 'deny' : 'deny-once');

// ---- the site's own icon (lord.webapps.fetchIcon / iconResult: ~/.cache/lordos/web/icons/<app>.<png|svg>) ----

export const ICON_CACHE_DIR = '.cache/lordos/web/icons';
// The cached icon's file name: the app id and the kind's extension, nothing taken from the site. null when either is bad.
export const iconCacheName = (app, kind) => (isWebId(app) && (kind === 'png' || kind === 'svg') ? `${app}.${kind}` : null);
// Is path exactly one of app's own icon files (the registry's, the cache's or a profile's), under home? A notification
// from lord-web names its icon as app_icon: the shell shows that file only when this holds, else the app's known icon.
export function webIconPathOk(path, app, home, profile = null) {
  if (typeof path !== 'string' || typeof home !== 'string' || !home.startsWith('/') || !isWebId(app) || path.includes('/../')) return false;
  const ok = [`${home}/.config/lordos/web/icons/${app}.png`, `${home}/.config/lordos/web/icons/${app}.svg`,
    `${home}/${ICON_CACHE_DIR}/${iconCacheName(app, 'png')}`, `${home}/${ICON_CACHE_DIR}/${iconCacheName(app, 'svg')}`];
  if (isWebId(profile)) ok.push(`${home}/.config/lordos/web/icons/${app}.${profile}.png`);
  return ok.includes(path);
}
// Whether the Store or a launch should fetch the site's icon now: lord-web there, not fetched yet and not tried
// this session (a failure is not retried until the next session or "Refresh icon").
export const wantsSiteIcon = ({ available, cached, tried }) => !!available && !cached && !tried;

// ---- unread counts on the Dock (org.lordos.WebApps1.SetBadge → the "webapps-badge" event) ----

// The counts after one event { app, profile, count }: { [app]: { [profile]: count } }, a 0 dropping the profile.
export function badgeReducer(state, ev) {
  if (!ev || !isWebId(ev.app) || !isWebId(ev.profile)) return state;
  const n = Math.max(0, Math.min(9999, Math.floor(Number(ev.count) || 0)));
  const app = { ...((state || {})[ev.app] || {}) };
  if (n) app[ev.profile] = n; else delete app[ev.profile];
  const next = { ...(state || {}) };
  if (Object.keys(app).length) next[ev.app] = app; else delete next[ev.app];
  return next;
}
// The sum of an app's profiles (or one profile's count).
export function badgeTotal(state, app, profile = null) {
  const a = (state || {})[app] || {};
  if (profile) return a[profile] || 0;
  return Object.values(a).reduce((s, n) => s + n, 0);
}
// What the badge says: '' for 0, the number up to 99, then '99+'.
export const badgeLabel = (n) => (!(n > 0) ? '' : n > 99 ? '99+' : String(Math.floor(n)));

// ---- notifications (Settings → Web apps "Notification sound": registry "notifySound") ----

export const NOTIFY_SOUNDS = ['auto', 'app', 'system', 'none'];
export const notifySoundOf = (entry) => (entry && NOTIFY_SOUNDS.includes(entry.notifySound) ? entry.notifySound : 'auto');
// Does LordOS play its own sound for a notification of this app? auto: only when the page played none of its own
// (lord-web sends the suppress-sound hint then); system: always; app and none: never.
export function playsSystemSound(entry, suppressed) {
  const s = notifySoundOf(entry);
  return s === 'system' || (s === 'auto' && !suppressed);
}
// The key Settings → Notifications files a web app's notifications under: its desktop id without the profile.
export const notifyKey = (app) => (isWebId(app) ? `${WINDOW_PREFIX}${app}` : null);
export const appOfNotifyKey = (key) => {
  const s = typeof key === 'string' && key.startsWith(WINDOW_PREFIX) ? key.slice(WINDOW_PREFIX.length) : '';
  return isWebId(s) ? s : null;
};

// ---- profile colors (the tag colors of Files and Databases) ----

export const PROFILE_COLORS = ['red', 'orange', 'yellow', 'green', 'blue', 'purple', 'gray'];
const DEFAULT_ORDER = ['orange', 'purple', 'blue', 'green', 'red', 'yellow', 'gray'];
// sdk/fileglyphs.jsx TAG_COLORS (Files' tags), the same as Databases' favorites and lord-server's webicon.c
const HEX = { red: '#ff453a', orange: '#ff9f0a', yellow: '#ffd60a', green: '#30d158', blue: '#0a84ff', purple: '#bf5af2', gray: '#8e8e93' };
export const profileHex = (color) => HEX[color] || null;
// A profile's color id, or null for none. A profile without one (made before colors) gets the default for its place:
// none for the first, then orange, purple, blue…
export function profileColor(entry, profileId) {
  const list = entry && Array.isArray(entry.profiles) ? entry.profiles : [];
  const i = list.findIndex((p) => p.id === profileId);
  if (i < 0) return null;
  const c = list[i].color;
  if (c === 'none') return null;
  if (PROFILE_COLORS.includes(c)) return c;
  return i === 0 ? null : DEFAULT_ORDER[(i - 1) % DEFAULT_ORDER.length];
}
// The letter in a profile's dot: its name's first character, in capitals.
export const profileInitial = (p) => (p && typeof p.name === 'string' && p.name.trim() ? Array.from(p.name.trim())[0].toLocaleUpperCase() : '');

// What LordOS calls a web app's window: with more than one profile the frame and the switcher say "<app> — <profile>"
// and the Dock's window list "<page title> — <profile>"; with one, the page's title (or the app's name).
// → { frame, menu, color } (color: the profile's color id or null).
export function webWindowTitles(entry, profileId, pageTitle) {
  const title = String(pageTitle || '') || (entry ? entry.name : '');
  const l = windowLabel(entry, profileId);
  if (!l || !l.profile) return { frame: title, menu: title, color: null };
  return { frame: `${l.name} — ${l.profile}`, menu: `${title} — ${l.profile}`, color: profileColor(entry, profileId) };
}

// ---- the shell's own choices per web app (per user, settings.json "webAppPrefs": { [app]: { badge, dockPerProfile } }) ----

export const WEB_PREFS_DEFAULTS = { badge: true, dockPerProfile: false };
export function webPrefs(s, app) {
  const p = s && s.webAppPrefs && typeof s.webAppPrefs === 'object' ? s.webAppPrefs[app] : null;
  return { badge: !(p && p.badge === false), dockPerProfile: !!(p && p.dockPerProfile === true) };
}
// The Dock entry of a web app's window: the app's own ("web:<app>"), or with "Each profile has its own Dock icon" a
// profile other than the first gets one of its own while it has windows ("linux:lordos-web.<app>.<profile>", never
// pinned; the first profile stays on the app's icon).
export function webDockId(entry, profileId, perProfile) {
  if (!entry) return null;
  const first = defaultProfile(entry);
  if (!perProfile || !first || first.id === profileId || !profileOf(entry, profileId)) return webKey(entry.id);
  return `linux:${windowAppId(entry.id, profileId)}`;
}
// What the Dock's badge counts for an entry: the app's sum, or one profile's on a profile's own icon.
// prefsOf(app): webPrefs; ownIcon(app, profile): that profile shows on an icon of its own now. A profile's icon
// counts its own; the app's icon sums every other profile (all of them without per-profile icons).
export function dockBadge(state, entryId, prefsOf, ownIcon = () => false) {
  const k = typeof entryId === 'string' ? entryId : '';
  const own = k.startsWith('linux:') ? parseWindowAppId(k.slice(6)) : null;
  const app = own ? own.app : webAppOfKey(k);
  if (!app || !prefsOf(app).badge) return '';
  if (own) return badgeLabel(badgeTotal(state, app, own.profile));
  const per = prefsOf(app).dockPerProfile;
  return badgeLabel(Object.entries((state || {})[app] || {}).reduce((s, [p, n]) => s + (per && ownIcon(app, p) ? 0 : n), 0));
}

// ---- Settings → Web apps: each profile's data ----

// When the page measures the profiles' data again: their ids and which of them run. A profile added during the visit
// has no folder until it first runs, so starting it changes the key (and the size is read again).
export const sizesKey = (entry, running = []) => {
  if (!entry || !Array.isArray(entry.profiles)) return '';
  const live = new Set(running.filter((r) => r.app === entry.id).map((r) => r.profile));
  return entry.profiles.map((p) => `${p.id}${live.has(p.id) ? '*' : ''}`).join(',');
};

// ---- editing (docs/WEBAPPS.md, "Editing") ----

// The Edit menu of a web app's window: [command, shortcut] rows and '-' separators. lord.webapps.edit(app,
// profile, command) sends the command to that window's lord-web (WebKit's editing command); the shortcuts are
// WebKit's own key bindings, which work in the page as well.
export const WEB_EDIT_ROWS = [['undo', 'Ctrl+Z'], ['redo', 'Ctrl+Shift+Z'], '-', ['cut', 'Ctrl+X'], ['copy', 'Ctrl+C'], ['paste', 'Ctrl+V'], '-', ['selectAll', 'Ctrl+A']];

// The Edit menu's items: label(command) is its text, run(command) what a click does.
export function webEditItems(label, run) {
  return WEB_EDIT_ROWS.map((r) => (r === '-' ? '-' : { label: label(r[0]), hint: r[1], onClick: () => run(r[0]) }));
}

// The items a page's context menu may have (server/src/web/webedit.c's closed list, in its order); the editing
// ones come first, then the link's, then the image's, with a separator between the groups.
export const WEB_MENU_ITEMS = ['cut', 'copy', 'paste', 'selectAll', 'copyLink', 'openLink', 'copyImage'];
const MENU_GROUP = { cut: 0, copy: 0, paste: 0, selectAll: 0, copyLink: 1, openLink: 1, copyImage: 2 };

// The "webapps-menu" event's items → ContextMenu items. Unknown names and repeats are left out (lord-server sends
// only known ones, once); pick(name) is what a click does.
export function webMenuItems(names, label, pick) {
  const out = [];
  let group = -1;
  for (const n of Array.isArray(names) ? names : []) {
    if (!WEB_MENU_ITEMS.includes(n) || out.some((i) => i !== '-' && i.id === n)) continue;
    if (group >= 0 && MENU_GROUP[n] !== group) out.push('-');
    group = MENU_GROUP[n];
    out.push({ id: n, label: label(n), onClick: () => pick(n) });
  }
  return out;
}
