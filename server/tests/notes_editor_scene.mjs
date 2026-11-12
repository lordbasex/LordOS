// notes_editor_scene.mjs — the scene of tests/textfield_menu_test.mjs notes_editor_scene.mjs (GitHub #117; not run by
// itself): Notes' editor (apps/notes/NoteEditor.jsx) with the rows of its Edit menu and right-click menu
// (apps/notes/editmenu.js). Each row sends the editor's own key, so Cut, Copy, Paste and Select All from a menu do what
// the keys do; Ctrl+Shift+C makes a code block instead of copying; the right-click menu has Cut, Copy, Paste and
// Select All. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useRef, useState } from 'react';
import * as rt from '@lordos/runtime';
import { ContextMenu } from '@lordos/sdk';
import { NoteEditor } from '../../apps/notes/NoteEditor.jsx';
import { editKey, editItems, formatItems, contextItems, fromReading } from '../../apps/notes/editmenu.js';
import { enableShellLayers } from '../../sdk/layers.js';

const { parents, texts, removed, clip } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const tr = (k) => k;

const colors = { text: '#000', dim: '#666', heading: '#000', code: '#000', accent: '#00f', selection: '#ccf', caret: '#00f', popupBg: '#fff', popupText: '#000', border: '#ccc' };
const now = { body: 'hello world' };
let api = null, openMenu = null;
function Scene() {
  const [body, setBody] = useState(now.body);
  const [ctx, setCtx] = useState(null);
  const ref = useRef(null);
  api = ref;
  now.body = body;
  openMenu = setCtx;
  return h(rt.View, { style: { width: 600, height: 400 } },
    h(NoteEditor, { value: body, onChange: setBody, colors, autoFocus: true, apiRef: ref,
      onContextMenu: (e) => setCtx({ x: e.x, y: e.y, items: contextItems(tr, { selected: !!ref.current.selection(), empty: !body }, (a) => ref.current.send(editKey(a))) }) }),
    ctx && h(ContextMenu, { x: ctx.x, y: ctx.y, items: ctx.items, onClose: () => setCtx(null), bounds: { w: 600, h: 400 } }));
}
enableShellLayers();
rt.render(h(Scene));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const item = (label) => { const raw = [...texts].find(([id, s]) => s === label && inTree(id))?.[0]; return raw ? parents.get(raw) : null; };
const click = async (id, x = 0, y = 5, button = 0) => { __lordEvent('down', id, x, y, button); __lordEvent('up', id, x, y, button); await flush(); };
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const send = async (action) => { api.current.send(editKey(action)); await flush(); };
const ctrl = 2, shift = 1;

// the menu model
const rows = editItems(tr, { editing: false, empty: false }, () => {});
const row = (list, label) => list.find((x) => x !== '-' && x.label === label);
check('reading: Copy is off', row(rows, 'notes.cmd.copy').disabled === true);
check('reading: Paste and Select All are on', !row(rows, 'notes.cmd.paste').disabled && !row(rows, 'notes.cmd.selectAll').disabled);
check('reading: Paste and Select All open the editor', fromReading('paste') && fromReading('selectAll') && !fromReading('copy'));
check('editing: every Edit row is on', editItems(tr, { editing: true }, () => {}).every((x) => x === '-' || !x.disabled));
check('no note: every Edit row is off', editItems(tr, { editing: true, none: true }, () => {}).every((x) => x === '-' || x.disabled));
check('Format is on only while editing', formatItems(tr, { editing: true }, () => {}).every((x) => x === '-' || !x.disabled)
  && formatItems(tr, { editing: false }, () => {}).every((x) => x === '-' || x.disabled));
const ctxRows = contextItems(tr, { selected: false, empty: false }, () => {}, [{ label: 'speak' }]);
check('right-click menu: Cut, Copy, Paste, Select All, then the extra rows',
  JSON.stringify(ctxRows.map((x) => (x === '-' ? '-' : x.label))) === JSON.stringify(['notes.cmd.cut', 'notes.cmd.copy', 'notes.cmd.paste', '-', 'notes.cmd.selectAll', '-', 'speak']));
check('right-click menu without a selection: Cut and Copy off', row(ctxRows, 'notes.cmd.copy').disabled && !row(ctxRows, 'notes.cmd.paste').disabled);

// the menu rows drive the editor
await send('selectAll');
check('Select All from the menu', api.current.selection() === 'hello world', api.current.selection());
await send('copy');
check('Copy from the menu', clip.text === 'hello world', clip.text);
clip.text = 'pasted';
await send('paste');
check('Paste from the menu replaces the selection', now.body === 'pasted', now.body);
await send('undo');
check('Undo from the menu', now.body === 'hello world', now.body);
await send('redo');
check('Redo from the menu', now.body === 'pasted', now.body);
await send('selectAll');
await send('cut');
check('Cut from the menu', now.body === '' && clip.text === 'pasted', `${now.body} / ${clip.text}`);
await send('undo');

// keys: Ctrl+C copies, Ctrl+Shift+C makes a code block (it copied before, so the code block was out of reach)
await key('a', '', ctrl);
clip.text = '';
await key('c', '', ctrl);
check('Ctrl+C copies', clip.text === 'pasted', clip.text);
clip.text = 'kept';
await key('C', '', ctrl | shift);
check('Ctrl+Shift+C makes a code block, copies nothing', now.body.startsWith('```\npasted\n```') && clip.text === 'kept', `${JSON.stringify(now.body)} / ${clip.text}`);
await send('undo');
await send('selectAll');
await send('bold');
check('Bold from the Format menu', now.body === '**pasted**', now.body);

// the right-click menu
await key('a', '', ctrl);
openMenu({ x: 10, y: 10, items: contextItems(tr, { selected: true, empty: false }, (a) => api.current.send(editKey(a))) });
await flush();
check('the right-click menu has Copy', !!item('notes.cmd.copy') && !!item('notes.cmd.paste'));
clip.text = '';
await click(item('notes.cmd.copy'));
check('Copy from the right-click menu', clip.text === '**pasted**' && !item('notes.cmd.copy'), clip.text);
process.exit(0);
