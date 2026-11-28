// displayapps.js — the apps used on each display, at runtime (the rules: sdk/displayappsmodel.js). A theme starts it
// once (startDisplayApps) and then shows, on an empty display, its quick launch (useQuickApps) and the usual apps as a
// suggestion (useDisplaySuggestion): it never opens anything itself. Workspaces does both in each display's
// empty-activity card; Settings shows the switch and "Clear" for a theme whose theme.json says "displayApps": true.
//
// What it records, in settings.displayApps (this computer only; nothing leaves it):
//   - an app opened on a display: its launcher id there, once per CREDIT_MS for the same app and display (the
//     window's display read SETTLE_MS after it shows, once the theme put it in its place);
//   - the apps open together on a display (on the desktop in view) once they stayed so for STABLE_MS, with the
//     theme's layout for them (describe), each set once per session.
// No polling and nothing per frame: one timer after a change of the windows, and only when the apps on a display
// changed (a title or a focus change doesn't restart it).
import { useStore } from './store.js';
import { settings } from './settings.js';
import { windows, getApp, activeDesktop, wm } from './windows.js';
import { displays as runtimeDisplays, displayFor } from '@lordos/runtime';
import { launcherIdOf } from './dock.js';
import { useLauncherApps, launchApp } from './apps.js';
import { displayWallpaperKey } from './wallpapermodel.js';
import {
  cleanDisplayApps, noteApp, noteSet, quickApps, suggestionFor, quietDisplay, clearDisplayApps, setDisplayAppsOff, displayAppsEmpty, recordable, QUICK_APPS,
} from './displayappsmodel.js';

export const SETTLE_MS = 2500;
export const STABLE_MS = 90000;
export const CREDIT_MS = 30 * 60000;

const rec = () => settings.get().displayApps;
const write = (next) => { if (JSON.stringify(next) !== JSON.stringify(cleanDisplayApps(rec()))) settings.set({ displayApps: next }); };

// The theme's own windows (its panels, the SDK's built-in windows) and dialogs are no app the person opened.
const own = (w) => !!w && !w.pip && !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'))
  && !(w.args && (w.args.dialog || w.args.parent || w.args.notice));
const frameOf = (w) => ({ x: w.x, y: w.y, w: w.w, h: w.h });

let opts = { keyOf: displayWallpaperKey, describe: null };
let started = false;
const seen = new Set();          // window ids already looked at
const credited = new Map();      // `${display key}\n${app}` → when it was last counted
const counted = new Set();       // `${display key}\n${set}`: the sets counted this session
let lastSig = '';
let timer = 0;

// keyOf(display): the display's key (the theme's, so its saved layouts and this record agree; the connector by
// default). describe(display, key, ids): { apps (launcher ids in the theme's order), layout, ratio } for the windows
// `ids` open there, or null to take them left to right.
export function startDisplayApps({ keyOf, describe } = {}) {
  if (keyOf) opts.keyOf = keyOf;
  if (describe) opts.describe = describe;
  if (started) return;
  started = true;
  for (const w of windows.get()) seen.add(w.id);           // what was open before the theme started counts once, below
  setTimeout(() => { for (const w of windows.get()) creditLater(w.id, 0); }, 0);
  windows.subscribe(onWindows);
  activeDesktop.subscribe(onWindows);
  onWindows();
}

const off = () => cleanDisplayApps(rec()).off === true;
const keyAt = (w) => { const d = displayFor(frameOf(w)); return d ? opts.keyOf(d) : null; };

function creditLater(id, ms = SETTLE_MS) {
  setTimeout(() => {
    if (off()) return;
    const w = wm.get(id);
    if (!own(w) || w.minimized) return;
    const app = launcherIdOf(w), key = keyAt(w);
    if (!recordable(app) || !key) return;
    const now = Date.now(), k = `${key}\n${app}`;
    if (now - (credited.get(k) || 0) < CREDIT_MS) return;
    credited.set(k, now);
    write(noteApp(rec(), key, app, now));
  }, ms);
}

// The apps open on each display of the desktop in view: [{ d, key, ids }]
function perDisplay() {
  const desk = activeDesktop.get();
  const out = new Map();
  for (const w of windows.get()) {
    if (w.desktop !== desk || w.minimized || !own(w)) continue;
    const d = displayFor(frameOf(w));
    if (!d) continue;
    const key = opts.keyOf(d);
    if (!out.has(key)) out.set(key, { d, key, ids: [] });
    out.get(key).ids.push(w.id);
  }
  return [...out.values()];
}
const appsOf = (ids) => [...new Set(ids.map((id) => wm.get(id)).filter(Boolean).map(launcherIdOf).filter(recordable))];
const signature = (list) => list.map((x) => `${x.key}:${appsOf(x.ids).sort().join(',')}`).sort().join('|');

function onWindows() {
  for (const w of windows.get()) if (!seen.has(w.id)) { seen.add(w.id); if (own(w)) creditLater(w.id); }
  if (seen.size > 4096) { const live = new Set(windows.get().map((w) => w.id)); for (const id of seen) if (!live.has(id)) seen.delete(id); }
  if (off()) return;
  const sig = signature(perDisplay());
  if (sig === lastSig) return;
  lastSig = sig;
  clearTimeout(timer);
  timer = setTimeout(recordSets, STABLE_MS);
}

function recordSets() {
  timer = 0;
  if (off()) return;
  let next = rec();
  const now = Date.now();
  for (const { d, key, ids } of perDisplay()) {
    let set = null;
    try { set = opts.describe ? opts.describe(d, key, ids) : null; } catch (e) { console.warn('displayapps: describe:', e); }
    if (!set) {
      const byPlace = [...ids].map((id) => wm.get(id)).filter(Boolean).sort((a, b) => a.x - b.x || a.y - b.y).map((w) => w.id);
      set = { apps: appsOf(byPlace) };
    }
    const apps = (set.apps || []).filter(recordable);
    if (!apps.length) continue;
    const k = `${key}\n${[...apps].sort().join(',')}`;
    if (counted.has(k)) continue;
    counted.add(k);
    next = noteSet(next, key, { ...set, apps }, now);
  }
  write(cleanDisplayApps(next));
}

// ---- what themes and Settings use ----

const installedIn = (list) => { const ids = new Set(list.map((a) => a.id)); return (id) => ids.has(id); };
// The apps to offer first on the display with key `key`: [{ id, name, Icon }] (installed ones only).
export function useQuickApps(key, max = QUICK_APPS) {
  const raw = useStore(settings, (s) => s.displayApps);
  const list = useLauncherApps();
  if (!key) return [];
  const ids = quickApps(raw, key, Date.now(), { have: installedIn(list), max });
  return ids.map((id) => list.find((a) => a.id === id)).filter(Boolean);
}
// The usual apps of that display, to suggest: { apps: [{ id, name, Icon }], layout, ratio } or null.
export function useDisplaySuggestion(key) {
  const raw = useStore(settings, (s) => s.displayApps);
  const list = useLauncherApps();
  if (!key) return null;
  const s = suggestionFor(raw, key, Date.now(), { have: installedIn(list) });
  return s ? { ...s, apps: s.apps.map((id) => list.find((a) => a.id === id)).filter(Boolean) } : null;
}
// "Don't suggest on this display" (Settings → Clear brings it back).
export const quietDisplayApps = (key) => write(quietDisplay(rec(), key));
// Settings: forget every display; turn the record off (which forgets) or on.
export function forgetDisplayApps() { counted.clear(); credited.clear(); write(clearDisplayApps(rec())); }
export function setDisplayAppsOn(on) { counted.clear(); credited.clear(); write(setDisplayAppsOff(rec(), !on)); }
export const displayAppsOn = (s) => cleanDisplayApps(s?.displayApps).off !== true;
export const displayAppsStored = (s) => !displayAppsEmpty(s?.displayApps);
// Open apps (launcher ids) one after the other, as a click on the suggestion does (the theme aims them first).
export function openDisplayApps(ids) { for (const id of ids) launchApp(id); }
// the displays here, keyed as the record keys them (for a theme without keys of its own)
export const displayAppsKeys = () => runtimeDisplays().map((d) => ({ d, key: opts.keyOf(d) }));
