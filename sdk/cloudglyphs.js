// cloudglyphs.js — Cloud's small drawings as RGBA pixels for <Image pixels>: the engine has no vector paths or
// conic gradients, so the mocks' glyphs (a cloud outline, a check in a circle, arrows, a progress ring) and the
// space rings are drawn here from distance functions, antialiased, at twice their size for sharp edges.
// Each result is cached by what it draws, so a row that doesn't change hands the same object back and the
// engine doesn't upload it again (runtime/index.js compares pixels by identity).
//
//   glyphPixels(kind, size, color, { pct, track, ink })   kind: cloud | down | here | kept | up | error | menu
//   ringPixels(size, parts, { thickness, track })          parts: [{ frac, color }] from 12 o'clock, clockwise

const SCALE = 2;
const cache = new Map();
const MAX_CACHE = 400;

function rgb(c) {
  const s = String(c || '#000000');
  if (/^#[0-9a-f]{6}$/i.test(s)) { const n = parseInt(s.slice(1), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255, 255]; }
  if (/^#[0-9a-f]{3}$/i.test(s)) return [...s.slice(1)].map((h) => parseInt(h + h, 16)).concat(255);
  const m = /^rgba?\(([^)]+)\)$/.exec(s);
  if (m) { const p = m[1].split(',').map((x) => parseFloat(x)); return [p[0] | 0, p[1] | 0, p[2] | 0, p.length > 3 ? Math.round(p[3] * 255) : 255]; }
  return [128, 128, 128, 255];
}

// ---- distance functions, in the mocks' 24-unit box ----

const segD = (px, py, ax, ay, bx, by) => {
  const dx = bx - ax, dy = by - ay;
  const h = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy || 1)));
  return Math.hypot(px - ax - dx * h, py - ay - dy * h);
};
const polyD = (px, py, pts) => { let d = Infinity; for (let i = 0; i + 1 < pts.length; i++) d = Math.min(d, segD(px, py, ...pts[i], ...pts[i + 1])); return d; };
const circD = (px, py, cx, cy, r) => Math.hypot(px - cx, py - cy) - r;
// the cloud: three lobes over a flat bottom (the mock's path, M7 18h10a4 4 0 0 0 .6-7.95A6 6 0 0 0 6.2 9.2 4.5 4.5 0 0 0 7 18z)
function cloudD(px, py) {
  const lobes = Math.min(circD(px, py, 7.4, 13.6, 4.4), circD(px, py, 12.3, 10.6, 5.6), circD(px, py, 16.9, 14.0, 4.0));
  const body = Math.max(Math.abs(px - 12.2) - 4.9, Math.abs(py - 15.6) - 2.4);
  return Math.max(Math.min(lobes, body), py - 18);
}

// coverage of a stroke of width w around a distance d (in units), one pixel = u units
const stroke = (d, w, u) => Math.max(0, Math.min(1, (w / 2 - Math.abs(d)) / u + 0.5));
const fill = (d, u) => Math.max(0, Math.min(1, -d / u + 0.5));

function paint(size, fn) {
  const n = Math.max(1, Math.round(size * SCALE));
  const data = new Uint8Array(n * n * 4);
  const u = 24 / n;                      // units per pixel
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      const c = fn((x + 0.5) * u, (y + 0.5) * u, u);
      if (!c || c[3] <= 0) continue;
      const o = (y * n + x) * 4;
      data[o] = c[0]; data[o + 1] = c[1]; data[o + 2] = c[2]; data[o + 3] = Math.round(Math.min(255, c[3]));
    }
  }
  return { width: n, height: n, data };
}
// source-over of colored layers [[rgba, coverage]]
function over(layers) {
  let r = 0, g = 0, b = 0, a = 0;
  for (const [c, cov] of layers) {
    const ca = (c[3] / 255) * cov;
    if (ca <= 0) continue;
    r = c[0] * ca + r * (1 - ca); g = c[1] * ca + g * (1 - ca); b = c[2] * ca + b * (1 - ca);
    a = ca + a * (1 - ca);
  }
  return a > 0 ? [r / a, g / a, b / a, a * 255] : null;
}
// angle from 12 o'clock, clockwise, 0..1
const turn = (px, py) => ((Math.atan2(px - 12, -(py - 12)) / (Math.PI * 2)) + 1) % 1;

const DRAW = {
  cloud: (col) => (x, y, u) => over([[col, stroke(cloudD(x, y), 1.9, u)]]),
  here: (col) => (x, y, u) => over([[col, Math.max(stroke(circD(x, y, 12, 12, 9), 1.9, u), stroke(polyD(x, y, [[8, 12], [11, 15], [16, 9]]), 2.1, u))]]),
  kept: (col, o) => (x, y, u) => over([[col, fill(circD(x, y, 12, 12, 10), u)], [o.ink, stroke(polyD(x, y, [[7.5, 12], [10.5, 15], [16.5, 8.5]]), 2.3, u)]]),
  down: (col, o) => (x, y, u) => {
    const ring = stroke(circD(x, y, 12, 12, 9), 2.5, u);
    const on = o.pct != null && turn(x, y) <= o.pct;
    return over([[o.track, ring], [col, on ? ring : 0], [col, Math.max(stroke(segD(x, y, 12, 8, 12, 15), 1.9, u), stroke(polyD(x, y, [[9, 12.5], [12, 15.5], [15, 12.5]]), 1.9, u))]]);
  },
  up: (col, o) => (x, y, u) => {
    const ring = stroke(circD(x, y, 12, 12, 9), 2.5, u);
    const on = o.pct != null && turn(x, y) <= o.pct;
    return over([[o.track, ring], [col, on ? ring : 0], [col, Math.max(stroke(segD(x, y, 12, 16, 12, 9), 1.9, u), stroke(polyD(x, y, [[9, 11.5], [12, 8.5], [15, 11.5]]), 1.9, u))]]);
  },
  error: (col, o) => (x, y, u) => over([[col, fill(circD(x, y, 12, 12, 10), u)], [o.ink, Math.max(stroke(segD(x, y, 12, 7, 12, 13), 2.5, u), fill(circD(x, y, 12, 16.7, 1.4), u))]]),
  // the menu bar's: the cloud with an arrow inside while files move (o.arrow 'up' | 'down')
  menu: (col, o) => (x, y, u) => {
    let a = 0;
    if (o.arrow === 'down') a = Math.max(stroke(segD(x, y, 12, 10, 12, 15), 1.8, u), stroke(polyD(x, y, [[10, 13], [12, 15], [14, 13]]), 1.8, u));
    if (o.arrow === 'up') a = Math.max(stroke(segD(x, y, 12, 15.5, 12, 10.5), 1.8, u), stroke(polyD(x, y, [[10, 12.5], [12, 10.5], [14, 12.5]]), 1.8, u));
    return over([[col, Math.max(stroke(cloudD(x, y), 1.9, u), a)]]);
  },
};

export const GLYPH_KINDS = Object.keys(DRAW);

function remember(key, make) {
  let v = cache.get(key);
  if (v) return v;
  v = make();
  if (cache.size >= MAX_CACHE) cache.delete(cache.keys().next().value);
  cache.set(key, v);
  return v;
}

export function glyphPixels(kind, size, color, opts = {}) {
  if (!DRAW[kind]) return null;
  const pct = opts.pct == null ? null : Math.round(Math.max(0, Math.min(1, opts.pct)) * 40) / 40;      // 2.5 % steps
  const o = { pct, track: rgb(opts.track || '#3a3a42'), ink: rgb(opts.ink || '#ffffff'), arrow: opts.arrow || null };
  const key = `${kind}|${size}|${color}|${pct}|${opts.track || ''}|${opts.ink || ''}|${o.arrow || ''}`;
  return remember(key, () => paint(size, DRAW[kind](rgb(color), o)));
}

// A ring of colored parts over a track, as the account page's space and the sidebar's small usage ring.
// thickness in units of the 24-unit box (the sidebar's 14 px ring is a filled pie: thickness 12).
export function ringPixels(size, parts, { thickness = 4, track = '#48484f' } = {}) {
  const ps = (parts || []).filter((p) => p.frac > 0).map((p) => ({ frac: Math.round(p.frac * 400) / 400, color: p.color }));
  const key = `ring|${size}|${thickness}|${track}|${ps.map((p) => `${p.frac}:${p.color}`).join(',')}`;
  return remember(key, () => {
    const stops = [];
    let acc = 0;
    for (const p of ps) { acc += p.frac; stops.push([Math.min(1, acc), rgb(p.color)]); }
    const tr = rgb(track);
    const r = 12 - thickness / 2;
    return paint(size, (x, y, u) => {
      const cov = stroke(circD(x, y, 12, 12, r), thickness, u);
      if (cov <= 0) return null;
      const a = turn(x, y);
      const hit = stops.find(([end]) => a <= end);
      return over([[hit ? hit[1] : tr, cov]]);
    });
  });
}

export const glyphCacheSize = () => cache.size;
