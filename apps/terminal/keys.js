// keys.js — Terminal shortcuts. "Super" is the main modifier; almost all of them
// tienen además una variante Ctrl+Shift, como en las terminales de Linux.
// Las teclas se identifican por su posición física (código evdev), no por la letra que
// produce la distribución, para que funcionen igual en latam y us.
//
// Configurable (Preferences → Keys):
//   · terminal.shortcuts { acción: [combinaciones] } reemplaza los atajos de una acción.
//   · terminal.keyMap y perfil.keyMap: [{ combo, action, param }] con action = 'text' (enviar
//     texto, admite \n \t \e \a \\ \xHH), 'escape' (Esc + texto), 'hex' ("0x1b 0x5b 0x41"),
//     'ignore' o el nombre de una acción. Las del perfil ganan a las globales, y ambas a los atajos.
import { createStore, t } from '@lordos/sdk';
import { terminalSettings } from './profiles.js';

const CODE_KEYS = {
  2: '1', 3: '2', 4: '3', 5: '4', 6: '5', 7: '6', 8: '7', 9: '8', 10: '9', 11: '0', 12: '-', 13: '=',
  26: '[', 27: ']', 39: ';', 40: "'", 41: '`', 43: '\\', 51: ',', 52: '.', 53: '/', 57: 'Space',
  16: 'q', 17: 'w', 18: 'e', 19: 'r', 20: 't', 21: 'y', 22: 'u', 23: 'i', 24: 'o', 25: 'p',
  30: 'a', 31: 's', 32: 'd', 33: 'f', 34: 'g', 35: 'h', 36: 'j', 37: 'k', 38: 'l',
  44: 'z', 45: 'x', 46: 'c', 47: 'v', 48: 'b', 49: 'n', 50: 'm',
};

// Combinación del evento, ej. "Super+Shift+d", "Ctrl+Tab", "F12".
export function comboOf(e) {
  const key = e.key && e.key.length > 1 && e.key !== 'Meta' ? e.key : CODE_KEYS[e.code];
  if (!key) return null;
  const parts = [];
  if (e.meta) parts.push('Super');
  if (e.ctrl) parts.push('Ctrl');
  if (e.alt) parts.push('Alt');
  if (e.shift) parts.push('Shift');
  parts.push(key);
  return parts.join('+');
}

export const DEFAULT_BINDINGS = {
  newTab: ['Super+t', 'Ctrl+Shift+t'],
  newWindow: ['Super+n', 'Ctrl+Shift+n'],
  closePane: ['Super+w', 'Ctrl+Shift+w'],
  closeTab: ['Super+Shift+w'],
  splitRight: ['Super+d', 'Ctrl+Shift+d'],
  splitDown: ['Super+Shift+d', 'Ctrl+Shift+e'],
  nextPane: ['Super+]', 'Ctrl+Alt+]'],
  prevPane: ['Super+[', 'Ctrl+Alt+['],
  paneLeft: ['Super+Alt+ArrowLeft', 'Ctrl+Alt+ArrowLeft'],
  paneRight: ['Super+Alt+ArrowRight', 'Ctrl+Alt+ArrowRight'],
  paneUp: ['Super+Alt+ArrowUp', 'Ctrl+Alt+ArrowUp'],
  paneDown: ['Super+Alt+ArrowDown', 'Ctrl+Alt+ArrowDown'],
  nextTab: ['Super+Shift+]', 'Ctrl+Tab', 'Ctrl+PageDown'],
  prevTab: ['Super+Shift+[', 'Ctrl+Shift+Tab', 'Ctrl+PageUp'],
  find: ['Super+f', 'Ctrl+Shift+f'],
  findNext: ['Super+g'],
  findPrev: ['Super+Shift+g'],
  copy: ['Super+c', 'Ctrl+Shift+c'],
  paste: ['Super+v', 'Ctrl+Shift+v', 'Shift+Insert'],
  pasteHistory: ['Super+Shift+h'],
  selectAll: ['Super+a', 'Ctrl+Shift+a'],
  selectOutput: ['Super+Shift+a'],
  clearBuffer: ['Super+k', 'Ctrl+Shift+k'],
  zoomIn: ['Super+=', 'Super+Shift+=', 'Ctrl+Shift+='],
  zoomOut: ['Super+-', 'Ctrl+Shift+-'],
  zoomReset: ['Super+0', 'Ctrl+Shift+0'],
  maximizePane: ['Super+Shift+Enter'],
  fullscreen: ['Super+Enter'],
  prevMark: ['Super+Shift+ArrowUp'],
  nextMark: ['Super+Shift+ArrowDown'],
  setMark: ['Super+Shift+m'],
  scrollTop: ['Super+Home', 'Shift+Home'],
  scrollBottom: ['Super+End', 'Shift+End'],
  scrollPageUp: ['Super+PageUp', 'Shift+PageUp'],
  scrollPageDown: ['Super+PageDown', 'Shift+PageDown'],
  scrollLineUp: ['Super+ArrowUp', 'Ctrl+Shift+ArrowUp'],
  scrollLineDown: ['Super+ArrowDown', 'Ctrl+Shift+ArrowDown'],
  broadcast: ['Super+Alt+i'],
  autocomplete: ['Super+;'],
  composer: ['Super+Shift+.'],
  openQuickly: ['Super+Shift+o'],
  openProfiles: ['Super+o'],
  filesMode: ['Super+Shift+f'],
  preferences: ['Super+,'],
  toolbelt: ['Super+Shift+b'],
  timestamps: ['Super+Shift+e'],
  editTitle: ['Super+i'],
  reset: ['Super+r'],
  copyMode: ['Super+Shift+c'],
  instantReplay: ['Super+Alt+b'],
  commandHistory: ['Super+Shift+;'],
  undoClose: ['Super+z'],
  alertNextMark: ['Super+Alt+a'],
  equalize: ['Super+Alt+='],
  paneToTab: ['Super+Alt+t'],
  moveTabLeft: ['Super+Shift+ArrowLeft'],
  moveTabRight: ['Super+Shift+ArrowRight'],
  logSession: ['Super+Alt+l'],
  exportText: ['Super+s'],
  pasteOneLine: ['Super+Alt+v'],
  pasteEscaped: ['Super+Shift+v'],
  saveArrangement: ['Super+Shift+s'],
  restoreArrangement: ['Super+Shift+r'],
};

// Atajos vigentes: los de fábrica con los cambios del usuario.
export function bindings() {
  return { ...DEFAULT_BINDINGS, ...(terminalSettings().shortcuts || {}) };
}

let lookup = null, lookupFor = null;
function table() {
  const custom = terminalSettings().shortcuts || {};
  if (lookup && lookupFor === custom) return lookup;
  lookup = new Map();
  for (let i = 1; i <= 9; i++) lookup.set(`Super+${i}`, `tab${i}`);
  for (const [action, combos] of Object.entries({ ...DEFAULT_BINDINGS, ...custom })) for (const c of combos) lookup.set(c, action);
  lookupFor = custom;
  return lookup;
}

// Zoom goes by the character the key types, as in iTerm2, not by its US position: on a Latin American
// keyboard "+" and "-" sit where a US one has "]" and "/" (❖] is the next pane, ❖/ nothing). Unless the
// user set their own zoom shortcuts.
const ZOOM_CHARS = { '+': 'zoomIn', '=': 'zoomIn', '-': 'zoomOut' };
function zoomByChar(e) {
  const own = terminalSettings().shortcuts || {};
  if (own.zoomIn || own.zoomOut) return null;
  const mods = (e.meta && !e.ctrl && !e.alt) || (e.ctrl && e.shift && !e.meta && !e.alt);
  return mods ? ZOOM_CHARS[e.key] || null : null;
}

export const actionOf = (e) => zoomByChar(e) || table().get(comboOf(e)) || null;

// Asignación de tecla del perfil o global para este evento (o null).
export function keyMappingOf(e, profile) {
  const combo = comboOf(e);
  if (!combo) return null;
  return (profile?.keyMap || []).find((m) => m.combo === combo) || (terminalSettings().keyMap || []).find((m) => m.combo === combo) || null;
}

// Texto a enviar al shell para una asignación 'text' / 'escape' / 'hex' (null si no envía).
export function mappingBytes(m) {
  if (m.action === 'text') {
    return String(m.param || '').replace(/\\(x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]+\}|.)/g, (_, c) => {
      if (c[0] === 'x') return String.fromCharCode(parseInt(c.slice(1), 16));
      if (c[0] === 'u') return String.fromCodePoint(parseInt(c.slice(2, -1), 16));
      return { n: '\n', r: '\r', t: '\t', e: '\x1b', a: '\x07', b: '\x08', '\\': '\\' }[c] ?? c;
    });
  }
  if (m.action === 'escape') return `\x1b${m.param || ''}`;
  if (m.action === 'hex') {
    const bytes = String(m.param || '').split(/[\s,]+/).filter(Boolean).map((h) => parseInt(h, 16)).filter((v) => v >= 0 && v < 256);
    return new Uint8Array(bytes).buffer;
  }
  return null;
}

// Captura de una combinación (Preferencias → "Cambiar…"): la ventana le pasa la próxima tecla.
export const keyCapture = createStore(null);   // { onCombo(combo) } mientras se espera una tecla

// Nombre de una acción en el idioma de la interfaz (catálogo terminal.action.*).
export const actionLabel = (id) => t(`terminal.action.${id}`);
export const ACTION_IDS = Object.keys(DEFAULT_BINDINGS);

// Texto para los menús: el primer atajo de la acción.
const SYMBOLS = { Super: '❖', Ctrl: '⌃', Alt: '⌥', Shift: '⇧', ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→',
  Enter: '⏎', PageUp: 'RePág', PageDown: 'AvPág', Home: 'Inicio', End: 'Fin', Insert: 'Insert', Tab: 'Tab', Backspace: '⌫',
  Delete: 'Supr', Escape: 'Esc', Space: 'Espacio' };
export const comboLabel = (c) => (c || '').split('+').map((k) => SYMBOLS[k] || (k.length === 1 ? k.toUpperCase() : k)).join('');
export const hint = (action) => comboLabel(bindings()[action]?.[0] || '');
