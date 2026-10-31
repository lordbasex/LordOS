// appswitcher.jsx — the app switcher, the same in every theme (docs/SHORTCUTS.md).
//
// A centered strip of large app icons with the selected app's name. Hold the modifier: Tab moves,
// Shift+Tab goes back, releasing it switches, Esc cancels. Most recently used app first, so a quick
// press goes back to the previous app. sdk/shortcuts.js opens it and hands it every key while it is
// open, and the theme mounts <AppSwitcher /> in its overlay layer inside its ThemeProvider.
//
//   openSwitcher(hold, back)  hold: the modifier's bit in e.mods (8 ❖, 2 ⌥); back: start backwards
//   cycleAppWindows(back)     the next window of the app in front (❖`)
import { createElement as h } from 'react';
import { View, Text, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { windows, wm, getApp, activeDesktop, onActiveDesktop } from './windows.js';
import { pickWindow } from './desktopmodel.js';
import { desktopSuffix } from './desktops.js';
import { dockIdOfWindow } from './dock.js';
import { runningApps } from './appcontrol.js';
import { windowIcon } from './apps.js';

// { items: [{ key, name, Icon, win }], index, hold } while it is open
export const switcherState = createStore(null);
export const switcherOpen = () => !!switcherState.get();

// Most recently used apps, newest first. A window that comes to the front moves its app up; a
// theme switch starts a new list, ordered by the windows' stacking.
let recent = [];
const keyOf = (w) => dockIdOfWindow(w);
function trackRecent() {
  const top = wm.focused();
  const w = top && wm.get(top);
  if (!w) return;
  const k = keyOf(w);
  if (recent[0] !== k) recent = [k, ...recent.filter((x) => x !== k)];
}
windows.subscribe(trackRecent);

// the window an app comes back with: on this desktop if it has one, a visible one before a minimized
// one. The switcher lists the apps of every desktop; choosing one elsewhere jumps there (wm.focus).
function windowOf(group) {
  return pickWindow(windows.get().filter((w) => group.windows.some((x) => x.id === w.id)), activeDesktop.get());
}

function listApps() {
  const stacked = windows.get().map((w) => keyOf(w)).reverse();
  const rank = (k) => { const i = recent.indexOf(k); return i >= 0 ? i : recent.length + Math.max(0, stacked.indexOf(k)); };
  return runningApps().map((g) => {
    const win = windowOf(g);
    return { key: keyOf(win), name: `${g.name}${desktopSuffix(win)}`, win: win.id, Icon: windowIcon(win, getApp) };
  }).sort((a, b) => rank(a.key) - rank(b.key));
}

export function openSwitcher(hold, back = false) {
  if (typeof lord !== 'undefined' && lord.wayland?.fullscreen?.()) return;    // a fullscreen app is presented above every layer
  const items = listApps();
  if (!items.length) return;
  const top = wm.get(wm.focused());
  const first = top && items[0].key === keyOf(top) ? 1 : 0;      // the app in front is not where a switch goes
  const index = (back ? items.length - 1 : first) % items.length;
  switcherState.set({ items, index, hold });
}

const close = () => switcherState.set(null);
function commit() {
  const s = switcherState.get();
  close();
  const it = s?.items[s.index];
  if (it && wm.get(it.win)) wm.focus(it.win);
}
function move(step) {
  switcherState.set((s) => (s ? { ...s, index: (s.index + step + s.items.length) % s.items.length } : s));
}

// Every key goes here while the switcher is open. True: taken. A key that arrives with the modifier
// already up (it was released while another session had the keyboard) also ends it.
export function switcherKey(e) {
  const s = switcherState.get();
  if (!(e.mods & s.hold)) { commit(); return true; }
  if (!e.down) return true;
  if (e.key === 'Tab' || e.key === 'ArrowRight') move(e.shift && e.key === 'Tab' ? -1 : 1);
  else if (e.key === 'ArrowLeft') move(-1);
  else if (e.key === 'Enter') commit();
  else if (e.key === 'Escape') close();
  return true;
}

export function cycleAppWindows(back = false) {
  const top = wm.get(wm.focused());
  if (!top) return;
  const key = keyOf(top);
  const mine = windows.get().filter((w) => keyOf(w) === key && !w.minimized && onActiveDesktop(w));
  if (mine.length < 2) return;
  if (!back) { wm.focus(mine[0].id); return; }           // the app's rearmost window comes up
  windows.set((ws) => [top, ...ws.filter((w) => w !== top)]);   // the front one goes under everything
  wm.focus(mine[mine.length - 2].id);
}

const ICON = 72, CELL = 104;

export function AppSwitcher() {
  const s = useStore(switcherState);
  const th = useTheme();
  if (!s) return null;
  const a = mainDisplay();
  const perRow = Math.max(1, Math.floor((a.w - 96) / CELL));
  const shown = s.items.length > perRow
    ? s.items.slice(Math.max(0, Math.min(s.index - Math.floor(perRow / 2), s.items.length - perRow))).slice(0, perRow)
    : s.items;
  return h(Layer, { name: 'overlay' },
    h(View, { style: { position: 'absolute', left: a.x, top: a.y, width: a.w, height: a.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'box-none' } },
      h(View, { style: { alignItems: 'center', padding: 16, borderRadius: th.groupRadius ? th.groupRadius + 10 : 20, maxWidth: a.w - 48,
        backgroundColor: th.mode === 'dark' ? 'rgba(30,30,34,0.92)' : 'rgba(245,245,247,0.94)', border: `1px solid ${th.separator}`,
        boxShadow: '0 18px 48px rgba(0,0,0,0.45)' } },
      h(View, { style: { flexDirection: 'row', direction: 'ltr' } }, shown.map((it) => {
        const on = it === s.items[s.index];
        return h(View, { key: it.key, onClick: () => { switcherState.set((x) => (x ? { ...x, index: x.items.indexOf(it) } : x)); commit(); },
          style: { width: CELL, height: CELL, borderRadius: 16, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? `${th.accent}55` : 'transparent' } },
        h(it.Icon, { size: ICON }));
      })),
      h(Text, { style: { marginTop: 10, fontSize: 15, fontWeight: 600, color: th.text, textAlign: 'center' } }, s.items[s.index]?.name || ''))));
}
