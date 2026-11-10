// ops.js — what the Files windows share: the file clipboard, copy jobs in progress, color tags
// and opening files with the right app.
import { createStore, fs, wm, launchApp, archive, defaultOpenerId, nativeOf, errorText, clipboard } from '@lordos/sdk';
import { kindOf, mimesOf, appOpens, isTextual, isImage } from './kinds.js';
import { pasteFrom, offerable } from './clipfiles.js';

// Changes every time something changed on disk (a copy finished, the trash…): the views reload.
export const fsVersion = createStore(0);
export const bump = () => fsVersion.set((v) => v + 1);

// ---- file clipboard ----
// Files keeps its own (a cut is a move only between its windows) and also puts the paths on the session clipboard,
// where Linux apps read them as files (text/uri-list, gnome-copied-files) and text editors as the paths.
export const fileClipboard = createStore(null);          // { mode: 'copy' | 'cut', paths: [] }
const sessionFiles = () => { try { return clipboard.files(); } catch (e) { return null; } };
// The session clipboard's files older than Files' own copy: one Files couldn't offer (a place without local paths)
// leaves them there, and they mustn't win over it.
let olderGen = -1;
function offer(paths, cut) {
  const list = offerable(paths);
  let ok = false;
  if (list) { try { ok = clipboard.setFiles(list, { cut }); } catch (e) { /* the session clipboard keeps what it had */ } }
  const f = ok ? null : sessionFiles();
  olderGen = f ? f.gen : -1;
}
export const copyFiles = (paths) => { fileClipboard.set({ mode: 'copy', paths }); offer(paths, false); };
export const cutFiles = (paths) => { fileClipboard.set({ mode: 'cut', paths }); offer(paths, true); };
// What a paste would bring: Files' own copy or cut, or the files a Linux file manager copied (apps/files/clipfiles.js).
const outside = () => { const f = sessionFiles(); return f && f.gen === olderGen ? null : f; };
export const canPaste = (inside) => !!pasteFrom(inside, outside());

// Free name in a folder: "photo.png" → "photo 2.png", "photo 3.png"…
export function freeName(dir, name, existing) {
  const taken = existing || new Set(lord.fs.list(dir).map((e) => e.name));
  if (!taken.has(name)) return name;
  const m = /^(.*?)(\.[^.]+)?$/.exec(name);
  const stem = m[1].replace(/ \d+$/, ''), ext = m[2] || '';
  for (let i = 2; i < 10000; i++) {
    const n = `${stem} ${i}${ext}`;
    if (!taken.has(n)) return n;
  }
  return `${stem} ${Date.now()}${ext}`;
}

// ---- copy / move jobs (server threads) ----
export const jobState = createStore([]);                  // [{ id, src, dst, move, done, bytes, total, … }]
let polling = null;
function poll() {
  const list = fs.jobs();
  const done = list.filter((j) => j.done);
  for (const j of done) {
    fs.forgetJob(j.id);
    if (j.failed) jobErrors.set((e) => [...e, `${fs.basename(j.src)}: ${errorText(j.error)}`].slice(-5));
  }
  if (done.length) bump();
  const running = list.filter((j) => !j.done);
  jobState.set(running);
  if (!running.length && polling) { clearInterval(polling); polling = null; }
}
export const jobErrors = createStore([]);
function watchJobs() { if (!polling) polling = setInterval(poll, 250); poll(); }

// Copy or move several paths to a folder. Copying to the same place = duplicate with a free name.
// known: the names already in destDir when the caller has them (a slow folder's listing, apps/files/remotedirs.js),
// so a Cloud or network folder isn't listed here on the interface's thread; a name taken since is refused (fs.exists)
export function transfer(paths, destDir, move, known) {
  const existing = known ? new Set(known) : new Set(lord.fs.list(destDir).map((e) => e.name));
  const errors = [];
  for (const p of paths) {
    const name = fs.basename(p);
    if (move && fs.parent(p) === destDir) continue;                       // already there
    if (move && (destDir === p || destDir.startsWith(`${p}/`))) { errors.push(name); continue; }
    const target = freeName(destDir, name, existing);
    existing.add(target);
    try {
      if (move) fs.move(p, `${destDir}/${target}`);
      else fs.copy(p, `${destDir}/${target}`);
    } catch (e) { errors.push(`${name}: ${e.message}`); }
  }
  watchJobs();
  if (errors.length) jobErrors.set((e) => [...e, ...errors].slice(-5));
  return errors;
}

// What a paste brings now (apps/files/clipfiles.js pasteFrom): files from another app are pasted only after the
// person says yes (pasteAsk), so the window asks first and then passes the same plan to paste.
export const pastePlan = () => pasteFrom(fileClipboard.get(), outside());
export function paste(destDir, known, c = pastePlan()) {
  if (!c) return;
  transfer(c.paths, destDir, c.move, known);
  if (c.move) fileClipboard.set(null);
}

// ---- tags (~/.local/share/lordos/tags.json: path → [colors]) ----
const TAGS_FILE = '.local/share/lordos/tags.json';
export const tagStore = createStore((() => {
  try { return JSON.parse(fs.read(TAGS_FILE)); } catch (e) { return {}; }
})());
export const tagsOf = (path) => tagStore.get()[path] || [];
export function toggleTag(paths, color) {
  const all = { ...tagStore.get() };
  const on = paths.every((p) => (all[p] || []).includes(color));
  for (const p of paths) {
    const cur = new Set(all[p] || []);
    if (on) cur.delete(color); else cur.add(color);
    if (cur.size) all[p] = [...cur]; else delete all[p];
  }
  tagStore.set(all);
  try { fs.mkdir('.local/share/lordos'); fs.write(TAGS_FILE, JSON.stringify(all, null, 1)); } catch (e) { /* no disk */ }
}
// Comments, kept like tags (~/.local/share/lordos/comments.json: path → text).
const COMMENTS_FILE = '.local/share/lordos/comments.json';
export const commentStore = createStore((() => {
  try { return JSON.parse(fs.read(COMMENTS_FILE)); } catch (e) { return {}; }
})());
export const commentOf = (path) => commentStore.get()[path] || '';
export function setComment(path, text) {
  const all = { ...commentStore.get() };
  if (text.trim()) all[path] = text.slice(0, 500); else delete all[path];
  commentStore.set(all);
  try { fs.mkdir('.local/share/lordos'); fs.write(COMMENTS_FILE, JSON.stringify(all, null, 1)); } catch (e) { /* no disk */ }
}
// Moving or renaming keeps the tags.
export function moveTags(from, to) {
  if (commentOf(from)) { const text = commentOf(from); setComment(from, ''); setComment(to, text); }
  const all = tagStore.get();
  if (!all[from]) return;
  const next = { ...all, [to]: all[from] };
  delete next[from];
  tagStore.set(next);
  try { fs.write(TAGS_FILE, JSON.stringify(next, null, 1)); } catch (e) { /* no disk */ }
}

// ---- open ----

// Apps that open an entry: [{ id, label, run }] (the first one is the default).
export function openersFor(e, linuxApps, t) {
  const out = [];
  const k = kindOf(e);
  if (k === 'lpk' || k === 'deb') out.push({ id: 'store', label: t('apps.store'), run: () => wm.open('store', { args: { install: e.path } }) });
  // documents (M9): PDFs in the PDF Viewer and fonts in Fonts
  if (k === 'pdf') out.push({ id: 'pdf', label: t('apps.pdf'), run: () => wm.open('pdf', { new: true, args: { path: e.path } }) });
  if (/\.(ttf|otf|ttc|otc)$/i.test(e.name)) out.push({ id: 'fonts', label: t('apps.fonts'), run: () => wm.open('fonts', { args: { path: e.path } }) });
  // media (M9): images in Photos; audio and video in the Player (the defaults)
  if (k === 'image') out.push({ id: 'photos', label: t('apps.photos'), run: () => wm.open('photos', { new: true, args: { path: e.path } }) });
  if (k === 'audio' || k === 'video') out.push({ id: 'player', label: t('apps.player'), run: () => wm.open('player', { args: { path: e.path } }) });
  if (isTextual(e) || k === 'other') out.push({ id: 'editor', label: t('apps.editor'), run: () => wm.open('editor', { args: { path: e.path } }) });
  // the system's MIME database's type first (lord.mime.guess), then Files' own table: an app that
  // declares either one opens the file (issue #49: an .xlsx had no type of its own here, so WPS and
  // GridCraft, which declare it, were never offered)
  const mimes = mimesOf(e, typeof lord.mime !== 'undefined' && lord.mime.guess ? (n) => lord.mime.guess(n) : null);
  for (const a of linuxApps) {
    if (a.kind === 'linux' && mimes.some((m) => appOpens(a.mimeTypes, m))) out.push({ id: a.id, desktopId: a.desktopId, label: a.name, run: () => lord.apps.launchWith(a.desktopId, e.path) });
  }
  // the default app for this type (Settings → Default Apps, ~/.config/mimeapps.list) goes first,
  // marked
  let def = null;
  for (const m of mimes) if ((def = defaultOpenerId(m))) break;
  const native = nativeOf(def);
  const i = def ? out.findIndex((o) => (native ? o.id === native : o.desktopId && `${o.desktopId}.desktop` === def)) : -1;
  if (i >= 0) {
    const [d] = out.splice(i, 1);
    out.unshift({ ...d, label: t('prefs.defaults.tag', { name: d.label }), isDefault: true });
  }
  return out;
}

export function openEntry(e, ctx) {
  if (e.app) { launchApp(e.id); return; }
  // archives (M9): browsed like a folder; what is inside is extracted to the cache when opened
  if (e.arc) { if (e.dir) ctx.navigate({ type: 'archive', path: e.arc.file, inner: e.arc.inner }); else ctx.openArc(e); return; }
  if (e.dir) { ctx.navigate({ type: 'dir', path: e.path }); return; }
  if (!e.trash && archive.isArchive(e.name)) { ctx.navigate({ type: 'archive', path: e.path, inner: '' }); return; }
  const list = openersFor(e, ctx.linuxApps, ctx.t);
  if (isImage(e) && !list.some((o) => o.id !== 'editor')) { ctx.quickLook(e); return; }    // no viewer: Quick Preview
  if (list.length) list[0].run();
  else ctx.quickLook(e);
}
