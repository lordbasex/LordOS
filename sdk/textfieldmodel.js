// textfieldmodel.js — the caret of a one-line TextInput (components.jsx), no screen (issue #59: a typo in an address had
// to be typed again from the end). Positions count code points, as the field draws them.
//   fieldKey(value, pos, e, rtl, lay) → { value, pos } for a key that edits or moves, or null for one it leaves alone
//   caretAt(value, x, measure, lay)   → the position nearest x, measured from the text's start edge
// lay (optional, 0.8.8.8) is what lord.text.carets returns for the text as drawn, or just { breaks } from
// lord.text.graphemes: with it the caret only stops between grapheme clusters (see "grapheme clusters" below).

const cps = (s) => [...(s || '')];
export const clampPos = (value, pos) => Math.max(0, Math.min(cps(value).length, pos == null ? Infinity : pos));

// rtl: the field reads right to left, so the arrows move the other way through the text
export function fieldKey(value, pos, e, rtl = false, lay = null) {
  const c = cps(value);
  const at = clampPos(value, pos);
  const st = stops(value, lay);
  const key = e && e.key;
  if (key === 'ArrowLeft' || key === 'ArrowRight') {
    if (e.ctrl || e.meta || e.alt) return null;               // the window's own shortcuts
    return { value, pos: arrowStep(value, at, key === 'ArrowRight', rtl, lay, st) };
  }
  if (key === 'Home') return { value, pos: 0 };
  if (key === 'End') return { value, pos: c.length };
  if (key === 'Backspace') { const a = prevStop(st, at); return { value: [...c.slice(0, a), ...c.slice(at)].join(''), pos: a }; }
  if (key === 'Delete') { const b = nextStop(st, at); return { value: [...c.slice(0, at), ...c.slice(b)].join(''), pos: at }; }
  return null;
}

// text typed or pasted at the caret
export function insertAt(value, pos, text) {
  const c = cps(value);
  const at = clampPos(value, pos);
  const add = cps(text);
  return { value: [...c.slice(0, at), ...add, ...c.slice(at)].join(''), pos: at + add.length };
}

// measure(text) → its width; x from where the text starts (the left edge, or the right one in a right-to-left field).
// With lay only grapheme boundaries count (a click never lands inside an emoji or between a letter and its accent).
export function caretAt(value, x, measure, lay = null) {
  const c = cps(value);
  if (!(x > 0) || !c.length) return 0;
  const st = stops(value, lay);
  let prev = 0, prevAt = 0;
  for (let k = 1; k < st.length; k++) {
    const i = st[k];
    const w = measure(c.slice(0, i).join(''));
    if (w >= x) return x - prev < w - x ? prevAt : i;
    prev = w;
    prevAt = i;
  }
  return c.length;
}

// ---- grapheme clusters and mixed directions (0.8.8.8) ----
// lay is lord.text.carets(text, size, weight) for the text as drawn: { width, rtl, left[i], right[i], levels[i], breaks,
// runs } per code point, x from the text's left edge, the code points of one grapheme sharing their edges, breaks the
// positions where grapheme clusters start (and the end). With it the caret and the selection follow the shaped, reordered
// text (Hebrew or Arabic with numbers or Latin draw their runs in their own direction) and the caret never stops inside
// an emoji ZWJ sequence, a flag, a skin tone or a letter with its combining marks.
//   stops(value, lay)          → the positions the caret may take: lay.breaks, or every code point without them
//   prevStop(st, pos) / nextStop(st, pos) → the stop before / after pos (Backspace, Delete, logical arrows)
//   caretX(lay, pos)           → the caret's x for logical position pos: where the character after it starts, or where
//                                the one before it ends; between two runs, the edge of the more deeply embedded one (so
//                                each gap on screen has its own position)
//   selRects(lay, a, b)        → [{ x, w }] left to right: the selection [a, b) as one rectangle per piece on screen
//   posAtX(lay, x)             → the stop whose caret is nearest x (a click)
//   visualStep(lay, pos, right) → the stop next to pos on screen, to the right or to the left (null at that edge): ← and
//                                → move visually through right-to-left runs, as on other desktops
export function stops(value, lay) {
  const n = cps(value).length;
  const b = lay && lay.breaks;
  if (Array.isArray(b) && b.length && b[0] === 0 && b[b.length - 1] === n) return b;
  return Array.from({ length: n + 1 }, (_, i) => i);
}
export function prevStop(st, pos) {
  let r = 0;
  for (const s of st) { if (s >= pos) break; r = s; }
  return r;
}
export function nextStop(st, pos) {
  for (const s of st) if (s > pos) return s;
  return st[st.length - 1] || 0;
}
// the stop at pos, or around it when pos is inside a cluster: the one before (back) or after
const snapStop = (st, pos, back) => (st.includes(pos) ? pos : back ? prevStop(st, pos) : nextStop(st, pos));

const hasLay = (lay) => !!(lay && Array.isArray(lay.left) && Array.isArray(lay.right) && Array.isArray(lay.levels));
const layStops = (lay) => (Array.isArray(lay.breaks) ? lay.breaks : Array.from({ length: lay.left.length + 1 }, (_, i) => i));
const lead = (lay, i) => (lay.levels[i] & 1 ? lay.right[i] : lay.left[i]);
const trail = (lay, i) => (lay.levels[i] & 1 ? lay.left[i] : lay.right[i]);

export function caretX(lay, pos) {
  if (!hasLay(lay) || !lay.left.length) return 0;
  const n = lay.left.length;
  if (pos <= 0) return lead(lay, 0);
  if (pos >= n) return trail(lay, n - 1);
  return lay.levels[pos] > lay.levels[pos - 1] ? lead(lay, pos) : trail(lay, pos - 1);
}

export function selRects(lay, a, b) {
  if (!hasLay(lay) || !(b > a)) return [];
  const pieces = [];
  for (let i = Math.max(0, a); i < Math.min(b, lay.left.length); i++) pieces.push([lay.left[i], lay.right[i]]);
  pieces.sort((p, q) => p[0] - q[0]);
  const out = [];
  for (const [l, r] of pieces) {
    const last = out[out.length - 1];
    if (last && l <= last.x + last.w + 0.5) last.w = Math.max(last.w, r - last.x);
    else out.push({ x: l, w: r - l });
  }
  return out;
}

export function posAtX(lay, x) {
  if (!hasLay(lay)) return 0;
  let best = 0, bestD = Infinity;
  for (const p of layStops(lay)) {
    const d = Math.abs(caretX(lay, p) - x);
    if (d < bestD - 0.01) { best = p; bestD = d; }
  }
  return best;
}

export function visualStep(lay, pos, right) {
  if (!hasLay(lay)) return null;
  const cur = caretX(lay, pos);
  let best = null, bestX = 0;
  for (const p of layStops(lay)) {
    if (p === pos) continue;
    const x = caretX(lay, p);
    if (right ? x <= cur + 0.5 : x >= cur - 0.5) continue;
    // the nearest on screen; at the same place, the one nearest in the text
    const nearer = best == null || (right ? x < bestX - 0.01 : x > bestX + 0.01);
    if (nearer || (Math.abs(x - bestX) <= 0.01 && Math.abs(p - pos) < Math.abs(best - pos))) { best = p; bestX = x; }
  }
  return best;
}

// ← or → (right): on screen with a layout of the drawn text, else through the text (backwards on ← unless the field reads
// right to left); a grapheme cluster at a time, and at an edge the caret stays
function arrowStep(value, pos, right, rtl, lay, st) {
  if (hasLay(lay) && lay.left.length === cps(value).length) {
    const to = visualStep(lay, pos, right);
    return to == null ? pos : to;
  }
  return right !== rtl ? nextStop(st, pos) : prevStop(st, pos);
}

// ---- selection (0.8.8.8): a field's state is { value, pos, anchor }; anchor is where the selection started (null, or
// the same as pos: none). The selected text is between the two, whichever comes first.
//   selRange(s)        → [from, to) or null
//   selectedText(s)    → the selected text ('' without a selection)
//   replaceSel(s, txt) → the selection (or nothing, at the caret) replaced by txt: typing, a paste, Backspace on it
//   wordAt(value, pos, lay) → [from, to) of the word at pos (a double click), or of the run of spaces/punctuation there,
//                      widened to whole grapheme clusters
//   editKey(s, e, rtl, lay) → the state after a key that edits, moves or selects, or null for one left alone:
//     ←/→ (Shift extends, Ctrl or ⌥ by word; ⌘ is the window's), Home/End (Shift extends), Ctrl/⌘+A selects all,
//     Backspace/Delete (on the selection when there is one; Ctrl or ⌥ by word); with lay, ← and → move on screen and
//     each step, Backspace and Delete take a whole grapheme cluster
export const selRange = (s) => (s && s.anchor != null && s.anchor !== s.pos ? [Math.min(s.anchor, s.pos), Math.max(s.anchor, s.pos)] : null);
export const selectedText = (s) => { const r = selRange(s); return r ? cps(s.value).slice(r[0], r[1]).join('') : ''; };

export function replaceSel(s, text) {
  const c = cps(s.value);
  const r = selRange(s);
  const at = r ? r[0] : clampPos(s.value, s.pos), end = r ? r[1] : at;
  const add = cps(text);
  return { value: [...c.slice(0, at), ...add, ...c.slice(end)].join(''), pos: at + add.length, anchor: null };
}

// letters, digits and _ make a word (any script); spaces and punctuation are runs of their own
const kind = (ch) => (ch == null ? -1 : /[\p{L}\p{N}\p{M}_]/u.test(ch) ? 1 : /\s/u.test(ch) ? 0 : 2);
export function wordAt(value, pos, lay = null) {
  const c = cps(value);
  if (!c.length) return [0, 0];
  const at = clampPos(value, pos);
  let i = Math.min(at, c.length - 1);
  if (i > 0 && (at === c.length || kind(c[i]) === 0) && kind(c[i - 1]) === 1) i--;     // just after a word, before a space or the end: that word
  const k = kind(c[i]);
  let a = i, b = i + 1;
  while (a > 0 && kind(c[a - 1]) === k) a--;
  while (b < c.length && kind(c[b]) === k) b++;
  const st = stops(value, lay);
  return [snapStop(st, a, true), snapStop(st, b, false)];
}

// the next word boundary from pos, backwards or forwards (as Ctrl+← and Ctrl+→ move)
function wordStep(c, pos, back) {
  let i = pos;
  if (back) {
    while (i > 0 && kind(c[i - 1]) !== 1) i--;
    while (i > 0 && kind(c[i - 1]) === 1) i--;
  } else {
    while (i < c.length && kind(c[i]) !== 1) i++;
    while (i < c.length && kind(c[i]) === 1) i++;
  }
  return i;
}

export function editKey(s, e, rtl = false, lay = null) {
  const value = s.value || '';
  const c = cps(value);
  const pos = clampPos(value, s.pos);
  const r = selRange(s);
  const st = stops(value, lay);
  const key = e && e.key;
  const mod = e && (e.ctrl || e.meta);
  if (mod && !e.alt && (e.base === 'a' || key === 'a' || key === 'A')) return { value, pos: c.length, anchor: 0 };
  const go = (to) => (e.shift ? { value, pos: to, anchor: s.anchor != null ? s.anchor : pos } : { value, pos: to, anchor: null });
  if (key === 'ArrowLeft' || key === 'ArrowRight') {
    if (e.meta) return null;                                     // the window's own shortcuts
    const back = (key === 'ArrowLeft') !== rtl;
    if (r && !e.shift && !e.ctrl && !e.alt) return { value, pos: back ? r[0] : r[1], anchor: null };
    if (e.ctrl || e.alt) return go(snapStop(st, wordStep(c, pos, back), back));
    return go(arrowStep(value, pos, key === 'ArrowRight', rtl, lay, st));
  }
  if (key === 'Home') return go(0);
  if (key === 'End') return go(c.length);
  if (key === 'Backspace' || key === 'Delete') {
    if (r) return replaceSel(s, '');
    const back = key === 'Backspace';
    const to = e.ctrl || e.alt ? snapStop(st, wordStep(c, pos, back), back) : back ? prevStop(st, pos) : nextStop(st, pos);
    const a = Math.min(pos, to), b = Math.max(pos, to);
    return { value: [...c.slice(0, a), ...c.slice(b)].join(''), pos: a, anchor: null };
  }
  return null;
}

// ---- undo and redo (0.8.8.8): a history of { value, pos, anchor } states, the one before each step.
//   newHistory(secure)               → an empty history; a secure one (a password field) never records anything, so an
//                                      old password can't come back with Undo or Redo
//   historyFor(h, secure)            → h, or a new empty history when the field's secure prop changed (the old steps go)
//   record(h, before, after, step, now) → h after an edit (or a move) from before to after; step 'type' (a character) and
//                                      'delete' (Backspace or Delete of one) join the step in progress while they go on
//                                      from where it left the caret, within a second, and typing doesn't start a new
//                                      word; anything else ('paste', 'cut', 'clear', 'external', …) is a step of its
//                                      own, and a caret that only moved ('move') ends the step in progress
//   undo(h, cur) / redo(h, cur)      → { h, s }: the history and the state to show, or null with nothing to undo or redo
//   historyKey(e)                    → 'undo' (Ctrl/⌘+Z), 'redo' (Ctrl/⌘+Shift+Z, Ctrl+Y) or null
//   lineAt(value, pos)               → [from, to) of the line at pos, without its line break (a triple click)
const STEPS = 200;                    // the oldest steps are forgotten past this many
const PAUSE = 1000;                   // ms without typing that end a step
const snap = (s) => ({ value: s.value || '', pos: clampPos(s.value, s.pos), anchor: s.anchor == null ? null : s.anchor });
export const newHistory = (secure = false) => ({ done: [], undone: [], last: null, secure: !!secure });
export const historyFor = (h, secure) => (h && !!h.secure === !!secure ? h : newHistory(secure));
export const canUndo = (h) => !!h && !h.secure && h.done.length > 0;
export const canRedo = (h) => !!h && !h.secure && h.undone.length > 0;

export function record(h, before, after, step, now = 0) {
  if (h.secure) return h;
  if ((before.value || '') === (after.value || '')) return h.last ? { ...h, last: null } : h;
  const l = h.last;
  const c = cps(after.value);
  const startsWord = step === 'type' && kind(c[after.pos - 1]) === 1 && kind(c[after.pos - 2]) !== 1;
  const joins = l && l.step === step && (step === 'type' || step === 'delete') && !selRange(before)
    && now - l.time < PAUSE && clampPos(before.value, before.pos) === l.pos && !startsWord;
  const last = { step, time: now, pos: clampPos(after.value, after.pos) };
  if (joins) return { done: h.done, undone: [], last, secure: false };
  return { done: [...h.done, snap(before)].slice(-STEPS), undone: [], last, secure: false };
}

export function undo(h, cur) {
  if (!canUndo(h)) return null;
  return { h: { done: h.done.slice(0, -1), undone: [...h.undone, snap(cur)], last: null, secure: false }, s: h.done[h.done.length - 1] };
}
export function redo(h, cur) {
  if (!canRedo(h)) return null;
  return { h: { done: [...h.done, snap(cur)], undone: h.undone.slice(0, -1), last: null, secure: false }, s: h.undone[h.undone.length - 1] };
}

export function historyKey(e) {
  if (!e || !(e.ctrl || e.meta) || e.alt) return null;
  const b = (e.base || e.key || '').toLowerCase();             // with Shift the key comes as 'Z'
  if (b === 'z') return e.shift ? 'redo' : 'undo';
  if (b === 'y' && e.ctrl && !e.shift) return 'redo';
  return null;
}

export function lineAt(value, pos) {
  const c = cps(value);
  const at = clampPos(value, pos);
  let a = at, b = at;
  while (a > 0 && c[a - 1] !== '\n') a--;
  while (b < c.length && c[b] !== '\n') b++;
  return [a, b];
}
