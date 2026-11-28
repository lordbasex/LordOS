// moddrag.js — moving and resizing a window from anywhere in it with ❖ (or Alt) held, the same in every theme
// (GitHub #219; docs/WINDOWS.md, "Modifier drag"). The rules are in moddragmodel.js.
//
// lord-server offers the press before the app under the pointer sees it (server/src/main.c: never while an app has the
// pointer locked or confined, holds the shortcuts, is full screen, or LordOS's own pointer lock runs), the runtime finds
// the window (runtime/events.js setModGrab) and this module answers: taken, the window moves or resizes through the
// same path as its title bar and borders (wm.beginMove / wm.beginResize: lord-server follows it in C, Workspaces'
// tiler and snapping still apply), and the app gets neither the press nor its release. A native LordOS window, a
// Wayland app and an X11 app (Xwayland) alike.
//
//   startModDrag()   the session only (startShell): tells lord-server the person's modifier and listens
import { setModGrab, addListener } from '@lordos/runtime';
import { settings } from './settings.js';
import { wm, getApp, isWindowLocked, overviewShowing, snapZoneAt, windowChrome } from './windows.js';
import { dragModifierPref, modGrabAction, nearestCorner, DRAG_OUT, dragOutAt } from './moddragmodel.js';

// a theme's own panel drawn as a window (not an app's): the Window menu leaves those out too (windowmenu.js)
const shellPanel = (id) => !!getApp(id)?.builtin && !String(id).startsWith('linux#');

// the maximized window being dragged out by JS (as each theme's title bar does): { id, x0, y0, g, moving }
let out = null;

export function onModGrab(id, x, y, button) {
  if (dragModifierPref(settings.get()) === 'off') return false;      // lord-server offers none then; never take one anyway
  const w = wm.get(id);
  const what = modGrabAction(w, button, { locked: isWindowLocked(id), overview: overviewShowing.get(), shell: shellPanel(id) });
  if (!what) return false;
  wm.focus(id);
  if (what === 'move') wm.beginMove(id);
  else if (what === 'resize') wm.beginResize(id, nearestCorner(wm.frame(w), x, y));
  else if (what === 'unmaximize') out = { id, x0: x, y0: y, g: wm.frame(w), moving: false };
  return true;
}

function followOut(e) {
  const d = out;
  if (!d) return;
  if (!d.moving) {
    if (Math.abs(e.x - d.x0) + Math.abs(e.y - d.y0) < DRAG_OUT) return;
    d.moving = true;
  }
  const w = wm.get(d.id);
  if (!w) { out = null; return; }
  const at = dragOutAt(d.g, d.x0, d.y0, e.x, e.y, w.w, windowChrome().h || 0);
  wm.move(d.id, at.x, at.y);
}

function dropOut(e) {
  const d = out;
  out = null;
  if (d && d.moving) { const z = snapZoneAt(e.x, e.y); if (z) wm.snap(d.id, z); }
}

function apply() {
  try { if (typeof lord !== 'undefined' && lord.wm && typeof lord.wm.dragModifier === 'function') lord.wm.dragModifier(dragModifierPref(settings.get())); } catch (e) { /* older server */ }
}

let started = false;
export function startModDrag() {
  if (started) return;
  started = true;
  setModGrab(onModGrab);
  addListener('pointermove', followOut);
  addListener('pointerup', dropOut);
  let last = dragModifierPref(settings.get());
  apply();
  settings.subscribe(() => {
    const now = dragModifierPref(settings.get());
    if (now !== last) { last = now; apply(); }
  });
}
