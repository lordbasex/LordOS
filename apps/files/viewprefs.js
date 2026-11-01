// viewprefs.js — what each place in Files shows and how wide the sidebar is, no screen (docs/FILES.md, "Settings").
export const SIDE_MIN = 150, SIDE_MAX = 360, SIDE_DEFAULT = 200;
export const clampSide = (w) => Math.max(SIDE_MIN, Math.min(SIDE_MAX, Math.round(Number(w) || SIDE_DEFAULT)));
const MAX_FOLDER_VIEWS = 400;
const VIEW_NAMES = ['icons', 'list', 'columns', 'gallery'];
const SORT_NAMES = /^[a-z]{1,16}$/;
// a location's key: a folder's path, or the kind of place
export const viewKey = (loc) => (loc.type === 'dir' ? loc.path : loc.type === 'archive' || loc.type === 'search' ? null : `:${loc.type}`);
const cleanView = (v) => (v && VIEW_NAMES.includes(v.view) && v.sort && SORT_NAMES.test(v.sort.by) ? { view: v.view, sort: { by: v.sort.by, desc: v.sort.desc === true } } : null);
// what a location opens with: its own, the default, or the built-in one
export function viewFor(p, loc) {
  const k = viewKey(loc);
  return cleanView(k && p.folderViews[k]) || cleanView(p.defaultView) || { view: 'icons', sort: { by: 'name', desc: false } };
}
export function folderViewsWith(map, key, v) {
  const out = { ...map };
  delete out[key];
  out[key] = cleanView(v);
  const keys = Object.keys(out);
  for (let i = 0; i < keys.length - MAX_FOLDER_VIEWS; i++) delete out[keys[i]];
  return out;
}
