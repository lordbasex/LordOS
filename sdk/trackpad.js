// trackpad.js — touchpad and touchscreens: settings (settings.trackpad → lord.input.configure,
// live) and desktop actions when swiping with 3 fingers (or 4). See docs/TRACKPAD.md.
//
// The theme chooses what each direction does with registerGestureActions({ up, down, left, right }):
// the default theme opens the app launcher upwards. Without a theme action, left/right
// switch windows (the one behind comes to the front) and up/down do nothing. The themes give left and
// right to sdk/desktops.js desktopSwipe, which switches desktops and with only one falls back to that.
import { useEffect } from 'react';
import { addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { windows, activeDesktop } from './windows.js';
import { visibleOn } from './desktopmodel.js';

export const TRACKPAD_DEFAULTS = {
  speed: 0.5,               // pointer speed (0..1)
  tap: true,                // tap to click
  tapDrag: true,            // tap and drag
  natural: true,            // natural scrolling
  secondary: 'two-fingers', // secondary click: 'two-fingers' | 'bottom-right' | 'bottom-left'
  threeDrag: false,         // three-finger drag (swiping moves to 4 fingers)
  dwt: true,                // ignore while typing
  scroll: true,             // two-finger scrolling
  kinetic: true,            // inertia
  pinch: true,              // pinch to zoom
  swipe3: true,             // 3-finger swipe: desktop actions
  swipe4: false,            // 4-finger swipe: desktop actions (otherwise they go to the apps)
};

const native = () => typeof lord !== 'undefined' && lord.input;
export const trackpadConfig = () => ({ ...TRACKPAD_DEFAULTS, ...(settings.get().trackpad || {}) });

function apply() { if (native()) lord.input.configure(trackpadConfig()); }
apply();
let applied = settings.get().trackpad;
settings.subscribe(() => {
  const now = settings.get().trackpad;
  if (now !== applied) { applied = now; apply(); }
});

// Devices (lord-sessiond lends them when showing the session on the monitor: they are re-scanned).
export const trackpadDevices = createStore(native() ? lord.input.devices() : []);
export function refreshTrackpads() {
  if (!native()) return;
  const list = lord.input.devices();
  if (JSON.stringify(list) !== JSON.stringify(trackpadDevices.get())) trackpadDevices.set(list);
}
export function useTrackpads() {
  useEffect(() => { refreshTrackpads(); const id = setInterval(refreshTrackpads, 3000); return () => clearInterval(id); }, []);
  return useStore(trackpadDevices);
}
export const hasTouchpad = () => trackpadDevices.get().some((d) => d.kind === 'touchpad');

export const setTrackpad = (patch) => settings.set((s) => ({ ...s, trackpad: { ...(s.trackpad || {}), ...patch } }));

// ---- desktop swipe ----

const actions = { up: null, down: null, left: null, right: null };
export function registerGestureActions(map) {
  Object.assign(actions, map);
  return () => { for (const k of Object.keys(map)) if (actions[k] === map[k]) actions[k] = null; };
}

// Switch windows: left brings the rearmost one; right sends the front one behind all the others.
export function cycleWindows(dir) {
  windows.set((ws) => {
    const vis = visibleOn(ws, activeDesktop.get());
    if (vis.length < 2) return ws;
    if (dir === 'left') { const back = vis[0]; return [...ws.filter((w) => w !== back), back]; }
    const front = vis[vis.length - 1];
    return [front, ...ws.filter((w) => w !== front)];
  });
}
const DEFAULTS = { left: () => cycleWindows('left'), right: () => cycleWindows('right') };

// Last recognized swipe (for the interface or tests).
export const lastSwipe = createStore(null);

let acc = null;
addListener('swipe', (e) => {
  if (!e.system) return;
  if (e.phase === 'begin') acc = { x: 0, y: 0, fingers: e.fingers };
  else if (e.phase === 'update' && acc) { acc.x += e.dx; acc.y += e.dy; }
  else if ((e.phase === 'end' || e.phase === 'cancel') && acc) {
    const { x, y, fingers } = acc;
    acc = null;
    if (e.phase === 'cancel' || Math.max(Math.abs(x), Math.abs(y)) < 60) return;
    const dir = Math.abs(x) > Math.abs(y) ? (x < 0 ? 'left' : 'right') : (y < 0 ? 'up' : 'down');
    lastSwipe.set({ dir, fingers, at: lord.now() });
    const fn = actions[dir] || DEFAULTS[dir];
    if (fn) fn({ dir, fingers });
  }
});

export const trackpad = {
  devices: () => trackpadDevices.get(),
  config: trackpadConfig,
  set: setTrackpad,
  refresh: refreshTrackpads,
};
