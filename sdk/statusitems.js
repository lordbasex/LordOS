// statusitems.js — status icons in the theme's bar (menu bar extras).
//
//   const off = registerStatusItem('clock', ClockStatus, 10)   // ClockStatus({ color }) draws its own
//   <StatusItems color={t.menuText} />                          // the theme shows them wherever it wants
//
// It is optional: a theme that does not draw <StatusItems /> simply doesn't show them. The default
// and classic themes put them next to the privacy indicators. An item with nothing to show returns
// null.
//
// An item with a panel opens it with statusPanelAt(button): below the button when the bar is in the top half of its
// display (Flow's menu bar, a Classic taskbar at the top) and above it when the bar is at the bottom (the Classic
// taskbar's default), aligned to the button's edge on the side away from the display's nearer edge.
import { createElement as h } from 'react';
import { View, displayAt } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { workAreaOf } from './windows.js';
import { panelPlacement, panelRoom } from './statusplace.js';

export { panelPlacement, panelRoom };

export const statusItems = createStore([]);          // [{ id, Component, order }]

export function registerStatusItem(id, Component, order = 0) {
  statusItems.set((l) => [...l.filter((x) => x.id !== id), { id, Component, order }].sort((a, b) => a.order - b.order));
  return () => statusItems.set((l) => l.filter((x) => x.id !== id));
}

// Is the bar's `color` (what <StatusItems color> passes to each item) a light one? An item drawn as a picture picks its
// light or dark variant by it; null when there is none or it can't be read (the item follows the theme's mode then).
// '#rgb', '#rrggbb', '#rrggbbaa', 'rgb(…)' and 'rgba(…)'.
export function lightColor(color) {
  const c = String(color || '').trim();
  let m = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(c), r, g, b;
  if (m) {
    const x = m[1].length === 3 ? m[1].split('').map((d) => d + d).join('') : m[1].slice(0, 6);
    [r, g, b] = [0, 2, 4].map((i) => parseInt(x.slice(i, i + 2), 16));
  } else if ((m = /^rgba?\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})/i.exec(c))) [r, g, b] = [m[1], m[2], m[3]].map(Number);
  else return null;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b >= 140;
}

export function StatusItems({ color, height = 22 }) {
  const list = useStore(statusItems);
  if (!list.length) return null;
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', height } },
    ...list.map(({ id, Component }) => h(Component, { key: id, color, height })));
}

// A theme can put the items' panels where its own open (Workspaces: where its notifications do, under its band):
// fn(box) → the style inside <Overlay anchor={button}> (box: the button's [x, y, w, h]), or null for the default.
let placer = null;
export function setStatusPanelPlacer(fn) { placer = typeof fn === 'function' ? fn : null; return () => { if (placer === fn) placer = null; }; }

// The style of an item's panel inside <Overlay anchor={button}>: toward the middle of the button's display.
export function statusPanelAt(button, gap = 4) {
  const n = button && typeof button === 'object' && 'current' in button ? button.current : button;
  let box = null;
  try { box = n ? lord.getLayout(n.id) : null; } catch (e) { box = null; }
  if (!box) return { position: 'absolute', top: 26, right: 0 };
  const own = placer ? placer(box) : null;
  if (own) return own;
  return panelPlacement(box, displayAt(box[0] + box[2] / 2, box[1] + box[3] / 2), gap);
}

// The most height that panel may take on its display, the Dock or taskbar left out (statusplace.js, panelRoom; #145).
// A panel whose rows can outgrow the screen sets it as its maxHeight and scrolls the rows.
export function statusPanelRoom(button, gap = 4) {
  const n = button && typeof button === 'object' && 'current' in button ? button.current : button;
  let box = null;
  try { box = n ? lord.getLayout(n.id) : null; } catch (e) { box = null; }
  if (!box) return undefined;
  const d = displayAt(box[0] + box[2] / 2, box[1] + box[3] / 2);
  let area = null;
  try { area = workAreaOf(d); } catch (e) { area = null; }
  return panelRoom(box, d, area, gap);
}
