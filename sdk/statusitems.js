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
// display (Lord Flow's menu bar, a Classic taskbar at the top) and above it when the bar is at the bottom (the Classic
// taskbar's default), aligned to the button's edge on the side away from the display's nearer edge.
import { createElement as h } from 'react';
import { View, displayAt } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { panelPlacement } from './statusplace.js';

export { panelPlacement };

export const statusItems = createStore([]);          // [{ id, Component, order }]

export function registerStatusItem(id, Component, order = 0) {
  statusItems.set((l) => [...l.filter((x) => x.id !== id), { id, Component, order }].sort((a, b) => a.order - b.order));
  return () => statusItems.set((l) => l.filter((x) => x.id !== id));
}

export function StatusItems({ color, height = 22 }) {
  const list = useStore(statusItems);
  if (!list.length) return null;
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', height } },
    ...list.map(({ id, Component }) => h(Component, { key: id, color, height })));
}

// The style of an item's panel inside <Overlay anchor={button}>: toward the middle of the button's display.
export function statusPanelAt(button, gap = 4) {
  const n = button && typeof button === 'object' && 'current' in button ? button.current : button;
  let box = null;
  try { box = n ? lord.getLayout(n.id) : null; } catch (e) { box = null; }
  if (!box) return { position: 'absolute', top: 26, right: 0 };
  return panelPlacement(box, displayAt(box[0] + box[2] / 2, box[1] + box[3] / 2), gap);
}
