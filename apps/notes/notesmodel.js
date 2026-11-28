// notesmodel.js — Notes' pure logic (no SDK, no files; tests/notes_model_test.mjs): a note's header and title, the
// merge of a fresh read of ~/Notes with the notes in memory, the list of the widget catalog's "pick a note" sheet, where
// a note goes on the desktop, and what an imported .md/.txt file becomes.

export const COLORS = ['yellow', 'pink', 'green', 'blue', 'purple', 'gray'];
export const MAX_NOTE = 8 * 1024 * 1024;   // a nota.md (or an imported file) larger than this is not a note
export const MAX_NOTES = 4000;              // notes read from ~/Notes at most (each one is read into memory)

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

// The header's fields, in the order they are written
// placed: the note's x and y are a place on the desktop (it has been there), not the default a new note gets
export const HEADER_FIELDS = ['title', 'color', 'x', 'y', 'w', 'h', 'desktop', 'placed', 'pinned', 'created', 'updated'];

// A header value: a string that would read back as something else (a colon, #, quotes, a line break, spaces at an end,
// empty, true/false, a number) goes in quotes; a line break becomes a space
const yaml = (v) => (typeof v === 'string' && (/[:#"'\n]|^\s|\s$/.test(v) || v === '' || v === 'true' || v === 'false' || /^-?\d/.test(v))
  ? `"${v.replace(/"/g, '\\"').replace(/\n/g, ' ')}"` : String(v));

// A note as its nota.md: the header, then the text
export function serialize(n) {
  const head = HEADER_FIELDS.filter((k) => n[k] !== undefined && n[k] !== null).map((k) => `${k}: ${yaml(n[k])}`).join('\n');
  return `---\n${head}\n---\n${n.body}`;
}

// Title: the first line with text, without the markdown marks. Control characters and direction overrides are left
// out (a title is shown in the list and written in the note's header, GitHub #238). Bounded work (security scan
// 0.8.9.6.2, F7: an imported file whose first line was 200 KB of "![" made the image pattern quadratic on lord-server's
// JS thread): the line is cut to TITLE_SCAN code points before any pattern, and the image pattern itself is bounded.
const TITLE_SCAN = 512;
export function titleOf(body) {
  const first = String(body).split('\n', 4096).find((l) => l.trim() && !/^(`{3,}|---)/.test(l.trim())) || '';
  const line = [...first.slice(0, TITLE_SCAN * 2)].slice(0, TITLE_SCAN).join('');
  return line.replace(/^\s*(#{1,6}\s+|>\s*|[-*+]\s+(\[[ xX]\]\s+)?|\d+[.)]\s+)/, '').replace(/[*_`~]|!\[[^\]\n]{0,256}\]\([^)\n]{0,1024}\)/g, '')
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

// A note in memory from its nota.md: what loadNotes keeps of it. mtime is the file's (for a date the header lacks).
export function noteFrom(slug, text, mtime) {
  const { meta, body } = parseNote(text);
  const when = (v) => (typeof v === 'string' && !Number.isNaN(Date.parse(v)) ? v : new Date(Number(mtime) || 0).toISOString());
  return {
    slug, body, title: typeof meta.title === 'string' && meta.title ? titleOf(meta.title) : titleOf(body), color: COLORS.includes(meta.color) ? meta.color : 'yellow',
    ...geometryOf(meta), desktop: meta.desktop === true, placed: meta.placed === true || meta.desktop === true, pinned: meta.pinned === true,
    created: when(meta.created), updated: when(meta.updated),
  };
}

const FIELDS = ['body', 'title', 'color', 'x', 'y', 'w', 'h', 'desktop', 'placed', 'pinned', 'created', 'updated'];
export const sameNote = (a, b) => !!a && !!b && FIELDS.every((k) => a[k] === b[k]);
export const newestFirst = (a, b) => String(b.updated).localeCompare(String(a.updated));

// A fresh read of ~/Notes (disk) merged with the notes in memory (mem). A note with a change not yet saved (dirty: its
// slugs) keeps what is in memory, even when its folder is gone (the save comes in a moment; the next read drops it if
// the folder is still gone). Any other note is what the disk says: an edit made elsewhere comes in, a folder deleted
// elsewhere goes. A note that did not change keeps its object (nothing redraws). The notes already listed keep their
// order; new ones go first, newest first. changed: false when the list is the same.
export function mergeScan(mem, disk, dirty = new Set()) {
  const onDisk = new Map(disk.map((n) => [n.slug, n]));
  const known = new Set(mem.map((n) => n.slug));
  let changed = false;
  const kept = [];
  for (const n of mem) {
    const d = onDisk.get(n.slug);
    if (dirty.has(n.slug)) { kept.push(n); continue; }
    if (!d) { changed = true; continue; }
    if (sameNote(n, d)) kept.push(n);
    else { kept.push(d); changed = true; }
  }
  const fresh = disk.filter((n) => !known.has(n.slug)).sort(newestFirst);
  if (fresh.length) changed = true;
  return { list: changed ? [...fresh, ...kept] : mem, changed };
}

// lower case without accents, so "cafe" finds "Café"
export const fold = (s) => String(s || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();

// The widget catalog's "pick a note" list: every word of the search in the title or the text, most recently updated
// first, at most `max` of them; each with whether it is already on the desktop.
export function pickList(list, query = '', max = 200) {
  const words = fold(query).trim().split(/\s+/).filter(Boolean);
  return list.filter((n) => { if (!words.length) return true; const hay = fold(`${n.title}\n${n.body}`); return words.every((w) => hay.includes(w)); })
    .slice().sort(newestFirst).slice(0, max)
    .map((n) => ({ slug: n.slug, title: n.title, color: n.color, updated: n.updated, desktop: !!n.desktop }));
}

// Whether a note at r = { x, y, w, h } can be seen and moved on one of the displays: its header bar (the top 26 px,
// at least 60 px of it across) on one of them.
export function visibleOn(r, displays) {
  return (displays || []).some((d) => {
    const w = Math.min(r.x + r.w, d.x + d.w) - Math.max(r.x, d.x), h = Math.min(r.y + 26, d.y + d.h) - Math.max(r.y, d.y);
    return w >= 60 && h >= 12;
  });
}

// A note's rect inside an area (a display's work area): moved in, never resized below what fits.
export function clampInto(r, area) {
  const w = Math.min(r.w, Math.max(160, area.w)), h = Math.min(r.h, Math.max(110, area.h));
  return { x: Math.round(Math.max(area.x, Math.min(area.x + area.w - w, r.x))), y: Math.round(Math.max(area.y, Math.min(area.y + area.h - h, r.y))), w, h };
}

// "Show on desktop": a note that has been on the desktop (placed) and whose place is still on a display keeps it; a new
// note (its x and y only a default: the ws170 QA found one put behind the window in front, on another display) or one
// off every display (a monitor gone, a place from another computer) goes to the work area given (the Notes window's
// display), 80 px in and 40 down, 30 px further for each desktop note already there (others: their rects).
export function placeOnDesktop(r, displays, area, others = []) {
  if (r.placed !== false && visibleOn(r, displays)) return { x: r.x, y: r.y, w: r.w, h: r.h };
  let x = area.x + 80, y = area.y + 40;
  for (let i = 0; i < 12 && (others || []).some((o) => Math.abs(o.x - x) < 12 && Math.abs(o.y - y) < 12); i++) { x += 30; y += 30; }
  return clampInto({ ...r, x, y }, area);
}

// ---- import (.md, .markdown, .txt) ----

export const IMPORT_EXT = /\.(md|markdown|txt)$/i;
export const importable = (name) => IMPORT_EXT.test(String(name || ''));

// What an imported file becomes: { body, title, color } or null (not text). The file is untrusted text: a Notes header
// gives only its title and colour (never a place, the desktop, a slug or dates); a title is cleaned like any title.
// Without a title in it, the file's name (without its extension) is the title when the text has no first line.
export function importNote(text, fileName = '') {
  if (typeof text !== 'string' || text.length > MAX_NOTE || text.includes('\u0000')) return null;
  const clean = text.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n');
  // only a Notes header is read as one: another program's front matter (other keys) stays in the text, whole
  let { meta, body } = parseNote(clean);
  if (Object.keys(meta).some((k) => !HEADER_FIELDS.includes(k))) { meta = {}; body = clean; }
  const fromName = titleOf(String(fileName).split('/').pop().replace(IMPORT_EXT, ''));
  const title = (typeof meta.title === 'string' && titleOf(meta.title)) || titleOf(body) || fromName;
  return { body, title, color: COLORS.includes(meta.color) ? meta.color : 'yellow' };
}
