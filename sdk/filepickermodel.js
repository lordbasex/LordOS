// filepickermodel.js — the system file dialog without a screen: where it may start, which disks it
// lists, what a path's crumbs are, and what Enter and the primary button do.
// node server/tests/filepicker_model_test.mjs
import { sortAccounts } from './cloudmodel.js';
import { compareNames } from './collate.js';

// a disk that goes away over the network would hold the interface: the dialog lists local disks only
const NETWORK_FS = ['cifs', 'smb3', 'nfs', 'nfs4', 'fuse.rclone', 'fuse.sshfs'];
export const localDisks = (volumes, home) => (volumes || []).filter((v) => !NETWORK_FS.includes(v.fs) && !v.path.startsWith(`${home}/.local/share/lordos/remote/`));

const inside = (path, root) => path === root || path.startsWith(`${root}/`);

// start: empty, relative to the home folder, or absolute. Nothing above the home folder or a disk.
export function startFolder(start, home, disks = []) {
  const s = String(start || '').replace(/\/+$/, '');
  if (!s) return home;
  const path = s.startsWith('/') ? s : `${home}/${s}`;
  if (path.split('/').includes('..')) return home;
  return [home, ...disks.map((d) => d.path)].some((r) => inside(path, r)) ? path : home;
}

// A Cloud account's folder (~/Cloud/<name>, docs/CLOUD.md) holding path, or null: its crumbs start there, like a disk's.
export function cloudRoot(path, home) {
  const c = `${home}/Cloud/`;
  if (!String(path).startsWith(c)) return null;
  const name = path.slice(c.length).split('/')[0];
  return name ? `${c}${name}` : null;
}

// The dialog's Cloud places: every account's folder, by name as in Files (#86), but a locked keyring's (nothing to list).
// accounts: cloud.state().accounts; hidden: the ids taken out of Files (settings.cloudHidden).
export const cloudPlaces = (accounts, home, hidden = []) => sortAccounts(accounts)
  .filter((a) => a && a.mount && cloudRoot(a.mount, home) === a.mount && a.state !== 'locked' && !hidden.includes(a.id))
  .map((a) => ({ id: a.id, name: a.name, path: a.mount }));

// Crumbs of a folder, from the place it belongs to: the home folder, a Cloud account, a disk or a connected server.
// Outside them (Files' This computer) they begin at /. The home folder's has home: true, for its name.
export function pathCrumbs(path, home) {
  const remote = `${home}/.local/share/lordos/remote/`;
  const base = path.startsWith(remote) ? path.split('/').slice(0, remote.split('/').length).join('/')
    : cloudRoot(path, home) ? cloudRoot(path, home)
    : inside(path, home) ? home
    : path.startsWith('/run/media/') ? path.split('/').slice(0, 5).join('/')
    : path.startsWith('/media/') ? path.split('/').slice(0, 4).join('/') : '/';
  const name = (p) => p.split('/').filter(Boolean).pop() || '/';
  const out = [{ label: name(base), path: base, home: base === home }];
  let acc = base;
  for (const r of path.slice(base.length).split('/').filter(Boolean)) { acc = `${acc}/${r}`; out.push({ label: r, path: acc }); }
  return out;
}

// The top bar shows the first crumb and the last ones; a null stands for what is left out.
export const fitCrumbs = (crumbs, room = 5) => (crumbs.length <= room ? crumbs : [crumbs[0], null, ...crumbs.slice(2 - room)]);

const collate = compareNames;

// Folders first and names as Files sorts them. sort: { by: 'name' | 'date' | 'size', desc }.
export function sortRows(list, sort) {
  const dir = sort.desc ? -1 : 1;
  return [...list].sort((a, b) => {
    if (!!a.dir !== !!b.dir) return a.dir ? -1 : 1;
    const r = sort.by === 'date' ? (a.mtime || 0) - (b.mtime || 0) : sort.by === 'size' ? (a.dir ? 0 : a.size) - (b.dir ? 0 : b.size) : 0;
    return (r || collate(a.name, b.name)) * dir;
  });
}

// The row that many steps from the cursor; from nothing, the first or the last.
export function stepRow(rows, cursor, by) {
  if (!rows.length) return null;
  const at = rows.findIndex((r) => r.path === cursor);
  if (at < 0) return rows[by < 0 ? rows.length - 1 : 0];
  return rows[Math.max(0, Math.min(rows.length - 1, at + by))];
}

// The files from the anchor to this row, whichever way (Shift+click).
export function rangeOf(rows, anchor, path) {
  const a = rows.findIndex((r) => r.path === anchor), b = rows.findIndex((r) => r.path === path);
  if (a < 0 || b < 0) return null;
  return rows.slice(Math.min(a, b), Math.max(a, b) + 1).filter((r) => !r.dir).map((r) => r.path);
}

export const validName = (name) => { const n = String(name || '').trim(); return !!n && !n.includes('/') && n !== '.' && n !== '..'; };

// What the primary button does. Never enters a folder in folder mode: there it chooses it.
//   mode, multiple, dir (in view), row (under the cursor, or null), chosen ([paths]), name (save), writable
// → { pick } | { pickMany } | { go } | { save } | null, and null means the button is disabled.
export function primaryAction({ mode, multiple, dir, row, chosen = [], name = '', writable = true }) {
  if (mode === 'folder') return { pick: row?.dir ? row.path : dir };
  if (mode === 'save') return writable && validName(name) ? { save: true } : null;
  if (multiple) return chosen.length ? { pickMany: chosen } : row?.dir ? { go: row.path } : null;
  if (!row) return null;
  return row.dir ? { go: row.path } : { pick: row.path };
}

// Enter opens the row under the cursor, as a double click; with no row, it is the primary button.
export function enterAction(a) {
  if (a.row?.dir) return { go: a.row.path };
  if (a.mode === 'open' && a.multiple && a.row && !a.chosen?.length) return { pickMany: [a.row.path] };
  return primaryAction(a);
}

// ---- the size of the thumbnails (issue #58) ----

// Files' zoom steps (apps/files/viewprefs.js, issue #33): ZOOM_MIN to ZOOM_MAX, 0 the usual size. The dialog's
// thumbnails take the same steps and the same control as Files' status bar (fileparts.jsx ZoomControl).
export const ZOOM_MIN = -2, ZOOM_MAX = 4;
export const clampZoom = (z) => (Number.isInteger(z) ? Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, z)) : 0);
const THUMBS = [48, 60, 72, 96, 128, 160, 200];
// A tile at a step: its picture, its width and its height (step 0 is the 72 px picture of a 104 × 118 tile).
export function thumbMetrics(z) {
  const thumb = THUMBS[clampZoom(z) - ZOOM_MIN];
  return { thumb, tileW: Math.max(88, thumb + 32), tileH: thumb + 46 };
}
// The step saved with the view (settings.pickerView.zoom: { files, images }), one per kind of dialog like the view.
export const zoomFor = (saved, images) => clampZoom(saved?.zoom?.[images ? 'images' : 'files'] ?? 0);
export function saveZoom(saved, images, z) {
  const zoom = { ...(saved?.zoom || {}), [images ? 'images' : 'files']: clampZoom(z) };
  return { ...(saved || {}), zoom };
}

// ---- the List / Thumbnails switch ----

// Does the dialog take only pictures (the wallpaper, a Notes image, a portal's image/* filter)?
// filter: a RegExp, { test } or a function of the name; null takes everything.
const IMAGE_NAMES = ['a.png', 'a.jpg', 'a.jpeg', 'a.webp', 'a.gif'];
const OTHER_NAMES = ['a.txt', 'a.pdf', 'a.mp3', 'a.mp4', 'a.zip', 'a.csv', 'a.pub', 'a.ics', 'a.odt', 'a'];
export function imagesOnly(filter) {
  if (!filter) return false;
  const test = typeof filter === 'function' ? filter : (n) => filter.test(n);
  const ok = (n) => { try { if (filter.lastIndex) filter.lastIndex = 0; return !!test(n); } catch (e) { return false; } };
  return IMAGE_NAMES.some(ok) && !OTHER_NAMES.some(ok);
}

// The view the dialog opens in. saved: settings.pickerView, { files, images }, each 'list' | 'thumbs',
// written when the person flips the switch; a dialog for pictures and one for anything else keep
// their own. Without a choice, pictures show as thumbnails and the rest as a list.
export const VIEWS = ['list', 'thumbs'];
export function viewFor(saved, images) {
  const v = saved?.[images ? 'images' : 'files'];
  return VIEWS.includes(v) ? v : images ? 'thumbs' : 'list';
}
export const saveView = (saved, images, view) => ({ ...(saved || {}), [images ? 'images' : 'files']: view });

// Thumbnails load only for the tiles in sight and a row either side: [from, to) of rows' indexes.
// offset and view in pixels, tileH the height of a row of tiles, cols the tiles in one.
export function liveRange(offset, view, tileH, cols, count, spare = 1) {
  if (!count || cols < 1 || tileH <= 0) return [0, 0];
  const first = Math.max(0, Math.floor(Math.max(0, offset) / tileH) - spare);
  const last = Math.floor((Math.max(0, offset) + Math.max(0, view)) / tileH) + spare;
  return [Math.min(count, first * cols), Math.min(count, (last + 1) * cols)];
}

// Keys in the thumbnails: ← → one tile (reversed right to left), ↑ ↓ one row.
export function gridStep(key, cols, rtl = false) {
  if (key === 'ArrowLeft') return rtl ? 1 : -1;
  if (key === 'ArrowRight') return rtl ? -1 : 1;
  if (key === 'ArrowUp') return -Math.max(1, cols);
  if (key === 'ArrowDown') return Math.max(1, cols);
  return 0;
}
