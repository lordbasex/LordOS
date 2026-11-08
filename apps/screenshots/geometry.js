// Where a selection lands in the captured image (server/tests/screenshots_model_test.mjs).
//
// The overlay, the pointer and wm.frame() work in logical pixels of the global space, but the
// capture is the main display at its physical resolution (docs/DISPLAYS.md, "Capture"): at 200 %
// a 1440 × 900 display is a 2880 × 1800 image. With "all displays" it is the whole logical space
// at 100 %. The rect is converted the same way lord-server paints the capture:
// physical = (logical - display origin) × scale, rounded outwards so no edge pixel is lost.

// The capture's view: { x, y, k } for an image of iw × ih, from lord.screen (displays, width, height).
// display (optional): the one the capture was taken of ({ display: id } in media.screenshot); else the main one.
export function captureView(iw, ih, screen = {}, display = null) {
  const ds = Array.isArray(screen.displays) ? screen.displays : [];
  const d = display || ds.find((o) => o && o.main) || (ds.length === 1 ? ds[0] : null);
  if (d && d.w > 0 && d.h > 0 && (!d.pw || (iw === d.pw && ih === d.ph))) {
    const k = d.scale > 0 ? d.scale / 100 : iw / d.w;
    return { x: d.x || 0, y: d.y || 0, k };
  }
  const sw = screen.width > 0 ? screen.width : iw;
  return { x: 0, y: 0, k: iw / sw };
}

// A logical rect { x, y, w, h } → the image's pixels, clamped to iw × ih (null if nothing is left).
// A window's rect also has its corner radius (wm.cornerRadius, logical px): it comes back in the
// image's pixels as `radius`, for the transparent rounded corners of a window capture (issue #70).
export function captureRect(r, iw, ih, screen, display = null) {
  if (!r || !(iw > 0) || !(ih > 0)) return null;
  const v = captureView(iw, ih, screen, display);
  const x0 = Math.max(0, Math.floor((r.x - v.x) * v.k + 1e-6)), y0 = Math.max(0, Math.floor((r.y - v.y) * v.k + 1e-6));
  const x1 = Math.min(iw, Math.ceil((r.x + r.w - v.x) * v.k - 1e-6)), y1 = Math.min(ih, Math.ceil((r.y + r.h - v.y) * v.k - 1e-6));
  if (!(x1 > x0 && y1 > y0)) return null;
  const out = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
  if (r.radius > 0) out.radius = Math.round(r.radius * v.k);
  return out;
}

// ---- several displays (a capture each: ❖⇧3 saves one file per display, ❖⇧4 selects on any of them) ----

// The displays to capture one by one: each one when there are several and they aren't mirrored, else [null]
// (Settings' choice, the main display).
export function displaysToCapture(screen = {}) {
  const ds = Array.isArray(screen.displays) ? screen.displays.filter((d) => d && d.w > 0 && d.h > 0) : [];
  return ds.length > 1 && !screen.mirror ? ds : [null];
}

// The display a logical point is on (null if none: the gap between displays of different sizes).
export function displayAt(displays, x, y) {
  return (displays || []).find((d) => d && x >= d.x && y >= d.y && x < d.x + d.w && y < d.y + d.h) || null;
}

// A rectangle from the press to the pointer, kept inside the display the press was on (a capture is of one
// display), in logical pixels.
export function dragRect(start, x, y, d) {
  const cx = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const ex = d ? cx(x, d.x, d.x + d.w) : x, ey = d ? cx(y, d.y, d.y + d.h) : y;
  return { x: Math.min(start.x, ex), y: Math.min(start.y, ey), w: Math.abs(ex - start.x), h: Math.abs(ey - start.y) };
}

// ---- the magnifier and the color picker (media.pixels: a few pixels of the capture around the pointer) ----

export const LOUPE_PX = 15;      // capture pixels across the magnifier (odd: one in the middle)
export const LOUPE_ZOOM = 8;     // screen pixels per capture pixel

// The capture pixel under a logical point, for an image of iw × ih taken of display (as captureRect converts).
export function capturePixel(x, y, iw, ih, screen, display = null) {
  const v = captureView(iw, ih, screen, display);
  return { x: Math.floor((x - v.x) * v.k + 1e-6), y: Math.floor((y - v.y) * v.k + 1e-6) };
}

// The n × n RGBA pixels (straight alpha) enlarged zoom times, with a faint grid and the middle pixel outlined:
// the magnifier's picture (the engine scales images smoothly, so the enlarging is done here).
export function magnify(rgba, n = LOUPE_PX, zoom = LOUPE_ZOOM) {
  const size = n * zoom, out = new Uint8Array(size * size * 4);
  const mid = (n >> 1) * zoom;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const sx = (x / zoom) | 0, sy = (y / zoom) | 0, si = (sy * n + sx) * 4, o = (y * size + x) * 4;
      let r = rgba[si], g = rgba[si + 1], b = rgba[si + 2];
      const a = rgba[si + 3];
      if (a < 255) { r = (r * a) / 255; g = (g * a) / 255; b = (b * a) / 255; }   // outside the capture: dark
      const edge = x % zoom === 0 || y % zoom === 0;
      const inMid = x >= mid && x < mid + zoom && y >= mid && y < mid + zoom;
      const ring = inMid && (x === mid || x === mid + zoom - 1 || y === mid || y === mid + zoom - 1);
      if (ring) { r = 255; g = 255; b = 255; } else if (edge) { r *= 0.82; g *= 0.82; b *= 0.82; }
      out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = 255;
    }
  }
  return out;
}

// The color of the middle pixel of an n × n RGBA block: { hex: '#RRGGBB', r, g, b }.
export function middleColor(rgba, n = LOUPE_PX) {
  const i = ((n >> 1) * n + (n >> 1)) * 4;
  const r = rgba[i] | 0, g = rgba[i + 1] | 0, b = rgba[i + 2] | 0;
  const h = (v) => v.toString(16).padStart(2, '0').toUpperCase();
  return { hex: `#${h(r)}${h(g)}${h(b)}`, r, g, b };
}
