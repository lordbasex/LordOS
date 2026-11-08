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
