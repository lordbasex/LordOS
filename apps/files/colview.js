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
// how many of the last columns fit in avail with at least preview left for the preview (at least one is shown);
// avail unknown (0): the last three, as before widths could change
export function colsThatFit(widths, avail, preview = 200) {
  if (!(avail > 0)) return Math.min(3, widths.length);
  let used = 0, n = 0;
  for (let i = widths.length - 1; i >= 0; i--) {
    if (n && used + widths[i] > avail - preview) break;
    used += widths[i];
    n++;
  }
  return Math.max(1, n);
}
