// imageeditmodel.js — the math of the image editor (Screenshots' markup editor, also opened on any picture
// from Photos and Files, and Photos → Edit; issue #30): crop rectangles with aspect presets and handles,
// view ↔ image coordinates, a new size that keeps the ratio, the pixelate block, and where a save goes.
// No screen and no files: server/tests/imageedit_model_test.mjs runs it in node.
//
// Rectangles are { x, y, w, h }. A "view" is how the image is drawn: { ox, oy, s } (image pixel → view
// pixel: ox + x·s). The pixel operations themselves are in C (server/src/media/imgops.c).

export const MAX_SIDE = 8192;                       // the largest side lord-media decodes (MP_MAX_DIM)
export const CROP_PRESETS = ['free', 'original', 'square', '16:9', '4:3'];
// Pictures the editor opens (decoded by lord-media); only PNG and JPEG are written back in place.
export const MARKUP_RE = /\.(png|jpe?g|webp|bmp|gif|tiff?|heic|heif|avif)$/i;
const REPLACE_RE = /\.(png|jpe?g)$/i;

const fin = (v) => Number.isFinite(v);
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

export const canMarkup = (name) => MARKUP_RE.test(String(name || ''));

// The ratio (w / h) of a preset for an image of iw × ih; 0 = free. Wide presets follow the picture:
// on a portrait picture 16:9 is 9:16 and 4:3 is 3:4.
export function presetRatio(preset, iw, ih) {
  if (!(iw > 0) || !(ih > 0)) return 0;
  const portrait = ih > iw;
  switch (preset) {
    case 'original': return iw / ih;
    case 'square': return 1;
    case '16:9': return portrait ? 9 / 16 : 16 / 9;
    case '4:3': return portrait ? 3 / 4 : 4 / 3;
    default: return 0;
  }
}

// The largest rectangle of that ratio centered in `b` (all of `b` when free).
export function fitRatio(b, ratio) {
  if (!(ratio > 0)) return { ...b };
  let w = b.w, h = w / ratio;
  if (h > b.h) { h = b.h; w = h * ratio; }
  return { x: b.x + (b.w - w) / 2, y: b.y + (b.h - h) / 2, w, h };
}

// From an anchor (ax, ay) to the pointer (px, py), with the ratio (0 = free), inside the bounds `b`.
export function dragRect(ax, ay, px, py, ratio, b) {
  ax = clamp(ax, b.x, b.x + b.w); ay = clamp(ay, b.y, b.y + b.h);
  const sx = px >= ax ? 1 : -1, sy = py >= ay ? 1 : -1;
  const maxW = sx > 0 ? b.x + b.w - ax : ax - b.x, maxH = sy > 0 ? b.y + b.h - ay : ay - b.y;
  let w = Math.abs(px - ax), h = Math.abs(py - ay);
  if (ratio > 0) {
    if (w < h * ratio) w = h * ratio; else h = w / ratio;   // the larger side leads
    const k = Math.min(1, maxW / (w || 1), maxH / (h || 1));
    w *= k; h *= k;
  } else { w = Math.min(w, maxW); h = Math.min(h, maxH); }
  return { x: sx > 0 ? ax : ax - w, y: sy > 0 ? ay : ay - h, w, h };
}

// What is under the pointer: a corner ('nw' 'ne' 'sw' 'se'), an edge ('n' 's' 'e' 'w', free crop only),
// 'move' inside, or null.
export function handleAt(r, x, y, tol = 10, ratio = 0) {
  if (!r || !(r.w > 0) || !(r.h > 0)) return null;
  const near = (a, b) => Math.abs(a - b) <= tol;
  const inX = x >= r.x - tol && x <= r.x + r.w + tol, inY = y >= r.y - tol && y <= r.y + r.h + tol;
  if (!inX || !inY) return null;
  const l = near(x, r.x), rt = near(x, r.x + r.w), t = near(y, r.y), bt = near(y, r.y + r.h);
  if (t && l) return 'nw';
  if (t && rt) return 'ne';
  if (bt && l) return 'sw';
  if (bt && rt) return 'se';
  if (!(ratio > 0)) {
    if (t) return 'n';
    if (bt) return 's';
    if (l) return 'w';
    if (rt) return 'e';
  }
  return x > r.x && x < r.x + r.w && y > r.y && y < r.y + r.h ? 'move' : null;
}

// Dragging a handle of `r` (as it was when the drag began) to (x, y).
export function resizeRect(r, handle, x, y, ratio, b) {
  const R = r.x + r.w, B = r.y + r.h;
  switch (handle) {
    case 'nw': return dragRect(R, B, x, y, ratio, b);
    case 'ne': return dragRect(r.x, B, x, y, ratio, b);
    case 'sw': return dragRect(R, r.y, x, y, ratio, b);
    case 'se': return dragRect(r.x, r.y, x, y, ratio, b);
    case 'n': { const ny = clamp(Math.min(y, B - 1), b.y, B); return { x: r.x, y: ny, w: r.w, h: B - ny }; }
    case 's': { const ny = clamp(Math.max(y, r.y + 1), r.y, b.y + b.h); return { x: r.x, y: r.y, w: r.w, h: ny - r.y }; }
    case 'w': { const nx = clamp(Math.min(x, R - 1), b.x, R); return { x: nx, y: r.y, w: R - nx, h: r.h }; }
    case 'e': { const nx = clamp(Math.max(x, r.x + 1), r.x, b.x + b.w); return { x: r.x, y: r.y, w: nx - r.x, h: r.h }; }
    default: return { ...r };
  }
}

// `r` moved by (dx, dy), kept whole inside the bounds.
export function moveRect(r, dx, dy, b) {
  return { x: clamp(r.x + dx, b.x, b.x + b.w - r.w), y: clamp(r.y + dy, b.y, b.y + b.h - r.h), w: r.w, h: r.h };
}

// How an iw × ih image is drawn contained in a box of bw × bh (centered).
export function fitView(iw, ih, bw, bh) {
  const s = Math.min(bw / (iw || 1), bh / (ih || 1));
  return { ox: (bw - iw * s) / 2, oy: (bh - ih * s) / 2, s: s > 0 && fin(s) ? s : 1 };
}

export const toImagePoint = (x, y, v) => ({ x: (x - v.ox) / v.s, y: (y - v.oy) / v.s });
export const toViewRect = (r, v) => ({ x: v.ox + r.x * v.s, y: v.oy + r.y * v.s, w: r.w * v.s, h: r.h * v.s });

// A rectangle in image pixels → whole pixels inside iw × ih (each edge to the nearest pixel line), at least
// 1 × 1; null when it is not usable.
export function imageRect(r, iw, ih) {
  if (!r || ![r.x, r.y, r.w, r.h].every(fin) || !(iw >= 1) || !(ih >= 1)) return null;
  const x0 = clamp(Math.round(Math.min(r.x, r.x + r.w)), 0, iw), x1 = clamp(Math.round(Math.max(r.x, r.x + r.w)), 0, iw);
  const y0 = clamp(Math.round(Math.min(r.y, r.y + r.h)), 0, ih), y1 = clamp(Math.round(Math.max(r.y, r.y + r.h)), 0, ih);
  if (x1 - x0 < 1 || y1 - y0 < 1) return null;
  return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

// A side typed by the user: a whole number 1..MAX_SIDE, or null.
export function parseSide(text) {
  const s = String(text ?? '').trim();
  if (!/^\d{1,5}$/.test(s)) return null;
  const n = Number(s);
  return n >= 1 && n <= MAX_SIDE ? n : null;
}

// The new size when one side is typed: with `keep`, the other side follows the w × h ratio (rounded, at
// least 1); both stay within MAX_SIDE (the typed side is lowered if the other would pass it).
export function resizeSize(w, h, side, value, keep) {
  let W = side === 'width' ? value : w, H = side === 'height' ? value : h;
  if (!(W >= 1) || !(H >= 1)) return null;
  W = Math.round(W); H = Math.round(H);
  if (keep && w > 0 && h > 0) {
    if (side === 'width') H = Math.max(1, Math.round(W * h / w));
    else W = Math.max(1, Math.round(H * w / h));
    if (W > MAX_SIDE) { W = MAX_SIDE; H = Math.max(1, Math.round(W * h / w)); }
    if (H > MAX_SIDE) { H = MAX_SIDE; W = Math.max(1, Math.round(H * w / h)); }
  }
  return { width: clamp(W, 1, MAX_SIDE), height: clamp(H, 1, MAX_SIDE) };
}

// The pixelate block for an image: big enough that text in it can't be read (about 1/60 of the larger
// side, never under 8 pixels), within what lord-server accepts (8..200).
export const pixelBlock = (iw, ih) => clamp(Math.round(Math.max(iw || 0, ih || 0) / 60), 8, 200);

// A drawn redaction rectangle → the annotation lord-server draws.
export function redactOp(type, x1, y1, x2, y2, iw, ih, color = '#000000') {
  const op = { type: type === 'pixelate' ? 'pixelate' : 'fill', x1: Math.min(x1, x2), y1: Math.min(y1, y2), x2: Math.max(x1, x2), y2: Math.max(y1, y2), color };
  if (op.type === 'pixelate') op.size = pixelBlock(iw, ih);
  return op;
}

const dirOf = (p) => String(p).split('/').slice(0, -1).join('/');
const nameOf = (p) => String(p).split('/').pop();

// Where a save goes. `path` is the picture opened (or null for a capture never saved).
//   replace: can "Save" write over it (PNG and JPEG; the previous file goes to the Trash)
//   format:  'jpeg' for a JPEG, 'png' otherwise
//   copy(n): the n-th "Save as copy" name next to it, "<name> (<suffix>[ n]).<ext>"
//   temp:    the hidden file a replacing save is written to first
export function savePlan(path, suffix, now = Date.now()) {
  const jpeg = /\.jpe?g$/i.test(String(path || ''));
  const ext = jpeg ? (/\.jpeg$/i.test(path) ? 'jpeg' : 'jpg') : 'png';
  const base = nameOf(path || '').replace(/\.[^.]+$/, '');
  const dir = path ? dirOf(path) : '';
  return {
    replace: !!path && REPLACE_RE.test(path),
    format: jpeg ? 'jpeg' : 'png',
    copy: (n = 1) => `${dir}/${base} (${suffix}${n > 1 ? ` ${n}` : ''}).${ext}`,
    temp: path ? `${dir}/.${base}.lordos-edit-${now}.${ext}` : null,
  };
}

// The first free copy name, given an exists(path) check.
export function freeCopy(plan, exists, max = 200) {
  for (let n = 1; n <= max; n++) { const p = plan.copy(n); if (!exists(p)) return p; }
  return null;
}
