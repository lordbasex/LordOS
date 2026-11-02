// sessionmodel.js — session restore as plain data, with no runtime or React (so it can be unit tested
// with node: server/tests/session_model_test.mjs). The runtime part is sdk/sessionrestore.jsx.
//
// settings.json keeps what was open as a list of entries:
//   { id: <typed Dock id>, f: { x, y, w, h }, max?: 1, min?: 1, snap?: <zone>, desk?: n, args?: {…} }
// (desk: the window's position in the list of desktops, left out for the first).
// Nothing else ever enters it: no Exec line, argv, environment, pid, title or Linux document path.
// args exist only for LordOS apps, only the fields below, and never "nonce" (wm.open adds a new one).
// The file can be edited by any program the user runs, so all of it is checked again when it is read.
import { parseDockId } from './dockmodel.js';
import { MAX_DESKTOPS } from './desktopmodel.js';

export const RESTORE_MODES = ['off', 'ask', 'always'];
export const RESTORE_DEFAULT = 'always';
export const MAX_WINDOWS = 12;      // a session never launches more than this

// Never reopened: tools about another window, one-shot helpers and apps of the live system.
export const NEVER = ['forcequit', 'appabout', 'installer', 'sysprobe', 'screenshots'];

const text = (v) => typeof v === 'string' && v.length > 0 && v.length <= 80 && /^[A-Za-z0-9._:-]+$/.test(v);
const path = (v) => typeof v === 'string' && v.length > 1 && v.length <= 1024 && /^[/~]/.test(v) && !v.includes('\0');
// Per LordOS app, the args it reads as "what this window shows" and how each must look. An app or a
// field that isn't here reopens without them: a Store install request, a Help tour or a Mail draft
// is an action that already happened, not a state.
const ARGS = {
  settings: { section: text, page: text },
  monitor: { tab: text, disk: text },
  disks: { disk: text },
  cleaner: { section: text },
  help: { article: text },
  notes: { slug: text },
  files: { path, place: text, view: text },
  editor: { path },
  pdf: { path },
  photos: { path },
  player: { path },
  fonts: { path },
};

export function cleanArgs(app, args) {
  const rules = ARGS[app];
  if (!rules || !args || typeof args !== 'object') return null;
  const out = {};
  for (const [k, ok] of Object.entries(rules)) if (ok(args[k])) out[k] = args[k];
  return Object.keys(out).length ? out : null;
}

const int = (v) => (Number.isFinite(v) ? Math.round(v) : null);
export function cleanFrame(f) {
  if (!f || typeof f !== 'object') return null;
  const x = int(f.x), y = int(f.y), w = int(f.w), h = int(f.h);
  if (x == null || y == null || w == null || h == null || w < 80 || h < 60 || w > 16384 || h > 16384) return null;
  return { x, y, w, h };
}

// One entry as read back from settings.json (null if anything is off). zones: the snap zones.
export function cleanEntry(e, zones = []) {
  if (!e || typeof e !== 'object' || typeof e.id !== 'string') return null;
  const p = parseDockId(e.id);
  const f = cleanFrame(e.f);
  if (!p || p.kind === 'shell' || !f || NEVER.includes(p.key)) return null;
  const out = { id: e.id, f };
  if (e.max) out.max = 1;
  if (e.min) out.min = 1;
  if (zones.includes(e.snap)) out.snap = e.snap;
  if (Number.isInteger(e.desk) && e.desk > 0 && e.desk < MAX_DESKTOPS) out.desk = e.desk;
  const args = p.kind === 'app' ? cleanArgs(p.key, e.args) : null;
  if (args) out.args = args;
  return out;
}
export const cleanList = (list, zones) => (Array.isArray(list) ? list.map((e) => cleanEntry(e, zones)).filter(Boolean).slice(-MAX_WINDOWS) : []);

// What a new session does with what the last one left. An unclean end (power cut, crash) always
// asks first, unless restore is off: the same apps may be why it fell.
export function restoreDecision(mode, clean, count) {
  if (!count || mode === 'off' || !RESTORE_MODES.includes(mode)) return 'none';
  return !clean || mode === 'ask' ? 'ask' : 'run';
}

// The launches of a saved list, in order (bottom window first). open: Map dock id → windows already
// open (Open at Login, the Terminal's live sessions): that many entries are skipped, nothing doubles.
// once(id): ids that bring back all their windows from a single launch (isolated apps, .lpk packages,
// a browser reopening its own session); the frames of the others still go to the windows that show up.
export function planRestore(list, open, once = () => false) {
  const left = new Map(open), started = new Set(), steps = [];
  for (const e of list) {
    const n = left.get(e.id) || 0;
    if (n > 0) { left.set(e.id, n - 1); continue; }
    const again = started.has(e.id);
    started.add(e.id);
    steps.push({ entry: e, launch: !(again && once(e.id)) });
  }
  return steps;
}

// A saved frame on today's screens: kept on the display it was on, or centered on the main one when
// that display is gone. displays: [{ x, y, w, h }], area(d): that display's work area; main: index.
export function fitFrame(f, displays, area, main = 0) {
  const on = (d) => f.x < d.x + d.w - 48 && f.x + f.w > d.x + 48 && f.y < d.y + d.h - 24 && f.y + 24 > d.y;
  const d = displays.find(on);
  const a = area(d || displays[main] || displays[0]);
  const w = Math.min(f.w, a.w), h = Math.min(f.h, a.h);
  if (!d) return { x: Math.round(a.x + (a.w - w) / 2), y: Math.round(a.y + (a.h - h) / 2), w, h };
  // the same limits a window has when the screen changes (windows.js): partly off the edge stays so
  return { x: Math.min(Math.max(f.x, a.x - w + 96), a.x + a.w - 96), y: Math.min(Math.max(f.y, a.y), a.y + a.h - Math.min(h, 48)), w, h };
}
