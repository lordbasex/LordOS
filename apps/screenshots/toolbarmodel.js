// toolbarmodel.js — the Screenshots toolbar (❖⇧5): its options, its modes, the keyboard's way through it and the
// remembered area with its handles. Pure; tested by server/tests/screenshots_toolbar_model_test.mjs.
//
//   normalizeOptions(raw)                 settings.screenshots → every option, valid (an older "clipboard": true is
//                                         "Save to: Clipboard")
//   MODES, isRecordMode(m), nextMode(m, ±1)
//   saveDirFor(saveTo, dirs, folder, video, otherDir)   the folder a capture (or a recording) goes to; null: the clipboard alone
//   barStops(), stepStop(stops, cur, ±1)  the toolbar's items in the order ← → go through them
//   keepRect(rect, display), restoreRect(saved, displays)   "Remember last selection"
//   hitHandle(r, x, y), adjustRect(r, grab, dx, dy, bounds) the selection's eight handles and its inside
//   PRINT_KEY, printKeyLabel(a)           what the Print Screen key does (GitHub #202) and its row in Settings → Keyboard
//   refusal(err)                          why a capture was not taken: { code, permission } (GitHub #219)

export const CAPTURE_MODES = ['full', 'window', 'area'];
export const RECORD_MODES = ['recordFull', 'recordArea'];
export const MODES = [...CAPTURE_MODES, ...RECORD_MODES];
// Save to (as macOS's ⇧⌘5 Options menu): a folder, the clipboard alone, Mail (a new message with the capture attached),
// the markup editor (opened on it at once), or another folder chosen once and remembered (otherDir). Mail and the editor
// keep the file where captures go by default (Pictures/Screenshots).
export const SAVE_TO = ['desktop', 'documents', 'pictures', 'clipboard', 'mail', 'editor', 'other'];
export const TIMERS = [0, 5, 10];
// What the Print Screen key does (GitHub #202): the whole screen (as before), an area, a window, the toolbar, or nothing
// (the key then goes to the app in front, as any key LordOS doesn't take).
export const PRINT_KEY = ['full', 'area', 'window', 'toolbar', 'none'];

export const OPTION_DEFAULTS = Object.freeze({
  mode: 'area', saveTo: 'pictures', otherDir: '', timer: 0, mic: false, micSource: '', thumbnail: true, remember: false, pointer: true, lastRect: null, barPos: null, printKey: 'full',
});

const finite = (v) => typeof v === 'number' && Number.isFinite(v);
function validRect(r) {
  if (!r || typeof r !== 'object' || !finite(r.x) || !finite(r.y) || !finite(r.w) || !finite(r.h) || r.w < 3 || r.h < 3) return null;
  return { x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.w), h: Math.round(r.h), display: typeof r.display === 'string' ? r.display : '' };
}
function validPos(p) {
  return p && typeof p === 'object' && typeof p.display === 'string' && finite(p.dx) && finite(p.dy) ? { display: p.display, dx: p.dx, dy: p.dy } : null;
}

// Every option with a valid value. Before the toolbar (0.8.9.x) the panel saved { timer, clipboard, mic }: "only copy
// to the clipboard" is now Save to → Clipboard, read here so nobody's choice is lost (no settings migration needed).
export function normalizeOptions(raw) {
  const r = raw && typeof raw === 'object' ? raw : {};
  const saveTo = SAVE_TO.includes(r.saveTo) ? r.saveTo : r.clipboard === true ? 'clipboard' : OPTION_DEFAULTS.saveTo;
  return {
    mode: MODES.includes(r.mode) ? r.mode : OPTION_DEFAULTS.mode,
    saveTo,
    otherDir: typeof r.otherDir === 'string' && r.otherDir.startsWith('/') && !r.otherDir.split('/').includes('..') ? r.otherDir.replace(/\/+$/, '') : '',
    timer: TIMERS.includes(r.timer) ? r.timer : 0,
    mic: r.mic === true,
    micSource: typeof r.micSource === 'string' ? r.micSource : '',
    thumbnail: r.thumbnail !== false,
    remember: r.remember === true,
    pointer: r.pointer !== false,
    lastRect: validRect(r.lastRect),
    barPos: validPos(r.barPos),
    printKey: PRINT_KEY.includes(r.printKey) ? r.printKey : OPTION_DEFAULTS.printKey,
  };
}

// The Print Screen key's row in Settings → Keyboard: the same label as the shortcut that does the same thing (so the
// whole screen shares ❖⇧3's row), a window its own; null when the key does nothing (not listed).
export function printKeyLabel(a) {
  if (a === 'area') return 'shell.shortcuts.captureArea';
  if (a === 'window') return 'shell.shortcuts.captureWindow';
  if (a === 'toolbar') return 'shell.shortcuts.capturePanel';
  if (a === 'none') return null;
  return 'shell.shortcuts.captureFull';
}

// Why nothing was captured (GitHub #219): err is what the capture threw (a code line such as "media.captureFailed", or
// { code, params }), or nothing when the permission prompt was answered no. permission: the reason is the screen
// permission or a restriction, so Settings → Privacy can change it; any other failure is told as it is.
export function refusal(err) {
  if (!err) return { code: 'media.denied.screen', permission: true };
  const code = typeof err === 'object' ? String(err.code || '') : String(err);
  if (!code) return { code: 'media.captureFailed', permission: false };
  return { code, permission: code.startsWith('media.denied.') || code.startsWith('fw.restricted') };
}

export const isRecordMode = (m) => RECORD_MODES.includes(m);
export const primaryOf = (m) => (isRecordMode(m) ? 'record' : 'capture');

export function nextMode(mode, step) {
  const i = MODES.indexOf(mode);
  if (i < 0) return MODES[0];
  return MODES[(i + (step < 0 ? MODES.length - 1 : 1)) % MODES.length];
}

// dirs: media.userDirs(); folder: the Screenshots folder's translated name, inside Pictures; otherDir: "Other
// Location…"'s folder. A recording never goes to the clipboard: with Clipboard chosen it is saved where captures go by
// default (Mail and the editor too).
export function saveDirFor(saveTo, dirs, folder, video = false, otherDir = '') {
  const d = dirs || {};
  if (saveTo === 'clipboard' && !video) return null;
  if (saveTo === 'desktop' && d.desktop) return d.desktop;
  if (saveTo === 'documents' && d.documents) return d.documents;
  if (saveTo === 'other' && otherDir) return otherDir;
  return `${d.pictures || `${d.home || ''}/Pictures`}/${folder}`;
}

// The toolbar's items, start to end, as the keyboard visits them (← →; Enter or Space presses the one it is on).
export const barStops = () => ['close', ...MODES, 'color', 'text', 'options', 'primary'];
export function stepStop(stops, cur, step) {
  const i = stops.indexOf(cur);
  if (i < 0) return step < 0 ? stops[stops.length - 1] : stops[0];
  return stops[Math.max(0, Math.min(stops.length - 1, i + (step < 0 ? -1 : 1)))];
}

// ---- "Remember last selection" ----

// rect in logical pixels of the global space; display: the one it was drawn on (its name), so it comes back there
export function keepRect(rect, display) {
  const r = validRect(rect);
  return r ? { ...r, display: display && typeof display.name === 'string' ? display.name : '' } : null;
}
// The remembered area, if it still fits on its display (or on the screen, with one display): else null, and a new one
// is drawn.
export function restoreRect(saved, displays, screen = null) {
  const r = validRect(saved);
  if (!r) return null;
  const list = Array.isArray(displays) ? displays.filter((d) => d && d.w > 0 && d.h > 0) : [];
  const d = list.length ? (list.find((x) => (x.name || '') === r.display) || (list.length === 1 ? list[0] : null))
    : screen && screen.width > 0 ? { x: 0, y: 0, w: screen.width, h: screen.height } : null;
  if (!d) return null;
  if (r.x < d.x || r.y < d.y || r.x + r.w > d.x + d.w || r.y + r.h > d.y + d.h) return null;
  return { x: r.x, y: r.y, w: r.w, h: r.h };
}

// ---- the selection's handles ----

export const HANDLES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];
const MIN = 8;

export function handlePoints(r) {
  const cx = r.x + r.w / 2, cy = r.y + r.h / 2, x1 = r.x + r.w, y1 = r.y + r.h;
  return [
    { id: 'nw', x: r.x, y: r.y }, { id: 'n', x: cx, y: r.y }, { id: 'ne', x: x1, y: r.y }, { id: 'e', x: x1, y: cy },
    { id: 'se', x: x1, y: y1 }, { id: 's', x: cx, y: y1 }, { id: 'sw', x: r.x, y: y1 }, { id: 'w', x: r.x, y: cy },
  ];
}

// What a press at (x, y) grabs: a handle's id, 'inside' (moves the whole area) or null (a new area is drawn)
export function hitHandle(r, x, y, tol = 8) {
  if (!r) return null;
  for (const p of handlePoints(r)) if (Math.abs(x - p.x) <= tol && Math.abs(y - p.y) <= tol) return p.id;
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h ? 'inside' : null;
}

// The area after dragging what was grabbed by (dx, dy), kept inside bounds ({ x, y, w, h }: its display). An edge
// dragged past the opposite one stops MIN px before it.
export function adjustRect(r, grab, dx, dy, bounds) {
  const b = bounds || { x: -Infinity, y: -Infinity, w: Infinity, h: Infinity };
  const bx1 = b.x + b.w, by1 = b.y + b.h;
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  if (grab === 'inside') {
    return { x: clamp(r.x + dx, b.x, bx1 - r.w), y: clamp(r.y + dy, b.y, by1 - r.h), w: r.w, h: r.h };
  }
  if (!HANDLES.includes(grab)) return { ...r };
  let x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
  if (grab.includes('w')) x0 = clamp(x0 + dx, b.x, x1 - MIN);
  if (grab.includes('e')) x1 = clamp(x1 + dx, x0 + MIN, bx1);
  if (grab.includes('n')) y0 = clamp(y0 + dy, b.y, y1 - MIN);
  if (grab.includes('s')) y1 = clamp(y1 + dy, y0 + MIN, by1);
  return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}
