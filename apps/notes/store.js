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

export { isImage };

export const DIR = 'Notes';
const OLD_DIR = 'Notas'; // before 0.7.0: moved to ~/Notes on first run (see ensureDir)
export const COLORS = ['yellow', 'pink', 'green', 'blue', 'purple', 'gray'];
export const notesStore = createStore({ loaded: false, list: [] });

const FIELDS = ['title', 'color', 'x', 'y', 'w', 'h', 'desktop', 'pinned', 'created', 'updated'];

export function parseNote(text) {
  const meta = {};
  let body = text;
  const m = /^---\n([\s\S]*?)\n---\n?/.exec(text.replace(/\r\n/g, '\n'));
  if (m) {
    body = text.replace(/\r\n/g, '\n').slice(m[0].length);
    for (const line of m[1].split('\n')) {
      const kv = /^(\w+):\s*(.*)$/.exec(line);
      if (!kv) continue;
      let v = kv[2].trim();
      if (/^".*"$/.test(v)) v = v.slice(1, -1).replace(/\\"/g, '"');
      else if (v === 'true' || v === 'false') v = v === 'true';
      else if (/^-?\d+(\.\d+)?$/.test(v)) v = Number(v);
      meta[kv[1]] = v;
    }
  }
  return { meta, body };
}

const yaml = (v) => (typeof v === 'string' && (/[:#"'\n]|^\s|\s$/.test(v) || v === '' || v === 'true' || v === 'false' || /^-?\d/.test(v))
  ? `"${v.replace(/"/g, '\\"').replace(/\n/g, ' ')}"` : String(v));

export function serialize(n) {
  const head = FIELDS.filter((k) => n[k] !== undefined && n[k] !== null).map((k) => `${k}: ${yaml(n[k])}`).join('\n');
  return `---\n${head}\n---\n${n.body}`;
}

// Title: the first line with text, without the markdown marks. Control characters and direction overrides are left
// out (a title is shown in the list and written in the note's header, GitHub #238).
export function titleOf(body) {
  const line = String(body).split('\n').find((l) => l.trim() && !/^(`{3,}|---)/.test(l.trim())) || '';
  return line.replace(/^\s*(#{1,6}\s+|>\s*|[-*+]\s+(\[[ xX]\]\s+)?|\d+[.)]\s+)/, '').replace(/[*_`~]|!\[[^\]]*\]\([^)]*\)/g, '')
    .replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, ' ').replace(/[\u202a-\u202e\u2066-\u2069]/g, '').trim().slice(0, 60);
}

// A note's place and size from its header, inside sane bounds (a header edited by hand may say anything)
const num = (v, def, lo, hi) => (typeof v === 'number' && Number.isFinite(v) ? Math.max(lo, Math.min(hi, Math.round(v))) : def);
export function geometryOf(meta) {
  return { x: num(meta.x, 80, -2000, 20000), y: num(meta.y, 80, 0, 20000), w: num(meta.w, 260, 160, 4000), h: num(meta.h, 240, 110, 4000) };
}

// a note's folder name: one plain name (what newSlug makes, or a folder made by hand), never a way out of ~/Notes
export const validSlug = (slug) => typeof slug === 'string' && !!slug && slug.length <= 200 && slug !== '.' && slug !== '..'
  && !slug.startsWith('.') && !/[/\\\u0000-\u001f\u007f]/.test(slug);
const MAX_NOTE = 8 * 1024 * 1024;       // a nota.md larger than this is not a note (it would be read into memory)
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

export function loadNotes() {
  ensureDir();
  const list = [];
  let dirs = [];
  try { dirs = fs.list(DIR).filter((e) => e.dir); } catch (e) { dirs = []; }
  for (const d of dirs) {
    // a note is a real folder with a real nota.md: a link (to a folder or a file elsewhere) is not followed, so a note
    // never reads or writes outside ~/Notes (GitHub #238)
    if (d.link || !validSlug(d.name)) continue;
    let txt = null;
    try {
      const st = fs.stat(`${DIR}/${d.name}/nota.md`);
      if (!st || st.link || st.dir || st.size > MAX_NOTE) continue;
      txt = fs.read(`${DIR}/${d.name}/nota.md`);
    } catch (e) { continue; }
    const { meta, body } = parseNote(txt);
    const when = (v) => (typeof v === 'string' && !Number.isNaN(Date.parse(v)) ? v : new Date(d.mtime).toISOString());
    list.push({
      slug: d.name, body, title: typeof meta.title === 'string' && meta.title ? titleOf(meta.title) : titleOf(body), color: COLORS.includes(meta.color) ? meta.color : 'yellow',
      ...geometryOf(meta), desktop: meta.desktop === true, pinned: meta.pinned === true,
      created: when(meta.created), updated: when(meta.updated),
    });
  }
  list.sort((a, b) => String(b.updated).localeCompare(String(a.updated)));
  notesStore.set({ loaded: true, list });
}

// ---- saving ----

const timers = new Map();
function writeNow(slug) {
  timers.delete(slug);
  const n = notesStore.get().list.find((x) => x.slug === slug);
  if (!n) return;
  try { fs.write(`${folder(slug)}/nota.md`, serialize(n)); } catch (e) { console.log(`notes: could not save ${slug}: ${e.message}`); }
}
function saveSoon(slug) {
  clearTimeout(timers.get(slug));
  timers.set(slug, setTimeout(() => writeNow(slug), 600));
}
export function flushAll() { for (const slug of [...timers.keys()]) { clearTimeout(timers.get(slug)); writeNow(slug); } }

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
  while (taken.has(s)) s = `note-${stamp()}-${i++}`;
  return s;
}

export function createNote({ body = '', color = 'yellow', desktop = false, x, y } = {}) {
  ensureDir();
  const slug = newSlug();
  try { fs.mkdir(folder(slug)); } catch (e) { return null; }
  const now = new Date().toISOString();
  const count = notesStore.get().list.filter((n) => n.desktop).length;
  // a new note's place: inside the desktop's work area (Lord Workspaces' top bar and side rail covered the note's bar)
  let area = null;
  try { area = typeof workArea === 'function' ? workArea() : null; } catch (e) { area = null; }
  const ax = area && Number.isFinite(area.x) ? area.x : 0, ay = area && Number.isFinite(area.y) ? area.y : 0;
  const n = { slug, body, title: titleOf(body), color, x: x ?? ax + 80 + (count % 6) * 30, y: y ?? ay + 40 + (count % 6) * 30, w: 260, h: 240,
    desktop, pinned: false, created: now, updated: now };
  patchList((list) => [n, ...list]);
  writeNow(slug);
  return slug;
}

export function duplicateNote(slug) {
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
  clearTimeout(timers.get(slug));
  timers.delete(slug);
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

// ---- notes on the desktop (widgets) ----

export function publishWidgets() {
  const list = notesStore.get().list.filter((n) => n.desktop);
  setWidgets('note', list.map((n) => ({
    id: n.slug, type: 'note', pinned: n.pinned,
    x: Math.max(0, Math.min(n.x, screen.width - 80)), y: Math.max(0, Math.min(n.y, screen.height - 60)), w: n.w, h: n.h,
  })));
}
notesStore.subscribe(publishWidgets);

export function setupWidgets(Card) {
  registerWidgetType('note', Card);
}
