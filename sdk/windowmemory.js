// windowmemory.js — each app reopens where it was (docs/WINDOWS.md, "Remembered places"): size, place, maximized and which
// display, per app, in settings.windowPlaces. Session restore's frames come first; an assignment in the Dock chooses the display.
// A Linux app's own request doesn't override this, except a dialog or a window of fixed size (sdk/wayland.js).
import { screen, displayFor, mainDisplay } from '@lordos/runtime';
import { settings } from './settings.js';
import { windows, workAreaOf, getApp, setWindowMemory, assignedDisplayOf, isLiveSystem } from './windows.js';
import { dockIdOfWindow } from './dock.js';
import { placeOf, putPlace, restoreFrame } from './placemodel.js';

const SAVE_MS = 800;
let timer = 0;
const last = new Map();                 // window id → the JSON of the place saved for it

const keyOf = (w) => dockIdOfWindow(w) || w.app;

function save() {
  timer = 0;
  let all = settings.get().windowPlaces || {};
  let changed = false;
  for (const w of windows.get()) {
    if (w.args?.dialog || (getApp(w.app)?.builtin && !String(w.id).startsWith('linux#'))) continue;
    if (keyOf(w).startsWith('linux:linux#') || keyOf(w) === 'linux:') continue;   // nothing names this app
    const p = placeOf(w, displayFor(w.snapped && w.restore ? w.restore : w));
    if (!p) continue;
    const json = JSON.stringify(p);
    if (last.get(w.id) === json) continue;
    last.set(w.id, json);
    all = putPlace(all, keyOf(w), p);
    changed = true;
  }
  const open = new Set(windows.get().map((w) => w.id));
  for (const id of last.keys()) if (!open.has(id)) last.delete(id);
  if (changed) settings.set({ windowPlaces: all });
}

export function rememberedFrame(key, opts = {}) {
  const p = (settings.get().windowPlaces || {})[key];
  if (!p) return null;
  const twins = windows.get().filter((w) => keyOf(w) === key).length;
  return restoreFrame(p, screen.displays, workAreaOf, { main: mainDisplay(), assigned: assignedDisplayOf(key), shift: twins * 30, ...opts });
}

export function startWindowMemory() {
  if (typeof lord === 'undefined' || typeof lord.appId === 'string' || typeof lord.greeter !== 'undefined' || isLiveSystem()) return;
  setWindowMemory((appId) => {
    const a = getApp(appId);
    return a && !a.builtin ? rememberedFrame(keyOf({ id: appId, app: appId }), { fixed: a.fixed === true, size: a.size }) : null;
  });
  windows.subscribe(() => { if (!timer) timer = setTimeout(save, SAVE_MS); });
}
