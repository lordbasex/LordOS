// keymap.js — keyboard events for themes and apps.
// Normally the server does the translation with libxkbcommon (all XKB layouts,
// compose and dead keys) and here we only build the event (nativeKey). The tables
// below ('latam' and 'us') are the fallback if the server could not load XKB.

const LETTERS = { 16: 'q', 17: 'w', 18: 'e', 19: 'r', 20: 't', 21: 'y', 22: 'u', 23: 'i', 24: 'o', 25: 'p',
  30: 'a', 31: 's', 32: 'd', 33: 'f', 34: 'g', 35: 'h', 36: 'j', 37: 'k', 38: 'l',
  44: 'z', 45: 'x', 46: 'c', 47: 'v', 48: 'b', 49: 'n', 50: 'm' };

const KEYPAD = { 79: '1', 80: '2', 81: '3', 75: '4', 76: '5', 77: '6', 71: '7', 72: '8', 73: '9', 82: '0',
  83: '.', 78: '+', 74: '-', 55: '*', 98: '/' };

const NAMES = { 1: 'Escape', 14: 'Backspace', 15: 'Tab', 28: 'Enter', 96: 'Enter', 57: ' ', 103: 'ArrowUp',
  105: 'ArrowLeft', 106: 'ArrowRight', 108: 'ArrowDown', 102: 'Home', 107: 'End', 111: 'Delete', 125: 'Meta',
  126: 'Meta', 104: 'PageUp', 109: 'PageDown', 110: 'Insert', 59: 'F1', 60: 'F2', 61: 'F3', 62: 'F4', 63: 'F5',
  64: 'F6', 65: 'F7', 66: 'F8', 67: 'F9', 68: 'F10', 87: 'F11', 88: 'F12' };

// code → [normal, shift, altgr]; 'dead:x' = dead key.
const LAYOUTS = {
  us: {
    2: ['1', '!'], 3: ['2', '@'], 4: ['3', '#'], 5: ['4', '$'], 6: ['5', '%'], 7: ['6', '^'], 8: ['7', '&'],
    9: ['8', '*'], 10: ['9', '('], 11: ['0', ')'], 12: ['-', '_'], 13: ['=', '+'], 26: ['[', '{'], 27: [']', '}'],
    39: [';', ':'], 40: ["'", '"'], 41: ['`', '~'], 43: ['\\', '|'], 51: [',', '<'], 52: ['.', '>'], 53: ['/', '?'],
    86: ['<', '>'],
  },
  latam: {
    2: ['1', '!'], 3: ['2', '"'], 4: ['3', '#'], 5: ['4', '$'], 6: ['5', '%'], 7: ['6', '&'], 8: ['7', '/'],
    9: ['8', '('], 10: ['9', ')'], 11: ['0', '='], 12: ["'", '?', '\\'], 13: ['¿', '¡'],
    26: ['dead:´', 'dead:¨'], 27: ['+', '*', '~'], 39: ['ñ', 'Ñ'], 40: ['{', '[', '^'], 41: ['|', '°', '¬'],
    43: ['}', ']', '`'], 51: [',', ';'], 52: ['.', ':'], 53: ['-', '_'], 86: ['<', '>'], 16: [null, null, '@'],
  },
};

const COMPOSE = {
  '´': { a: 'á', e: 'é', i: 'í', o: 'ó', u: 'ú', A: 'Á', E: 'É', I: 'Í', O: 'Ó', U: 'Ú' },
  '¨': { a: 'ä', e: 'ë', i: 'ï', o: 'ö', u: 'ü', A: 'Ä', E: 'Ë', I: 'Ï', O: 'Ö', U: 'Ü' },
};

let layout = 'latam';
const mods = { shift: false, ctrl: false, alt: false, altgr: false, meta: false, caps: false };
let dead = null;

export const setLayout = (name) => { if (LAYOUTS[name]) layout = name; };
export const getLayout = () => layout;
export const layouts = Object.keys(LAYOUTS);

// Event from what the server translated. flags: 1 Shift, 2 Ctrl, 4 Alt, 8 AltGr, 16 Super, 32 Caps Lock,
// 128 forwarded (a Linux app already got this key as Ctrl: the shell only learns it happened).
export function nativeKey(code, value, key, chr, base, flags) {
  const down = value !== 0;
  mods.shift = !!(flags & 1); mods.ctrl = !!(flags & 2); mods.alt = !!(flags & 4);
  mods.altgr = !!(flags & 8); mods.meta = !!(flags & 16); mods.caps = !!(flags & 32);
  return { code, down, repeat: value === 2, key: key || NAMES[code] || '', char: down ? chr : '', base: down ? base : '',
    shift: mods.shift, ctrl: mods.ctrl, alt: mods.alt, altgr: mods.altgr, meta: mods.meta, caps: mods.caps, mods: modifiers(), forwarded: !!(flags & 128) };
}

// Modifiers in use, as a mask: 1 Shift, 2 Alt, 4 Ctrl, 8 Super.
export const modifiers = () => (mods.shift ? 1 : 0) | (mods.alt ? 2 : 0) | (mods.ctrl ? 4 : 0) | (mods.meta ? 8 : 0);
// Caps Lock as the last key said (every key event also carries it as `caps`): a password field's hint
export const capsLock = () => mods.caps;

// Character the key produces with Shift/AltGr/Caps Lock but without Ctrl/Alt (for shortcuts
// and so the terminal can build Ctrl+letter or Alt+letter).
function baseChar(code) {
  const table = LAYOUTS[layout][code];
  if (mods.altgr && table?.[2]) return table[2];
  if (LETTERS[code] && !table?.[0]) return (mods.shift !== mods.caps) ? LETTERS[code].toUpperCase() : LETTERS[code];
  if (table) { const c = table[mods.shift ? 1 : 0] || ''; return c.startsWith('dead:') ? c.slice(5) : c; }
  if (KEYPAD[code]) return KEYPAD[code];
  return code === 57 ? ' ' : '';
}

// Returns the keyboard event ready for themes and apps.
export function translate(code, value) {
  const down = value !== 0;
  switch (code) {
    case 42: case 54: mods.shift = down; break;
    case 29: case 97: mods.ctrl = down; break;
    case 56: mods.alt = down; break;
    case 100: mods.altgr = down; break;
    case 125: case 126: mods.meta = down; break;
    case 58: if (value === 1) mods.caps = !mods.caps; break;
    default: break;
  }
  const ev = { code, down, repeat: value === 2, key: NAMES[code] || '', char: '', base: down ? baseChar(code) : '',
    shift: mods.shift, ctrl: mods.ctrl, alt: mods.alt, altgr: mods.altgr, meta: mods.meta, caps: mods.caps, mods: modifiers() };
  if (!down || mods.ctrl || mods.alt || mods.meta) return ev;

  let ch = '';
  const table = LAYOUTS[layout][code];
  if (mods.altgr) ch = table?.[2] || '';
  else if (LETTERS[code] && !table?.[0]) {
    const upper = mods.shift !== mods.caps;
    ch = upper ? LETTERS[code].toUpperCase() : LETTERS[code];
  } else if (table) ch = table[mods.shift ? 1 : 0] || '';
  else if (KEYPAD[code]) ch = KEYPAD[code];
  else if (code === 57) ch = ' ';

  if (ch.startsWith('dead:')) {
    if (dead) { ev.char = dead; dead = null; return ev; }
    dead = ch.slice(5);
    return ev;
  }
  if (dead && ch) {
    ev.char = COMPOSE[dead][ch] || (ch === ' ' ? dead : dead + ch);
    dead = null;
  } else ev.char = ch;
  if (ev.char && !ev.key) ev.key = ev.char;
  return ev;
}
