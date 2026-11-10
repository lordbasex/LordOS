// filedrop.js — which of the files dragged from Files an app takes (its onFilesDrop / onFilesOver, see apps/files).
//
// sortDrop(paths, { accepts, folders, isDir }): accepts(name) says if the app opens a file of that name, folders if it
// takes folders too, and isDir(path) tells the two apart (fs.stat on the desktop, a stub in the test). What is left is
// counted in skipped, so the hint can say that some of the files will not be used.

const nameOf = (p) => String(p || '').replace(/\/+$/, '').split('/').pop();

export function sortDrop(paths, { accepts = () => true, folders = false, isDir = () => false } = {}) {
  const out = { files: [], folders: [], skipped: 0 };
  for (const p of paths || []) {
    if (!p) continue;
    let dir = false;
    try { dir = !!isDir(p); } catch (e) { dir = false; }
    if (dir) { if (folders) out.folders.push(p); else out.skipped++; } else if (accepts(nameOf(p))) out.files.push(p); else out.skipped++;
  }
  return out;
}

// how many of them the app takes (0: the hint says it can't use any)
export const dropCount = (s) => (s ? s.files.length + s.folders.length : 0);

// the same sort for every move of one drag: Files passes the same paths array until the drag ends
export function dropSorter(opts) {
  let last = null, sorted = null;
  return (paths) => {
    if (paths !== last) { last = paths; sorted = sortDrop(paths, opts); }
    return sorted;
  };
}

export { nameOf as dropName };
