// store.js — the notes on disk: ~/Notes/<slug>/nota.md (markdown with a minimal YAML
// header) and its attachments next to it. They save themselves, about 600 ms after the last change.
//
//   ---
//   title: Groceries
//   color: yellow
//   x: 120
//   y: 90
//   w: 260
//   h: 240
//   desktop: true
//   pinned: false
//   created: 2026-10-16T10:12:00.000Z
//   updated: 2026-10-16T10:20:31.000Z
//   ---
//   # Groceries
//   - [ ] yerba mate
import { createStore, fs, screen, registerWidgetType, setWidgets, workArea } from '@lordos/sdk';
import { linkTarget, safeAttachName, attachmentRef, linkLabel, imageKind, isImage } from './links.js';
import { serialize, COLORS, MAX_NOTE, MAX_NOTES, parseNote, titleOf, geometryOf, validSlug, noteFrom, mergeScan, newestFirst, importNote, importable, clampInto, placeOnDesktop } from './notesmodel.js';

export { isImage, serialize, COLORS, parseNote, titleOf, geometryOf, validSlug, importable };

export const DIR = 'Notes';
const OLD_DIR = 'Notas'; // before 0.7.0: moved to ~/Notes on first run (see ensureDir)
export const notesStore = createStore({ loaded: false, list: [] });

export const folder = (slug) => {
  if (!validSlug(slug)) throw new Error('notes: bad note folder');
  return `${DIR}/${slug}`;
};
export const absFolder = (slug) => `${fs.home()}/${DIR}/${slug}`;

const exists = (p) => { try { return !!fs.stat(p); } catch (e) { return false; } };

// First run after the rename: ~/Notas becomes ~/Notes. If both exist, ~/Notes wins and ~/Notas is
// left untouched.
let migrated = false;
function migrateOldDir() {
  if (migrated) return;
  migrated = true;
  if (exists(DIR) || !exists(OLD_DIR)) return;
  try { fs.rename(OLD_DIR, DIR); console.log('notes: moved ~/Notas to ~/Notes'); } catch (e) { console.log(`notes: could not move ~/Notas to ~/Notes: ${e.message}`); }
}

function ensureDir() {
  migrateOldDir();
  try { fs.list(DIR); } catch (e) { try { fs.mkdir(DIR); } catch (e2) { /* no home folder */ } }
}

// Every note in ~/Notes, read from disk. A nota.md whose size and time did not change since the last read is not read
// again (cache: slug → { size, mtime, note }), so reading the folder again when the window opens or gets the focus
// costs one list and a stat per note.
const cache = new Map();
// note folders without a nota.md yet, made in the last 10 minutes (another program made the folder and is about to write
// its note): watched for a moment, since a file written inside a folder does not change ~/Notes itself
let pending = [];
function scanDisk() {
  ensureDir();
  const list = [];
  let dirs = [];
  try { dirs = fs.list(DIR).filter((e) => e.dir); } catch (e) { dirs = []; }
  const seen = new Set();
  const waiting = [];
  for (const d of dirs) {
    if (list.length >= MAX_NOTES) break;
    // a note is a real folder with a real nota.md: a link (to a folder or a file elsewhere) is not followed, so a note
    // never reads or writes outside ~/Notes (GitHub #238)
    if (d.link || !validSlug(d.name)) continue;
    let st = null;
    try { st = fs.stat(`${DIR}/${d.name}/nota.md`); } catch (e) { st = null; }
    if (!st && Date.now() - Number(d.mtime || 0) < 10 * 60 * 1000) waiting.push(d);
    if (!st || st.link || st.dir || st.size > MAX_NOTE) continue;
    const hit = cache.get(d.name);
    if (hit && hit.size === st.size && hit.mtime === st.mtime) { list.push(hit.note); seen.add(d.name); continue; }
    let txt = null;
    try { txt = fs.read(`${DIR}/${d.name}/nota.md`); } catch (e) { continue; }
    if (typeof txt !== 'string' || txt.length > MAX_NOTE) continue;
    const note = noteFrom(d.name, txt, d.mtime);
    cache.set(d.name, { size: st.size, mtime: st.mtime, note });
    seen.add(d.name);
    list.push(note);
  }
  for (const k of [...cache.keys()]) if (!seen.has(k)) cache.delete(k);
  pending = waiting.sort((a, b) => Number(b.mtime) - Number(a.mtime)).slice(0, MAX_PENDING_WATCH).map((d) => d.name);
  return list;
}

export function loadNotes() {
  const list = scanDisk();
  list.sort(newestFirst);
  notesStore.set({ loaded: true, list });
  watchNotes(true);
}

// Read ~/Notes again (the Notes window opening or getting the focus, the widget catalog's sticky note): notes made or
// changed elsewhere come in, deleted folders go, and a note with a change not saved yet keeps it (notesmodel.js
// mergeScan). The notes with desktop: true show on the desktop by themselves (publishWidgets).
export function rescanNotes() {
  if (!notesStore.get().loaded) { loadNotes(); return; }
  const { list, changed } = mergeScan(notesStore.get().list, scanDisk(), new Set([...unsaved, ...timers.keys()]));
  if (changed) notesStore.set((s) => ({ ...s, list }));
  watchNotes(true);
}

// ---- seeing changes at once (lord.fs.watch: read-only inotify, coalesced to one call every 150 ms, no file names) ----
// One watch on ~/Notes: a note folder made or deleted by another program (an assistant writing
// ~/Notes/<slug>/nota.md with desktop: true) is read at once, and a desktop note appears within a second, with no window
// open. A file changed inside a note's folder does not change ~/Notes, so a few folders are watched too: the ones made
// a moment ago that have no nota.md yet (MAX_PENDING_WATCH) and the notes shown on the desktop (MAX_DESK_WATCH, most
// recently updated first). At most 1 + 3 + 4 of the session's 16 watches; on a cloud or network folder (refused) the
// window's open and focus are what read the folder again. No timers. A folder's change in the second after Notes saved
// that note is its own save and is not read again (no loop; the note in memory is what was written).
const MAX_PENDING_WATCH = 3, MAX_DESK_WATCH = 4;
let rootWatch = null;
let lastKey = null;
const folderWatches = new Map();           // slug → stop
const savedAt = new Map();                 // slug → when Notes last wrote its nota.md
const onChange = () => { try { rescanNotes(); } catch (e) { console.log(`notes: ${e.message}`); } };
function watchOne(path, fn) {
  if (typeof fs.watch !== 'function') return null;
  try { return fs.watch(path, fn); } catch (e) { console.log(`notes: not watched (${e.code || e.message}); read again on focus`); return null; }
}
function watchNotes(again = false) {
  if (!rootWatch && again) {
    rootWatch = watchOne(`${fs.home()}/${DIR}`, (ev) => {
      // ~/Notes itself deleted or moved: the next read makes it again and watches it there
      if (ev && ev.gone) { const stop = rootWatch; rootWatch = null; if (stop) stop(); }
      onChange();
    });
  }
  const desk = notesStore.get().list.filter((n) => n.desktop).sort(newestFirst).slice(0, MAX_DESK_WATCH).map((n) => n.slug);
  const want = new Set([...pending, ...desk].filter(validSlug));
  const key = [...want].join('/');
  if (key === lastKey && !again) return;
  lastKey = key;
  for (const [slug, stop] of folderWatches) if (!want.has(slug)) { stop(); folderWatches.delete(slug); }
  for (const slug of want) {
    if (folderWatches.has(slug)) continue;
    const stop = watchOne(`${fs.home()}/${DIR}/${slug}`, (ev) => {
      if (ev && ev.gone) { const s2 = folderWatches.get(slug); folderWatches.delete(slug); if (s2) s2(); }
      else if (Date.now() - (savedAt.get(slug) || 0) < 1000) return;
      onChange();
    });
    if (stop) folderWatches.set(slug, stop);
  }
}

// ---- saving ----

const timers = new Map();
// notes changed in memory and not written yet (waiting for their save, or a save that failed): a read of the folder
// keeps them as they are in memory until a save succeeds (mergeScan's dirty)
const unsaved = new Set();
// A save: on a worker (lord.fs.writeAsync, a temporary file, fsync, rename), so a slow disk never holds typing; one at a
// time per note (a change made meanwhile is written right after it). A failed save keeps the note not saved and is tried
// again in RETRY_MS; flushAll (closing, logout, the desktop note's ✕) writes at once (sync), after any save still on its
// way (lord.fs.write waits for it), so the last text is the one on the disk.
export const RETRY_MS = 5000;
const writing = new Set(), writeAgain = new Set();
function writeNow(slug, sync = false) {
  timers.delete(slug);
  const n = notesStore.get().list.find((x) => x.slug === slug);
  if (!n) { unsaved.delete(slug); writeAgain.delete(slug); return; }
  unsaved.add(slug);                       // until it is on the disk (a read of the folder meanwhile keeps it)
  const text = serialize(n);
  const path = `${folder(slug)}/nota.md`;
  const failed = (e) => {
    unsaved.add(slug);
    console.log(`notes: could not save ${slug}: ${e && e.message}`);
    if (!timers.has(slug)) timers.set(slug, setTimeout(() => writeNow(slug), RETRY_MS));
  };
  // saved: unless something newer is waiting (typed since, or another save asked)
  const saved = () => {
    savedAt.set(slug, Date.now());
    const cur = notesStore.get().list.find((x) => x.slug === slug);
    if (!live.has(slug) && !timers.has(slug) && !writeAgain.has(slug) && cur && serialize(cur) === text) unsaved.delete(slug);
  };
  savedAt.set(slug, Date.now());
  if (sync || typeof fs.writeAsync !== 'function') {
    writeAgain.delete(slug);
    try { fs.write(path, text); saved(); } catch (e) { failed(e); }
    return;
  }
  if (writing.has(slug)) { writeAgain.add(slug); return; }
  writing.add(slug);
  let p;
  try { p = fs.writeAsync(path, text); } catch (e) { p = Promise.reject(e); }
  p.then(saved, failed).then(() => {
    writing.delete(slug);
    if (writeAgain.delete(slug)) writeNow(slug);
  });
}
function saveSoon(slug) {
  unsaved.add(slug);
  clearTimeout(timers.get(slug));
  timers.set(slug, setTimeout(() => writeNow(slug), 600));
}
export function flushAll() {
  for (const slug of [...live.keys()]) commitLive(slug, false);
  for (const slug of new Set([...timers.keys(), ...writeAgain, ...unsaved])) { clearTimeout(timers.get(slug)); writeNow(slug, true); }
}

// ---- typing: what the editor holds, kept out of notesStore while the person types ----
// Each key used to put a new list in notesStore: the whole Notes window (and every desktop note) drew again and the
// desktop's widgets were published again. Now the editor keeps its text (NoteEditor) and gives it here: it goes into
// notesStore, and is saved, once typing stops for LIVE_MS (or when the editor closes, the note changes, flushAll). Until
// then the note counts as not saved (a read of the folder keeps it as it is in memory).
export const LIVE_MS = 500;
const live = new Map();                    // slug → the text typed, not yet in notesStore
const liveTimers = new Map();
export function editBody(slug, body) {
  if (!validSlug(slug)) return;
  live.set(slug, body);
  unsaved.add(slug);
  clearTimeout(liveTimers.get(slug));
  liveTimers.set(slug, setTimeout(() => commitLive(slug), LIVE_MS));
}
// the text typed into notesStore (the list's title and preview, the reading view, other views of the note) and saved
export function commitLive(slug, save = true) {
  clearTimeout(liveTimers.get(slug));
  liveTimers.delete(slug);
  if (!live.has(slug)) return;
  const body = live.get(slug);
  live.delete(slug);
  const n = notesStore.get().list.find((x) => x.slug === slug);
  if (!n) { unsaved.delete(slug); return; }
  if (n.body !== body) updateNote(slug, { body });
  if (save) { clearTimeout(timers.get(slug)); writeNow(slug); }
}
export const liveBody = (slug) => (live.has(slug) ? live.get(slug) : notesStore.get().list.find((x) => x.slug === slug)?.body);

const patchList = (fn) => notesStore.set((s) => ({ ...s, list: fn(s.list) }));

export function updateNote(slug, patch, { touch = true } = {}) {
  patchList((list) => list.map((n) => {
    if (n.slug !== slug) return n;
    const next = { ...n, ...patch };
    if (patch.body !== undefined) next.title = titleOf(patch.body) || n.title;
    if (touch) next.updated = new Date().toISOString();
    return next;
  }));
  saveSoon(slug);
}

const stamp = () => new Date().toISOString().replace(/[-:]/g, '').replace(/\..*/, '').replace('T', '-');
function newSlug() {
  const taken = new Set(notesStore.get().list.map((n) => n.slug));
  let s = `note-${stamp()}`, i = 2;
  while (taken.has(s) || exists(`${DIR}/${s}`)) s = `note-${stamp()}-${i++}`;   // a folder made elsewhere counts too
  return s;
}

export function createNote({ body = '', color = 'yellow', desktop = false, x, y, title } = {}) {
  ensureDir();
  const slug = newSlug();
  try { fs.mkdir(folder(slug)); } catch (e) { return null; }
  const now = new Date().toISOString();
  const count = notesStore.get().list.filter((n) => n.desktop).length;
  // a new note's place: inside the desktop's work area (Lord Workspaces' top bar and side rail covered the note's bar)
  let area = null;
  try { area = typeof workArea === 'function' ? workArea() : null; } catch (e) { area = null; }
  const ax = area && Number.isFinite(area.x) ? area.x : 0, ay = area && Number.isFinite(area.y) ? area.y : 0;
  const n = { slug, body, title: (title && titleOf(title)) || titleOf(body), color: COLORS.includes(color) ? color : 'yellow', x: x ?? ax + 80 + (count % 6) * 30, y: y ?? ay + 40 + (count % 6) * 30, w: 260, h: 240,
    desktop, pinned: false, created: now, updated: now };
  patchList((list) => [n, ...list]);
  writeNow(slug);
  return slug;
}

export function duplicateNote(slug) {
  commitLive(slug);
  const n = notesStore.get().list.find((x) => x.slug === slug);
  if (!n) return null;
  const copy = createNote({ body: n.body, color: n.color, desktop: n.desktop, x: n.x + 30, y: n.y + 30 });
  // attachments: copied next to it (in the background)
  try {
    for (const e of fs.list(folder(slug))) if (e.name !== 'nota.md' && !e.link && !e.dir) fs.copy(`${folder(slug)}/${e.name}`, `${folder(copy)}/${e.name}`);
  } catch (e) { /* no attachments */ }
  return copy;
}

// Delete: to the Trash if it exists; otherwise, to ~/Notes/.trash (can be recovered by hand).
export function deleteNote(slug) {
  clearTimeout(liveTimers.get(slug));
  liveTimers.delete(slug);
  live.delete(slug);
  clearTimeout(timers.get(slug));
  timers.delete(slug);
  unsaved.delete(slug);
  let ok = false;
  try { if (fs.trash) { fs.trash(folder(slug)); ok = true; } } catch (e) { ok = false; }
  if (!ok) {
    try { fs.mkdir(`${DIR}/.trash`); } catch (e) { /* already there */ }
    try { fs.rename(folder(slug), `${DIR}/.trash/${slug}`); ok = true; } catch (e) { ok = false; }
  }
  if (ok) patchList((list) => list.filter((n) => n.slug !== slug));
  return ok;
}

// ---- attachments ----

function freeName(slug, name) {
  let names;
  try { names = new Set(fs.list(folder(slug), { hidden: true }).map((e) => e.name)); } catch (e) { names = new Set(); }
  const clean = safeAttachName(name);
  const dot = clean.lastIndexOf('.');
  const base = dot > 0 ? clean.slice(0, dot) : clean, ext = dot > 0 ? clean.slice(dot) : '';
  let out = clean, i = 2;
  while (names.has(out) || out === 'nota.md') out = `${base}-${i++}${ext}`;
  return out;
}

// The markdown that shows an attachment: an image drawn in the note (PNG and JPEG, what lord-server draws) or a 📎
// link; its name is encoded so spaces and parentheses don't end the link (links.js)
export const attachmentMarkdown = (name) => (isImage(name) ? `![${linkLabel(name)}](${attachmentRef(name)})` : `[📎 ${linkLabel(name)}](${attachmentRef(name)})`);

// Copies a file from the home folder (or a mounted disk) into the note and returns the markdown that shows it. A
// folder or a link is not attached (a link would point out of the note's folder); the copy runs in the background.
export function attachFile(slug, path) {
  let st = null;
  try { st = fs.stat(path); } catch (e) { st = null; }
  if (!st || st.dir || st.link) return null;
  const name = freeName(slug, String(path).split('/').pop());
  const dst = `${folder(slug)}/${name}`;
  try { fs.copy(path, dst); } catch (e) { return null; }
  return attachmentMarkdown(name);
}

// Where a link or an image of the note points, if it may be opened or drawn: one of its own files, a real file (not
// a link or a folder), never nota.md itself. null otherwise.
export function attachmentPath(slug, url) {
  const t = linkTarget(url);
  if (t.kind !== 'file' || t.name === 'nota.md' || !validSlug(slug)) return null;
  const p = `${absFolder(slug)}/${t.name}`;
  let st = null;
  try { st = fs.stat(p); } catch (e) { st = null; }
  if (!st || st.dir || st.link) return null;
  return { path: p, name: t.name, size: st.size };
}

// an image the note draws: only its own PNG and JPEG files (no other folder, no address on the network)
export function noteImageSrc(slug, url) {
  const a = attachmentPath(slug, url);
  return a && imageKind(a.name) === 'inline' ? a.path : null;
}

// The clipboard image (copied in any app) as the note's image-N.png.
export function pasteClipboardImage(slug) {
  const data = lord.clipboard.imageData ? lord.clipboard.imageData() : null;
  if (!data) return null;
  let names;
  try { names = new Set(fs.list(folder(slug)).map((e) => e.name)); } catch (e) { names = new Set(); }
  let i = 1;
  while (names.has(`image-${i}.png`)) i++;
  const name = `image-${i}.png`;
  try { fs.write(`${folder(slug)}/${name}`, data); } catch (e) { return null; }
  return `![${name}](${name})`;
}

// ---- import ----

// A .md, .markdown or .txt file (picked with Import… or dropped on the list) as a new note: its text is the note's text,
// a Notes header in it gives only its title and colour (notesmodel.js importNote). It always gets a new folder, so an
// import never writes over a note. The file is untrusted: a link, a folder or a file over MAX_NOTE is not read, and
// nothing it names is copied (its images and links point into the new note's own, empty folder). The new slug or null.
export function importFile(path) {
  if (!importable(path)) return null;
  let st = null;
  try { st = fs.stat(path); } catch (e) { st = null; }
  if (!st || st.dir || st.link || !(st.size <= MAX_NOTE)) return null;
  let txt = null;
  try { txt = fs.read(path); } catch (e) { return null; }
  const n = importNote(txt, path);
  return n ? createNote(n) : null;
}

// ---- notes on the desktop (widgets) ----

// published only when the desktop's notes change (which, where, their size, pinned): a change of a note's text is not
// a change of the widgets (it was published again on every change before)
let lastPublished = null;
export function publishWidgets() {
  const list = notesStore.get().list.filter((n) => n.desktop).map((n) => ({
    id: n.slug, type: 'note', pinned: n.pinned,
    x: Math.max(0, Math.min(n.x, screen.width - 80)), y: Math.max(0, Math.min(n.y, screen.height - 60)), w: n.w, h: n.h,
  }));
  const sig = JSON.stringify(list);
  if (sig === lastPublished) return;
  lastPublished = sig;
  setWidgets('note', list);
}
notesStore.subscribe(publishWidgets);
// a note put on (or taken off) the desktop: its folder is watched (or no longer)
notesStore.subscribe(() => { if (notesStore.get().loaded) watchNotes(); });

export function setupWidgets(Card) {
  registerWidgetType('note', Card);
}

// Show a note on the desktop at a place (the widget catalog's pick: where the widget is being added), inside that
// display's work area; it keeps its text and size.
export function showNoteAt(slug, x, y, area) {
  const n = notesStore.get().list.find((v) => v.slug === slug);
  if (!n) return null;
  const r = area ? clampInto({ x, y, w: n.w, h: n.h }, area) : { x: Math.round(x), y: Math.round(y), w: n.w, h: n.h };
  updateNote(slug, { desktop: true, ...r }, { touch: false });
  flushAll();
  return slug;
}

// "Show on desktop" from the window: a note whose place is off every display goes to the given work area (the Notes
// window's display); one that can be seen keeps its place (notesmodel.js placeOnDesktop).
export function toggleDesktop(slug, displays, area) {
  const n = notesStore.get().list.find((v) => v.slug === slug);
  if (!n) return;
  if (n.desktop) { updateNote(slug, { desktop: false }, { touch: false }); return; }
  updateNote(slug, { desktop: true, ...placeOnDesktop(n, displays, area) }, { touch: false });
}
