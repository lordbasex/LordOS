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
