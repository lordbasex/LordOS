// shellxmodel.js — the shell's side of the Wayland "shell extras" (docs/WAYLAND.md, "Shell extras"), as plain data
// with no runtime (tested with node: server/tests/shellx_model_test.mjs). sdk/wayland.js uses it.

// What the theme's frame adds around a window drawn with its frame (windowChrome(): { w, h }): borders of w / 2 on the
// sides and the bottom, the title bar on top. A window that draws its own frame (csd) has none.
// strip: the window has menus, so a theme that gives each window its own menu strip under the title bar (chrome.menu:
// Classic, Workspaces, Celeste) draws it, and it is part of the frame too: the app's content is the frame
// less all of it. Left out, a Linux app got the strip's height less than it asked for, and lost it again on each of
// its own resizes (a fullscreen exit), which window memory then kept.
export function chromeInset(chrome, csd, strip = false) {
  if (csd || !chrome) return { left: 0, top: 0, w: 0, h: 0 };
  const side = Math.max(0, Math.round((chrome.w || 0) / 2));
  const menu = strip ? Math.max(0, Math.round(chrome.menu || 0)) : 0;
  return { left: side, top: Math.max(0, (chrome.h || 0) - side) + menu, w: chrome.w || 0, h: (chrome.h || 0) + menu };
}

// xdg-toplevel-drag: where the frame goes so the window's geometry point (ox, oy) is under the pointer (px, py).
// g: where lord-server says the geometry is now (gx, gy), or null before it is placed: then the frame's inset is
// assumed (chromeInset). f: the frame now.
export function dragFrameAt(f, g, ox, oy, px, py, inset) {
  const ix = g && Number.isFinite(g.gx) ? g.gx - f.x : inset.left;
  const iy = g && Number.isFinite(g.gy) ? g.gy - f.y : inset.top;
  return { x: Math.round(px - ox - ix), y: Math.round(py - oy - iy) };
}

// xdg-toplevel-tag: the tag as part of a key in settings (the app's own words): letters, digits, spaces and . _ : -
// kept, at most 64 characters; '' for none.
export function tagKey(tag) {
  if (typeof tag !== 'string') return '';
  const t = tag.replace(/[^\p{L}\p{N} ._:-]/gu, '').replace(/\s+/g, ' ').trim().slice(0, 64);
  return t;
}
// The key a window's place is remembered under (sdk/windowmemory.js): its app's, and with a tag that app's window of
// that kind. Only ever beside the app's own Dock id, so one app's tag can't reach another app's places.
export const placeKeyOf = (appKey, tag) => (appKey && tagKey(tag) ? `${appKey}#${tagKey(tag)}` : appKey);
// The description an app gave a window, as its name while it has no title (at most 120 characters).
export const descriptionTitle = (d) => (typeof d === 'string' ? d.replace(/\s+/g, ' ').trim().slice(0, 120) : '');

// xdg-session-management: the frame of a window restored at its geometry rect r ({ x, y, w, h }, screen, logical).
export function restoredFrame(r, inset) {
  if (!r || ![r.x, r.y, r.w, r.h].every(Number.isFinite) || r.w <= 0 || r.h <= 0) return null;
  return { x: Math.round(r.x - inset.left), y: Math.round(r.y - inset.top), w: Math.round(r.w + inset.w), h: Math.round(r.h + inset.h) };
}

// xdg-system-bell: what a ring does. The alert sound unless Do Not Disturb is on (the system volume and mute apply to it
// as to every event sound); a window that isn't in front is marked as wanting attention (the visual bell).
export function bellAction({ window = false, focused = false, dnd = false } = {}) {
  return { sound: !dnd, attention: !!window && !focused };
}

// ext-background-effect: the blur rects lord-server sent ([x, y, w, h], the window geometry's coordinates), checked: at
// most 16, whole numbers, inside sane bounds, none empty.
export function blurBoxes(rects) {
  if (!Array.isArray(rects)) return [];
  const out = [];
  for (const r of rects.slice(0, 16)) {
    if (!Array.isArray(r) || r.length !== 4 || !r.every(Number.isFinite)) continue;
    const [x, y, w, h] = r.map(Math.round);
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || Math.abs(x) > 32768 || Math.abs(y) > 32768) continue;
    out.push({ x, y, w, h });
  }
  return out;
}
export const BLUR_RADIUS = 24;          // as the theme's own translucent panels
