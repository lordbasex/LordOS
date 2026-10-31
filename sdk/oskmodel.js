// oskmodel.js — the on-screen keyboard's logic with nothing on screen (server/tests/osk_model_test.mjs).
//
// The layout is not data of our own: rows are lists of Linux key codes, and what each key shows comes
// from the active XKB layout (lord.input.keyLabels, server/src/keyboard.c), so the keyboard follows it
// like a physical one. `labels` = { altgr, keys: { code: [plain, shift, altgr, altgr + shift] } }.
// A key here is { code, level, label } ready for lord.input.tap(code, level); level 1 is Shift, 2 AltGr.

export const KEY = { BACKSPACE: 14, TAB: 15, ENTER: 28, ESC: 1, SPACE: 57, UP: 103, LEFT: 105, RIGHT: 106, DOWN: 108, COMMA: 51, DOT: 52 };

const range = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
const ROW1 = range(16, 25), ROW2 = range(30, 38), ROW3 = range(44, 50);
const DIGITS = range(2, 11);
// keys beside the letters: they join a row only when the layout puts a letter or an accent on them
// (ñ, ç, Arabic letters); plain punctuation stays on the symbol pages
const EXTRA1 = [26, 27], EXTRA2 = [39, 40], EXTRA3 = [86, 43, 51, 52, 53];

// Without XKB (no xkb-data installed): the US layout.
export function fallbackLabels() {
  const keys = {};
  const put = (codes, plain, shifted) => codes.forEach((c, i) => { keys[c] = [plain[i], shifted[i] || plain[i], '', '']; });
  put(ROW1, 'qwertyuiop', 'QWERTYUIOP');
  put(ROW2, 'asdfghjkl', 'ASDFGHJKL');
  put(ROW3, 'zxcvbnm', 'ZXCVBNM');
  put(DIGITS, '1234567890', '!@#$%^&*()');
  put([12, 13, 26, 27, 39, 40, 41, 43, 51, 52, 53], "-=[];'`\\,./", '_+{}:"~|<>?');
  keys[KEY.SPACE] = [' ', ' ', '', ''];
  return { altgr: false, keys };
}

const isDead = (s) => typeof s === 'string' && s.startsWith('dead:');
const shown = (s) => (isDead(s) ? s.slice(5) : s);
const lettery = (s) => !!s && (isDead(s) || /\p{L}/u.test(s));

// The key at this level, or nothing if the layout has no symbol there.
function keyAt(labels, code, level) {
  const row = labels.keys[code];
  const s = row && (row[level] || (level === 1 ? row[0] : ''));
  return s ? { code, level: row[level] ? level : 0, label: shown(s) } : null;
}

// Where the active layout has this symbol. A symbol that exists only as a dead key is that key and
// then Space (`after`), as a person types it.
export function keyFor(labels, ch) {
  const levels = labels.altgr ? [0, 1, 2, 3] : [0, 1];
  let dead = null;
  for (const level of levels) {
    for (const [code, row] of Object.entries(labels.keys)) {
      if (row[level] === ch) return { code: Number(code), level, label: ch };
      if (!dead && row[level] === `dead:${ch}`) dead = { code: Number(code), level, label: ch, after: KEY.SPACE };
    }
  }
  return dead;
}

// Rows of the letters page. shift: 0 off, 1 next key, 2 locked.
export function letterRows(labels, shift = 0) {
  const level = shift ? 1 : 0;
  const row = (codes, extra) => [...codes, ...extra.filter((c) => lettery(labels.keys[c]?.[0]))]
    .map((c) => keyAt(labels, c, level)).filter(Boolean);
  return [row(ROW1, EXTRA1), row(ROW2, EXTRA2), row(ROW3, EXTRA3)];
}

const typable = (labels, chars) => [...chars].map((c) => keyFor(labels, c)).filter(Boolean);

// The digits row: the real digits wherever the layout has them, or else what that row types.
function digitRow(labels) {
  return DIGITS.map((code, i) => keyFor(labels, '1234567890'[i]) || keyAt(labels, code, 0)).filter(Boolean);
}

// Page 1: numbers and the usual symbols. Page 2: the rest, then the keys that move the caret. Only
// what this layout can type is offered.
export function symbolRows(labels, page) {
  if (page === 1) return [digitRow(labels), typable(labels, '@#$%&-+()/'), typable(labels, '*"\':;!?')];
  return [typable(labels, '~`|\\^_=[]{}'), typable(labels, '<>€£°¿¡§¬'), [
    { code: KEY.TAB, level: 0, name: 'tab' },
    { code: KEY.LEFT, level: 0, label: '←' }, { code: KEY.DOWN, level: 0, label: '↓' },
    { code: KEY.UP, level: 0, label: '↑' }, { code: KEY.RIGHT, level: 0, label: '→' },
    { code: KEY.ESC, level: 0, name: 'esc' },
  ]];
}

export const punctuation = (labels) => [keyFor(labels, ',') || { code: KEY.COMMA, level: 0, label: ',' },
  keyFor(labels, '.') || { code: KEY.DOT, level: 0, label: '.' }];

// A display's keyboard: tablet proportions, the whole width up to a limit, four rows.
export function panelSize(box) {
  const height = Math.round(Math.max(236, Math.min(336, box.h * 0.4)));
  const width = Math.min(box.w, Math.max(760, Math.round(box.w * 0.8)), Math.round(height * 3.6));
  const gap = height < 280 ? 6 : 8, pad = gap + 2;
  return { width, height, gap, pad, keyH: Math.floor((height - pad * 2 - gap * 3) / 4) };
}

// mode: Settings → Keyboard ('off' | 'auto' | 'always').
export const opensByItself = (mode, hasKeyboard) => mode === 'always' || (mode !== 'off' && !hasKeyboard);

const MARGIN = 12;

// What to do so the field stays in view. caret: { y, h } on screen or null when only the window is
// known (a Linux app that doesn't say where its caret is: then the whole window); frame: the window
// with the field or null (a shell surface such as Search); area: its work area; top: the keyboard's
// top edge. → { dock: 'bottom' | 'top', frame?: where the window goes }.
export function planAvoid({ caret, frame, area, top, minHeight = 160 }) {
  const need = (caret ? caret.y + caret.h + MARGIN : frame ? frame.y + frame.h : 0) - top;
  if (need <= 0) return { dock: 'bottom' };
  if (!frame) return { dock: 'top' };                   // nothing to move: the keyboard goes up
  const room = frame.y - area.y;
  if (need <= room) return { dock: 'bottom', frame: { ...frame, y: frame.y - need } };
  const height = Math.max(minHeight, top - area.y);
  if (height >= frame.h && !room) return caret ? { dock: 'top' } : { dock: 'bottom' };   // it can't do more
  return { dock: 'bottom', frame: { ...frame, y: area.y, h: Math.min(frame.h, height) } };
}
