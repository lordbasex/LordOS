// shortcuts.js — the keyboard shortcuts of the desktop, one dispatcher for every theme
// (docs/SHORTCUTS.md).
//
// It sees each key before any listener (runtime/events.js setKeyFilter). A key it takes reaches
// nobody else, so one key never fires twice: not a theme's action and an app's as well.
//
//   BASE                      the keys every theme shares
//   useShortcuts({ grabs, table, onSuperTap, appsPaste })  a theme's own: its table, which fixed list
//                             of keys lord-server keeps from apps ('flow' or 'classic',
//                             server/src/wl/keygrab.c), what ❖ pressed alone does and whether ❖V
//                             belongs to the apps
//   addShortcuts(entries)     a module's own (Screenshots, the Terminal's drop-down window);
//                             runShortcut(id) runs one from elsewhere
//   shortcutList()            what is active now, for Settings → Keyboard
//
// An entry: { id, keys: ['Meta+Shift+V', …], run, label?, group?, when?, repeat? }. `run` is always a
// function, called with the key event, and `when(e)` says whether the entry applies now: when it
// does not, the key goes on to the app. Without `repeat`, a held key runs once and its repeats are
// still taken. `label` (a locale key) lists it in Settings; `show` replaces its keys there.
// Letters follow the keyboard layout; other keys their position, in every layout.
import { useEffect, useRef } from 'react';
import { setKeyFilter } from '@lordos/runtime';
import { createStore } from './store.js';
import { t } from './i18n.js';
import { keyboard } from './keyboard.js';
import { spotlight, spotlightOpen } from './spotlight.js';
import { toggleClipboardPanel, clipboardPanel } from './clipboard.js';
import { toggleEmojiPanel } from './emoji.jsx';
import { quitFrontApp } from './dock.js';
import { hasNowPlaying, mediaToggle, mediaNext, mediaPrev, mediaStop } from './media.js';
import { wm } from './windows.js';
import { switcherOpen, switcherKey } from './appswitcher.jsx';

const MOD_BITS = { Shift: 1, Alt: 2, Ctrl: 4, Meta: 8 };
const NAMED = {
  1: 'Escape', 15: 'Tab', 41: '`', 51: ',', 52: '.', 57: 'Space', 62: 'F4', 87: 'F11', 88: 'F12', 99: 'PrintScreen',
  103: 'ArrowUp', 105: 'ArrowLeft', 106: 'ArrowRight', 108: 'ArrowDown',
  163: 'MediaNext', 164: 'MediaPlayPause', 165: 'MediaPrev', 166: 'MediaStop', 200: 'MediaPlay', 201: 'MediaPause',
};
for (let d = 1; d <= 9; d++) NAMED[d + 1] = String(d);
const LETTERS = { 16: 'q', 17: 'w', 18: 'e', 19: 'r', 20: 't', 21: 'y', 22: 'u', 23: 'i', 24: 'o', 25: 'p', 30: 'a', 31: 's', 32: 'd',
  33: 'f', 34: 'g', 35: 'h', 36: 'j', 37: 'k', 38: 'l', 44: 'z', 45: 'x', 46: 'c', 47: 'v', 48: 'b', 49: 'n', 50: 'm' };
const SUPER = (code) => code === 125 || code === 126;

// As keygrab_letter in server/src/wl/keygrab.c: the layout's letter, or the position's on a layout
// without Latin letters.
function keyName(e) {
  const b = String(e.base || '').toLowerCase();
  if (/^[a-z]$/.test(b)) return b;
  if (LETTERS[e.code] && !/^[\x20-\x7e]$/.test(b)) return LETTERS[e.code];
  return NAMED[e.code] || '';
}

function parseCombo(combo) {
  const parts = combo.split('+');
  const key = parts.pop();
  let mods = 0;
  for (const m of parts) mods |= MOD_BITS[m];
  return `${mods}:${key.toLowerCase()}`;
}

// ---- the base every theme shares ----

const lockScreen = () => { if (typeof lord.session !== 'undefined' && lord.session.managed()) lord.session.lock(); };
const canLock = () => typeof lord.session !== 'undefined' && lord.session.managed();
const toggleSearch = () => spotlight.toggle();
const nextKeyboard = () => keyboard.next();
const severalKeyboards = () => keyboard.configured().length > 1;
const quickClipboard = () => toggleClipboardPanel('quick');
const fullClipboard = () => toggleClipboardPanel('full');
// Ctrl+Space stays with the program in the Terminal (in emacs it marks the start of a selection).
const notInTerminal = () => spotlightOpen.get() || String(wm.focused() || '').split('#')[0] !== 'terminal';

// With `appsPaste` (Lord Flow) ❖V is paste in a LordOS app inside the session, as it is in a Linux app:
// the key goes to the app, and ❖⇧V is the clipboard. An isolated app never receives ❖V (wl.c).
let appsPaste = false;
const clipboardKeyIsOurs = () => {
  const id = String(wm.focused() || '');
  return !appsPaste || !id || id.startsWith('linux#') || !!clipboardPanel.get();
};

export const BASE = [
  { id: 'search', keys: ['Meta+Space'], run: toggleSearch, label: 'shell.shortcuts.search', group: 'system' },
  { id: 'searchCtrl', keys: ['Ctrl+Space'], run: toggleSearch, when: notInTerminal, label: 'shell.shortcuts.search', group: 'system' },
  { id: 'nextKeyboard', keys: ['Meta+Alt+Space', 'Ctrl+Alt+Space'], run: nextKeyboard, when: severalKeyboards, label: 'shell.shortcuts.nextKeyboard', group: 'system' },
  { id: 'clipboard', keys: ['Meta+V'], run: quickClipboard, when: clipboardKeyIsOurs, label: 'shell.shortcuts.clipboard', group: 'system' },
  { id: 'clipboardFull', keys: ['Meta+Shift+V'], run: fullClipboard, label: 'shell.shortcuts.clipboardFull', group: 'system' },
  { id: 'emoji', keys: ['Meta+.', 'Meta+Ctrl+Space'], run: toggleEmojiPanel, label: 'shell.shortcuts.emoji', group: 'system' },
  { id: 'lock', keys: ['Meta+L', 'Meta+Ctrl+Q'], run: lockScreen, when: canLock, label: 'shell.shortcuts.lock', group: 'system' },
  { id: 'quit', keys: ['Meta+Q'], run: quitFrontApp, label: 'shell.shortcuts.quit', group: 'apps' },
  { id: 'mediaPlay', keys: ['MediaPlayPause', 'MediaPlay', 'MediaPause'], run: mediaToggle, when: hasNowPlaying },
  { id: 'mediaNext', keys: ['MediaNext'], run: mediaNext, when: hasNowPlaying },
  { id: 'mediaPrev', keys: ['MediaPrev'], run: mediaPrev, when: hasNowPlaying },
  { id: 'mediaStop', keys: ['MediaStop'], run: mediaStop, when: hasNowPlaying },
];

// ---- the dispatcher ----

let themeTable = [];
let added = [];
let onSuperTap = null;
let lookup = null;            // 'mods:key' → entries, the first that applies wins
// changes when the active shortcuts do: Settings → Keyboard redraws
export const shortcutsRev = createStore(0);

function changed() { lookup = null; shortcutsRev.set((v) => v + 1); }
function index() {
  const map = new Map();
  for (const entry of [...added, ...themeTable, ...BASE]) {
    for (const combo of entry.keys) {
      const k = parseCombo(combo);
      if (!map.has(k)) map.set(k, []);
      map.get(k).push(entry);
    }
  }
  return map;
}

const taken = new Set();      // keys whose press was taken: their repeats and release are too
const repeating = new Set();  // the same, for an entry that runs again while held
let superAlone = false;

function filter(e) {
  if (!e.code) return false;                       // text an input method committed
  if (switcherOpen()) { if (e.down) taken.add(e.code); else taken.delete(e.code); return switcherKey(e); }
  if (!e.down) {
    const tap = SUPER(e.code) && superAlone;
    if (SUPER(e.code)) superAlone = false;
    if (tap) onSuperTap?.();
    return taken.delete(e.code) || !!repeating.delete(e.code);
  }
  if (SUPER(e.code)) { if (!e.repeat) superAlone = true; return false; }
  superAlone = false;
  if (e.forwarded) return true;                    // a Linux app got it as Ctrl
  if (taken.has(e.code)) return e.repeat;          // the repeats of a key held down
  const entries = (lookup = lookup || index()).get(`${e.mods}:${keyName(e).toLowerCase()}`);
  const entry = entries?.find((x) => !x.when || x.when(e));
  if (!entry) return false;
  (entry.repeat ? repeating : taken).add(e.code);
  entry.run(e);
  return true;
}

// startShell: the base only, and no keys kept from apps, until the theme says otherwise.
export function resetShortcuts() {
  themeTable = [];
  onSuperTap = null;
  appsPaste = false;
  setGrabs('');
  setKeyFilter(filter);
  changed();
}
function setGrabs(name) { if (typeof lord.wayland !== 'undefined') lord.wayland.grabKeys(name); }

// A theme's shortcuts. The table is read once: `run` should be stable or go through a ref.
export function useShortcuts({ grabs = '', table = [], onSuperTap: tap = null, appsPaste: paste = false }) {
  const tapRef = useRef(tap);
  tapRef.current = tap;
  useEffect(() => {
    themeTable = table;
    appsPaste = paste;
    onSuperTap = () => tapRef.current?.();
    setGrabs(grabs);
    changed();
    return () => { themeTable = []; onSuperTap = null; changed(); };
  }, []);
}

// Runs a module's entry by its id (Lord Classic's ❖⇧S is Screenshots' area capture).
export function runShortcut(id) { added.find((x) => x.id === id)?.run(); }

export function addShortcuts(entries) {
  added = [...added, ...entries];
  changed();
  return () => { added = added.filter((x) => !entries.includes(x)); changed(); };
}

// ---- the list in Settings → Keyboard ----

const SYMBOLS = { Meta: '❖', Ctrl: '⌃', Alt: '⌥', Shift: '⇧' };
const KEY_LABELS = { Escape: 'Esc', ArrowLeft: '←', ArrowRight: '→', ArrowUp: '↑', ArrowDown: '↓' };
function comboLabel(combo) {
  const parts = combo.split('+');
  const key = parts.pop();
  const name = key === 'Space' ? t('shell.shortcuts.space') : key === 'PrintScreen' ? t('shell.shortcuts.printScreen') : KEY_LABELS[key] || key;
  return parts.map((m) => SYMBOLS[m]).join('') + name;
}

// [{ group, items: [{ id, label, keys: '❖Space · ⌃Space' }] }], groups in a fixed order. Entries with
// the same label become one row.
export function shortcutList() {
  const groups = new Map([['system', []], ['windows', []], ['apps', []]]);
  for (const entry of [...added, ...themeTable, ...BASE]) {
    if (!entry.label) continue;
    const keys = entry.show || entry.keys.map(comboLabel);
    const list = groups.get(entry.group) || groups.get('system');
    const same = list.find((x) => x.labelKey === entry.label);
    if (same) same.keys.push(...keys);
    else list.push({ id: entry.id, labelKey: entry.label, label: t(entry.label), keys: [...keys] });
  }
  return [...groups].filter(([, items]) => items.length)
    .map(([group, items]) => ({ group, items: items.map((x) => ({ ...x, keys: x.keys.join(' · ') })) }));
}
