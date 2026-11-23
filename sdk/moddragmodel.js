// moddragmodel.js — moving and resizing a window from anywhere in it with a modifier held, no screen (GitHub #219;
// docs/WINDOWS.md, "Modifier drag"). sdk/moddrag.js applies it; lord-server offers the press (server/src/main.c).
// node server/tests/moddrag_model_test.mjs
//
//   ❖ + left drag    moves the window (a maximized one comes out of its place, as from its title bar)
//   ❖ + right drag   resizes it from the corner nearest the pointer (the middle button too)
//
// Settings → Appearance → Window options: off (the default: nothing changes for anyone who doesn't choose it, and lord-server
// then takes no press at all), ❖, or Alt (programs such as GIMP, Blender or a terminal use Alt and a click for their own
// things).

export const DRAG_MODIFIERS = ['off', 'super', 'alt'];

// The saved choice (settings.windowDragModifier), 'off' for anything else.
export const dragModifierPref = (s) => (DRAG_MODIFIERS.includes(s && s.windowDragModifier) ? s.windowDragModifier : 'off');

// The corner of frame f = { x, y, w, h } nearest (x, y), as xdg edges (1 top, 2 bottom, 4 left, 8 right): 5, 6, 9 or 10.
export function nearestCorner(f, x, y) {
  return (y < f.y + f.h / 2 ? 1 : 2) | (x < f.x + f.w / 2 ? 4 : 8);
}

// What a press with the modifier on window w does:
//   null         not the window manager's: the press goes on to the app (no window, a theme panel, full screen,
//                picture in picture, the Overview, a button other than left, middle and right)
//   'move'       the window's move (wm.beginMove: lord-server follows it; a tiler's rules still apply)
//   'unmaximize' a maximized window: out of its place once the pointer really moves, as from its title bar
//   'resize'     resized from the corner nearest the pointer (wm.beginResize)
//   'hold'       taken, nothing happens: a locked window, or a maximized one asked to resize (the press still never
//                reaches the app, so ❖ + click on a window is always the window manager's)
// ctx: { locked, overview, shell } — the theme locks it, the Overview is open, it is one of the shell's own panels.
export function modGrabAction(w, button, ctx = {}) {
  if (!w || ctx.overview || ctx.shell || w.fullscreen || w.minimized || w.pip) return null;
  if (button !== 0 && button !== 1 && button !== 2) return null;
  if (ctx.locked) return 'hold';
  if (button === 0) return w.maximized ? 'unmaximize' : 'move';
  return w.maximized ? 'hold' : 'resize';
}

// px the pointer moves before a maximized window comes out of its place (a click's jitter must not restore it)
export const DRAG_OUT = 6;

// Where a maximized window goes when the pointer at (px, py), grabbed at (x0, y0) on its frame g, has moved: its own
// size w × h, under the pointer at the same fraction of its width, and at most half a title bar (bar) below its top.
export function dragOutAt(g, x0, y0, px, py, w, bar) {
  const rel = g.w ? Math.min(1, Math.max(0, (x0 - g.x) / g.w)) : 0.5;
  const dx = Math.round(rel * w), dy = Math.min(Math.round(bar / 2), Math.max(0, y0 - g.y));
  return { x: px - dx, y: py - dy };
}
