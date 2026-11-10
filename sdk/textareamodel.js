// textareamodel.js — the rows and the caret of a TextArea (components.jsx), no screen. Positions count code points, as
// in textfieldmodel.js; a row is [start, end) of the value: a line, or a piece of a long one wrapped at the width.
//   wrapRows(value, width, measure) → [{ start, end }]: each line cut where it no longer fits, after a space if it has one
//   rowOf(rows, pos)                → the row the caret is drawn in (at a wrap, the start of the next row)
//   areaKey(value, pos, e, rows, measure, lay) → { value, pos } for a key that edits or moves, or null
//   posAt(value, rows, x, row, measure, lay)   → the position nearest x in that row (a click)
//   areaEdit(s, e, rows, measure, lay)         → as areaKey on a { value, pos, anchor } state: Shift extends a selection
// lay (optional) is { breaks } from lord.text.graphemes(value): the caret then stops only between grapheme clusters
// (textfieldmodel.js), also when ↑ and ↓ land in a row.
import { fieldKey, insertAt, caretAt, clampPos, editKey, replaceSel, stops } from './textfieldmodel.js';

const cps = (s) => [...(s || '')];

export function wrapRows(value, width, measure) {
  const c = cps(value);
  const rows = [];
  let start = 0;
  for (let i = 0; i <= c.length; i++) {
    if (i < c.length && c[i] !== '\n') continue;
    wrapLine(c, start, i, width, measure, rows);
    start = i + 1;
  }
  return rows;
}

function wrapLine(c, from, to, width, measure, rows) {
  let s = from;
  while (true) {
    if (!(width > 0) || measure(c.slice(s, to).join('')) <= width) { rows.push({ start: s, end: to }); return; }
    // the longest piece that fits, at least one character
    let e = s + 1;
    while (e < to && measure(c.slice(s, e + 1).join('')) <= width) e++;
    // cut after the last space in it, when there is one past its start
    let cut = e;
    for (let k = e; k > s + 1; k--) if (c[k - 1] === ' ') { cut = k; break; }
    rows.push({ start: s, end: cut });
    s = cut;
  }
}

// the last row that starts at or before pos: after a wrap the next row starts where the last one ended, after a line
// break one later
export function rowOf(rows, pos) {
  let i = 0;
  while (i + 1 < rows.length && rows[i + 1].start <= pos) i++;
  return i;
}

const rowText = (c, r) => c.slice(r.start, r.end).join('');

// the stops of the whole value that fall in the row, counted from its start
const rowLay = (value, r, lay) => (lay ? { breaks: stops(value, lay).filter((p) => p >= r.start && p <= r.end).map((p) => p - r.start) } : null);

export function posAt(value, rows, x, row, measure, lay = null) {
  const r = rows[Math.max(0, Math.min(rows.length - 1, row))];
  if (!r) return 0;
  const text = rowText(cps(value), r), rl = rowLay(value, r, lay);
  // a row cut inside a cluster (a long word wrapped at the width) gets its own ends as stops too
  if (rl && (rl.breaks[0] !== 0 || rl.breaks[rl.breaks.length - 1] !== r.end - r.start))
    rl.breaks = [...new Set([0, ...rl.breaks, r.end - r.start])].sort((a, b) => a - b);
  return r.start + caretAt(text, x, measure, rl);
}

export function areaKey(value, pos, e, rows, measure, lay = null) {
  const key = e && e.key;
  const at = clampPos(value, pos);
  if (key === 'Enter') return e.ctrl || e.meta || e.alt ? null : insertAt(value, at, '\n');
  if (key === 'ArrowUp' || key === 'ArrowDown' || key === 'Home' || key === 'End') {
    if (e.ctrl || e.meta || e.alt) return null;
    const c = cps(value);
    const i = rowOf(rows, at), r = rows[i];
    if (!r) return { value, pos: at };
    if (key === 'Home') return { value, pos: r.start };
    if (key === 'End') return { value, pos: r.end };
    const j = i + (key === 'ArrowUp' ? -1 : 1);
    if (j < 0) return { value, pos: 0 };
    if (j >= rows.length) return { value, pos: c.length };
    return { value, pos: posAt(value, rows, measure(c.slice(r.start, at).join('')), j, measure, lay) };
  }
  return fieldKey(value, at, e, false, lay);
}

// with a selection (textfieldmodel.js): ↑ ↓ Home End move by rows here, Shift extends; Enter replaces the selection with
// a line break; the rest as a one-line field (Ctrl/⌘+A, ← →, Backspace, Delete)
export function areaEdit(s, e, rows, measure, lay = null) {
  const key = e && e.key;
  const value = s.value || '';
  const at = clampPos(value, s.pos);
  if (key === 'Enter') return e.ctrl || e.meta || e.alt ? null : replaceSel({ ...s, value }, '\n');
  if (key === 'ArrowUp' || key === 'ArrowDown' || key === 'Home' || key === 'End') {
    if (e.ctrl || e.meta || e.alt) return key === 'Home' || key === 'End' ? editKey({ ...s, value }, e, false, lay) : null;
    const moved = areaKey(value, at, { key }, rows, measure, lay);
    if (!moved) return null;
    return e.shift ? { value, pos: moved.pos, anchor: s.anchor != null ? s.anchor : at } : { value, pos: moved.pos, anchor: null };
  }
  return editKey({ ...s, value }, e, false, lay);
}
