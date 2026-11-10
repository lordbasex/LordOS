// textfieldmodel.js — the caret of a one-line TextInput (components.jsx), no screen (issue #59: a typo in an address had
// to be typed again from the end). Positions count code points, as the field draws them.
//   fieldKey(value, pos, e, rtl) → { value, pos } for a key that edits or moves, or null for one it leaves alone
//   caretAt(value, x, measure)   → the position nearest x, measured from the text's start edge

const cps = (s) => [...(s || '')];
export const clampPos = (value, pos) => Math.max(0, Math.min(cps(value).length, pos == null ? Infinity : pos));

// rtl: the field reads right to left, so the arrows move the other way through the text
export function fieldKey(value, pos, e, rtl = false) {
  const c = cps(value);
  const at = clampPos(value, pos);
  const key = e && e.key;
  if (key === 'ArrowLeft' || key === 'ArrowRight') {
    if (e.ctrl || e.meta || e.alt) return null;               // the window's own shortcuts
    const back = (key === 'ArrowLeft') !== rtl;
    return { value, pos: Math.max(0, Math.min(c.length, at + (back ? -1 : 1))) };
  }
  if (key === 'Home') return { value, pos: 0 };
  if (key === 'End') return { value, pos: c.length };
  if (key === 'Backspace') return at > 0 ? { value: [...c.slice(0, at - 1), ...c.slice(at)].join(''), pos: at - 1 } : { value, pos: at };
  if (key === 'Delete') return at < c.length ? { value: [...c.slice(0, at), ...c.slice(at + 1)].join(''), pos: at } : { value, pos: at };
  return null;
}

// text typed or pasted at the caret
export function insertAt(value, pos, text) {
  const c = cps(value);
  const at = clampPos(value, pos);
  const add = cps(text);
  return { value: [...c.slice(0, at), ...add, ...c.slice(at)].join(''), pos: at + add.length };
}

// measure(text) → its width; x from where the text starts (the left edge, or the right one in a right-to-left field)
export function caretAt(value, x, measure) {
  const c = cps(value);
  if (!(x > 0) || !c.length) return 0;
  let prev = 0;
  for (let i = 1; i <= c.length; i++) {
    const w = measure(c.slice(0, i).join(''));
    if (w >= x) return x - prev < w - x ? i - 1 : i;
    prev = w;
  }
  return c.length;
}

// ---- selection (0.8.8.8): a field's state is { value, pos, anchor }; anchor is where the selection started (null, or
// the same as pos: none). The selected text is between the two, whichever comes first.
//   selRange(s)        → [from, to) or null
//   selectedText(s)    → the selected text ('' without a selection)
//   replaceSel(s, txt) → the selection (or nothing, at the caret) replaced by txt: typing, a paste, Backspace on it
//   wordAt(value, pos) → [from, to) of the word at pos (a double click), or of the run of spaces/punctuation there
//   editKey(s, e, rtl) → the state after a key that edits, moves or selects, or null for one left alone:
//     ←/→ (Shift extends, Ctrl or ⌥ by word; ⌘ is the window's), Home/End (Shift extends), Ctrl/⌘+A selects all,
//     Backspace/Delete (on the selection when there is one; Ctrl or ⌥ by word)
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
export function wordAt(value, pos) {
  const c = cps(value);
  if (!c.length) return [0, 0];
  const at = clampPos(value, pos);
  let i = Math.min(at, c.length - 1);
  if (i > 0 && (at === c.length || kind(c[i]) === 0) && kind(c[i - 1]) === 1) i--;     // just after a word, before a space or the end: that word
  const k = kind(c[i]);
  let a = i, b = i + 1;
  while (a > 0 && kind(c[a - 1]) === k) a--;
  while (b < c.length && kind(c[b]) === k) b++;
  return [a, b];
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

export function editKey(s, e, rtl = false) {
  const value = s.value || '';
  const c = cps(value);
  const pos = clampPos(value, s.pos);
  const r = selRange(s);
  const key = e && e.key;
  const mod = e && (e.ctrl || e.meta);
  if (mod && !e.alt && (e.base === 'a' || key === 'a' || key === 'A')) return { value, pos: c.length, anchor: 0 };
  const go = (to) => (e.shift ? { value, pos: to, anchor: s.anchor != null ? s.anchor : pos } : { value, pos: to, anchor: null });
  if (key === 'ArrowLeft' || key === 'ArrowRight') {
    if (e.meta) return null;                                     // the window's own shortcuts
    const back = (key === 'ArrowLeft') !== rtl;
    if (r && !e.shift && !e.ctrl && !e.alt) return { value, pos: back ? r[0] : r[1], anchor: null };
    if (e.ctrl || e.alt) return go(wordStep(c, pos, back));
    return go(Math.max(0, Math.min(c.length, pos + (back ? -1 : 1))));
  }
  if (key === 'Home') return go(0);
  if (key === 'End') return go(c.length);
  if (key === 'Backspace' || key === 'Delete') {
    if (r) return replaceSel(s, '');
    const back = key === 'Backspace';
    const to = e.ctrl || e.alt ? wordStep(c, pos, back) : Math.max(0, Math.min(c.length, pos + (back ? -1 : 1)));
    const a = Math.min(pos, to), b = Math.max(pos, to);
    return { value: [...c.slice(0, a), ...c.slice(b)].join(''), pos: a, anchor: null };
  }
  return null;
}
