// textfield_menu_scene.mjs — the scene of tests/textfield_menu_test.mjs (not run by itself): a TextInput and a TextArea
// rendered through runtime/index.js with a stub `lord`, driven with the server's own events (__lordEvent, __lordKey):
// a right click keeps the caret and opens the menu, its items edit as the keys do, Ctrl+Z and Ctrl+Shift+Z undo and
// redo, the open menu takes the keys (arrows, letters, Enter, Escape), and a triple click selects the line. Every character is 10 px wide and every box starts at 0, 0, so a click at
// x selects position x / 10. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { TextInput, TextArea } from '../../sdk/components.jsx';
import { enableShellLayers } from '../../sdk/layers.js';

const { parents, texts, removed, clip } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const ids = {};
const now = { input: 'hello world', area: 'one\ntwo three\nfour' };
function Scene() {
  const [v, setV] = useState(now.input);
  const [a, setA] = useState(now.area);
  now.input = v; now.area = a;
  return h(rt.View, null,
    h(rt.View, { ref: (r) => { if (r) ids.inputBox = r.id; } }, h(TextInput, { value: v, onChange: setV })),
    h(rt.View, { ref: (r) => { if (r) ids.areaBox = r.id; } }, h(TextArea, { value: a, onChange: setA })));
}
enableShellLayers();
rt.render(h(Scene));
await flush();

const childOf = (p) => [...parents].find(([, q]) => q === p)?.[0];
const click = async (id, x, y = 5, button = 0) => { __lordEvent('down', id, x, y, button); __lordEvent('up', id, x, y, button); await flush(); };
// a key as lord-server sends it with libxkbcommon: flags 1 Shift, 2 Ctrl
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const type = async (s) => { for (const ch of s) await key(ch, ch); };
// the menu item with this label, as the node a click lands on (its Text), while it is in the tree
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const item = (label) => { const raw = [...texts].find(([id, s]) => s === label && inTree(id))?.[0]; return raw ? parents.get(raw) : null; };
const ctrl = 2, shift = 1;

const input = childOf(ids.inputBox), area = childOf(ids.areaBox);
check('found the fields', !!input && !!area);

// a click puts the caret, a right click elsewhere keeps it and opens the menu
await click(input, 21);
await type('X');
check('a click puts the caret', now.input === 'heXllo world', now.input);
await click(input, 81, 5, 2);
check('the menu opens on a right click', !!item('Undo') && !!item('Select All'));
await type('Y');
check('the open menu keeps the keys', now.input === 'heXllo world' && !!item('Undo'), now.input);
await key('Escape');
check('Escape closes the menu', !item('Undo'));
await type('Y');
check('a right click moves no caret', now.input === 'heXYllo world', now.input);
__lordEvent('down', input, 91, 5, 1); __lordEvent('up', input, 91, 5, 1); await flush();
await type('Z');
check('a middle click moves no caret either', now.input === 'heXYZllo world', now.input);

// undo and redo with the keys: the three letters were typed in a row, one step
await key('z', '', ctrl);
check('Ctrl+Z undoes the typing', now.input === 'hello world', now.input);
await key('Z', '', ctrl | shift);
check('Ctrl+Shift+Z redoes it', now.input === 'heXYZllo world', now.input);
await key('z', '', ctrl);

// the menu: Select All, then Cut, then Undo and Paste, each from its item
await click(input, 10, 5, 2);
await click(item('Select All'), 0);
check('Select All closes the menu', !item('Undo'));
await click(input, 10, 5, 2);
await click(item('Cut'), 0);
check('Cut from the menu', now.input === '' && clip.text === 'hello world', `${now.input} / ${clip.text}`);
await click(input, 10, 5, 2);
await click(item('Undo'), 0);
check('Undo from the menu', now.input === 'hello world', now.input);
await click(input, 500);
await click(input, 0, 5, 2);
await click(item('Paste'), 0);
check('Paste from the menu keeps the focus and the caret', now.input === 'hello worldhello world', now.input);
await key('z', '', ctrl);
check('a paste is a step of its own', now.input === 'hello world', now.input);

// the menu from the keyboard (sdk/menunav.js): a letter goes to the row it starts, Enter chooses it, and the field
// has the keys again
await click(input, 0, 5, 2);
await type('s');
await key('Enter');
check('type-ahead and Enter choose Select All', !item('Undo'));
await type('k');
check('the field has the keys back', now.input === 'k', now.input);
await key('z', '', ctrl);
await click(input, 0, 5, 2);
// ↑ from nothing is the last row (Select All), ↓ wraps to the first and ↑ back past the disabled rows
await key('ArrowUp'); await key('ArrowDown'); await key('ArrowUp');
await key('Enter');
await type('q');
check('the arrows wrap', now.input === 'q', now.input);
await key('z', '', ctrl);

// a triple click: the whole field; in the TextArea, the line (not the row)
await click(input, 30); await click(input, 30); await click(input, 30);
await type('new');
check('a triple click selects the one-line field', now.input === 'new', now.input);
await click(area, 21, 30); await click(area, 21, 30); await click(area, 21, 30);
await type('2');
check('a triple click selects the line in a TextArea', now.area === 'one\n2\nfour', JSON.stringify(now.area));
await key('z', '', ctrl);
check('and Undo brings the line back', now.area === 'one\ntwo three\nfour', JSON.stringify(now.area));
// a double click still selects a word, once
await click(area, 1, 0); await click(area, 1, 0);
await type('ONE');
check('a double click still selects a word', now.area === 'ONE\ntwo three\nfour', JSON.stringify(now.area));
process.exit(0);
