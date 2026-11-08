// dockmodel.js — the Dock's pinned list as plain data, with no runtime or React (so it can be
// unit tested with node: server/tests/dock_model_test.mjs).
//
// A pinned item is a typed id, so the three kinds of app can never collide:
//   "app:<id>"      a LordOS app bundled with the theme (apps/<id>), opened with wm.open
//   "lpk:<id>"      an installed .lpk package, launched with lord.apps.launchPackage(id)
//   "desktop:<id>"  a Linux app's .desktop entry, launched with lord.apps.launch(id)
// Lists written before 0.8.1 hold untyped ids ("files", "store"); normalizePinned() migrates them.
//
// Positions passed to moveItem/pinItem count only the items the Dock shows (isShown): an
// uninstalled app keeps its place in settings.json, hidden, and comes back if it is reinstalled.
import { onDesktop } from './desktopmodel.js';

//   "shell:launcher" the theme's app launcher (the only "shell" item): it moves like an app
export const DOCK_KINDS = ['app', 'lpk', 'desktop', 'shell'];
export const LAUNCHER = 'shell:launcher';
// The Dock's rules:
//   - Files, Settings and the app launcher can't be removed (no «Remove from Dock»; dragging them out
//     or onto the Trash puts them back) but move freely, like any other app (0.8.8.6, issue #62: Files
//     used to be fixed in the first place); Files is first in the theme's defaults;
//   - the Trash is always the last one, drawn by the theme after a separator (not a pinned item);
//   - every other app is added, moved and removed freely.
// "First" and "last" follow the Dock's reading order (the theme mirrors a bottom Dock in RTL).
export const FIRST = 'app:files';
export const LOCKED = [FIRST, LAUNCHER, 'app:settings'];

// The same rules as the server: lpk_valid_id (server/src/lpk.c) and valid_id (server/src/apps.c).
const APP_RE = /^[a-z][a-z0-9-]{0,39}$/;
const LPK_RE = /^[a-z][a-z0-9-]{1,39}$/;
const DESKTOP_RE = /^[A-Za-z0-9_][A-Za-z0-9._-]{0,61}$/;
const validKey = {
  app: (k) => APP_RE.test(k),
  shell: (k) => k === 'launcher',
  lpk: (k) => LPK_RE.test(k),
  desktop: (k) => DESKTOP_RE.test(k) && !k.includes('..') && !k.startsWith('lpk-'),
};

// "lpk:notes" → { kind: 'lpk', key: 'notes' }; null if it isn't a valid typed id.
export function parseDockId(id) {
  if (typeof id !== 'string') return null;
  const i = id.indexOf(':');
  if (i <= 0) return null;
  const kind = id.slice(0, i), key = id.slice(i + 1);
  return DOCK_KINDS.includes(kind) && validKey[kind](key) ? { kind, key } : null;
}

export const dockId = (kind, key) => (DOCK_KINDS.includes(kind) && typeof key === 'string' && validKey[kind](key) ? `${kind}:${key}` : null);
export const isDockId = (id) => parseDockId(id) !== null;

// One stored entry → a typed id, or null to drop it. classify(untypedId) says what an old untyped
// id is now: 'app', 'lpk' or null (unknown: kept as a LordOS app id if it looks like one).
export function toTypedId(id, classify = () => null) {
  if (typeof id !== 'string' || !id) return null;
  if (parseDockId(id)) return id;
  if (id.includes(':')) return null;                    // "linux:…" (a window with no .desktop) or junk
  const kind = classify(id) || 'app';
  return dockId(kind, id);
}

// The stored list → { list, changed }: typed, valid and without repeats.
export function normalizePinned(list, classify) {
  if (!Array.isArray(list)) return { list: [], changed: list != null };
  const out = [];
  for (const raw of list) {
    const id = toTypedId(raw, classify);
    if (id && !out.includes(id)) out.push(id);
  }
  return { list: out, changed: out.length !== list.length || out.some((x, i) => x !== list[i]) };
}

// Inserts id before the shown item at position index (null, past the end or not a number: after the
// last item). list must not contain id.
function insertAt(list, id, index, isShown) {
  const shown = list.filter(isShown);
  const k = Number.isFinite(+index) && index !== null ? Math.max(0, Math.round(+index)) : shown.length;
  if (k >= shown.length) return [...list, id];
  const at = list.indexOf(shown[k]);
  return [...list.slice(0, at), id, ...list.slice(at)];
}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// Moves a pinned item to position toIndex among the shown items (counted without it).
export function moveItem(list, id, toIndex, isShown = () => true) {
  if (!list.includes(id)) return list;
  const next = insertAt(list.filter((x) => x !== id), id, toIndex, isShown);
  return same(next, list) ? list : next;
}

// Pins id at position atIndex among the shown items (at the end without it); already pinned: moves.
export function pinItem(list, id, atIndex, isShown = () => true) {
  if (!isDockId(id)) return list;
  if (list.includes(id)) return atIndex == null ? list : moveItem(list, id, atIndex, isShown);
  return insertAt(list, id, atIndex, isShown);
}

export const isLocked = (id, locked = LOCKED) => locked.includes(id);
// Can the item change place? Every pinned item, Files included (the Trash is not in the list at all).
export const isMovable = (id) => isDockId(id);
export const unpinItem = (list, id, locked = LOCKED) => (list.includes(id) && !isLocked(id, locked) ? list.filter((x) => x !== id) : list);

// Puts back every required item missing from list (an older settings file, or one edited by
// hand), each after the item that comes before it in defaults (Files, first in the defaults, goes
// back first); an item already there keeps the place the person gave it.
export function ensureItems(list, required = LOCKED, defaults = []) {
  let out = list;
  for (const id of required) {
    if (out.includes(id) || !isDockId(id)) continue;
    const d = defaults.indexOf(id);
    let at = out.length;
    if (d >= 0) {
      at = 0;
      for (let i = d - 1; i >= 0; i--) { const k = out.indexOf(defaults[i]); if (k >= 0) { at = k + 1; break; } }
    }
    out = [...out.slice(0, at), id, ...out.slice(at)];
  }
  return out;
}

// Quick actions at the top of an app's Dock menu: [{ id, label, args }], from its manifest
// ("dockActions") or its module (registerDockActions). Anything malformed is left out.
const ACTION_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$/;
const plainArgs = (a) => {
  if (a == null) return {};
  if (typeof a !== 'object' || Array.isArray(a)) return null;
  const out = {};
  for (const [k, v] of Object.entries(a)) {
    if (!/^[A-Za-z_][A-Za-z0-9_]{0,31}$/.test(k) || k === 'nonce') return null;
    if (v === null || ['string', 'number', 'boolean'].includes(typeof v)) out[k] = v;
    else return null;
  }
  return out;
};
export function cleanActions(list, max = 40) {
  if (!Array.isArray(list)) return [];
  const out = [];
  for (const a of list) {
    if (!a || typeof a !== 'object' || !ACTION_RE.test(String(a.id ?? '')) || out.some((x) => x.id === a.id)) continue;
    const label = typeof a.label === 'string' ? a.label : '';
    const args = plainArgs(a.args);
    if (!label || !args) continue;
    out.push({ id: a.id, label, args, new: !!a.new });
    if (out.length >= max) break;
  }
  return out;
}

// ---- Hiding (Settings → Desktop & Dock; issue #46) ----
// Three modes, stored as two booleans so that a settings file read by an older LordOS still means
// something close (it doesn't know dockIntellihide: the Dock just hides, as with 'always'):
//   'never'    dockAutohide false                        the Dock stays; windows don't go under it
//   'overlap'  dockAutohide true, dockIntellihide true   it hides only while a window covers its place
//   'always'   dockAutohide true, dockIntellihide false  it hides until the pointer touches the edge
// Both hiding modes give windows the whole display (the theme's work area), and in both the pointer
// at the edge brings the Dock back. dockIntellihide is kept when 'never' is chosen, so the Dock's
// keyboard shortcut (which flips dockAutohide) goes back to the mode the person picked.
export const DOCK_HIDE_MODES = ['never', 'overlap', 'always'];
export const dockHideMode = (p) => (!p || p.dockAutohide !== true ? 'never' : p.dockIntellihide === true ? 'overlap' : 'always');
export function dockHideSettings(mode) {
  if (mode === 'overlap') return { dockAutohide: true, dockIntellihide: true };
  if (mode === 'always') return { dockAutohide: true, dockIntellihide: false };
  return { dockAutohide: false };
}

// Does a window cover the Dock's place? rect: where the Dock is when shown, { x0, y0, x1, y1 } in
// global coordinates (on its own display, so windows on other displays never touch it). list: every
// window, bottom to top; active: the desktop in view. Only what is seen counts: not minimized, on this
// desktop, and not picture-in-picture (small, floating over everything, and it can be moved aside).
// frameOf(w) → { x, y, w, h }: where the window is drawn (a maximized one fills its work area: wm.frame).
// displayOf(w) → { x, y, w, h }: the display of a fullscreen window, which covers all of it.
const meets = (f, r) => !!f && f.w > 0 && f.h > 0 && f.x < r.x1 && r.x0 < f.x + f.w && f.y < r.y1 && r.y0 < f.y + f.h;
export function dockCovered(list, active, rect, { frameOf = (w) => w, displayOf = () => null } = {}) {
  if (!rect || !Array.isArray(list) || !(rect.x1 > rect.x0 && rect.y1 > rect.y0)) return false;
  for (const w of list) {
    if (!w || w.minimized || w.pip || !onDesktop(w, active)) continue;
    if (meets(w.fullscreen ? displayOf(w) : frameOf(w), rect)) return true;
  }
  return false;
}
