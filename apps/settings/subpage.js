// subpage.js — the place inside a Settings section that the mouse's back and forward buttons walk to (GitHub #234), as
// a browser does: Wallpaper's collection pages ("wallpaper/anime") and
// AI's capability pages and Sentinela ("ai/voice", "ai/sentinela"). index.jsx records `section/sub` in its visit
// history and sets this store when it goes back or forward; a page reads it with useStore and sets it when it opens a
// sub-page. Changing section from the sidebar clears it.
import { createStore } from '@lordos/sdk';

export const subPage = createStore(null);
export const placeOf = (section, sub) => (sub ? `${section}/${sub}` : section);
export const splitPlace = (place) => { const i = String(place).indexOf('/'); return i < 0 ? [String(place), null] : [place.slice(0, i), place.slice(i + 1) || null]; };
