// draw.js — dibujo en píxeles RGBA (prop `pixels` de <Image>) para los gráficos y el ícono del
// Monitor (el mapa lo dibuja axia-server: server/src/monitor/map.c). Todo con Uint32Array (un
// píxel por escritura) y solo cuando cambian los datos.

const cache = new Map();

// '#rgb' | '#rrggbb' | '#rrggbbaa' | 'rgb(…)' | 'rgba(…)' → [r, g, b, a(0..1)]
export function parseColor(c) {
  if (cache.has(c)) return cache.get(c);
  let out = [10, 132, 255, 1];
  const s = String(c || '').trim();
  let m;
  if ((m = /^#([0-9a-f]{3,8})$/i.exec(s))) {
    const h = m[1];
    if (h.length === 3) out = [...h].map((x) => parseInt(x + x, 16)).concat(1);
    else if (h.length >= 6) out = [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16), h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1];
  } else if ((m = /^rgba?\(([^)]+)\)$/i.exec(s))) {
    const p = m[1].split(/[ ,/]+/).filter(Boolean).map(Number);
    out = [p[0] | 0, p[1] | 0, p[2] | 0, p.length > 3 ? p[3] : 1];
  }
  cache.set(c, out);
  return out;
}

// RGBA (little-endian) empaquetado para Uint32Array
export const pack = (r, g, b, a) => ((Math.round(a * 255) << 24) | (b << 16) | (g << 8) | r) >>> 0;

export function canvas(w, h) {
  w = Math.max(1, Math.min(4096, Math.round(w)));
  h = Math.max(1, Math.min(4096, Math.round(h)));
  const data = new Uint8Array(w * h * 4);
  return { width: w, height: h, data, px: new Uint32Array(data.buffer) };
}

// Mezcla "over" de un color con alfa sobre el píxel i.
export function blend(cv, i, r, g, b, a) {
  if (a <= 0) return;
  const d = cv.data, o = i * 4, da = d[o + 3] / 255;
  const oa = a + da * (1 - a);
  if (oa <= 0) return;
  d[o] = (r * a + d[o] * da * (1 - a)) / oa;
  d[o + 1] = (g * a + d[o + 1] * da * (1 - a)) / oa;
  d[o + 2] = (b * a + d[o + 2] * da * (1 - a)) / oa;
  d[o + 3] = oa * 255;
}

// Gráfico de área: series [{ values, color }] de hasta `capacity` muestras, alineadas a la
// derecha (la más nueva al borde). Relleno degradado (0.32 → 0.02, como TangoOS) y línea.
export function drawArea(cv, series, top, capacity) {
  const { width: w, height: h, px } = cv;
  px.fill(0);
  const step = (w - 1) / (capacity - 1);
  const ys = series.map((s) => {
    const vs = s.values, n = vs.length, off = capacity - n;
    const col = new Float32Array(w).fill(-1);
    if (n < 2) return col;
    const x0 = Math.ceil(off * step);
    for (let x = x0; x < w; x++) {
      const fi = x / step - off, i = Math.floor(fi), f = fi - i;
      const v = i + 1 < n ? vs[i] * (1 - f) + vs[i + 1] * f : vs[n - 1];
      col[x] = h - 1 - (Math.max(0, Math.min(v, top)) / top) * (h - 3);
    }
    return col;
  });
  // rellenos (la serie de adelante tapa a la de atrás)
  series.forEach((s, k) => {
    const [r, g, b] = parseColor(s.color), col = ys[k];
    const grad = new Uint32Array(h);
    for (let y = 0; y < h; y++) grad[y] = pack(r, g, b, 0.32 - 0.3 * (y / h));
    for (let x = 0; x < w; x++) {
      const y0 = col[x];
      if (y0 < 0) continue;
      for (let y = Math.ceil(y0); y < h; y++) px[y * w + x] = grad[y];
    }
  });
  // líneas de 1,6 px: tramo vertical entre columnas vecinas, más un borde suave
  series.forEach((s, k) => {
    const [r, g, b] = parseColor(s.color), col = ys[k], solid = pack(r, g, b, 1);
    let prev = -1;
    for (let x = 0; x < w; x++) {
      const y = col[x];
      if (y < 0) { prev = -1; continue; }
      const a = prev < 0 ? y : prev, lo = Math.max(0, Math.floor(Math.min(a, y))), hi = Math.min(h - 1, Math.ceil(Math.max(a, y)));
      for (let yy = lo; yy <= hi; yy++) px[yy * w + x] = solid;
      if (lo > 0) blend(cv, (lo - 1) * w + x, r, g, b, 0.45);
      if (hi + 1 < h) blend(cv, (hi + 1) * w + x, r, g, b, 0.3);
      prev = y;
    }
  });
  return cv;
}

// Barras de subida (arriba) y bajada (abajo) por segundo: pares [rx, tx].
export function drawBars(cv, pairs, colors) {
  const { width: w, height: h, px } = cv;
  px.fill(0);
  const n = pairs.length;
  if (!n) return cv;
  const top = Math.max(1, ...pairs.map(([a, b]) => Math.max(a, b)));
  const mid = Math.floor(h / 2), bw = w / n;
  const [cr, cg, cb] = parseColor(colors[0]), [tr, tg, tb] = parseColor(colors[1]);
  const down = pack(cr, cg, cb, 0.9), up = pack(tr, tg, tb, 0.9);
  for (let i = 0; i < n; i++) {
    const x0 = Math.floor(i * bw), x1 = Math.max(x0 + 1, Math.floor((i + 1) * bw) - 1);
    const hu = Math.round((pairs[i][1] / top) * (mid - 1)), hd = Math.round((pairs[i][0] / top) * (mid - 1));
    for (let x = x0; x < x1 && x < w; x++) {
      for (let y = mid - hu; y < mid; y++) px[y * w + x] = up;
      for (let y = mid + 1; y <= mid + hd && y < h; y++) px[y * w + x] = down;
    }
  }
  return cv;
}

// Mirrored traffic chart: one bar per time bucket, sent (tx) above the baseline and received (rx)
// below, every bar on a faint full-height track so the chart reads as one continuous strip even
// when idle. pairs: [rx, tx] per bucket, oldest first; the newest bar ends at the right edge.
// shift (0..1): how far the newest bucket has filled (it grows in from the right edge while the
// others slide left), so the strip scrolls smoothly instead of jumping one bar at a time. top: the
// value that fills half the height.
export function drawMirror(cv, pairs, { top, rx, tx, track, shift = 0, gap = 1 }) {
  const { width: w, height: h, px } = cv;
  px.fill(0);
  const n = pairs.length;
  if (!n) return cv;
  const half = Math.floor((h - 1) / 2), mid = half;            // row `mid` is the baseline
  const bw = w / (n - 1);                                      // one bucket hangs off the left edge while scrolling
  const [rr, rg, rb] = parseColor(rx), [tr, tg, tb] = parseColor(tx), [kr, kg, kb, ka] = parseColor(track);
  const trackPx = pack(kr, kg, kb, ka), down = pack(rr, rg, rb, 0.95), up = pack(tr, tg, tb, 0.95);
  const downTop = pack(rr, rg, rb, 1), upTop = pack(tr, tg, tb, 1);
  const base = pack(kr, kg, kb, Math.min(1, ka * 3));
  const scale = (v) => (v > 0 ? Math.max(1, Math.round((Math.min(v, top) / top) * half)) : 0);   // anything moving shows at least 1 px
  for (let i = 0; i < n; i++) {
    // the newest bucket (still filling) grows in from the right edge; the others slide left with it
    const right = i === n - 1 ? w : w - shift * bw - (n - 2 - i) * bw;
    const left = i === n - 1 ? w - shift * bw : right - bw;
    const x0 = Math.max(0, Math.round(left)), x1 = Math.min(w, Math.round(right) - gap);
    if (x1 <= x0) continue;
    const hu = scale(pairs[i][1]), hd = scale(pairs[i][0]);
    for (let x = x0; x < x1; x++) {
      for (let y = 0; y < h; y++) px[y * w + x] = trackPx;
      for (let y = mid - hu; y < mid; y++) px[y * w + x] = y === mid - hu ? upTop : up;
      for (let y = mid + 1; y <= mid + hd && y < h; y++) px[y * w + x] = y === mid + hd ? downTop : down;
      px[mid * w + x] = base;
    }
  }
  return cv;
}

// Punto con borde suave (radio en px, centro en x, y).
export function dot(cv, cx, cy, rad, color, alpha = 1) {
  const [r, g, b] = parseColor(color);
  const { width: w, height: h } = cv;
  for (let y = Math.max(0, Math.floor(cy - rad - 1)); y <= Math.min(h - 1, Math.ceil(cy + rad + 1)); y++) {
    for (let x = Math.max(0, Math.floor(cx - rad - 1)); x <= Math.min(w - 1, Math.ceil(cx + rad + 1)); x++) {
      const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
      const a = Math.max(0, Math.min(1, rad + 0.5 - d));
      if (a > 0) blend(cv, y * w + x, r, g, b, a * alpha);
    }
  }
}

// Escala "linda" para el eje Y: 1, 2, 2.5, 5 × 10^n.
export function niceMax(v) {
  if (!(v > 0)) return 1;
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}
