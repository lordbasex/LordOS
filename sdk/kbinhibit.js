// kbinhibit.js — a LordOS window that takes the desktop's shortcuts while it has the keyboard: Machines' VM window,
// so ❖, ⌥Tab and the rest reach the guest. The same rule as a Linux app's keyboard-shortcuts inhibitor
// (server/src/wl/kbinhibit.h, docs/SHORTCUTS.md): the person allows it once per app (the "shortcuts" permission,
// Settings → Privacy), ❖Esc turns it off for that window and back on, and the lock, the console switch and the
// media keys stay LordOS's (sdk/kbinhibitmodel.js). The dispatcher (sdk/shortcuts.js) asks shortcutsKey first.
import { routeKey, keyOutcome } from './kbinhibitmodel.js';
import { requestPermission, perms } from './privacy.js';
import { wm } from './windows.js';

let held = null;              // { win, app, off }: the window that asked, one at a time
const pressed = new Map();    // code → where its press went, so its release goes there too

// Hold the shortcuts for window `win` of app `app` while it has the keyboard; asks the first time. Returns
// the function that lets them go (the window closed, or no longer wants them).
export function inhibitShortcuts(win, app) {
  const me = { win, app, off: false };
  held = me;
  requestPermission(app, 'shortcuts');
  return () => { if (held === me) held = null; };
}

// For the dispatcher: 'app', 'taken' or null (see kbinhibitmodel.js keyOutcome).
export function shortcutsKey(e, key) {
  const h = held;
  const on = !!h && wm.focused() === h.win && perms.get(h.app, 'shortcuts') === 'allow';
  const to = keyOutcome(pressed, e, e.down && !e.repeat && on ? routeKey({ code: e.code, mods: e.mods, key }, !h.off) : null);
  if (to === 'toggle') { h.off = !h.off; return 'taken'; }
  return to;
}
