// seal.js — Monitor → Processes' verified seal (docs/MONITOR.md, "Origin badges"): a round seal with eight rounded
// scallops and a white check mark, our own drawing (no one's logo), as RGBA pixels for <Image pixels>. Blue is LordOS
// itself (the executable belongs to the lordos package), pink a LordOS app. Drawn at the screen's scale with 4×4
// samples per pixel, once per size, scale and color.
import { canvas, pack, parseColor } from './draw.js';

export const SEAL_COLORS = { system: '#1d9bf0', lordos: '#ff2e93' };
const cache = new Map();

// distance from (px, py) to the segment (ax, ay)–(bx, by)
function segDist(px, py, ax, ay, bx, by) {
  const dx = bx - ax, dy = by - ay;
  const t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)));
  return Math.hypot(px - ax - t * dx, py - ay - t * dy);
}

// In unit coordinates (0..1): the seal's edge (eight scallops) and the check mark's stroke.
export function sealAt(x, y) {
  const cx = x - 0.5, cy = y - 0.5;
  const r = Math.hypot(cx, cy), a = Math.atan2(cy, cx);
  const edge = 0.5 * (0.88 + 0.12 * Math.cos(8 * a));
  if (r > edge) return 0;                                     // outside
  const w = 0.065;                                            // half the stroke
  const d = Math.min(segDist(x, y, 0.29, 0.52, 0.44, 0.67), segDist(x, y, 0.44, 0.67, 0.72, 0.36));
  return d <= w ? 2 : 1;                                      // 2: the check mark, 1: the seal
}

export function sealPixels(size, scale = 1, color = SEAL_COLORS.system) {
  const n = Math.max(8, Math.min(256, Math.round(size * scale)));
  const key = `${n}|${color}`;
  if (cache.has(key)) return cache.get(key);
  const cv = canvas(n, n);
  const [r, g, b] = parseColor(color);
  const S = 4;
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      let seal = 0, mark = 0;
      for (let sy = 0; sy < S; sy++) {
        for (let sx = 0; sx < S; sx++) {
          const v = sealAt((x + (sx + 0.5) / S) / n, (y + (sy + 0.5) / S) / n);
          if (v) seal++;
          if (v === 2) mark++;
        }
      }
      if (!seal) continue;
      const a = seal / (S * S), m = mark / seal;
      // the mark is white over the seal's color
      cv.px[y * n + x] = pack(Math.round(r + (255 - r) * m), Math.round(g + (255 - g) * m), Math.round(b + (255 - b) * m), a);
    }
  }
  const out = { width: n, height: n, data: cv.data };
  cache.set(key, out);
  return out;
}
