// list_scene.mjs — (0.8.8.8) the scene of `tests/focus_scene_test.mjs list_scene.mjs` (not run by itself): the SDK's
// List and Table, Tabs, Dropdown, RadioGroup, Popover and the wheel on a Slider, rendered through runtime/index.js and
// AppHost with the stub `lord` of focus_scene_test.mjs and driven with the server's own events (__lordKey,
// __lordEvent). A List is one Tab stop whose arrows, Shift+arrows, Ctrl+A, type-to-find and Enter work, and a press on
// a row gives it the keys without the ring; a Table selects and activates from the keys; Tabs and a RadioGroup follow
// the arrows; a Dropdown opens, moves, chooses and closes from the keys; a Popover opened from the keyboard takes the
// focus and Escape gives it back; the wheel moves a Slider. Prints "ok <name>" or "FAIL <name> <detail>" per check.
import React, { useRef, useState } from 'react';
import * as rt from '@lordos/runtime';
import { AppHost, Button, Slider } from '../../sdk/components.jsx';
import { Tabs, Dropdown, RadioGroup, Popover } from '../../sdk/controls.jsx';
import { List, Table } from '../../sdk/lists.jsx';
import { registerBuiltinApp } from '../../sdk/windows.js';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const fruits = ['Apple', 'Banana', 'Blueberry', 'Cherry', 'Date', 'Fig'];
const now = { sel: [], activated: null, tsel: [], tact: null, tab: 'a', drop: 'one', radio: 'x', slider: 50, pop: false, popPressed: 0 };
function Demo() {
  const [sel, setSel] = useState([]);
  const [tsel, setTsel] = useState([]);
  const [tab, setTab] = useState('a');
  const [drop, setDrop] = useState('one');
  const [radio, setRadio] = useState('x');
  const [slider, setSlider] = useState(50);
  const [pop, setPop] = useState(false);
  const opener = useRef(null);
  Object.assign(now, { sel, tsel, tab, drop, radio, slider, pop });
  return h(rt.View, null,
    h(Button, { label: 'Start', onClick: () => {} }),
    h(List, { items: fruits, itemKey: (f) => f, selection: sel, onSelectionChange: setSel, multiple: true, onActivate: (f) => { now.activated = f; } }),
    h(Table, { columns: [{ key: 'name', label: 'Name', flex: 1 }, { key: 'n', label: 'N', width: 40, num: true }], rows: [{ id: 1, name: 'Red' }, { id: 2, name: 'Green' }, { id: 3, name: 'Blue' }],
      rowKey: (r) => r.id, renderCell: (r, c) => (c.key === 'name' ? r.name : r.id), selection: tsel, onSelectionChange: setTsel, onActivate: (r) => { now.tact = r.name; } }),
    h(Tabs, { value: tab, tabs: [['a', 'TabA'], ['b', 'TabB'], ['c', 'TabC', { disabled: true }], ['d', 'TabD']], onChange: setTab }),
    h(Dropdown, { value: drop, options: [['one', 'One'], ['two', 'Two'], ['three', 'Three'], ['four', 'Four']], onChange: setDrop }),
    h(RadioGroup, { value: radio, options: [['x', 'RadX'], ['y', 'RadY'], ['z', 'RadZ']], onChange: setRadio }),
    h(Slider, { value: slider, onChange: setSlider, min: 0, max: 100 }),
    h(rt.View, { ref: opener }, h(Button, { label: 'Open', onClick: () => setPop(true) })),
    pop ? h(Popover, { anchor: opener, onClose: () => setPop(false) },
      h(Button, { label: 'InPop', onClick: () => { now.popPressed++; } }), h(Button, { label: 'InPop2', onClick: () => {} })) : null,
    h(Button, { label: 'End', onClick: () => {} }));
}
registerBuiltinApp({ id: 'demo', Component: Demo });
rt.render(h(AppHost, { id: 'demo', focused: true }));
await flush();

// a key as lord-server sends it with libxkbcommon: flags 1 Shift, 2 Ctrl
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const tab = async (n = 1) => { for (let i = 0; i < n; i++) await key('Tab'); };
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const rings = () => [...styles].filter(([id, s]) => s.borderWidth === 2 && s.pointerEvents === 'none' && s.position === 'absolute' && inTree(id) && parents.has(id));
const ringOn = () => { const r = rings(); return r.length === 1 ? parents.get(r[0][0]) : r.length ? 'many' : null; };
const textNode = (label) => [...texts].find(([id, s]) => s === label && inTree(id))?.[0];
const ringAround = (label) => { const r = ringOn(); for (let n = textNode(label); n; n = parents.get(n)) if (n === r) return true; return false; };
const click = async (id) => { __lordEvent('down', id, 1, 1, 0); __lordEvent('up', id, 1, 1, 0); await flush(); };
const heard = [];
rt.addListener('keydown', (e) => heard.push(e.key));

// ---- List ----
await tab(2);
check('the List is one Tab stop', ringAround('Apple') && ringAround('Fig'), String(ringOn()));
await key('ArrowDown');
check('↓ selects the first row', JSON.stringify(now.sel) === '["Apple"]', JSON.stringify(now.sel));
await key('ArrowDown', '', 1);
await key('ArrowDown', '', 1);
check('Shift+↓ extends the range', JSON.stringify(now.sel) === '["Apple","Banana","Blueberry"]', JSON.stringify(now.sel));
await key('End');
check('End selects the last alone', JSON.stringify(now.sel) === '["Fig"]', JSON.stringify(now.sel));
await key('a', 'a', 2);
check('Ctrl+A selects every row', now.sel.length === fruits.length, JSON.stringify(now.sel));
heard.length = 0;
await key('c', 'c');
check('typing a letter goes to the row that starts with it', JSON.stringify(now.sel) === '["Cherry"]', JSON.stringify(now.sel));
check('the letter reaches no listener', !heard.includes('c'), heard.join(','));
await key('Home');                     // an arrow or Home ends the search
await key('b', 'b');
await key('l', 'l');
check('more letters narrow the search', JSON.stringify(now.sel) === '["Blueberry"]', JSON.stringify(now.sel));
await key('Enter');
check('Enter activates the row', now.activated === 'Blueberry', String(now.activated));
await key('Home');
check('Home the first', JSON.stringify(now.sel) === '["Apple"]', JSON.stringify(now.sel));
await key('PageDown');
check('Page Down a view further', now.sel.length === 1 && now.sel[0] !== 'Apple', JSON.stringify(now.sel));

// a press on a row gives the List the keys, without the ring
await click(parents.get(textNode('Date')));
check('a press selects the row', JSON.stringify(now.sel) === '["Date"]', JSON.stringify(now.sel));
check('and shows no ring', ringOn() === null, String(ringOn()));
heard.length = 0;
await key('ArrowUp');
check('the arrows go on working after the press', JSON.stringify(now.sel) === '["Cherry"]' && !heard.includes('ArrowUp'), `${JSON.stringify(now.sel)} ${heard.join(',')}`);

// ---- Table ----
await tab();
check('Tab goes on to the Table', ringAround('Red'), String(ringOn()));
await key('ArrowDown'); await key('ArrowDown');
check('↓ chooses its rows', JSON.stringify(now.tsel) === '["2"]', JSON.stringify(now.tsel));
await key('ArrowDown', '', 1);
check('a single selection takes no Shift range', JSON.stringify(now.tsel) === '["2"]', JSON.stringify(now.tsel));
await key('b', 'b');
check('type-to-find on the first column', JSON.stringify(now.tsel) === '["3"]', JSON.stringify(now.tsel));
await key('Enter');
check('Enter activates it', now.tact === 'Blue', String(now.tact));

// ---- Tabs ----
await tab();
check('Tab goes to the tabs', ringAround('TabA'));
await key('ArrowRight');
check('→ the next tab', now.tab === 'b', now.tab);
await key('ArrowRight');
check('→ skips a disabled tab', now.tab === 'd', now.tab);
await key('ArrowRight');
check('→ goes round', now.tab === 'a', now.tab);
await key('ArrowLeft');
check('← goes back round', now.tab === 'd', now.tab);

// ---- Dropdown ----
await tab();
check('Tab goes to the Dropdown', ringAround('One') && !textNode('Two'));
await key(' ', ' ');
check('Space opens it', !!textNode('Two') && !!textNode('Four'));
await key('ArrowDown'); await key('ArrowDown');
await key('Enter');
check('↓ ↓ Enter chooses the third', now.drop === 'three' && !textNode('Four'), now.drop);
await key('ArrowUp');
check('↑ opens it too', !!textNode('Four'));
await key('Escape');
check('Escape closes it without choosing', !textNode('Four') && now.drop === 'three', now.drop);
check('the focus stays on it', ringAround('Three'));
await key('f', 'f');
check('a letter chooses while closed', now.drop === 'four', now.drop);

// ---- RadioGroup ----
await tab();
await key('ArrowDown');
check('↓ chooses the next radio button', now.radio === 'y', now.radio);
await key('ArrowUp'); await key('ArrowUp');
check('↑ goes round', now.radio === 'z', now.radio);

// ---- the wheel on a Slider ----
await tab();
const sliderNode = ringOn();          // the node the ring is drawn in
__lordEvent('wheel', sliderNode, 1, 1, 1); await flush();
check('a notch up moves the Slider a step', now.slider === 55, String(now.slider));
__lordEvent('wheel', sliderNode, 1, 1, -2); await flush();
check('two down take two steps', now.slider === 45, String(now.slider));

// ---- Popover ----
await tab();
check('on the button that opens the popover', ringAround('Open'));
await key(' ', ' ');
check('it opens', now.pop && !!textNode('InPop'));
check('the focus moves into it', ringAround('InPop'), String(ringOn()));
await tab();
check('Tab goes round inside it', ringAround('InPop2'));
await tab();
check('and comes back to its first control', ringAround('InPop'));
await key('Enter');
check('its controls work from the keys', now.popPressed === 1);
await key('Escape');
check('Escape closes it', !now.pop && !textNode('InPop'));
check('and the focus goes back to the button', ringAround('Open'), String(ringOn()));
await tab();
check('Tab goes on after it', ringAround('End'));
process.exit(0);
