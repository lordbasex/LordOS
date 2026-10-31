// osk.js — the on-screen keyboard's state (docs/ON-SCREEN-KEYBOARD.md). The panel is sdk/osk.jsx, the
// logic without a screen sdk/oskmodel.js.
//
// It opens by itself when a text field takes focus, depending on Settings → Keyboard (settings.osk:
// 'off', 'auto' with no physical keyboard, 'always'), and by hand from the status item, ❖⌃K or
// Settings. The fields come from three places: this process (textfocus.js), a Linux app
// (zwp_text_input_v3, sdk/wayland.js) and an isolated app (a broker hint, sdk/privacy.js). A hint
// from an app is only obeyed for the window in front, and all it can do is show the keyboard.
//
// Keys go through lord.input.tap, which exists only in this process and the login screen's, down the
// same path as a physical key. Nothing here keeps what is typed, offers suggestions or remembers
// keys; the panel is `protected`, so it is black in screenshots, recordings and remote desktop.
import { addListener, displayFor, mainDisplay, shellHasKeyboard } from '@lordos/runtime';
import { createStore } from './store.js';
import { settings } from './settings.js';
import { keyboard } from './keyboard.js';
import { wm, windows, workAreaOf, windowChrome, appIdOf, appMenus } from './windows.js';
import { shellLayers } from './layers.js';
import { onTextField, textFieldActive, textFieldCaret } from './textfocus.js';
import { fallbackLabels, opensByItself, panelSize, planAvoid } from './oskmodel.js';

const CLOSED = { visible: false, dock: 'bottom', page: 0, shift: 0, box: null };
export const oskState = createStore({ ...CLOSED, hasKeyboard: true, labels: null });

const native = () => typeof lord !== 'undefined' && !!lord.input?.tap;
const mode = () => settings.get().osk || 'auto';
const hasKeyboard = () => (native() ? lord.input.hasKeyboard() : true);

let linux = null;           // { id, y, h }: a Linux app's field, window coordinates
let appField = null;        // { id, y, h }: an isolated app's field
let manual = false;         // opened by hand: stays open without a field
let dismissed = false;      // hidden with its button: only a new field or a click brings it back
let lifted = null;          // the window moved out of the way
let liftAsked = 0;          // the height it was asked for: an app with a minimum size may keep its own
let hideTimer = 0;

const remoteField = () => linux || appField;
// An isolated app is a Wayland window: its id is linux#<surface>, and args.appId says which app it is.
const appOfWindow = (id) => { const w = id ? wm.get(id) : null; return w?.args?.pkg ? w.args.appId : appIdOf(id); };
const fieldActive = () => textFieldActive() || !!remoteField();

const refreshLabels = () => {
  let labels = null;
  try { labels = native() ? lord.input.keyLabels() : null; } catch (e) { /* server without it */ }
  oskState.set((s) => ({ ...s, labels: labels || fallbackLabels() }));
};

function dropLift() {
  if (lifted) wm.dropFromKeyboard(lifted);
  lifted = null;
  liftAsked = 0;
}

// The window with the field, unless a shell surface (Search, a sheet) has the keyboard.
function fieldWindow() {
  if (!shellLayers() || shellHasKeyboard()) return null;
  const id = wm.focused();
  const w = id ? wm.get(id) : null;
  return w && !w.minimized ? w : null;
}

// From the window's top to its content: the title bar, and Lord Classic's menu strip when it has one.
const contentTop = (w) => (w.args?.csd ? 0 : windowChrome().h + (appMenus.get()[w.id]?.length ? windowChrome().menu || 0 : 0));

function boxFor(w) {
  const d = w ? displayFor(wm.frame(w)) : mainDisplay();
  return { x: d.x, y: d.y, w: d.w, h: d.h };
}

// Keeps the field in view: the window moves or shrinks, or if there is no window the keyboard goes to
// the top of the display. The login screen moves its own column instead (themes/greeter).
function avoid() {
  if (!oskState.get().visible || !shellLayers()) return;
  const w = fieldWindow();
  if (lifted && lifted !== w?.id) dropLift();
  if (lifted && !remoteField() && !textFieldActive() && !String(lifted).startsWith('linux#')) dropLift();
  const box = boxFor(w);
  const size = panelSize(box);
  const top = box.y + box.h - size.height;
  const frame = w ? wm.frame(w) : null;
  const area = w ? workAreaOf(displayFor(frame)) : null;
  const r = remoteField();
  const caret = r ? (r.y >= 0 && w ? { y: frame.y + contentTop(w) + r.y, h: r.h } : null) : textFieldCaret();
  // opened by hand over a Linux app that doesn't report fields: the whole window stays in view
  const linuxWindow = !!w && String(w.id).startsWith('linux#');
  const refused = !!w && lifted === w.id && liftAsked > 0 && frame.h > liftAsked;
  const plan = fieldActive() || linuxWindow ? planAvoid({ caret, frame, area, top, refused }) : { dock: 'bottom' };
  if (plan.frame && w) {
    wm.liftForKeyboard(w.id, plan.frame);
    lifted = w.id;
    liftAsked = plan.frame.h;
    setTimeout(avoid, 400);                // did it take the size?
  }
  if (plan.dock !== oskState.get().dock || oskState.get().box?.x !== box.x) oskState.set((s) => ({ ...s, dock: plan.dock, box }));
}

function show() {
  clearTimeout(hideTimer);
  hideTimer = 0;
  const w = fieldWindow();
  if (!oskState.get().visible) {
    refreshLabels();
    oskState.set((s) => ({ ...s, visible: true, page: 0, shift: 0, dock: 'bottom', box: boxFor(w) }));
  }
  setTimeout(avoid, 40);                   // the caret is drawn after the render that gave the focus
}

function close() {
  clearTimeout(hideTimer);
  hideTimer = 0;
  dropLift();
  manual = false;
  oskState.set((s) => ({ ...s, ...CLOSED }));
}

function fieldChanged() {
  if (fieldActive()) {
    dismissed = false;
    if (opensByItself(mode(), hasKeyboard()) || manual || oskState.get().visible) show();
    return;
  }
  // moving from one field to another passes through "no field": wait a moment before closing
  if (oskState.get().visible && !manual && !hideTimer) {
    hideTimer = setTimeout(() => { hideTimer = 0; if (!fieldActive() && !manual) close(); }, 180);
  }
}

export const osk = {
  available: native,
  show() { manual = true; dismissed = false; show(); },
  hide() { dismissed = true; close(); },
  toggle() { if (oskState.get().visible) osk.hide(); else osk.show(); },
  setPage: (page) => oskState.set((s) => ({ ...s, page, shift: 0 })),
  setShift: (shift) => oskState.set((s) => ({ ...s, shift })),
  // A key of the panel. Shift for one key lets go after it; a level comes from the layout.
  press(k) {
    if (!native()) return;
    lord.input.tap(k.code, k.level || 0);
    if (k.after) lord.input.tap(k.after, 0);
    if (oskState.get().shift === 1) oskState.set((s) => ({ ...s, shift: 0 }));
    setTimeout(avoid, 60);                 // the caret may have moved to another line
  },
  nextLayout() { keyboard.next(); setTimeout(refreshLabels, 0); },
  // Hints from elsewhere. id: the window; rect: field position in it, y < 0 when unknown.
  linuxField(id, on, y, h) {
    if (id !== wm.focused()) return;
    linux = on ? { id, y, h } : null;
    fieldChanged();
  },
  appField(app, on, y, h) {
    const id = wm.focused();
    if (on && (!id || appOfWindow(id) !== app)) return;
    appField = on ? { id, y, h } : null;
    fieldChanged();
  },
};

// Called by startShell and by the login screen.
export function startOsk() {
  if (!native()) return;
  oskState.set((s) => ({ ...s, hasKeyboard: hasKeyboard() }));
  refreshLabels();
  globalThis.__lordKeyboards = (present) => {
    oskState.set((s) => ({ ...s, hasKeyboard: !!present }));
    if (present && !manual && oskState.get().visible && mode() === 'auto') close();     // plugged in: not needed
    else if (!present) fieldChanged();
  };
  onTextField(fieldChanged);
  settings.subscribe(() => { if (oskState.get().visible) setTimeout(refreshLabels, 0); });
  // hidden with its button: a click in the field that still has the focus brings it back
  addListener('pointerdown', () => setTimeout(() => { if (dismissed && fieldActive()) fieldChanged(); }, 0));
  // the field's window changed: a focus hint belongs to the window that gave it
  windows.subscribe(() => {
    const id = wm.focused();
    if (linux && linux.id !== id) { linux = null; fieldChanged(); }
    if (appField && appField.id !== id) { appField = null; fieldChanged(); }
  });
  // ❖⌃K: for whoever does have a keyboard and needs this one (a hand that can't hold keys, a pointer)
  addListener('keydown', (e) => {
    if (e.meta && e.ctrl && !e.alt && !e.repeat && String(e.base || e.key).toLowerCase() === 'k') osk.toggle();
  });
}
