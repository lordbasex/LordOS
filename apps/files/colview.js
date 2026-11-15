// colview.js — what the column view shows as data, no screen: the items of every folder down the chain, so what is
// picked in a deeper column is part of the selection (GitHub #100), Shift's range inside one column, and whether the
// folder shown takes changes.

// root: the first column's folder; chain: the item picked in each column ([level] = a path in column level);
// itemsOf(dir): that folder's items in the order shown. A chain item that is not a folder ends the columns.
export function columnEntries(root, chain, itemsOf) {
  const out = [];
  let dir = root;
  for (let level = 0; dir != null && level <= chain.length; level++) {
    const items = itemsOf(dir) || [];
    out.push(...items);
    const next = items.find((e) => e.path === chain[level]);
    dir = next && next.dir ? next.path : null;
  }
  return out;
}

const parentOf = (p) => p.slice(0, p.lastIndexOf('/'));

// Shift + click: the items between the anchor and the clicked one, in the clicked one's column only; an anchor in
// another column (or gone) selects just the clicked item.
export function columnRange(pool, anchor, path) {
  const col = pool.filter((e) => parentOf(e.path) === parentOf(path));
  const a = col.findIndex((e) => e.path === anchor), b = col.findIndex((e) => e.path === path);
  if (a < 0 || b < 0) return [path];
  return col.slice(Math.min(a, b), Math.max(a, b) + 1).map((e) => e.path);
}

// Duplicate: each item is copied next to itself, so the paths go by the folder they are in (a deeper column's
// item stays in its own folder, not the first column's).
export function byFolder(paths) {
  const m = new Map();
  for (const p of paths) {
    const d = parentOf(p) || '/';
    if (!m.has(d)) m.set(d, []);
    m.get(d).push(p);
  }
  return [...m].map(([dir, list]) => ({ dir, paths: list }));
}

// Whether the folder shown is only browsed (the actions that change it greyed out). The home folder itself isn't a
// place lord.fs writes (its parent is outside the home), so its stat says read-only, but what is in it is: Move to
// Trash, Cut, Paste and New Folder were greyed out in Home and in the column view opened from there (GitHub #100; the
// file chooser knows it too, sdk/filepicker.jsx). stat(dir) → lord.fs.stat, or throws.
export function folderReadOnly(dir, home, stat) {
  if (!dir) return false;
  if (dir === home) return false;
  try { return !stat(dir).writable; } catch (e) { return true; }
}

// ---- widths (GitHub #99) ----
// Each column is as wide as its folder's stored width (dragging its divider; a double click fits it to the longest
// name), else COL_W. The widths are kept per folder, at most MAX_COL_WIDTHS of them, the newest last.
export const COL_W = 220;
export const COL_MIN = 140;
export const COL_MAX = 600;
export const MAX_COL_WIDTHS = 200;
// what a row takes besides its name: the column's and the row's padding, the icon and its gap, the › and the divider
export const COL_EXTRA = 6 * 2 + 8 * 2 + 16 + 6 + 6 + 8 + 5;
export const clampCol = (w) => Math.min(COL_MAX, Math.max(COL_MIN, Math.round(Number(w) || COL_W)));
export const colWidth = (widths, dir) => {
  const w = widths && typeof widths === 'object' ? widths[dir] : undefined;
  return typeof w === 'number' && Number.isFinite(w) ? clampCol(w) : COL_W;
};
// w null forgets the folder's width (back to COL_W)
export function colWidthsWith(widths, dir, w) {
  const out = {};
  if (widths && typeof widths === 'object') for (const [k, v] of Object.entries(widths)) if (k !== dir && typeof v === 'number' && Number.isFinite(v)) out[k] = clampCol(v);
  if (w != null) out[dir] = clampCol(w);
  const keys = Object.keys(out);
  for (let i = 0; i < keys.length - MAX_COL_WIDTHS; i++) delete out[keys[i]];
  return out;
}
// the width that shows the longest of these names (their measured widths, plus what tags add to each row)
export const fitCol = (nameWidths, extras = []) =>
  clampCol(Math.ceil(nameWidths.reduce((m, w, i) => Math.max(m, (Number(w) || 0) + (Number(extras[i]) || 0)), 0)) + COL_EXTRA);
// ---- every column, side by side (GitHub #133) ----
// The columns are never dropped to make room (#99 showed only the last ones that fit, so widening one, or opening a
// subfolder, pushed the first ones out of the view for good): they scroll sideways, as in Finder, with the preview
// last. The preview takes what is left of the view, at least PREVIEW_MIN.
export const PREVIEW_MIN = 200;
// widths: each column's; avail: the view's width (0 before it is laid out)
export function colStrip(widths, avail) {
  const cols = widths.reduce((a, w) => a + (Number(w) || 0), 0);
  const preview = avail > 0 ? Math.max(PREVIEW_MIN, avail - cols) : PREVIEW_MIN;
  const total = cols + preview;
  return { preview, total, over: avail > 0 ? Math.max(0, total - avail) : 0 };
}
// the deepest folder the columns show: the last folder picked down the chain (a file there shows its preview)
export function chainFolder(chain, isDir) {
  for (let i = chain.length - 1; i >= 0; i--) if (isDir(chain[i])) return chain[i];
  return null;
}

// ---- Back and Forward in the column view (GitHub #133) ----
// A history entry is a place and, for the column view, the items picked down its columns ({ ...loc, chain }). A click
// in a column that opens another folder is a step Back returns to (as in Finder); it was none, so Back left the column
// view for the place before, in that place's own view (Home in icons).
// tab: { loc, chain, back, fwd }; returns the tab's new fields.
export function chainStep(tab, chain, isDir) {
  if (chainFolder(chain, isDir) === chainFolder(tab.chain || [], isDir)) return { chain };
  return { chain, back: [...tab.back, { ...tab.loc, chain: tab.chain || [] }].slice(-50), fwd: [] };
}
// Back (dir -1) or Forward (+1): the entry's place and chain. In the same place only the chain changes and the view
// stays; another place opens in its own view (viewOf). same(a, b): whether two places are the same (locations.js).
export function historyStep(tab, d, viewOf, same) {
  const list = d < 0 ? tab.back : tab.fwd;
  if (!list.length) return null;
  const entry = d < 0 ? list[list.length - 1] : list[0];
  const { chain = [], ...loc } = entry;
  const here = { ...tab.loc, chain: tab.chain || [] };
  return {
    ...(same(loc, tab.loc) ? null : viewOf(loc)),
    loc, chain,
    back: d < 0 ? tab.back.slice(0, -1) : [...tab.back, here],
    fwd: d < 0 ? [here, ...tab.fwd] : tab.fwd.slice(1),
  };
}

// ---- where New Folder puts the folder (GitHub #134) ----
const within = (p, dir) => p === dir || p.startsWith(dir === '/' ? '/' : `${dir}/`);
// It went in the folder shown even with another one picked. Now, as in Finder: in the column view, in the last
// column's folder (a folder picked there, or the folder of the file picked); in the list, in the folder that is
// selected (one folder), or in the folder the selected items are in (an unfolded subfolder's). The icon and gallery
// views show one folder: there it is that one. Never outside the folder shown.
// view: the view; dir: the folder shown (null: Recents, a search…); chain: the column view's; selected: the paths
// selected; isDir(path).
export function newFolderDir({ view, dir, chain = [], selected = [], isDir }) {
  if (!dir) return null;
  let at = dir;
  if (view === 'columns') {
    const last = chain[chain.length - 1];
    if (last) at = isDir(last) ? last : parentOf(last);
  } else if (view === 'list' && selected.length) {
    if (selected.length === 1 && isDir(selected[0])) at = selected[0];
    else {
      const parents = new Set(selected.map((p) => parentOf(p) || '/'));
      if (parents.size === 1) at = [...parents][0];
    }
  }
  return at && within(at, dir) ? at : dir;
}

// ---- the list's unfolded folders ----
// The items the list shows, each unfolded folder's after it (as its rows are drawn), so what is selected inside one is
// part of the selection: Copy, Rename, Move to Trash and New Folder had nothing to act on there (as #100 in the
// column view). itemsOf(entry): that folder's items in the order shown, or null.
export function listEntries(items, expanded, itemsOf, depth = 0) {
  const out = [];
  for (const e of items) {
    out.push(e);
    if (e.dir && expanded.has(e.path) && depth < 32) out.push(...listEntries(itemsOf(e) || [], expanded, itemsOf, depth + 1));
  }
  return out;
}

// ---- why a folder is only browsed (GitHub #139) ----
// 'outside': it is out of where Files writes (lord.fs: the home folder and the user's disks under /media/$USER and
// /run/media/$USER), whatever its permissions say, so another app (the Terminal) has to change it; 'denied': it is in
// there, but its own permissions don't let this user change it; null: it takes changes.
export function readOnlyWhy(dir, readOnly, home, user) {
  if (!dir || !readOnly) return null;
  const roots = [home, ...(user ? [`/media/${user}`, `/run/media/${user}`] : [])];
  return roots.some((r) => r && r.length > 1 && within(dir, r)) ? 'denied' : 'outside';
}
