// kbinhibitmodel.js — where a key goes while a LordOS window holds the desktop's shortcuts (Machines' VM window,
// sdk/kbinhibit.js): the session's side of kbi_route in server/src/wl/kbinhibit.c, with the same keys kept.
// Pure, tested by server/tests/kbinhibit_model_test.mjs.
//
//   routeKey({ code, mods, key }, active) → 'toggle' | 'lord' | 'app'
//     mods: the dispatcher's bits (Shift 1, Alt 2, Ctrl 4, Meta 8); key: its name (sdk/shortcuts.js keyName).
//     ❖Esc toggles; the lock (❖L, ❖⌃Q), Ctrl+Alt+F1…F12 and the media and light keys stay LordOS's; off, every
//     key is LordOS's as usual.

const SHIFT = 1, ALT = 2, CTRL = 4, META = 8;
const ESC = 1;
const F_KEYS = new Set([59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88]);
// keygrab_system_key: the player, sound, screen and keyboard light, and Print Screen
const SYSTEM = new Set([164, 163, 165, 166, 200, 201, 113, 114, 115, 224, 225, 229, 230, 99]);

export function routeKey({ code, mods = 0, key = '' }, active) {
  if (code === ESC && mods === META) return 'toggle';
  if (!active) return 'lord';
  const k = String(key).toLowerCase();
  if (mods === META && k === 'l') return 'lord';
  if (mods === (META | CTRL) && k === 'q') return 'lord';
  if ((mods & (CTRL | ALT)) === (CTRL | ALT) && !(mods & META) && F_KEYS.has(code)) return 'lord';
  if (SYSTEM.has(code)) return 'lord';
  return 'app';
}

// One key event: 'app' (the window's listeners get it, the shortcuts don't), 'taken' (nobody gets it), 'toggle'
// (the press of ❖Esc: turn it off or on) or null (as if nothing held the shortcuts). A release and the repeats go
// where the press went. pressed: Map code → 'app' | 'taken', kept by the caller; route: routeKey's answer for a
// press, null when no allowed window has the keyboard.
export function keyOutcome(pressed, e, route) {
  if (!e.down) { const r = pressed.get(e.code) || null; pressed.delete(e.code); return r; }
  if (pressed.has(e.code)) return pressed.get(e.code);
  if (!route || route === 'lord') return null;
  pressed.set(e.code, route === 'toggle' ? 'taken' : 'app');
  return route;
}

export { SHIFT, ALT, CTRL, META };
