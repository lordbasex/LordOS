// menu_keys_scene.mjs — the scene of tests/menu_keys_test.mjs (not run by itself): Lord Flow's and Lord Classic's menus
// rendered through runtime/index.js with a stub `lord`, driven with the server's own events (__lordEvent, __lordKey):
// the arrows wrap past separators and disabled rows, letters go to the row they start, → opens a submenu with its
// first row highlighted and Escape closes one level, ← and → step along the bar that opened the menu, a submenu
// opens on hover only after a pause, and F10 or ⌥ alone opens a registered menu bar (sdk/shortcuts.js). Prints one
// line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { ThemeProvider, addMenuBarKeys, SUB_DELAY } from '../../sdk/index.js';
import { resetShortcuts } from '../../sdk/shortcuts.js';
import { makeTheme } from '../../themes/default/tokens.js';
import { Menu } from '../../themes/default/components/Menu.jsx';
import { Menu as ClassicMenu } from '../../themes/classic/components/Menu.jsx';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const shown = (s) => { for (const [id, v] of texts) if (v === s && attached(id)) return true; return false; };
const textNode = (s) => { let r = 0; for (const [id, v] of texts) if (v === s && attached(id)) r = parents.get(id) || 0; return r; };
// a key as lord-server sends it with libxkbcommon (code: the evdev code, which the shortcuts dispatcher reads)
const key = async (name, chr = '', code = 30) => { __lordKey(code, 1, name, chr, (chr || name).toLowerCase(), 0); __lordKey(code, 0, name, '', '', 0); await flush(); };
const KEYS = { ArrowDown: 108, ArrowUp: 103, ArrowLeft: 105, ArrowRight: 106, Enter: 28, Escape: 1, Home: 102, End: 107 };
const press = (name) => key(name, '', KEYS[name]);
const type = async (s) => { for (const ch of s) await key(ch, ch); };

enableShellLayers();
resetShortcuts();
const t = makeTheme({ mode: 'dark', accent: 'azul', effects: true });
const log = [];
const pick = (s) => () => log.push(s);
const items = [
  { label: 'Open', onClick: pick('open') },
  '-',
  { label: 'Recent', submenu: [{ label: 'Alpha', onClick: pick('alpha') }, '-', { label: 'Beta', onClick: pick('beta') }] },
  { label: 'Paste', disabled: true, onClick: pick('paste') },
  { label: 'Print', onClick: pick('print') },
  { label: 'Properties', onClick: pick('properties') },
];
let which = null, keys = false, rerender = null, round = 0;
function Scene() {
  const [, set] = React.useState(0);
  rerender = () => set((v) => v + 1);
  const close = () => { log.push('close'); which = null; rerender(); };
  const step = (d) => log.push(`step${d}`);
  const body = which === 'flow' ? h(Menu, { key: round, x: 100, y: 100, items, onClose: close, keys, onStep: step })
    : which === 'classic' ? h(ClassicMenu, { key: round, t, x: 100, y: 100, items, onClose: close, keys, onStep: step }) : null;
  return h(ThemeProvider, { tokens: t }, body);
}
rt.render(h(Scene));
await flush();
const open = async (kind, withKeys = false) => { which = kind; keys = withKeys; round++; log.length = 0; rerender(); await flush(); };
const last = () => log.filter((x) => x !== 'close').join(',');

for (const kind of ['flow', 'classic']) {
  await open(kind);
  check(`${kind}: opens`, shown('Open') && shown('Print'));
  await press('ArrowDown'); await press('Enter');
  check(`${kind}: ↓ from nothing is the first row`, last() === 'open' && log.includes('close'), log.join());
  await open(kind);
  await press('ArrowUp'); await press('Enter');
  check(`${kind}: ↑ from nothing is the last row`, last() === 'properties', log.join());
  await open(kind);
  await press('End'); await press('ArrowDown'); await press('Enter');
  check(`${kind}: ↓ wraps to the top`, last() === 'open', log.join());
  await open(kind);
  await type('p'); await press('Enter');
  check(`${kind}: a letter skips the disabled row`, last() === 'print', log.join());
  await open(kind);
  await type('pp'); await press('Enter');
  check(`${kind}: the same letter again goes to the next row`, last() === 'properties', log.join());
  await open(kind, true);
  await press('Enter');
  check(`${kind}: opened from the keyboard, its first row is highlighted`, last() === 'open', log.join());
  await open(kind);
  await press('Escape');
  check(`${kind}: Escape closes it`, log.includes('close') && !shown('Open'), log.join());
}

// Lord Classic flattens submenus; ← and → step along the strip
await open('classic', true);
await press('ArrowRight');
check('classic: → steps to the next menu', last() === 'step1', log.join());
await press('ArrowLeft');
check('classic: ← steps to the previous one', last() === 'step1,step-1', log.join());

// Lord Flow: a submenu from the keyboard, one level at a time
await open('flow');
await type('r'); await press('ArrowRight');
check('flow: → opens the submenu', shown('Alpha') && shown('Open'));
await press('ArrowDown'); await press('ArrowDown');
await press('Escape');
check('flow: Escape closes only the submenu', !shown('Alpha') && shown('Open') && !log.includes('close'), log.join());
await press('ArrowRight'); await press('ArrowDown'); await press('Enter');
check('flow: the submenu starts on its first row and skips its separator', last() === 'beta', log.join());
await open('flow');
await type('r'); await press('ArrowRight'); await press('ArrowLeft');
check('flow: ← closes the submenu', !shown('Alpha') && shown('Open'));
await press('ArrowLeft');
check('flow: ← in the top menu steps along the bar', last() === 'step-1', log.join());
await type('o'); await press('ArrowRight');
check('flow: → on a row without a submenu steps along the bar', last() === 'step-1,step1', log.join());

// the pointer: the submenu opens after a pause, not as the pointer passes
await open('flow');
__lordEvent('move', textNode('Recent'), 110, 150, 0); await flush();
check('flow: no submenu as the pointer passes', !shown('Alpha'));
await wait(SUB_DELAY + 60); await flush();
check('flow: the submenu after a pause on its row', shown('Alpha'));
__lordEvent('move', textNode('Print'), 110, 200, 0); await flush();
await wait(SUB_DELAY + 60); await flush();
check('flow: another row closes it after a pause', !shown('Alpha') && shown('Open'));
await press('Escape');

// F10 and ⌥ alone open the menu bar that says it can, and nothing when none can
let can = false, runs = 0;
const off = addMenuBarKeys({ can: () => can, run: () => { runs++; } });
await key('F10', '', 68);
check('F10 does nothing when no bar can open', runs === 0);
can = true;
await key('F10', '', 68);
check('F10 opens the menu bar', runs === 1);
__lordKey(56, 1, 'Alt', '', '', 4); __lordKey(56, 0, 'Alt', '', '', 0); await flush();
check('⌥ pressed and released alone opens it', runs === 2);
__lordKey(56, 1, 'Alt', '', '', 4); __lordKey(15, 1, 'Tab', '', '', 4); __lordKey(15, 0, 'Tab', '', '', 4); __lordKey(56, 0, 'Alt', '', '', 0); await flush();
check('⌥ used with another key does not', runs === 2);
off();
process.exit(0);
