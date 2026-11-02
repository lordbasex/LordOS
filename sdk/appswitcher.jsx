// appswitcher.jsx — the app switcher, the same in every theme (docs/SHORTCUTS.md).
//
// A centered strip of large app icons with the selected app's name. Hold the modifier: Tab moves,
// Shift+Tab goes back, releasing it switches, Esc cancels. Most recently used app first, so a quick
// press goes back to the previous app. sdk/shortcuts.js opens it and hands it every key while it is
// open, and the theme mounts <AppSwitcher /> in its overlay layer inside its ThemeProvider.
// 1 to 9 go straight to an app. Z, the key under Esc, or ↓ move through the selected app's windows, shown in a row below
// (↑ goes back). Settings → Keyboard chooses the keys, which apps show, the size and the look (switchermodel.js).
//
//   openSwitcher(hold, back)  hold: the modifier's bit in e.mods (8 ❖, 2 ⌥); back: start backwards
//   cycleAppWindows(back)     the next window of the app in front (❖`)
import { createElement as h } from 'react';
import { View, Text, Surface, mainDisplay, displayFor } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { windows, wm, getApp, activeDesktop, onActiveDesktop, windowTitle } from './windows.js';
import { settings } from './settings.js';
import { switcherPrefs, groupWindows, SIZES, skinColors, digitOf, WINDOW_NEXT, WINDOW_BACK, stepWindow, fitScale, switcherGrabName, switcherMods, switcherBits } from './switchermodel.js';
import { pickWindow } from './desktopmodel.js';
import { desktopSuffix } from './desktops.js';
import { dockIdOfWindow } from './dock.js';
import { runningApps } from './appcontrol.js';
import { windowIcon } from './apps.js';

// { items: [{ key, name, Icon, win, windows }], index, pick, hold } while it is open; pick: the selected app's window
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
  const prefs = switcherPrefs(settings.get().switcher);
  const main = mainDisplay();
  const front = wm.get(wm.focused());
  const place = front ? displayFor(front) : main;
  const here = { inDesktop: onActiveDesktop, inDisplay: (w) => { const d = displayFor(w); return !!d && d.x === place.x && d.y === place.y; } };
  return runningApps().flatMap((g) => {
    const list = groupWindows(g, prefs, here);
    if (!list.length) return [];
    const win = windowOf({ ...g, windows: list });
    return [{ key: keyOf(win), name: `${g.name}${desktopSuffix(win)}`, win: win.id, Icon: windowIcon(win, getApp), windows: [...list].reverse() }];
  }).sort((a, b) => rank(a.key) - rank(b.key));
}

export function openSwitcher(hold, back = false) {
  if (typeof lord !== 'undefined' && lord.wayland?.fullscreen?.()) return;    // a fullscreen app is presented above every layer
  const items = listApps();
  if (!items.length) return;
  const top = wm.get(wm.focused());
  const first = top && items[0].key === keyOf(top) ? 1 : 0;      // the app in front is not where a switch goes
  const index = (back ? items.length - 1 : first) % items.length;
  switcherState.set({ items, index, pick: 0, hold });
}

const close = () => switcherState.set(null);
function commit() {
  const s = switcherState.get();
  close();
  const it = s?.items[s.index];
  const win = s?.pick ? it?.windows[s.pick]?.id : it?.win;
  if (win && wm.get(win)) wm.focus(win);
}
function move(step) {
  switcherState.set((s) => (s ? { ...s, index: (s.index + step + s.items.length) % s.items.length, pick: 0 } : s));
}
function moveWindow(step) {
  switcherState.set((s) => (s ? { ...s, pick: stepWindow(s.pick, s.items[s.index]?.windows.length || 0, step) } : s));
}
const prefsNow = () => switcherPrefs(settings.get().switcher);

// Every key goes here while the switcher is open. True: taken. A key that arrives with the modifier
// already up (it was released while another session had the keyboard) also ends it.
export function switcherKey(e) {
  const s = switcherState.get();
  if (!(e.mods & s.hold)) { commit(); return true; }
  if (!e.down) return true;
  const n = digitOf(e.code);
  if (n >= 0) {
    if (prefsNow().numbers && n < s.items.length) { switcherState.set((x) => ({ ...x, index: n, pick: 0 })); commit(); }
    return true;
  }
  if (WINDOW_NEXT.has(e.code)) { moveWindow(e.shift ? -1 : 1); return true; }
  if (e.code === WINDOW_BACK) { moveWindow(-1); return true; }
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

// A window of the selected app: a linux# or an isolated app's window is its real surface, smaller and live; a window drawn
// by the shell itself shows its app's icon.
const CARD_W = 180, CARD_H = 112;
function WindowCard({ w, on, Icon, colors, pick }) {
  const k = fitScale(w.w, w.h, CARD_W - 12, CARD_H - 34);
  const live = w.args?.surface && !w.minimized && onActiveDesktop(w);
  return h(View, { onClick: pick, style: { width: CARD_W, height: CARD_H, padding: 6, borderRadius: 12, alignItems: 'center', backgroundColor: on ? `${colors.accent}55` : 'transparent' } },
    h(View, { style: { width: CARD_W - 12, height: CARD_H - 34, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' } },
      live ? h(View, { style: { width: Math.round(w.w * k), height: Math.round(w.h * k), overflow: 'hidden', borderRadius: 4 } },
        h(View, { style: { width: w.w, height: w.h, transform: `scale(${k})`, transformOrigin: 'left top' } }, h(Surface, { surface: w.args.surface, style: { flex: 1 } })))
        : h(Icon, { size: 48 })),
    h(Text, { style: { marginTop: 6, maxWidth: CARD_W - 16, fontSize: 12, color: colors.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden' } }, windowTitle(w)));
}

export function AppSwitcher() {
  const s = useStore(switcherState);
  const th = useTheme();
  if (!s) return null;
  const prefs = prefsNow();
  const { icon: ICON, cell: CELL } = SIZES[prefs.size];
  const colors = skinColors(prefs, th);
  const a = mainDisplay();
  const perRow = Math.max(1, Math.floor((a.w - 96) / CELL));
  const start = s.items.length > perRow ? Math.max(0, Math.min(s.index - Math.floor(perRow / 2), s.items.length - perRow)) : 0;
  const shown = s.items.slice(start, start + perRow);
  const current = s.items[s.index];
  const many = prefs.windows && current?.windows.length > 1;
  const choose = (index, pick) => { switcherState.set((x) => (x ? { ...x, index, pick } : x)); commit(); };
  return h(Layer, { name: 'overlay' },
    h(View, { style: { position: 'absolute', left: a.x, top: a.y, width: a.w, height: a.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'box-none' } },
      h(View, { style: { alignItems: 'center', padding: 16, borderRadius: th.groupRadius ? th.groupRadius + 10 : 20, maxWidth: a.w - 48,
        backgroundColor: colors.bg, backdropFilter: colors.blur ? `blur(${colors.blur}px)` : undefined, border: `1px solid ${colors.border}`,
        boxShadow: '0 18px 48px rgba(0,0,0,0.45)' } },
      h(View, { style: { flexDirection: 'row', direction: 'ltr' } }, shown.map((it, i) => {
        const on = it === current;
        const number = start + i + 1;
        return h(View, { key: it.key, onClick: () => choose(start + i, 0),
          style: { width: CELL, height: CELL, borderRadius: 16, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? `${colors.accent}55` : 'transparent' } },
        h(it.Icon, { size: ICON }),
        prefs.numbers && number <= 9 ? h(View, { style: { position: 'absolute', right: 8, bottom: 8, width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.78)' } },
          h(Text, { style: { fontSize: 12, fontWeight: 600, color: '#ffffff' } }, String(number))) : null);
      })),
      h(Text, { style: { marginTop: 10, fontSize: 15, fontWeight: 600, color: colors.text, textAlign: 'center' } }, current?.name || ''),
      many ? h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', maxWidth: a.w - 96, marginTop: 12, gap: 4, direction: 'ltr' } },
        current.windows.slice(0, 8).map((w, i) => h(WindowCard, { key: w.id, w, on: i === s.pick, Icon: current.Icon, colors, pick: () => choose(s.index, i) }))) : null)));
}

// The keys it opens with, for a theme whose own modifier is `own`.
export function switcherKeys(own) {
  const keys = switcherPrefs(settings.get().switcher).keys;
  return switcherMods(keys, own).flatMap((m) => (m === 'meta' ? ['Meta+Tab', 'Meta+Shift+Tab'] : ['Alt+Tab', 'Alt+Shift+Tab']));
}
export const switcherGrabs = (base) => switcherGrabName(base, switcherPrefs(settings.get().switcher).keys);
export const switcherHold = switcherBits;
