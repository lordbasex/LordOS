// sessionrestore.jsx — the apps that were open come back at the next login (docs/SESSIONS.md, "Session
// restore"). Settings → Session: off, ask, always.
//
// A power cut gives no warning and a crash runs nothing, so what is open is saved as it changes, not
// at the end: a debounced snapshot of the windows store goes to settings.sessionSaved (settings.json, already on lord.fs's deny list; the format is in
// sessionmodel.js). Its "clean" field is the unclean-exit marker: false while a session runs, true only
// after a logout, a Restart or Shut Down confirmed here, or a stop that reached __lordBeforeExit.
//
// Everything reopens through the paths a Dock click uses (launchDockItem: launchApp → wm.open,
// lord.apps.launchPackage, lord.apps.launch(desktop id)), so a saved list can't run anything the Dock
// couldn't. A Linux window has no state to give back: the app starts again and its new window takes
// the saved frame by Dock id, first match wins.
//
// Never on a live system, the login screen or in an isolated app's process.
import { createElement as h, useEffect } from 'react';
import { View, Text, screen, mainDisplay, addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { useTheme } from './theme.js';
import { spacing, typeScale, controlSize } from './controls.jsx';
import { settings, saveSettingsNow } from './settings.js';
import { windows, wm, getApp, isLiveSystem, workAreaOf, setLinuxPlacement, SNAP_ZONES } from './windows.js';
import { dockIdOfWindow, launchDockItem } from './dock.js';
import { parseDockId } from './dockmodel.js';
import { useLauncherApps, shortAppName } from './apps.js';
import { admin } from './admin.js';
import { clipboard } from './clipboard.js';
import { RESTORE_MODES, RESTORE_DEFAULT, MAX_WINDOWS, NEVER, cleanArgs, cleanFrame, cleanList, restoreDecision, planRestore, fitFrame } from './sessionmodel.js';

const AUTOSAVE_MS = 2000;     // a dragged window or a new one reaches the disk this long after
const START_MS = 1500;        // after the theme's first frames
const STAGGER_MS = 450;       // between launches: a login must not start everything at once
const SETTLE_MS = 10000;      // after the last launch, the slow apps' windows may still arrive
const FRAME_TTL_MS = 90000;   // a saved frame nobody claimed by then is dropped

export const restoreMode = (s = settings.get()) => (RESTORE_MODES.includes(s.sessionRestore) ? s.sessionRestore : RESTORE_DEFAULT);
export const restoreBrowsers = (s = settings.get()) => s.sessionBrowsers !== false;

let armed = false;         // this session keeps the list (startSessionRestore ran)
let hold = true;           // the saved list is not ours yet: restoring, or waiting for the answer
let frozen = false;        // a logout or a shutdown took the last snapshot: closing windows don't count
let timer = 0;
let lastJson = '';

const allowed = () => typeof lord !== 'undefined' && typeof lord.appId !== 'string' && typeof lord.greeter === 'undefined' && !isLiveSystem();

// ---- what is open now ----

function entryOf(w) {
  if (w.pip || !w.id) return null;
  const isLinux = String(w.id).startsWith('linux#');
  const app = isLinux ? (w.args?.pkg ? getApp(w.args.appId) : null) : getApp(w.id);
  if (!isLinux && (!app || app.builtin)) return null;
  if (app && (app.hidden || app.live)) return null;
  const id = dockIdOfWindow(w);
  const p = parseDockId(id);                      // "linux:…": a window with no .desktop can't be launched
  if (!p || p.kind === 'shell' || NEVER.includes(p.key)) return null;
  const f = cleanFrame(w.snapped && w.restore ? w.restore : w);
  if (!f) return null;
  const e = { id, f };
  if (w.maximized) e.max = 1;
  if (w.minimized) e.min = 1;
  if (w.snapped) e.snap = w.snapped;
  const args = isLinux ? null : cleanArgs(app.id, w.args);
  if (args) e.args = args;
  return e;
}
const snapshot = () => windows.get().map(entryOf).filter(Boolean).slice(-MAX_WINDOWS);

// ---- autosave ----

function write(list, clean) {
  settings.set({ sessionSaved: { clean, windows: list } });
  saveSettingsNow(true);
}
function fire() {
  timer = 0;
  if (!armed || hold || frozen || restoreMode() === 'off') return;
  const list = snapshot(), json = JSON.stringify(list);
  if (json === lastJson) return;
  lastJson = json;
  write(list, false);
}
function schedule() { if (armed && !hold && !frozen && !timer) timer = setTimeout(fire, AUTOSAVE_MS); }

const savedList = () => settings.get().sessionSaved?.windows || [];

export function setRestoreMode(mode) {
  if (!RESTORE_MODES.includes(mode)) return;
  settings.set({ sessionRestore: mode, ...(mode === 'off' ? { sessionSaved: null } : null) });
  lastJson = '';
  schedule();
}

// The last word of a session whose JS is still in control: session.quit and the power dialog. Taken
// before anything closes; the windows closing afterwards are not the user closing them.
export function flushSession() {
  if (!armed || frozen || restoreMode() === 'off') return;
  frozen = true;
  clearTimeout(timer);
  timer = 0;
  write(hold ? savedList() : snapshot(), true);
}
// Every logout in the shell comes through here (session.quit, Search).
export function logOut() {
  flushSession();
  clipboard.beforeLogout();
  lord.quit();
}
// lord-sessiond refused the Restart or Shut Down: the session goes on.
export function resumeSession() {
  if (!armed || !frozen) return;
  frozen = false;
  write(savedList(), false);
  schedule();
}

// main.c, once, after the main loop and before the Wayland apps are closed. On a stop from outside
// (systemctl, the login screen's Restart) the apps may be going down at the same moment, so a shorter
// list than the saved one doesn't replace it.
globalThis.__lordBeforeExit = () => {
  if (!armed || frozen || restoreMode() === 'off') return;
  frozen = true;
  const list = hold ? savedList() : snapshot();
  write(list.length >= savedList().length ? list : savedList(), true);
};

// A theme switch discards this JS context in a running session: not an exit (themes.use).
export function markThemeSwitch() {
  if (!armed || frozen) return;
  clearTimeout(timer);
  timer = 0;
  settings.set({ sessionSaved: { windows: savedList(), clean: 'theme' } });
}

// ---- restoring ----

const claims = [];         // { id, entry, at }: frames waiting for their Linux window
let topEntry = null, topWin = null;

function afterOpen(id, e) {
  if (e.snap) wm.snap(id, e.snap);
  else if (e.max) wm.toggleMaximize(id);
  if (e.min) wm.minimize(id);
  if (e === topEntry) topWin = id;
  else if (topWin && windows.get().some((w) => w.id === topWin)) wm.focus(topWin);   // the front window stays in front
}
const fit = (f) => fitFrame(f, screen.displays, (d) => workAreaOf(d), Math.max(0, screen.displays.findIndex((d) => d.main)));

// sdk/wayland.js: a new Linux window of this Dock id. Gives its saved frame; then() finishes the job.
setLinuxPlacement((dockId) => {
  const now = Date.now();
  while (claims.length && now - claims[0].at > FRAME_TTL_MS) claims.shift();
  const i = claims.findIndex((c) => c.id === dockId);
  if (i < 0) return null;
  const [c] = claims.splice(i, 1);
  return { frame: fit(c.entry.f), then: (id) => afterOpen(id, c.entry) };
});

// Launched by a restore: Open at Login leaves these alone (sdk/dock.js).
export const restoredIds = new Set();

// A browser brings back its own windows from one launch (the action below); with that off, each
// window is a launch of its own.
const isBrowser = (id) => /^desktop:(chromium|firefox|firefox-esr)$/.test(id);
const runsOnce = (id) => {
  const p = parseDockId(id);
  if (p?.kind === 'lpk' || (p?.kind === 'app' && getApp(p.key)?.isolated)) return true;
  return isBrowser(id) && restoreBrowsers();
};

function restore(list) {
  hold = true;
  const open = new Map();
  for (const w of windows.get()) { const k = dockIdOfWindow(w); open.set(k, (open.get(k) || 0) + 1); }
  const steps = planRestore(list, open, runsOnce);
  topEntry = [...steps].reverse().find((s) => !s.entry.min)?.entry || null;
  topWin = null;
  const finish = () => { hold = false; lastJson = ''; schedule(); };
  if (!steps.length) { finish(); return; }
  let i = 0;
  const step = () => {
    const { entry: e, launch } = steps[i++];
    const p = parseDockId(e.id);
    restoredIds.add(e.id);
    try {
      const a = p.kind === 'desktop' ? null : getApp(p.key);
      if (a && !a.isolated && !a.package) {
        if (!a.builtin && !a.hidden && !a.live) {
          const first = !windows.get().some((w) => w.app === p.key);
          const id = wm.open(p.key, { ...(first ? null : { new: true }), ...(e.args ? { args: e.args } : null), frame: fit(e.f) });
          if (id) afterOpen(id, e);
        }
      } else if (a || p.kind === 'desktop') {
        claims.push({ id: e.id, entry: e, at: Date.now() });
        if (launch) launchDockItem(e.id);
      }
    } catch (err) { /* uninstalled: nothing to bring back */ }
    if (i < steps.length) setTimeout(step, STAGGER_MS);
    else setTimeout(finish, SETTLE_MS);
  };
  step();
}

// ---- the question ----

// null, or { list, unclean }
const askStore = createStore(null);

function answer(yes) {
  const a = askStore.get();
  askStore.set(null);
  if (!a) return;
  if (yes) { restore(a.list); return; }
  hold = false;
  lastJson = '';
  write(snapshot(), false);
}

function Button({ th, label, onClick, primary }) {
  return h(View, {
    onClick,
    style: { height: controlSize.regular, paddingHorizontal: 14, borderRadius: th.controlRadius ?? controlSize.radius, justifyContent: 'center', alignItems: 'center',
      backgroundColor: primary ? th.accent : th.control },
  }, h(Text, { style: { fontSize: typeScale.body, fontWeight: 500, color: primary ? '#fff' : th.text, whiteSpace: 'nowrap' } }, label));
}

export function RestorePrompt() {
  const ask = useStore(askStore);
  const th = useTheme();
  const apps = useLauncherApps();
  useEffect(() => {
    if (!ask) return undefined;
    return addListener('keydown', (e) => {
      if (e.key === 'Escape') answer(false);
      else if (e.key === 'Enter') answer(true);
    });
  }, [!!ask]);
  if (!ask) return null;
  const names = [];
  for (const e of ask.list) {
    const p = parseDockId(e.id);
    const a = apps.find((x) => (p.kind === 'desktop' ? x.desktopId === p.key : x.id === p.key));
    const n = a ? (a.kind === 'linux' ? shortAppName(a) : a.name) : null;
    if (n && !names.includes(n)) names.push(n);
  }
  const d = mainDisplay();
  const line = (key, s) => h(Text, { key, style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, s);
  return h(View, { style: { position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' } },
    h(View, { style: { width: 400, padding: spacing.xl, gap: spacing.s, borderRadius: (th.groupRadius ?? 10) + 2, backgroundColor: th.windowBg,
      border: `1px solid ${th.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' } },
    h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('shell.restore.title')),
    ask.unclean ? line('why', t('shell.restore.unclean')) : null,
    names.length ? line('apps', t('shell.restore.apps', { list: names.join(', ') })) : null,
    h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m } },
      h(Button, { th, label: t('shell.restore.no'), onClick: () => answer(false) }),
      h(Button, { th, primary: true, label: t('shell.restore.yes'), onClick: () => answer(true) }))));
}

// ---- browsers ----
// The closed admin action browser-restore (server/src/hw/hw_admin.c, docs/SESSIONS.md): each browser's
// own "continue where you left off" as a fixed policy, on or off. It is a machine-wide file.
let appliedBrowsers = '';
function applyBrowsers() {
  if (!lord.admin?.available()) return;
  const arg = restoreBrowsers() && restoreMode() !== 'off' ? 'on' : 'off';
  if (arg === appliedBrowsers) return;
  appliedBrowsers = arg;
  admin.run('browser-restore', arg, undefined, { quiet: true }).catch(() => { appliedBrowsers = ''; });
}

export function setRestoreBrowsers(on) { settings.set({ sessionBrowsers: !!on }); }

// ---- start (startShell, once per JS context) ----

export function startSessionRestore() {
  if (!allowed()) return;
  settings.subscribe(applyBrowsers);
  applyBrowsers();
  const saved = settings.get().sessionSaved;
  const mode = restoreMode();
  armed = true;
  windows.subscribe(schedule);
  if (mode === 'off') { if (saved) settings.set({ sessionSaved: null }); hold = false; return; }
  const switched = saved?.clean === 'theme';           // the same session in a new theme (themeReopen)
  const list = cleanList(saved?.windows, SNAP_ZONES);
  const now = switched ? 'none' : restoreDecision(mode, saved?.clean === true, list.length);
  // from here on this session is the one that must end well; written now, so that a crash during
  // the restore itself is an unclean end and asks next time
  write(now === 'none' && !switched ? [] : list, false);
  if (now === 'none') { hold = false; schedule(); return; }
  if (now === 'run') setTimeout(() => restore(list), START_MS);
  else setTimeout(() => askStore.set({ list, unclean: saved?.clean !== true }), START_MS);
}
