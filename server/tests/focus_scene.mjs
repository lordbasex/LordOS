// focus_scene.mjs — the scene of tests/focus_scene_test.mjs (not run by itself): a window of the SDK's controls rendered
// through runtime/index.js and AppHost with a stub `lord`, driven with the server's own events (__lordKey, __lordEvent).
// Tab and Shift+Tab go round them in tree order skipping the disabled and the hidden, the ring is a node of its own
// inside the focused one and goes away on a press, Space and Enter press, the arrows move a Slider, a Stepper and a
// Segmented, a MenuButton opens and chooses from the keyboard, a TextInput takes the keys and lets Tab go on, a key the
// focus used reaches no listener, a listener that prevents Tab keeps it, and a confirmation dialog keeps its keys.
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { AppHost, Button, Toggle, TextInput, Slider, Stepper, Segmented, ScrollView, hasTextFocus } from '../../sdk/components.jsx';
import { MenuButton } from '../../sdk/controls.jsx';
import { registerBuiltinApp } from '../../sdk/windows.js';
import { dialogStack } from '../../sdk/dialogkeys.js';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const now = { presses: {}, on: false, slider: 50, stepper: 3, seg: 'a', text: 'hi', chose: '', showLast: true, hide: true };
const press = (k) => () => { now.presses[k] = (now.presses[k] || 0) + 1; };
let rerender = null;
function Demo() {
  const [, set] = useState(0);
  rerender = () => set((n) => n + 1);
  const [on, setOn] = useState(false);
  const [slider, setSlider] = useState(50);
  const [stepper, setStepper] = useState(3);
  const [seg, setSeg] = useState('a');
  const [text, setText] = useState('hi');
  now.on = on; now.slider = slider; now.stepper = stepper; now.seg = seg; now.text = text;
  return h(rt.View, null,
    h(Button, { label: 'One', onClick: press('one') }),
    h(Button, { label: 'Off', disabled: true, onClick: press('off') }),
    h(rt.View, { style: { display: now.hide ? 'none' : 'flex' } }, h(Button, { label: 'Hidden', onClick: press('hidden') })),
    h(Toggle, { on, onChange: setOn }),
    h(TextInput, { value: text, onChange: setText }),
    h(Slider, { value: slider, onChange: setSlider, min: 0, max: 100 }),
    h(Stepper, { value: stepper, onChange: setStepper, min: 0, max: 10 }),
    h(Segmented, { options: [['a', 'A'], ['b', 'B'], ['c', 'C']], value: seg, onChange: setSeg }),
    h(MenuButton, { title: 'More', items: [{ label: 'First', onClick: () => { now.chose = 'first'; } }, '-', { label: 'Second', onClick: () => { now.chose = 'second'; } }] }),
    now.showLast ? h(Button, { label: 'Last', onClick: press('last') }) : null,
    h(ScrollView, { style: { height: 100 } }, h(rt.View, { style: { height: 1000 } })));
}
registerBuiltinApp({ id: 'demo', Component: Demo });
rt.render(h(AppHost, { id: 'demo', focused: true }));
await flush();
await new Promise((res) => setTimeout(res, 80));     // the scroll view measures its content 50 ms after a render
await flush();

// a key as lord-server sends it with libxkbcommon: flags 1 Shift
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const tab = (n = 1) => (async () => { for (let i = 0; i < n; i++) await key('Tab'); })();
const shiftTab = () => key('Tab', '', 1);
const space = () => key(' ', ' ');
// a click lands on a label's Text node (the raw text inside it is no hit target)
const click = async (id) => { __lordEvent('down', id, 1, 1, 0); __lordEvent('up', id, 1, 1, 0); await flush(); };
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// the ring: a node with a 2 px border that takes no clicks, still in the tree; what it is drawn around
const rings = () => [...styles].filter(([id, s]) => s.borderWidth === 2 && s.pointerEvents === 'none' && s.position === 'absolute' && inTree(id) && parents.has(id));
const ringOn = () => { const r = rings(); return r.length === 1 ? parents.get(r[0][0]) : r.length ? 'many' : null; };
// the node a label is drawn in (its Text's parent), and whether the ring is around it
const textNode = (label) => [...texts].find(([id, s]) => s === label && inTree(id))?.[0];
const ringAround = (label) => { const r = ringOn(); for (let n = textNode(label); n; n = parents.get(n)) if (n === r) return true; return false; };
const heard = [];
rt.addListener('keydown', (e) => heard.push(e.key));

check('no ring before the keyboard', ringOn() === null);
await space();
check('Space with no focus goes to the app', heard.includes(' ') && !now.presses.one);

await tab();
check('Tab goes to the first control', ringAround('One'), String(ringOn()));
heard.length = 0;
await space();
check('Space presses the focused button', now.presses.one === 1, JSON.stringify(now.presses));
check('the key it used reaches no listener', !heard.includes(' '), heard.join(','));
await key('Enter');
check('Enter presses it too', now.presses.one === 2);

await tab();
check('Tab skips the disabled and the hidden buttons', ringOn() !== null && !ringAround('One') && !ringAround('Off') && !ringAround('Hidden'));
await space();
check('Space flips the Toggle', now.on === true);
await shiftTab();
check('Shift+Tab goes back', ringAround('One'));
await tab();

await tab();
check('Tab gives a TextInput the keys', hasTextFocus());
check('a TextInput draws its own focus, no ring', ringOn() === null);
await key('x', 'x');
check('typing replaces its text, all selected', now.text === 'x', now.text);
await tab();
check('Tab goes on from the TextInput', !hasTextFocus() && ringOn() !== null);
await key('ArrowRight');
check('→ moves the Slider a step', now.slider === 55, String(now.slider));
await key('End');
check('End takes it to the end', now.slider === 100, String(now.slider));
await key('PageDown');
check('Page Down a tenth', now.slider === 90, String(now.slider));
await key('Home');
check('Home to the start', now.slider === 0, String(now.slider));

await tab();
await key('ArrowUp');
check('↑ adds a step to the Stepper', now.stepper === 4, String(now.stepper));
await key('End');
check('End takes the Stepper to max', now.stepper === 10, String(now.stepper));

await tab();
await key('ArrowRight');
check('→ chooses the next segment', now.seg === 'b', now.seg);
await key('End');
check('End the last one', now.seg === 'c', now.seg);

await tab();
await space();
check('Space opens the MenuButton', !!textNode('First'));
await key('ArrowDown');
await key('ArrowDown');
await key('Enter');
check('↓ skips the separator and Enter chooses', now.chose === 'second' && !textNode('First'), now.chose);
await key('ArrowDown');
check('↓ on the button opens it too', !!textNode('First'));
await key('Escape');
check('Escape closes it', !textNode('Second'));

await tab();
check('Tab reaches the last button', ringAround('Last'));
await tab();
check('a scroll view whose content does not fit is a stop', ringOn() !== null && !ringAround('Last'));
const shift = () => { for (const [id, s] of styles) if ('translateY' in s && inTree(id)) return -s.translateY || 0; return null; };
await key('PageDown');
check('Page Down scrolls it', shift() > 0, String(shift()));
await key('End');
check('End scrolls to the end', shift() === 900, String(shift()));
await tab();
check('Tab wraps round to the first', ringAround('One'));

// a press of the pointer ends the focus; Tab goes on from where it pressed
await click(parents.get(textNode('Last')));
check('a press hides the ring', ringOn() === null);
heard.length = 0;
await space();
check('and Space is the app\'s again', heard.includes(' ') && now.presses.last === 1, `${heard.join(',')} ${now.presses.last}`);
await tab();
check('Tab goes on from the press', ringOn() !== null && !ringAround('Last') && !ringAround('One'));

// a button shown again counts, one that goes away gives up the focus
now.hide = false; rerender(); await flush();
await click(parents.get(textNode('One')));
await tab();
check('a button shown again is a stop', ringAround('Hidden'));
await tab(); await tab(); await tab(); await tab(); await tab(); await tab(); await tab();
check('on Last again', ringAround('Last'));
now.showLast = false; rerender(); await flush();
check('the ring goes with the button', ringOn() === null);
await tab();
check('Tab goes on after it went away', ringOn() !== null);

// a listener that wants Tab (the Terminal) keeps it
const off = rt.addListener('keydown', (e) => { if (e.key === 'Tab') e.preventDefault(); });
const before = ringOn();
await tab();
check('a prevented Tab moves no focus', ringOn() === before);
off();

// a confirmation dialog in front keeps its keys and hides the ring
const tok = {};
dialogStack.open(tok, null, true);
await flush();
check('a dialog hides the ring', ringOn() === null);
heard.length = 0;
await key('Enter');
check('Enter goes to the dialog, not the focused control', heard.includes('Enter'));
dialogStack.close(tok);
await flush();
check('the ring is back when it closes', ringOn() !== null);
process.exit(0);
