// state.js — what Screenshots' windows share (index.jsx, toolbar.jsx): the recording in progress, the recent captures,
// the toolbar's keyboard and menu state, and the options (settings.screenshots, toolbarmodel.js normalizeOptions).
import { createStore, settings } from '@lordos/sdk';
import { normalizeOptions } from './toolbarmodel.js';

export const APP = 'screenshots';
export const recording = createStore(null);   // { id, path, start }
export const recent = createStore([]);        // [{ path, time, video }]
// ring: the toolbar item the keyboard is on (toolbarmodel.js barStops), null until an arrow key is pressed;
// menu: Options is open, on page 'main' or 'recent', with the keyboard on row lit (-1: none)
export const bar = createStore({ ring: null, menu: false, page: 'main', lit: -1 });

export const options = () => normalizeOptions(settings.get().screenshots);
export function setOptions(patch) {
  const cur = settings.get().screenshots || {};
  // "clipboard" was the panel's own key before Save to: it follows the choice, so an older LordOS reads it right
  const legacy = 'saveTo' in patch ? { clipboard: patch.saveTo === 'clipboard' } : null;
  settings.set({ screenshots: { ...cur, ...patch, ...legacy } });
}
export const addRecent = (item) => recent.set((l) => [item, ...l.filter((x) => x.path !== item.path)].slice(0, 20));
