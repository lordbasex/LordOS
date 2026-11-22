// model.js: Lord Celeste's geometry and its launcher's calculator, as plain functions with no SDK (tested by
// server/tests/celeste_model_test.mjs). Coordinates are global (several displays side by side). In a right-to-left
// language the frame is mirrored: the bar goes on the right, and the drawers of the "end" edge open on the left.
export const FRAME = 10;    // the frame's thickness on the three sides without the bar
export const BAR_W = 56;    // the bar's width (it is the frame on its side)
export const R = 20;        // the frame's inner rounding
export const F = 18;        // the rounding where a drawer joins the frame
export const GAP = 8;       // between the frame and the windows

// The bar and the free area inside the frame of display d (null when d gets no frame).
export function frameGeom(d, rtl) {
  if (!d) return null;
  const h = d.h - FRAME * 2;
  return {
    bar: { x: rtl ? d.x + d.w - BAR_W : d.x, y: d.y + FRAME, w: BAR_W, h },
    inner: { x: rtl ? d.x + FRAME : d.x + BAR_W, y: d.y + FRAME, w: d.w - BAR_W - FRAME, h },
  };
}

// The area for windows on display d: inside the frame with a gap, or the whole display without one.
export function workArea(d, framed, rtl) {
  if (!framed) return { x: d.x, y: d.y, w: d.w, h: d.h };
  const { inner } = frameGeom(d, rtl);
  return { x: inner.x + GAP, y: inner.y + GAP, w: Math.max(320, inner.w - GAP * 2), h: Math.max(240, inner.h - GAP * 2) };
}

// Where each drawer sits inside `inner`: { x, y, w, h, edge } with edge 'top' | 'bottom' | 'end'. h: the launcher's
// height (it grows with its results). The sizes never pass the inner area.
export const DRAWER_SIZE = { dash: [640, 300], launcher: [480, 420], session: [98, 332], sidebar: [360, 0] };
export function drawerRect(name, inner, rtl, h) {
  const size = DRAWER_SIZE[name];
  if (!size || !inner) return null;
  const w = Math.min(size[0], inner.w - 2 * F);
  const endX = rtl ? inner.x : inner.x + inner.w - w;
  const midX = Math.round(inner.x + (inner.w - w) / 2);
  if (name === 'dash') return { x: midX, y: inner.y, w, h: Math.min(size[1], inner.h), edge: 'top' };
  if (name === 'launcher') { const hh = Math.min(h || size[1], inner.h); return { x: midX, y: inner.y + inner.h - hh, w, h: hh, edge: 'bottom' }; }
  if (name === 'session') { const hh = Math.min(size[1], inner.h - 2 * F); return { x: endX, y: Math.round(inner.y + (inner.h - hh) / 2), w, h: hh, edge: 'end' }; }
  return { x: endX, y: inner.y + 12, w, h: Math.max(0, inner.h - 24), edge: 'end' };
}

// The drawer's slide: the translation that hides it behind its edge (it moves by this much and back to 0).
export function hiddenOffset(rect, rtl) {
  if (!rect) return { x: 0, y: 0 };
  if (rect.edge === 'top') return { x: 0, y: -(rect.h + F) };
  if (rect.edge === 'bottom') return { x: 0, y: rect.h + F };
  return { x: (rtl ? -1 : 1) * (rect.w + F), y: 0 };
}

// The hover edges: the pointer pushed against an edge of display d, near the middle of the top (dashboard) or bottom
// (launcher) edge, or on the end edge near its middle (session) or near its top (sidebar). Deliberately small, and
// each with its own dwell (ms) before it opens: the session drawer needs the longest. null: no zone.
export const DWELL = { dash: 450, launcher: 450, sidebar: 500, session: 900 };
export function hoverZone(x, y, d, rtl) {
  const g = frameGeom(d, rtl);
  if (!g || x < d.x || x >= d.x + d.w || y < d.y || y >= d.y + d.h) return null;
  const { inner } = g;
  const cx = inner.x + inner.w / 2, cy = inner.y + inner.h / 2;
  if (y <= d.y + 1 && Math.abs(x - cx) < 120) return 'dash';
  if (y >= d.y + d.h - 2 && Math.abs(x - cx) < 120) return 'launcher';
  const atEnd = rtl ? x <= d.x + 1 : x >= d.x + d.w - 2;
  if (atEnd) {
    if (Math.abs(y - cy) < 50) return 'session';
    if (y > inner.y && y < inner.y + 160) return 'sidebar';
  }
  return null;
}

// The pointer is more than m px outside rect.
export const outside = (x, y, r, m = 0) => !r || x < r.x - m || x > r.x + r.w + m || y < r.y - m || y > r.y + r.h + m;

// A small, safe calculator for the launcher ("= 2*(3+4)"): numbers, + - * / % ^ and brackets, nothing else.
export function calc(src) {
  const s = String(src).replace(/\s+/g, '').replace(/,/g, '.').replace(/x/g, '*');
  if (!s || s.length > 200 || /[^0-9+\-*/%().^]/.test(s)) return null;
  let i = 0;
  let depth = 0;
  const peek = () => s[i];
  const num = () => {
    const m = /^\d*\.?\d+/.exec(s.slice(i));
    if (!m) throw new Error('n');
    i += m[0].length;
    return parseFloat(m[0]);
  };
  const atom = () => {
    if (peek() === '(') {
      if (++depth > 32) throw new Error('deep');
      i += 1;
      const v = add();
      if (peek() !== ')') throw new Error(')');
      i += 1;
      depth -= 1;
      return v;
    }
    if (peek() === '-') { i += 1; return -atom(); }
    if (peek() === '+') { i += 1; return atom(); }
    return num();
  };
  const pow = () => {
    const b = atom();
    if (peek() === '^') { i += 1; return Math.pow(b, pow()); }
    return b;
  };
  const mul = () => {
    let v = pow();
    while (peek() === '*' || peek() === '/' || peek() === '%') {
      const o = s[i]; i += 1;
      const r = pow();
      v = o === '*' ? v * r : o === '/' ? v / r : v % r;
    }
    return v;
  };
  const add = () => {
    let v = mul();
    while (peek() === '+' || peek() === '-') {
      const o = s[i]; i += 1;
      const r = mul();
      v = o === '+' ? v + r : v - r;
    }
    return v;
  };
  try {
    const v = add();
    if (i !== s.length || !Number.isFinite(v)) return null;
    return Math.round(v * 1e9) / 1e9;
  } catch (e) {
    return null;
  }
}
