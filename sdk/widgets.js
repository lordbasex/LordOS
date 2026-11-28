// widgets.js — the desktop widget layer (for example, the Notes app's notes).
//
// An app registers a type and publishes its widgets; the theme draws the layer with <DesktopWidgets />:
// once between the wallpaper and the windows (layer="below") and again above the windows for those
// pinned to the front (layer="above").
//
//   registerWidgetType('note', NoteCard)              // NoteCard({ widget }) draws one
//   setWidgets('note', [{ id, x, y, w, h, pinned }])  // replaces those of that type
import { createElement as h, memo } from 'react';
import { View } from '@lordos/runtime';
import { createStore, useStore } from './store.js';

const types = new Map();
export const desktopWidgets = createStore({});        // type → [widget]

export function registerWidgetType(type, Component) {
  types.set(type, Component);
  desktopWidgets.set((s) => ({ ...s }));
}

export function setWidgets(type, list) {
  desktopWidgets.set((s) => ({ ...s, [type]: list }));
}

// The theme's <DesktopWidgets /> draws again with the whole desktop (a window's title, a clock): the layer itself only
// for its own store, so the widgets' cards (a calendar, a graph, a clock face) don't draw again with it (BUG-9 of the
// 0.8.9.6.2 QA: ~4300 nodes a second). A function for the theme API (sdk/themeapi.js wraps components), memo inside.
export function DesktopWidgets({ layer = 'below' }) {
  return h(WidgetLayer, { layer: layer === 'above' ? 'above' : 'below' });
}
const WidgetLayer = memo(function WidgetLayer({ layer }) {
  const all = useStore(desktopWidgets);
  const items = [];
  for (const [type, list] of Object.entries(all)) {
    const C = types.get(type);
    if (!C || !list) continue;
    for (const w of list) if (!!w.pinned === (layer === 'above')) items.push(h(C, { key: `${type}:${w.id}`, widget: w }));
  }
  if (!items.length) return null;
  return h(View, { style: { position: 'absolute', left: 0, top: 0, width: 0, height: 0 } }, ...items);
});
