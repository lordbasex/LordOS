// archive.js — archives in Files: browse them like a folder, open or view a file inside
// (only that one is extracted, to the cache), "Extract Here" and "Compress". The work is done by
// the confined helper axia-doc (libarchive), which never writes outside the destination folder
// (see docs/DOCUMENTS.md).
//   location: { type: 'archive', path: '/…/photos.zip', inner: 'folder/sub' }
import { archive, createStore, fs } from '@lordos/sdk';
import { bump, freeName } from './ops.js';

const lists = new Map();            // path → { mtime, state: 'loading' | 'ok' | 'error', dirs, info, error }
export const archiveVersion = createStore(0);

export const isArchiveFile = (e) => !e.dir && !e.app && !e.trash && archive.isArchive(e.name);

function load(path) {
  let st;
  try { st = fs.stat(path); } catch (e) { return { state: 'error', error: e.code, message: e.message }; }
  const cur = lists.get(path);
  if (cur && cur.mtime === st.mtime) return cur;
  const entry = { mtime: st.mtime, state: 'loading' };
  lists.set(path, entry);
  archive.list(path).then((r) => {
    entry.state = 'ok';
    entry.info = r;
    entry.dirs = archive.tree(r.entries);
    archiveVersion.set((v) => v + 1);
  }).catch((e) => {
    entry.state = 'error';
    entry.error = e.code;
    entry.message = e.message;       // already translated (sdk/doc.js)
    archiveVersion.set((v) => v + 1);
  });
  return entry;
}

// Entries of the location (synchronous: returns [] while loading and then notifies through archiveVersion).
export function archiveEntries(loc) {
  const a = load(loc.path);
  if (a.state === 'error') throw new Error(a.error === 'invalid' ? 'archive-invalid' : a.message);
  if (a.state !== 'ok') return [];
  const list = a.dirs.get(loc.inner || '') || [];
  return list.map((x) => ({ name: x.name, path: `archive:${loc.path}::${x.path}`, dir: x.dir, size: x.size, mtime: x.mtime, arc: { file: loc.path, inner: x.path }, enc: x.enc }));
}
export const archiveState = (path) => lists.get(path) || null;

// cache folder for what is extracted from this archive (one per path and date)
function cacheDir(file) {
  let h = 2166136261;
  const key = `${file}|${lists.get(file)?.mtime || 0}`;
  for (let i = 0; i < key.length; i++) { h ^= key.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
  for (const p of ['.cache', '.cache/axia', '.cache/axia/archives']) { try { fs.mkdir(p); } catch (e) { /* already there */ } }
  const dir = `${fs.home()}/.cache/axia/archives/${h.toString(16)}`;
  try { fs.mkdir(dir); } catch (e) { /* already there */ }
  return dir;
}

// Extracts an entry to the cache and returns its real path (to open or view it).
const extracted = new Map();
export async function extractEntry(e) {
  const dir = cacheDir(e.arc.file);
  const real = `${dir}/${e.arc.inner}`;
  if (extracted.get(real)) return real;
  try { fs.stat(real); extracted.set(real, true); return real; } catch (err) { /* it has to be extracted */ }
  await archive.extract(e.arc.file, dir, { only: [e.arc.inner], existing: true });
  extracted.set(real, true);
  return real;
}

// "Extract Here": a new folder named after the archive, next to it.
export function extractHere(e, report, t) {
  const dir = fs.parent(e.path);
  const name = freeName(dir, archive.stem(e.name));
  report(t('docs.extracting', { name: e.name }));
  return archive.extract(e.path, `${dir}/${name}`, {
    onProgress: (p) => report(t('docs.extractProgress', { name: e.name, files: p.files, pct: p.size ? Math.round(100 * p.pos / p.size) : 0 })),
  }).then((r) => {
    bump();
    report(r.skipped ? t('docs.extractedSkipped', { name, count: r.files, skipped: r.skipped }) : t('docs.extracted', { name, count: r.files }));
    return { dir: `${dir}/${name}`, ...r };
  }).catch((err) => {
    bump();
    report(t('docs.extractError', { name: e.name, error: err.message }));
  });
}

// "Compress": a .zip next to it (the item's name, or "Archive.zip" for several).
export function compress(paths, dir, report, t) {
  if (!paths.length) return Promise.resolve(null);
  const base = paths.length === 1 ? fs.basename(paths[0]) : t('docs.archiveName');
  const name = freeName(dir, `${base}.zip`);
  report(t('docs.compressing', { name }));
  return archive.create(paths, `${dir}/${name}`, {
    onProgress: (p) => report(t('docs.compressProgress', { name, files: p.files })),
  }).then((r) => { bump(); report(t('docs.compressed', { name, count: r.files })); return name; })
    .catch((err) => { bump(); report(t('docs.compressError', { name, error: err.message })); });
}
