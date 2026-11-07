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
export function captureRect(r, iw, ih, screen, display = null) {
  if (!r || !(iw > 0) || !(ih > 0)) return null;
  const v = captureView(iw, ih, screen, display);
  const x0 = Math.max(0, Math.floor((r.x - v.x) * v.k + 1e-6)), y0 = Math.max(0, Math.floor((r.y - v.y) * v.k + 1e-6));
  const x1 = Math.min(iw, Math.ceil((r.x + r.w - v.x) * v.k - 1e-6)), y1 = Math.min(ih, Math.ceil((r.y + r.h - v.y) * v.k - 1e-6));
  return x1 > x0 && y1 > y0 ? { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } : null;
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
