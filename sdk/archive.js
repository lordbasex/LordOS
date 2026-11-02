// archive.js — archives through the confined helper (libarchive inside axia-doc). Extracting
// never leaves the destination folder: no "..", no absolute paths, no links pointing outside
// (see docs/DOCUMENTS.md, "Extracting without leaving the folder").
//
//   archive.isArchive(name)                        .zip, .tar(.gz/.xz/.zst/.bz2), .7z, .gz…?
//   archive.list(path) → { entries: [{ p, s, m, d, l, bad, enc }], format, raw, truncated, encrypted }
//   archive.extract(path, folder, { only, existing, onProgress }) → { files, bytes, skipped, skips }
//       creates the folder (unless existing: true) and extracts inside it
//   archive.create([paths], dest.zip, { onProgress }) → { files, bytes }
//   archive.tree(entries) → Map folder → [{ name, path, dir, size, mtime }]
import { waitFor } from './doc.js';

const EXTS = /\.(zip|tar|tgz|tbz2?|txz|tzst|tar\.(gz|xz|zst|bz2|lz|lzma)|7z|gz|xz|zst|bz2|cpio|iso|ar|deb|rar|lha|lzh|cab|jar|apk|war|xpi)$/i;
// what gets offered "Extract" (the rest can be browsed inside if libarchive reads it)
export const isArchive = (name) => EXTS.test(String(name)) && !/\.(deb|apk)$/i.test(String(name));
// bare .gz/.xz/.zst/.bz2 (not tar): a single file named without the extension
export const isRawCompressed = (name) => /\.(gz|xz|zst|bz2)$/i.test(name) && !/\.tar\.[^.]+$/i.test(name);
export const rawName = (name) => String(name).replace(/\.(gz|xz|zst|bz2)$/i, '');
// destination folder name: "photos.tar.gz" → "photos"
export const stem = (name) => String(name).replace(/\.(tar\.(gz|xz|zst|bz2|lz|lzma)|tgz|tbz2?|txz|tzst|zip|tar|7z|gz|xz|zst|bz2|rar|cpio|iso)$/i, '') || name;

function list(path) {
  let r;
  try { r = lord.doc.arcList(path); } catch (e) { return Promise.reject(e); }
  return waitFor(r.sid, r.req).finally(() => lord.doc.close(r.sid));
}

function extract(path, dest, { only, existing = false, onProgress } = {}) {
  let r;
  const args = {};
  if (only) args.only = only;
  if (isRawCompressed(path)) args.rawname = rawName(String(path).split('/').pop());
  try { r = lord.doc.arcExtract(path, dest, JSON.stringify(args), existing); } catch (e) { return Promise.reject(e); }
  const p = waitFor(r.sid, r.req, { onProgress }).finally(() => lord.doc.close(r.sid));
  p.sid = r.sid;
  return p;
}

function create(paths, out, { onProgress } = {}) {
  let r;
  try { r = lord.doc.arcCreate(paths, out); } catch (e) { return Promise.reject(e); }
  const p = waitFor(r.sid, r.req, { onProgress }).finally(() => lord.doc.close(r.sid));
  p.sid = r.sid;
  return p;
}

// Folder tree from the flat list (folders without their own entry are
// inferred from the paths).
function tree(entries) {
  const dirs = new Map([['', []]]);
  const ensure = (d) => {
    if (dirs.has(d)) return;
    dirs.set(d, []);
    const i = d.lastIndexOf('/');
    const parent = i < 0 ? '' : d.slice(0, i);
    ensure(parent);
    dirs.get(parent).push({ name: d.slice(i + 1), path: d, dir: true, size: 0, mtime: 0 });
  };
  for (const e of entries) {
    if (e.bad) continue;
    const p = e.p.replace(/\/+$/, '');
    if (!p) continue;
    const i = p.lastIndexOf('/');
    const parent = i < 0 ? '' : p.slice(0, i);
    ensure(parent);
    if (e.d) {
      if (!dirs.has(p)) { dirs.set(p, []); dirs.get(parent).push({ name: p.slice(i + 1), path: p, dir: true, size: 0, mtime: e.m }); }
      else { const x = dirs.get(parent).find((q) => q.path === p); if (x) x.mtime = e.m; }
    } else dirs.get(parent).push({ name: p.slice(i + 1), path: p, dir: false, size: e.s, mtime: e.m, link: e.l, enc: e.enc });
  }
  return dirs;
}

export const archive = { isArchive, isRawCompressed, rawName, stem, list, extract, create, tree };
