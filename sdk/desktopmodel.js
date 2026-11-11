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

// What the theme keeps in its tree, in stacking order. A LordOS app minimized or on a hidden desktop
// stays mounted (and display: none, WindowShell), so it keeps its state, unsaved text included. A Linux
// window has its state in its own process, and with no node it can't be reached or painted. In the
// overview, the minimized windows of this desktop are shown too.
export function mountedOn(list, active, overview = false) {
  return list.filter((w) => {
    if (w.fullscreen || w.pip) return false;
    if (!w.args?.surface) return true;
    return onDesktop(w, active) && (overview || !w.minimized);
  });
}

// In the overview a minimized surface window may have nothing to draw: an X11 window is iconic, and only
// a Wayland app keeps its last buffer. So a placeholder goes behind the body: what the window does have
// covers it, and otherwise it is seen. A LordOS app in the session has its body, and the clear parts of
// that body would show the placeholder through it.
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

// xdg-activation (server/src/wl/xdgact.c): a granted token means the person asked (LordOS launched it,
// or the app in front handed it over), so the window comes forward, its desktop too. Any other request
// never takes the focus: the window only asks for attention, unless it is already in front.
export function activationOutcome(w, focusedId, granted) {
  if (!w) return null;
  if (granted) return 'focus';
  return w.id === focusedId ? null : 'attention';
}

// The compositor's view of a Linux window: parked = on a hidden desktop. It loses the keyboard and any
// fullscreen until it comes back. X11 windows go iconic for both, so X's stacking can't catch the pointer.
export function surfaceState(w, active) {
  const parked = !onDesktop(w, active);
  return { maximized: !!w.maximized, parked, iconic: !!w.minimized || parked };
}

// A Linux app's own notification popup (docs/WAYLAND.md, "Notifications of Linux apps"). Chromium and
// Electron draw their notifications themselves when they find no notification service on the session bus
// (none there, no bus at all, or the feature turned off), each one an xdg toplevel with no app id, no
// title and no parent, which they can neither place nor mark as "no focus". Taken as a normal window it
// opened in the middle, in front, with the keyboard, one on top of the other. So such a window floats as
// picture-in-picture does: above the windows, on every desktop, never the front window (the keyboard
// stays where it was), in the top corner of the work area. A real window has an app id or a title; one
// that gets a title later goes back to being a normal window (sdk/wayland.js).
// Only notification-sized: a nameless window bigger than this is a normal window, so no app can float a large
// surface above everything (to cover the screen and take its clicks) by leaving its name empty.
export const NOTICE_MAX_W = 480, NOTICE_MAX_H = 300;
export const isNoticeWindow = (d) => !!d && !d.x11 && !d.appId && !d.title && !d.parent && !d.dialog && !d.modal &&
  (Math.round(d.width) || 0) <= NOTICE_MAX_W && (Math.round(d.height) || 0) <= NOTICE_MAX_H;

// Where a notice popup goes: the top corner of the work area at the end of the reading direction (right,
// or left in right-to-left languages), below the ones already there (others: their frames) while it fits,
// else back at the top. Never larger than the area.
export const NOTICE_MARGIN = 12;
export function noticeFrame(area, others, width, height, rtl = false) {
  const w = Math.max(1, Math.min(Math.round(width) || 1, NOTICE_MAX_W, area.w - 2 * NOTICE_MARGIN));
  const h = Math.max(1, Math.min(Math.round(height) || 1, NOTICE_MAX_H, area.h - 2 * NOTICE_MARGIN));
  const x = rtl ? area.x + NOTICE_MARGIN : area.x + area.w - NOTICE_MARGIN - w;
  let y = area.y + NOTICE_MARGIN;
  for (const o of others || []) {
    if (!o || o.x + o.w <= area.x || o.x >= area.x + area.w || o.y < area.y || o.y >= area.y + area.h) continue;
    y = Math.max(y, o.y + o.h + NOTICE_MARGIN / 2);
  }
  if (y + h > area.y + area.h - NOTICE_MARGIN) y = area.y + NOTICE_MARGIN;
  return { x, y, w, h };
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

// ---- names and order ----

// A desktop's own name, as typed (rename in the Overview or Settings) or read from the settings file:
// one line, no control characters, at most MAX_NAME characters. '' means the default ("Desktop 2").
export const MAX_NAME = 32;
export function cleanName(s) {
  if (typeof s !== 'string') return '';
  const one = s.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, ' ').replace(/\s+/g, ' ').trim();
  return [...one].slice(0, MAX_NAME).join('').trim();
}
// The saved names, by position in the list (settings.desktopNames): exactly `count` clean strings.
export function cleanNames(saved, count) {
  const a = Array.isArray(saved) ? saved : [];
  return Array.from({ length: Math.max(0, count) }, (_, i) => cleanName(a[i]));
}
// What is saved: the names in the list's order, with the unnamed ones at the end left out (null for none).
export function namesByPosition(list, names) {
  const out = list.map((id) => cleanName(names?.[id]));
  while (out.length && !out[out.length - 1]) out.pop();
  return out.length ? out : null;
}
// Renaming: id → name, with no entry for a desktop back on its default.
export function withName(names, id, name) {
  const next = { ...names };
  const n = cleanName(name);
  if (n) next[id] = n; else delete next[id];
  return next;
}

// The list with desktop `id` taken out and put back at `index` (dragging it in the Overview's strip).
// Its windows go with it: they name the desktop, not a position.
export function moveDesktopTo(list, id, index) {
  const from = list.indexOf(id);
  if (from < 0 || !Number.isInteger(index)) return list;
  const to = Math.max(0, Math.min(list.length - 1, index));
  if (to === from) return list;
  const rest = list.filter((d) => d !== id);
  return [...rest.slice(0, to), id, ...rest.slice(to)];
}

// While a card of the strip is dragged: the position whose card's center is nearest to x (the cards are
// already mirrored in right-to-left languages, so this needs no direction).
export function stripSlot(cards, x) {
  let best = -1, dist = Infinity;
  cards.forEach((c, i) => {
    const d = Math.abs(c.x + c.w / 2 - x);
    if (d < dist) { dist = d; best = i; }
  });
  return best;
}

// ---- the overview's layout ----

const PAD = 48, GAP = 28, LABEL = 30;
export const OVERVIEW_LABEL = LABEL;

// id → { x, y, scale, w, h } in screen coordinates of the scaled frame: a grid in `a`, the last row
// centered, never larger than 85 %. frame gives the size a window is drawn at: a maximized one takes the
// work area, not its own w and h, and scaling by those let it run off the screen.
export function gridLayout(list, a, frame = (w) => w) {
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
    const f = frame(w);
    const scale = Math.max(0.05, Math.min(0.85, cw / f.w, (ch - LABEL) / f.h));
    const sw = f.w * scale, sh = f.h * scale;
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
