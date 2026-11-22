// flathubmodel.js — the Store's Flathub section as plain data (docs/FLATPAK.md, phase 1 step 5): an app's declared
// permissions from `flatpak remote-info --show-metadata` (lord.flatpak.start('info', ref)) in plain language with what
// LordOS changes, the catalog
// filtered (verified only unless the setting shows the rest), the installed list (`flatpak list`) and the progress of a
// job. Only for showing: how a Flatpak app is launched never reads its metadata (server/src/fpak.c fpak_argv sets the
// never-granted options and LordOS's restrictions whatever the app declares).
//
// Tested by server/tests/flathub_model_test.mjs.

export const ARCH = 'x86_64';                 // phase 1: PCs only (lord.flatpak.available() is false elsewhere)
export const APPSTREAM_EVERY = 24 * 3600e3;   // the catalog is fetched again when the Store opens after a day

const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_-]*){2,}$/;
export const idOk = (id) => typeof id === 'string' && id.length <= 59 && ID_RE.test(id) && !/-.*\./.test(id);
export const branchOk = (b) => typeof b === 'string' && /^[A-Za-z0-9._-]{1,64}$/.test(b) && b !== '.' && b !== '..';
export const refOf = (id, branch = 'stable') => (idOk(id) && branchOk(branch) ? `app/${id}/${ARCH}/${branch}` : null);

// The metadata key file (lines as the command printed them) → { runtime, base, context, session, system }; groups and
// keys LordOS doesn't show are skipped. Lists are split on ';' with the empty ends dropped.
export function parseMetadata(lines) {
  const out = { runtime: '', base: '', command: '', context: { shared: [], sockets: [], devices: [], features: [], filesystems: [] }, session: {}, system: {} };
  let group = '';
  const list = (v) => v.split(';').map((s) => s.trim()).filter(Boolean);
  for (const raw of lines || []) {
    const line = String(raw).trim();
    if (!line || line[0] === '#') continue;
    const g = /^\[([^\]]+)\]$/.exec(line);
    if (g) { group = g[1]; continue; }
    const eq = line.indexOf('=');
    if (eq <= 0) continue;
    const k = line.slice(0, eq).trim(), v = line.slice(eq + 1).trim();
    if (group === 'Application') {
      if (k === 'runtime') out.runtime = v;
      else if (k === 'base') out.base = v;
      else if (k === 'command') out.command = v;
    } else if (group === 'Context') {
      if (Object.prototype.hasOwnProperty.call(out.context, k)) out.context[k] = list(v);
    } else if (group === 'Session Bus Policy') out.session[k] = v;
    else if (group === 'System Bus Policy') out.system[k] = v;
  }
  return out;
}

// Session bus names LordOS never lets a Flatpak app reach (fpak.c never_talk) and the ones it always may (talk_only);
// the rest of the declared names are "other apps". org.freedesktop.portal.Flatpak is LordOS's own Spawn (sub-sandboxes
// of the same app, E9): a portal like the others, never a way out of the sandbox.
const NEVER_TALK = {
  'org.freedesktop.Flatpak': 'spawn', 'org.freedesktop.systemd1': 'systemd', 'org.freedesktop.secrets': 'secrets',
};
const QUIET_TALK = ['org.freedesktop.Notifications', 'org.kde.StatusNotifierWatcher', 'org.freedesktop.ScreenSaver', 'com.canonical.AppMenu.Registrar', 'org.a11y.Bus'];
const quiet = (name) => QUIET_TALK.includes(name) || name.startsWith('org.freedesktop.portal.');

// What the app declares, row by row: { perm, args, change, level }. perm and change are keys under "flathub.perm." and
// "flathub.change." (change null when LordOS changes nothing); level 'never' (declared, and LordOS removes it: the app
// may not work), 'limited' (LordOS narrows it) or 'info'.
export function permissionRows(meta) {
  if (!meta) return [];
  const rows = [];
  const add = (perm, change, level, args) => rows.push({ perm, change, level, args: args || {} });
  const c = meta.context;
  const has = (arr, v) => arr.includes(v);
  // files: the whole computer or the home folder; other places listed as they are written
  const fsl = c.filesystems.filter((f) => f[0] !== '!');
  const plain = (f) => f.replace(/:(rw|create)$/, '');
  const whole = fsl.map(plain).filter((f) => ['host', 'host-os', 'host-etc', 'home'].includes(f.replace(/:ro$/, '')));
  if (whole.some((f) => f.startsWith('host'))) add('filesHost', 'files', 'limited');
  else if (whole.length) add('filesHome', 'files', 'limited');
  const other = fsl.map(plain).filter((f) => !['host', 'host-os', 'host-etc', 'home'].includes(f.replace(/:ro$/, '')));
  if (other.length) add('filesOther', null, 'info', { list: other.join(', ') });
  if (has(c.shared, 'network')) add('network', 'network', 'info');
  if (has(c.sockets, 'pulseaudio')) add('sound', 'sound', 'info');
  if (has(c.devices, 'all')) add('devicesAll', 'devices', 'info');
  else {
    if (has(c.devices, 'usb')) add('usb', 'devices', 'info');
    if (has(c.devices, 'input')) add('input', null, 'info');
  }
  if (has(c.devices, 'kvm')) add('kvm', null, 'info');
  if (has(c.features, 'bluetooth')) add('bluetooth', 'devices', 'info');
  if (has(c.features, 'devel')) add('devel', null, 'info');
  if (has(c.sockets, 'x11')) add('x11', 'x11', 'limited');
  if (has(c.sockets, 'cups')) add('printing', null, 'info');
  if (has(c.sockets, 'ssh-auth')) add('ssh', null, 'info');
  if (has(c.sockets, 'gpg-agent')) add('gpg', null, 'info');
  if (has(c.sockets, 'pcsc')) add('smartcard', null, 'info');
  if (has(c.sockets, 'session-bus')) add('sessionBus', 'never', 'never');
  if (has(c.sockets, 'system-bus')) add('systemBus', 'never', 'never');
  // the session bus names it asks for
  const seen = new Set();
  for (const [name, policy] of Object.entries(meta.session)) {
    const k = NEVER_TALK[name];
    if (k && policy !== 'none' && !seen.has(k)) { seen.add(k); add(k, 'never', 'never'); }
  }
  const talks = Object.entries(meta.session).filter(([name, p]) => p !== 'none' && !NEVER_TALK[name] && !quiet(name)).map(([name]) => name);
  if (talks.length) add('talks', 'talks', 'limited', { list: talks.join(', '), count: talks.length });
  const sys = Object.entries(meta.system).filter(([, p]) => p !== 'none').map(([name]) => name);
  if (sys.length) add('system', null, 'info', { list: sys.join(', '), count: sys.length });
  const known = { sockets: ['x11', 'fallback-x11', 'wayland', 'pulseaudio', 'session-bus', 'system-bus', 'ssh-auth', 'pcsc', 'cups', 'gpg-agent', 'inherit-wayland-socket'],
    devices: ['dri', 'all', 'usb', 'input', 'kvm', 'shm'], features: ['bluetooth', 'devel', 'multiarch', 'canbus', 'per-app-dev-shm'], shared: ['network', 'ipc'] };
  const unknown = Object.entries(known).flatMap(([k, ok]) => c[k].filter((v) => !ok.includes(v) && v[0] !== '!'));
  if (unknown.length) add('other', null, 'info', { list: unknown.join(', ') });
  return rows;
}

// How the app shows its windows, from the sockets it declares, by Flatpak's own rule (1.16, Debian 13's:
// common/flatpak-run-sockets.c, `allow_x11 = !has_wayland` when fallback-x11 is set): 'wayland' (Wayland only),
// 'waylandFallback' (wayland and fallback-x11: X11 only when it gets no Wayland, and LordOS always gives it, so Wayland;
// a declared x11 is ignored then too), 'both' (wayland and x11: it gets both and its toolkit chooses), 'x11' (X11 only,
// or fallback-x11 without wayland: through Xwayland in LordOS) or 'none' (neither); null without metadata.
// The keys are under "flathub.display.".
export function displayOf(meta) {
  if (!meta || !meta.context) return null;
  const s = meta.context.sockets;
  const wl = s.includes('wayland'), x = s.includes('x11'), fb = s.includes('fallback-x11');
  if (fb) return wl ? 'waylandFallback' : 'x11';
  if (wl) return x ? 'both' : 'wayland';
  return x ? 'x11' : 'none';
}

// The toolkit an app is built with, from its runtime and base app: { kind, runtime, version } with kind a key under
// "flathub.toolkit." ('electron' and 'chromium' from the base, Electron's old io.atom base too; 'gtk' on GNOME's or
// elementary's platform; 'qt' on KDE's or a Qt base; else 'other': the app brings its own on the Freedesktop platform)
// and the runtime's id and branch as written; null without metadata. Only for showing, like the permission rows.
export function toolkitOf(meta) {
  if (!meta) return null;
  const rt = String(meta.runtime || '').split('/');
  const base = String(meta.base || '').split('/')[1] || '';
  const id = rt[0] || '';
  let kind = 'other';
  if (base === 'org.electronjs.Electron2.BaseApp' || base === 'io.atom.electron.BaseApp') kind = 'electron';
  else if (base === 'org.chromium.Chromium.BaseApp') kind = 'chromium';
  else if (id === 'org.gnome.Platform' || id === 'io.elementary.Platform') kind = 'gtk';
  else if (id === 'org.kde.Platform' || base.startsWith('io.qt.')) kind = 'qt';
  return { kind, runtime: id, version: rt[2] || '' };
}

// The catalog language from LordOS's ('zh-Hant' → AppStream's 'zh_TW'); lord.flatpak.loadCatalog takes [A-Za-z0-9_-].
export function catalogLang(lang) {
  const l = String(lang || 'en');
  if (l === 'zh-Hant') return 'zh_TW';
  if (l === 'zh-Hans') return 'zh_CN';
  return /^[A-Za-z0-9_-]{2,16}$/.test(l) ? l : 'en';
}

const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');

// The catalog the Store shows: the verified apps unless showAll, those in category cat ('Utility', a freedesktop main
// category), those matching every word of query (name, summary, developer, id); by name.
export function filterCatalog(cat, { query = '', showAll = false, category = null } = {}) {
  const words = fold(query).split(/\s+/).filter(Boolean);
  const out = (cat || []).filter((a) => (showAll || a.verified)
    && (!category || String(a.categories || '').split(';').includes(category))
    && (!words.length || words.every((w) => fold(`${a.name} ${a.summary} ${a.developer} ${a.id}`).includes(w))));
  // by name; while searching, the apps whose name starts with the first word come first
  // each name folded once, not in every comparison (thousands of apps)
  const first = words[0];
  const keyed = out.map((a) => { const n = fold(a.name); return { a, n, s: first && n.startsWith(first) ? 0 : 1 }; });
  keyed.sort((x, y) => (x.s !== y.s ? x.s - y.s : x.n < y.n ? -1 : x.n > y.n ? 1 : 0));
  return keyed.map((x) => x.a);
}

// How many shown apps each category has (the sidebar's counts).
export function categoryCounts(cat, showAll) {
  const out = {};
  for (const a of cat || []) {
    if (!showAll && !a.verified) continue;
    for (const c of String(a.categories || '').split(';')) if (c) out[c] = (out[c] || 0) + 1;
  }
  return out;
}

// `flatpak list --app --columns=application,branch,version,origin` → [{ id, branch, version, origin }]; a line that
// isn't one (a header, a warning) is skipped. Columns are tab-separated when the output is not a terminal.
export function parseList(lines) {
  const out = [];
  for (const raw of lines || []) {
    const p = String(raw).split('\t').map((s) => s.trim());
    if (p.length < 2 || !idOk(p[0]) || !branchOk(p[1])) continue;
    out.push({ id: p[0], branch: p[1], version: p[2] || '', origin: p[3] || '' });
  }
  return out;
}

// The progress in a line of `flatpak install` / `update`: its percentage ("Installing 2/3… ████ 45%  1.2 MB/s"), else
// from its step ("Installing 2/3…" without a terminal: the steps before it done), or null.
export function progressOf(line) {
  const s = String(line || '');
  const m = /(?:^|\s)(\d{1,3})%(?=\s|$)/.exec(s);
  const step = /(?:^|\s)(\d{1,3})\/(\d{1,3})(?=[\s…:.]|$)/.exec(s);
  const of = step ? [Number(step[1]), Number(step[2])] : null;
  const okStep = of && of[0] >= 1 && of[0] <= of[1];
  if (m && Number(m[1]) <= 100) return okStep ? Math.round(((of[0] - 1) + Number(m[1]) / 100) / of[1] * 100) : Number(m[1]);
  return okStep ? Math.round((of[0] - 1) / of[1] * 100) : null;
}

// Does the catalog need fetching again (the Store opened and the last update --appstream is older than a day)?
export const appstreamStale = (at, now) => !(at > 0) || now - at > APPSTREAM_EVERY || at > now + 60e3;

// The Flathub settings (settings.flathub): { on, showAll, appstreamAt }.
export const prefsOf = (s) => ({ on: !!(s && s.on), showAll: !!(s && s.showAll), appstreamAt: (s && Number(s.appstreamAt)) || 0 });

// A note on an app's page about how it works in LordOS (the Flathub compatibility run, 2026-11-22): a key under
// "flathub.note.", or null. OBS Studio: the home folder is read-only in a Flatpak app's view (docs/FLATPAK.md, E2), so
// recordings can only go to the person's folders; OBS's Flatpak asks for Videos, where they go.
const NOTES = { 'com.obsproject.Studio': 'obsVideos' };
export const appNote = (id) => (typeof id === 'string' && Object.prototype.hasOwnProperty.call(NOTES, id) ? NOTES[id] : null);
