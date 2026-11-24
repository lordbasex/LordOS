// ops.js — what the Files windows share: the file clipboard, copy jobs in progress, color tags
// and opening files with the right app.
import { bundleFor, createStore, fs, wm, launchApp, archive, defaultOpenerId, nativeOf, errorText, clipboard, appsOff, openersAvailable, offerActivate,
  openProvidersFor, suggestedProviderFor, defaultProviderFor, openHintsFor } from '@lordos/sdk';
import { kindOf, mimesOf, appOpens, isTextual, isImage, isProgram, isForeignProgram } from './kinds.js';
import { quarantineBlocks } from './quarantine.js';
import { pasteFrom, offerable } from './clipfiles.js';
import { createJobQueue } from './jobqueue.js';
import { toggledTags, withComment, movedKey } from './tags.js';
import { freeNameOf } from './names.js';

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
  return freeNameOf(existing || new Set(lord.fs.list(dir).map((e) => e.name)), name);      // within 255 bytes (#102)
}

// ---- copy / move jobs (server threads) ----
export const jobState = createStore([]);                  // [{ id, src, dst, move, done, bytes, total, … }]
// lord-server runs a few at a time: the rest wait here and start as those finish (apps/files/jobqueue.js, GitHub #104)
const queue = createJobQueue({ start: (src, dst, move) => (move ? fs.move(src, dst) : fs.copy(src, dst)), jobs: () => fs.jobs(), forget: (id) => fs.forgetJob(id) });
export const jobsWaiting = createStore(0);
const failText = (f) => `${fs.basename(f.src)}: ${f.error?.message || f.error}`;
let polling = null;
function poll() {
  const { finished, failed, running } = queue.drain();
  const errs = [...finished.filter((j) => j.failed).map((j) => `${fs.basename(j.src)}: ${errorText(j.error)}`), ...failed.map(failText)];
  if (errs.length) jobErrors.set((e) => [...e, ...errs].slice(-5));
  if (finished.length) bump();
  jobState.set(running);
  jobsWaiting.set(queue.size());
  if (!running.length && !queue.size() && polling) { clearInterval(polling); polling = null; }
}
export const jobErrors = createStore([]);
function watchJobs() { if (!polling) polling = setInterval(poll, 250); poll(); }
// the ✕ of the copy shown: it stops, and what waits behind it doesn't start
export function cancelJob(id) {
  queue.clear();
  jobsWaiting.set(0);
  fs.cancelJob(id);
}

// Copy or move several paths to a folder. Copying to the same place = duplicate with a free name.
// known: the names already in destDir when the caller has them (a slow folder's listing, apps/files/remotedirs.js),
// so a Cloud or network folder isn't listed here on the interface's thread; a name taken since is refused (fs.exists)
export function transfer(paths, destDir, move, known) {
  const existing = known ? new Set(known) : new Set(lord.fs.list(destDir).map((e) => e.name));
  const errors = [];
  const items = [];
  for (const p of paths) {
    const name = fs.basename(p);
    if (move && fs.parent(p) === destDir) continue;                       // already there
    if (move && (destDir === p || destDir.startsWith(`${p}/`))) { errors.push(name); continue; }
    const target = freeName(destDir, name, existing);
    existing.add(target);                                                 // the name is taken now, even while it waits
    items.push({ src: p, dst: `${destDir}/${target}`, move });
  }
  for (const f of queue.add(items)) errors.push(failText(f));
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
  const all = toggledTags(tagStore.get(), paths, color);
  tagStore.set(() => all);                 // whole, not merged: a removed last tag stays removed (#181)
  try { fs.mkdir('.local/share/lordos'); fs.write(TAGS_FILE, JSON.stringify(all, null, 1)); } catch (e) { /* no disk */ }
}
// Comments, kept like tags (~/.local/share/lordos/comments.json: path → text).
const COMMENTS_FILE = '.local/share/lordos/comments.json';
export const commentStore = createStore((() => {
  try { return JSON.parse(fs.read(COMMENTS_FILE)); } catch (e) { return {}; }
})());
export const commentOf = (path) => commentStore.get()[path] || '';
const saveComments = (all) => {
  commentStore.set(() => all);
  try { fs.mkdir('.local/share/lordos'); fs.write(COMMENTS_FILE, JSON.stringify(all, null, 1)); } catch (e) { /* no disk */ }
};
export function setComment(path, text) { saveComments(withComment(commentStore.get(), path, text)); }
// Moving or renaming keeps the tags and the comment.
export function moveTags(from, to) {
  const comments = movedKey(commentStore.get(), from, to);
  if (comments !== commentStore.get()) saveComments(comments);
  const next = movedKey(tagStore.get(), from, to);
  if (next === tagStore.get()) return;
  tagStore.set(() => next);
  try { fs.write(TAGS_FILE, JSON.stringify(next, null, 1)); } catch (e) { /* no disk */ }
}

// ---- open ----

// Apps that open an entry: [{ id, label, run }] (the first one is the default).
export function openersFor(e, linuxApps, t) { return openersOf(e, linuxApps, t).list; }

// { list, off }: off names the deactivated LordOS apps (Store → Installed) that would have opened it, left
// out of the list; a file only they open offers to activate one (openEntry).
function openersOf(e, linuxApps, t) {
  const all = [];
  const k = kindOf(e);
  if (k === 'lpk' || k === 'deb') all.push({ id: 'store', label: t('apps.store'), run: () => wm.open('store', { args: { install: e.path } }) });
  // documents (M9): PDFs in the PDF Viewer and fonts in Fonts
  if (k === 'pdf') all.push({ id: 'pdf', label: t('apps.pdf'), run: () => wm.open('pdf', { new: true, args: { path: e.path } }) });
  if (/\.(ttf|otf|ttc|otc)$/i.test(e.name)) all.push({ id: 'fonts', label: t('apps.fonts'), run: () => wm.open('fonts', { args: { path: e.path } }) });
  // media (M9): images in Photos; audio and video in the Player (the defaults)
  if (k === 'image') all.push({ id: 'photos', label: t('apps.photos'), run: () => wm.open('photos', { new: true, args: { path: e.path } }) });
  if (k === 'audio' || k === 'video') all.push({ id: 'player', label: t('apps.player'), run: () => wm.open('player', { args: { path: e.path } }) });
  if (isTextual(e) || (k === 'other' && !isProgram(e) && !isForeignProgram(e))) all.push({ id: 'editor', label: t('apps.editor'), run: () => wm.open('editor', { args: { path: e.path } }) });
  // the system's MIME database's type first (lord.mime.guess), then Files' own table: an app that
  // declares either one opens the file (issue #49: an .xlsx had no type of its own here, so WPS and
  // GridCraft, which declare it, were never offered)
  const mimes = mimesOf(e, typeof lord.mime !== 'undefined' && lord.mime.guess ? (n) => lord.mime.guess(n) : null);
  for (const a of linuxApps) {
    if (a.kind === 'linux' && mimes.some((m) => appOpens(a.mimeTypes, m))) all.push({ id: a.id, desktopId: a.desktopId, label: a.name, run: () => lord.apps.launchWith(a.desktopId, e.path) });
    // a Flatpak app sees only that file (lord.apps.launchFlatpakWith lends it with --filesystem, docs/FLATPAK.md)
    if (a.kind === 'flatpak' && typeof lord.apps.launchFlatpakWith === 'function' && mimes.some((m) => appOpens(a.mimeTypes, m)))
      all.push({ id: a.id, label: a.name, run: () => lord.apps.launchFlatpakWith(a.flatpakId, e.path) });
  }
  const { list: out, off } = openersAvailable(all, appsOff());
  // the default app for this type (Settings → Default Apps, ~/.config/mimeapps.list) goes first,
  // marked
  let def = null;
  for (const m of mimes) if ((def = defaultOpenerId(m))) break;
  const native = nativeOf(def);
  const i = def ? out.findIndex((o) => (native ? o.id === native : o.desktopId && `${o.desktopId}.desktop` === def)) : -1;
  // "Open with" providers after the apps (sdk/openwith.js: a machine whose guest tools open files…); the person's
  // "Always open .ext files with …" one goes first, marked, ahead of any app's default
  const chosen = defaultProviderFor(e);
  if (i >= 0 && !chosen) {
    const [d] = out.splice(i, 1);
    out.unshift({ ...d, label: t('prefs.defaults.tag', { name: d.label }), isDefault: true });
  }
  for (const p of openProvidersFor(e)) {
    const o = { id: p.id, label: p.label, run: () => p.open(e), provider: p };
    if (chosen && chosen.id === p.id) out.unshift({ ...o, label: t('prefs.defaults.tag', { name: p.label }), isDefault: true });
    else out.push(o);
  }
  return { list: out, off };
}

export function openEntry(e, ctx) {
  if (e.app) { launchApp(e.id); return; }
  // a file from a Machines guest, or one inside an archive from one (its marks are the archive's: quarantine.js)
  const from = e.arc ? (Array.isArray(e.arc.file) ? e.arc.file[0] : e.arc.file) : e.path;
  if (!e.dir && !e.trash && typeof from === 'string' && !fs.isRemote(from)) {
    let st = null;
    try { st = fs.stat(from); } catch (err) { st = null; }
    if (quarantineBlocks(e, st)) { if (ctx.say) ctx.say(ctx.t('files.fromMachineNoRun', { name: e.name, machine: st.fromMachine || '?' })); return; }
  }
  // archives (M9): browsed like a folder; what is inside is extracted to the cache when opened
  if (e.arc) { if (e.dir) ctx.navigate({ type: 'archive', path: e.arc.file, inner: e.arc.inner }); else ctx.openArc(e); return; }
  // a bundle (a machine's folder): its app opens it; Open in New Tab still goes in
  if (e.dir && !e.trash) { const b = bundleFor(e); if (b) { b.open(e.path); return; } }
  if (e.dir) { ctx.navigate({ type: 'dir', path: e.path }); return; }
  if (!e.trash && archive.isArchive(e.name)) { ctx.navigate({ type: 'archive', path: e.path, inner: '' }); return; }
  const { list, off } = openersOf(e, ctx.linuxApps, ctx.t);
  // the person's "Always open .ext files with …" provider; one that starts something first (an off machine) asks
  const chosen = list[0]?.provider && list[0].isDefault ? list[0].provider : null;
  if (chosen) {
    if (ctx.askProvider && chosen.needsStart && chosen.needsStart() === true) ctx.askProvider(e, chosen, { start: true });
    else chosen.open(e);
    return;
  }
  const apps = list.filter((o) => !o.provider);
  // no app here opens it, but a provider is its natural handler (a program for another system and a machine that runs
  // it): offered, with "Always open .ext files with …"
  const suggested = !apps.length ? suggestedProviderFor(e) : null;
  if (suggested && ctx.askProvider) { ctx.askProvider(e, suggested, { start: suggested.needsStart?.() === true, offerAlways: true }); return; }
  // only a deactivated LordOS app opens it (Photos, PDF Viewer…): the notice offers to activate it
  if (off.length && !list.some((o) => o.id !== 'editor')) { offerActivate(off[0], () => openEntry(e, ctx)); return; }
  if (isImage(e) && !apps.some((o) => o.id !== 'editor')) { ctx.quickLook(e); return; }    // no viewer: Quick Preview
  if (apps.length) apps[0].run();
  else if (isProgram(e) && ctx.say) ctx.say(ctx.t('files.programNoOpener', { name: e.name }));  // not TextEdit (#159)
  // a provider says why none opens it yet (a Windows machine whose LordOS tools are too old): that, else the general line
  else if (isForeignProgram(e) && ctx.say) ctx.say(openHintsFor(e)[0] || ctx.t('files.foreignProgramNoOpener', { name: e.name }));
  else ctx.quickLook(e);
}
