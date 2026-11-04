// Where a selection lands in the captured image (server/tests/screenshots_model_test.mjs).
//
// The overlay, the pointer and wm.frame() work in logical pixels of the global space, but the
// capture is the main display at its physical resolution (docs/DISPLAYS.md, "Capture"): at 200 %
// a 1440 × 900 display is a 2880 × 1800 image. With "all displays" it is the whole logical space
// at 100 %. The rect is converted the same way lord-server paints the capture:
// physical = (logical - display origin) × scale, rounded outwards so no edge pixel is lost.

// The capture's view: { x, y, k } for an image of iw × ih, from lord.screen (displays, width, height).
export function captureView(iw, ih, screen = {}) {
  const ds = Array.isArray(screen.displays) ? screen.displays : [];
  const d = ds.find((o) => o && o.main) || (ds.length === 1 ? ds[0] : null);
  if (d && d.w > 0 && d.h > 0 && (!d.pw || (iw === d.pw && ih === d.ph))) {
    const k = d.scale > 0 ? d.scale / 100 : iw / d.w;
    return { x: d.x || 0, y: d.y || 0, k };
  }
  const sw = screen.width > 0 ? screen.width : iw;
  return { x: 0, y: 0, k: iw / sw };
}

// A logical rect { x, y, w, h } → the image's pixels, clamped to iw × ih (null if nothing is left).
export function captureRect(r, iw, ih, screen) {
  if (!r || !(iw > 0) || !(ih > 0)) return null;
  const v = captureView(iw, ih, screen);
  const x0 = Math.max(0, Math.floor((r.x - v.x) * v.k + 1e-6)), y0 = Math.max(0, Math.floor((r.y - v.y) * v.k + 1e-6));
  const x1 = Math.min(iw, Math.ceil((r.x + r.w - v.x) * v.k - 1e-6)), y1 = Math.min(ih, Math.ceil((r.y + r.h - v.y) * v.k - 1e-6));
  return x1 > x0 && y1 > y0 ? { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } : null;
}
