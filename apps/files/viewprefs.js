// viewprefs.js — what each place in Files shows and how wide the sidebar is, no screen (docs/FILES.md, "Settings").
import { ZOOM_MIN, ZOOM_MAX, clampZoom } from '../../sdk/filepickermodel.js';
export const SIDE_MIN = 150, SIDE_MAX = 360, SIDE_DEFAULT = 200;
export const clampSide = (w) => Math.max(SIDE_MIN, Math.min(SIDE_MAX, Math.round(Number(w) || SIDE_DEFAULT)));
const MAX_FOLDER_VIEWS = 400;
const VIEW_NAMES = ['icons', 'list', 'columns', 'gallery'];
const SORT_NAMES = /^[a-z]{1,16}$/;
// a location's key: a folder's path, or the kind of place
export const viewKey = (loc) => (loc.type === 'dir' ? loc.path : loc.type === 'archive' || loc.type === 'search' ? null : `:${loc.type}`);

// Zoom (issue #33): a step from ZOOM_MIN to ZOOM_MAX, 0 the usual size, kept per place and per view (icons and list
// each have their own, as a picture folder may want big icons and a small list). Steps, not a free scale, so every
// size is one that was looked at and the keys and the slider land on the same ones.
// The steps are shared with the file dialog's thumbnails (sdk/filepickermodel.js, issue #58).
export { ZOOM_MIN, ZOOM_MAX, clampZoom };
export const ZOOM_VIEWS = ['icons', 'list'];
const ICONS = { icon: [32, 48, 64, 80, 96, 128, 160], font: [10, 11, 12, 13, 14, 15, 16] };
const LIST = { icon: [12, 14, 16, 20, 24, 32, 40], font: [11, 12, 13, 14, 15, 17, 19], row: [20, 22, 24, 28, 32, 40, 48] };
// sizes of the icon view at a step: the icon, the card's width, the name's font and line height
export function iconMetrics(z) {
  const i = clampZoom(z) - ZOOM_MIN;
  const icon = ICONS.icon[i], font = ICONS.font[i];
  return { icon, cell: Math.max(76, icon + 36), font, line: Math.round(font * 1.34) };
}
// sizes of the list view at a step: the icon, the name's font, the other columns' font and the row's height
export function listMetrics(z) {
  const i = clampZoom(z) - ZOOM_MIN;
  return { icon: LIST.icon[i], font: LIST.font[i], cell: LIST.font[i] - 1, row: LIST.row[i] };
}
// how many cards fit in a row of the icon view (8 px between them, 12 px padding on each side)
export const iconColumns = (width, z) => Math.max(1, Math.floor((width - 24 + 8) / (iconMetrics(z).cell + 8)));
const cleanZoom = (z) => {
  const out = {};
  for (const v of ZOOM_VIEWS) if (z && Number.isInteger(z[v]) && clampZoom(z[v]) === z[v] && z[v] !== 0) out[v] = z[v];
  return Object.keys(out).length ? out : null;
};
// the zoom map with one view's step changed (0 drops it)
export const zoomWith = (zoom, view, z) => cleanZoom({ ...(zoom || {}), [view]: clampZoom(z) }) || {};
export const zoomOf = (zoom, view) => clampZoom(zoom?.[view] ?? 0);

const cleanView = (v) => {
  if (!(v && VIEW_NAMES.includes(v.view) && v.sort && SORT_NAMES.test(v.sort.by))) return null;
  const out = { view: v.view, sort: { by: v.sort.by, desc: v.sort.desc === true } };
  const z = cleanZoom(v.zoom);
  if (z) out.zoom = z;
  return out;
};
// what a location opens with: its own, the default, or the built-in one (zoom: {} when none was chosen)
export function viewFor(p, loc) {
  const k = viewKey(loc);
  const v = cleanView(k && p.folderViews[k]) || cleanView(p.defaultView) || { view: 'icons', sort: { by: 'name', desc: false } };
  return { ...v, zoom: v.zoom || {} };
}
export function folderViewsWith(map, key, v) {
  const out = { ...map };
  delete out[key];
  out[key] = cleanView(v);
  const keys = Object.keys(out);
  for (let i = 0; i < keys.length - MAX_FOLDER_VIEWS; i++) delete out[keys[i]];
  return out;
}
