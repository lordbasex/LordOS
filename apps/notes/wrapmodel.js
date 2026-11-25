// wrapmodel.js — the rows of Notes' editor (NoteEditor.jsx), pure (tests/notes_wrap_test.mjs): soft wrapping at the
// editor's width, the caret's x and the offset under a click, measured the way the row is drawn.
//
// A row is drawn as one Text of its display string (display(): a tab is four spaces, other control characters such as
// a CR from CRLF line ends are not drawn), so every width is the width of a display string measured whole by
// measure(s, bold) (lord.measureText with JetBrains Mono, regular or bold): a run of CJK, emoji or combining marks is
// shaped together, exactly as drawn, and a printable ASCII string is its length times one cell. Before, each
// character was measured alone and summed: next to CJK the spaces were drawn narrower, a tab was drawn as one box but
// measured as four spaces, headings were drawn bold but measured regular, so the caret drifted off the letters and a row
// could be wider than the editor.
//
// Each logical line is wrapped on its own and remembered (a cache keyed by width, bold and the line's text): typing
// wraps only the line being edited again.

export const TAB = '    ';
const CONTROL = /[\u0000-\u0008\u000a-\u001f\u007f-\u009f]/g;
export const display = (s) => (/[\t\u0000-\u001f\u007f-\u009f]/.test(s) ? s.replace(/\t/g, TAB).replace(CONTROL, '') : s);
const PRINTABLE_ASCII = /^[\x20-\x7e]*$/;

// A measurer over a native measure(s, bold) → px, with an ASCII fast path and a bounded cache.
export function makeMeasure(native, cell, boldCell = cell, max = 4000) {
  const cache = new Map();
  return (s, bold = false) => {
    const d = display(s);
    if (PRINTABLE_ASCII.test(d)) return d.length * (bold ? boldCell : cell);
    const k = (bold ? 'b' : 'r') + d;
    let w = cache.get(k);
    if (w === undefined) {
      w = native(d, bold);
      if (cache.size >= max) cache.clear();
      cache.set(k, w);
    }
    return w;
  };
}

const isHigh = (c) => c >= 0xd800 && c <= 0xdbff;
// the next offset that is not inside a surrogate pair or before a combining mark (a cut never splits a character)
const COMBINING = /[\u0300-\u036f\u0483-\u0489\u0591-\u05bd\u064b-\u065f\u0670\u06d6-\u06dc\u0e31\u0e34-\u0e3a\u0e47-\u0e4e\u200c\u200d\ufe0f\u20e3]/;
function nextBoundary(s, i) {
  let j = i + (isHigh(s.charCodeAt(i)) ? 2 : 1);
  while (j < s.length && (COMBINING.test(s[j]) || (s.charCodeAt(j) >= 0xdc00 && s.charCodeAt(j) <= 0xdfff))) j++;
  // an emoji modifier or a joiner sequence: kept with what it modifies
  while (j < s.length - 1 && s.charCodeAt(j) === 0xd83c && s.charCodeAt(j + 1) >= 0xdffb && s.charCodeAt(j + 1) <= 0xdfff) j += 2;
  return Math.min(j, s.length);
}

// The rows of one logical line: [[start, end], …] (offsets in the line). Greedy, at the last space that fits; a word
// longer than the width is cut between characters. Always at least one row.
export function wrapLine(line, width, measure, bold = false) {
  if (!line.length) return [[0, 0]];
  const out = [];
  let rs = 0;
  while (rs < line.length) {
    if (measure(line.slice(rs), bold) <= width) { out.push([rs, line.length]); break; }
    // the longest prefix from rs that fits: binary search over the character boundaries
    const marks = [];
    for (let i = rs; i < line.length; i = nextBoundary(line, i)) marks.push(i);
    marks.push(line.length);
    let lo = 1, hi = marks.length - 1, fit = 1;           // marks[fit] is the end; at least one character per row
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (measure(line.slice(rs, marks[mid]), bold) <= width) { fit = mid; lo = mid + 1; } else hi = mid - 1;
    }
    let end = marks[fit];
    // at a word's edge: the last space inside the row (the space stays at the row's end, as before)
    if (end < line.length && line[end] !== ' ') {
      const sp = line.lastIndexOf(' ', end - 1);
      if (sp >= rs && sp + 1 > rs) end = sp + 1;
    } else if (end < line.length && line[end] === ' ') end += 1;
    out.push([rs, end]);
    rs = end;
  }
  return out;
}

// The editor's rows for a whole text: [{ start, end, line }] in text offsets. cache: a Map the caller keeps (per
// editor), so an unchanged line is not wrapped again; isBold(lineIndex, text) says which lines are drawn bold.
export function wrapText(text, width, measure, cache, isBold = () => false) {
  const rows = [];
  let off = 0, li = 0;
  const used = new Set();
  for (const ln of text.split('\n')) {
    const bold = !!isBold(li, ln);
    const k = `${width}|${bold ? 1 : 0}|${ln}`;
    let parts = cache && cache.get(k);
    if (!parts) { parts = wrapLine(ln, width, measure, bold); if (cache) cache.set(k, parts); }
    used.add(k);
    for (const [a, b] of parts) rows.push({ start: off + a, end: off + b, line: li });
    off += ln.length + 1;
    li++;
  }
  // forget the wraps of lines that are gone (the cache holds what the text has now)
  if (cache && cache.size > used.size * 2 + 64) for (const k of [...cache.keys()]) if (!used.has(k)) cache.delete(k);
  return rows;
}

// The row an offset is on: a binary search (rows are in order). At a soft wrap the offset belongs to the next row.
export function rowAt(rows, off) {
  let lo = 0, hi = rows.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (rows[mid].start <= off) lo = mid; else hi = mid - 1;
  }
  return lo;
}

// x of an offset within its row, and the offset nearest an x in a row
export const xIn = (text, row, off, measure, bold = false) => measure(text.slice(row.start, Math.max(row.start, Math.min(off, row.end))), bold);
export function offsetAt(text, row, x, measure, bold = false) {
  const s = text.slice(row.start, row.end);
  let prev = 0;
  for (let i = 0; i < s.length;) {
    const j = nextBoundary(s, i);
    const w = measure(s.slice(0, j), bold);
    if (x < (prev + w) / 2) return row.start + i;
    prev = w;
    i = j;
  }
  // after a soft wrap the row's last offset is the next row's first: stay before it
  return row.end;
}
