// model/launches.js — where the windows of an app launched from a display go (plain functions, tested with node:
// server/tests/workspaces_model_test.mjs). Lord Workspaces' "+ Add window", a display's quick launch ("most used on this
// screen"), its pinned apps and its usual apps each launch on one display of one activity; the app's windows must land
// there, in that activity's layout, wherever the app itself would put them.
//
// A launch leaves a pending placement: { key, desk, dk, left, t, owned, rearms }. key is the launcher id the windows
// will carry (sdk/dock.js launcherIdOf: "desktop:google-chrome.desktop", an app id), or '' when the launcher can't name
// it (a Search result that isn't an app): then any new window takes it, as before. A new window takes the newest live
// placement of its own key, or, failing that, a keyless one, or a Linux or Flatpak app's whose app has shown no window
// yet when the window's own key names no installed app ("linux:…": its app_id doesn't match the .desktop the person
// launched; "terminal": a Terminal=true app runs in a Terminal tab).
//
// Google Chrome with several profiles shows its profile picker first; the person picks, the picker closes and the real
// browser window opens. The picker takes the placement; when a window that took one closes soon after
// (within FOLLOW_MS of its arrival), the placement is armed again for FOLLOW_MS, so the next window of that app goes
// to the same display and activity. A browser window that arrived just before its picker closed (within EARLY_MS) is
// claimed at that moment. At most MAX_REARMS re-arms per launch and MAX_PENDING placements live at once: an app that
// keeps opening and closing windows can't hold a placement for long.
//
// What an app controls here is only which of its own windows land where the person just launched it: it never picks
// the display or the activity, and a window that names another app's key only takes a placement the person made for
// that app within its time (the same reach as the launcher's own "the next new window" before this, which any window
// took).
export const LAUNCH_MS = 30000;      // a launch waits this long for its app's first window (a Linux app can be slow)
export const FOLLOW_MS = 20000;      // a picker that closed: its app's next window within this goes to the same place
export const EARLY_MS = 3000;        // a window of the app that arrived this long before its picker closed is claimed
export const MAX_REARMS = 3;
export const MAX_PENDING = 9;

// a Linux app launched from its .desktop (or a Flatpak) may show windows under a key that names no installed app (its
// app_id doesn't match the entry) or, Terminal=true, in a Terminal tab: only such a window, never one that names another
// installed app or a LordOS app, takes its placement while the app has shown nothing under its own key
const LINUX = /^(desktop|flatpak):/;
const UNNAMED = /^(linux:|terminal$)/;
const live = (p, now) => p.left > 0 && now >= p.t && now - p.t < p.ttl;

// A new pending placement (the newest first). n: how many windows (the usual apps of a display: one per app).
export function addLaunch(list, { key = '', desk, dk, n = 1, now, profile = null }) {
  const p = { key: typeof key === 'string' ? key : '', profile: typeof profile === 'string' ? profile : null, desk, dk, left: Math.max(1, Math.min(9, n | 0 || 1)), t: now, ttl: LAUNCH_MS, owned: new Map(), rearms: 0, seen: false };
  return [p, ...list.filter((x) => live(x, now) || x.owned.size)].slice(0, MAX_PENDING);
}

// The placement a new window takes, or null. key: its launcher id. The list is changed in place (left, owned).
export function claim(list, id, key, now) {
  const ok = list.filter((p) => live(p, now));
  const p = ok.find((x) => x.key && x.key === key)
    || ok.find((x) => !x.key)
    || (UNNAMED.test(String(key)) ? ok.find((x) => LINUX.test(x.key) && !x.seen) : null);
  if (!p) return null;
  p.left--;
  p.seen = p.seen || p.key === key;
  p.owned.set(id, now);
  return p;
}

// A window went away. If it took a placement and closed within FOLLOW_MS of arriving (a profile picker), the placement
// waits FOLLOW_MS for the app's next window. born: id → when it arrived, for the windows still open; returns the ids of
// windows of the same key that arrived in the EARLY_MS before (already open when the picker closed) to claim now.
// keyOf: id → launcher id of an open window.
export function gone(list, id, now, born = new Map(), keyOf = () => '', placed = () => false) {
  const p = list.find((x) => x.owned.has(id));
  if (!p) return { p: null, early: [] };
  const at = p.owned.get(id);
  p.owned.delete(id);
  // a keyless launch has no app to follow: a closed window never arms it again
  if (!p.key || now - at > FOLLOW_MS || now < at || p.rearms >= MAX_REARMS) return { p: null, early: [] };
  p.rearms++;
  const early = [];
  for (const [wid, t] of born) {
    if (wid === id || p.owned.has(wid) || placed(wid) || now - t > EARLY_MS || t > now) continue;
    if (keyOf(wid) === p.key) early.push(wid);
  }
  const take = early.slice(0, 1);
  for (const wid of take) p.owned.set(wid, now);
  if (!take.length) { p.left = Math.max(p.left, 1); p.t = now; p.ttl = FOLLOW_MS; }
  return { p, early: take };
}

// The placements still waiting (for the tests and the bar's state).
export const pending = (list, now) => list.filter((p) => live(p, now));
