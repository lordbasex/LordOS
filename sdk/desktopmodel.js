// desktopmodel.js — Desktops as plain data, with no runtime or React (so it can be unit tested with
// node: server/tests/desktop_model_test.mjs). The runtime parts are sdk/windows.js (the stores and
// wm.*), sdk/desktops.js (switching, persistence, the animation) and sdk/overview.jsx.
//
// A desktop is a stable number; `list` is their order on screen and `active` the one showing. A
// window carries w.desktop. Everything that decides what is shown, what is in front and who has the
// keyboard goes through the functions here, so a window on a hidden desktop has one definition.
//
// Picture-in-picture is sticky: it shows on every desktop and never counts as the front window.

export const MAX_DESKTOPS = 9;
export const FIRST_DESKTOP = 1;

export const onDesktop = (w, active) => !!w.pip || (w.desktop ?? FIRST_DESKTOP) === active;
// the window is out of sight: minimized, or on a desktop that isn't showing
export const windowHidden = (w, active) => !w || !!w.minimized || !onDesktop(w, active);

// The windows the theme frames on this desktop (fullscreen and picture-in-picture are the SDK's).
export const visibleOn = (list, active) => list.filter((w) => !w.minimized && !w.fullscreen && !w.pip && onDesktop(w, active));

// What the theme keeps in its tree, in stacking order. A LordOS app on a hidden desktop stays mounted
// (and display: none, WindowShell), so it keeps its state; a Linux window has its state in its own
// process, and with no node it can't be reached or painted. In the overview, minimized windows of
// this desktop are shown too.
export function mountedOn(list, active, overview = false) {
  return list.filter((w) => {
    if (w.fullscreen || w.pip) return false;
    if (onDesktop(w, active)) return overview || !w.minimized;
    return !w.minimized && !w.args?.surface;
  });
}

// In the overview a minimized surface window may have nothing to draw: an X11 window is iconic, and only
// a Wayland app keeps its last buffer. So a placeholder goes behind the body: what the window does have
// covers it, and otherwise it is seen. A LordOS app in the session mounts again and has its body, and the
// clear parts of that body would show the placeholder through it.
export const overviewPlaceholder = (w, inOverview) => !!inOverview && !!w.minimized && !!w.args?.surface;

// The front window of a desktop (windows are ordered bottom to top). Never a window of another
// desktop, and never picture-in-picture, which floats over all of them with no keyboard.
export function focusedOn(list, active) {
  for (let i = list.length - 1; i >= 0; i--) {
    const w = list[i];
    if (!w.minimized && !w.pip && onDesktop(w, active)) return w;
  }
  return null;
}

// Which Linux surface gets the Wayland keyboard (sdk/wayland.js). 0 with a shell surface open, or
// when the front window of the showing desktop isn't a Linux one: never another desktop's.
export function keyboardSurface(list, active, shellBusy, surfaceOf) {
  const top = shellBusy ? null : focusedOn(list, active);
  return top ? surfaceOf(top.id) : 0;
}

// A request from a Linux app that would bring its window forward (moving, resizing, the window menu).
// These come from the app, so a window on a hidden desktop doesn't get to change the desktop.
export const acceptsAppRequest = (w, active) => !!w && onDesktop(w, active);

// The compositor's view of a Linux window: parked = on a hidden desktop. It loses the keyboard and any
// fullscreen until it comes back. X11 windows go iconic for both, so X's stacking can't catch the pointer.
export function surfaceState(w, active) {
  const parked = !onDesktop(w, active);
  return { maximized: !!w.maximized, parked, iconic: !!w.minimized || parked };
}

// The window an app comes back with: on this desktop if it has one, a visible one before a minimized
// one, else the top one of any desktop (focusing that one switches desktop).
export function pickWindow(wins, active) {
  const here = wins.filter((w) => onDesktop(w, active));
  const from = here.length ? here : wins;
  return [...from].reverse().find((w) => !w.minimized) || from[from.length - 1] || null;
}

export function addDesktop(state) {
  if (state.list.length >= MAX_DESKTOPS) return { state, id: null };
  const id = Math.max(0, ...state.list) + 1;
  return { state: { ...state, list: [...state.list, id] }, id };
}

// The desktop whose windows move in when `id` closes: the next one, or the previous for the last.
export function nextDesktop(list, id) {
  const i = list.indexOf(id);
  if (i < 0) return null;
  if (i + 1 < list.length) return list[i + 1];
  return i > 0 ? list[i - 1] : null;
}

// Removes a desktop. target: where its windows go (null: nothing changed, the last desktop stays).
export function removeDesktop(state, id) {
  const target = nextDesktop(state.list, id);
  if (target == null) return { state, target: null };
  return { state: { list: state.list.filter((d) => d !== id), active: state.active === id ? target : state.active }, target };
}

// step: -1 previous, 1 next. No wrap: the first and the last desktop are ends.
export function neighbor(list, active, step) {
  const i = list.indexOf(active) + step;
  return i >= 0 && i < list.length ? list[i] : null;
}

// Left and Right follow the layout: the strip is mirrored in right-to-left languages.
export const arrowStep = (dir, rtl) => ((dir === 'left') !== !!rtl ? -1 : 1);
// Swiping left brings what is on the right, as a page does.
export const swipeStep = (dir, rtl) => -arrowStep(dir, rtl);

// A saved number of desktops, from a file any program of the user can edit.
export const cleanCount = (n) => (Number.isInteger(n) && n >= 1 && n <= MAX_DESKTOPS ? n : 1);
// A saved position in the list (0 is the first desktop).
export const cleanIndex = (i) => (Number.isInteger(i) && i >= 0 && i < MAX_DESKTOPS ? i : 0);

// ---- the overview's layout ----

const PAD = 48, GAP = 28, LABEL = 30;
export const OVERVIEW_LABEL = LABEL;

// id → { x, y, scale, w, h } in screen coordinates of the scaled frame: a grid in `a`, the last row
// centered, never larger than 85 %.
export function gridLayout(list, a) {
  const out = new Map();
  const n = list.length;
  if (!n) return out;
  const cols = Math.ceil(Math.sqrt((n * (a.w / Math.max(1, a.h))) / 1.6));
  const c = Math.max(1, Math.min(n, cols)), r = Math.ceil(n / c);
  const cw = (a.w - 2 * PAD - GAP * (c - 1)) / c, ch = (a.h - PAD - GAP * (r - 1)) / r;
  list.forEach((w, i) => {
    const col = i % c, row = Math.floor(i / c);
    const inRow = row === r - 1 ? n - row * c : c;
    const rowLeft = a.x + PAD + ((c - inRow) * (cw + GAP)) / 2;
    const scale = Math.max(0.05, Math.min(0.85, cw / w.w, (ch - LABEL) / w.h));
    const sw = w.w * scale, sh = w.h * scale;
    out.set(w.id, {
      x: Math.round(rowLeft + col * (cw + GAP) + (cw - sw) / 2),
      y: Math.round(a.y + row * (ch + GAP) + LABEL + (ch - LABEL - sh) / 2),
      scale, w: sw, h: sh,
    });
  });
  return out;
}

// The strip of desktops, centered along the top of area `a`: one card per desktop and the add button
// (cards in the list's order; RTL mirrors them). Returns { cards: [{ id, x, y, w, h }], add, h }.
export function stripLayout(list, a, { aspect = 1.6, canAdd = true, rtl = false } = {}) {
  const gap = 12, addW = 44, top = 14;
  const n = list.length, room = a.w - 2 * PAD - (canAdd ? addW + gap : 0) - gap * Math.max(0, n - 1);
  const w = Math.round(Math.max(72, Math.min(168, room / Math.max(1, n))));
  const h = Math.round(w / aspect);
  const total = n * w + gap * Math.max(0, n - 1) + (canAdd ? addW + gap : 0);
  const left = Math.round(a.x + (a.w - total) / 2);
  const at = (slot, width) => (rtl ? left + total - slot - width : left + slot);
  const cards = list.map((id, i) => ({ id, x: at(i * (w + gap), w), y: a.y + top, w, h }));
  const add = canAdd ? { x: at(n * (w + gap), addW), y: a.y + top, w: addW, h } : null;
  return { cards, add, h: top + h + 30 };
}

export const hitRect = (r, x, y) => !!r && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
