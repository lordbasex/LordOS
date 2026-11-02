// placemodel.js — where an app's window goes when it opens again (docs/WINDOWS.md, "Remembered places"), no screen. A place
// is kept relative to its display, and a display is known by what it is, not by its index: name (from its EDID) plus connector.
export const MAX_PLACES = 200;
const num = (v) => Number.isFinite(v) ? Math.round(v) : null;

export const displayKey = (d) => `${d.name || ''}|${d.connector || ''}`;

// The same monitor on the same port; then the same monitor on another; then that port with another monitor is NOT it.
export function findDisplay(key, displays) {
  if (!key) return null;
  const hit = displays.find((d) => displayKey(d) === key);
  if (hit) return hit;
  const name = key.slice(0, key.lastIndexOf('|'));
  const same = name ? displays.filter((d) => d.name === name) : [];
  return same.length === 1 ? same[0] : null;
}

// What a window leaves: its normal frame (not the snapped or keyboard-lifted one), and whether it was maximized.
export function placeOf(w, display) {
  if (w.pip || w.fullscreen) return null;
  const f = w.oskFrame || (w.snapped && w.restore) || w;
  if (!(f.w > 0 && f.h > 0)) return null;
  return { d: displayKey(display), x: Math.round(f.x - display.x), y: Math.round(f.y - display.y), w: Math.round(f.w), h: Math.round(f.h), max: !!(w.oskFrame ? w.oskFrame.maximized : w.maximized) };
}

// A saved value is settings data: anything odd is no place at all.
export function cleanPlace(p) {
  if (!p || typeof p !== 'object' || typeof p.d !== 'string' || p.d.length > 300) return null;
  const x = num(p.x), y = num(p.y), w = num(p.w), h = num(p.h);
  if (x === null || y === null || !(w >= 40 && w <= 32768) || !(h >= 40 && h <= 32768)) return null;
  return { d: p.d, x, y, w, h, max: p.max === true };
}

// A frame on a display that exists, fully usable: the title bar reachable and at least part of the window in the work area.
// fixed: the app's size can't change, so only the position gives way. shift: windows of this app already there.
export function restoreFrame(p, displays, areaOf, { main, fixed, assigned, size, shift = 0 } = {}) {
  const c = cleanPlace(p);
  if (!c) return null;
  const known = findDisplay(c.d, displays);
  const d = assigned || known || main || displays[0];
  const a = areaOf(d);
  const w = fixed && size ? size[0] : Math.min(c.w, a.w), h = fixed && size ? size[1] : Math.min(c.h, a.h);
  const wanted = known && known === d ? { x: d.x + c.x, y: d.y + c.y } : { x: a.x + Math.round((a.w - w) / 2), y: a.y + Math.round((a.h - h) / 2) };
  const x = wanted.x + shift, y = wanted.y + shift;
  return {
    x: Math.round(Math.max(a.x, Math.min(x, a.x + Math.max(0, a.w - w)))),
    y: Math.round(Math.max(a.y, Math.min(y, a.y + Math.max(0, a.h - h)))),
    w, h, maximized: c.max && !fixed,
  };
}

// The newest last, at most MAX_PLACES: an app not opened in a long time gives its place up.
export function putPlace(all, key, p) {
  const out = { ...all };
  delete out[key];
  out[key] = p;
  const keys = Object.keys(out);
  for (let i = 0; i < keys.length - MAX_PLACES; i++) delete out[keys[i]];
  return out;
}
