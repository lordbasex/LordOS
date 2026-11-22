// vm_keys_scene.mjs — the scene of `node tests/focus_scene_test.mjs vm_keys_scene.mjs` (not run by itself): a machine's
// window and LordOS's Terminal rendered through runtime/index.js, the SDK's AppHost and FloatingWindows over a stub
// `lord`, driven with the server's own key events (__lordKey). Security: a key typed while another window or a system
// prompt has the keyboard never reaches lord.vm.key, whether the machine is framed or full screen, and what was held
// in the guest is let go of. The machine forwards keys as apps/machines (vmwindow.jsx, appsmode.jsx GuestWindow) do:
// a listener installed while its window is focused, asking wm.hasKeyboard at each key (model.js guestKeys).
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useEffect } from 'react';
import * as rt from '@lordos/runtime';
import { AppHost, FloatingWindows, useAppWindow } from '../../sdk/components.jsx';
import { registerBuiltinApp, wm, useWindows } from '../../sdk/windows.js';
import { Layer, enableShellLayers } from '../../sdk/layers.js';
import { guestKeys } from '../../apps/machines/model.js';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

// lord.vm: what the guest was sent
const sent = [];
lord.vm = { key: (sid, code, down) => sent.push(['key', code, down]), release: () => sent.push(['release']) };

const typedIn = { term: [] };
const focusOf = {};
function Machine() {
  const { id, focused } = useAppWindow();
  focusOf[id] = focused;
  useEffect(() => {
    if (!focused) { lord.vm.release(1); return undefined; }
    const gk = guestKeys({ send: (c, v) => lord.vm.key(1, c, v), owns: () => wm.hasKeyboard(id), release: () => lord.vm.release(1) });
    const a = rt.addListener('keydown', (e) => { if (gk.down(e.code)) e.preventDefault?.(); });
    const b = rt.addListener('keyup', (e) => { gk.up(e.code); });
    return () => { a(); b(); lord.vm.release(1); };
  }, [focused]);
  return h(rt.View, null);
}
function Term() {
  const { id, focused } = useAppWindow();
  focusOf[id] = focused;
  useEffect(() => (focused ? rt.addListener('keydown', (e) => { if (e.char) typedIn.term.push(e.char); }) : undefined), [focused]);
  return h(rt.View, null);
}
registerBuiltinApp({ id: 'vm', Component: Machine });
registerBuiltinApp({ id: 'term', Component: Term });

// a theme as the stock ones are: each framed window with focused = the top framed window of the desktop
let prompt = false, setPrompt = null;
function Theme() {
  const { mounted, focused } = useWindows();
  const [p, set] = React.useState(false);
  setPrompt = set;
  prompt = p;
  return h(rt.View, null,
    ...mounted.map((w) => h(rt.View, { key: w.id }, h(AppHost, { id: w.id, focused: w.id === focused }))),
    h(FloatingWindows),
    p ? h(Layer, { name: 'system' }, h(rt.View, null)) : null);
}
enableShellLayers();
wm.open('vm');
wm.open('term');                     // the Terminal in front
rt.render(h(Theme));
await flush();

// a key as lord-server sends it with libxkbcommon (code, value, key, chr)
const type = async (code, ch) => { __lordKey(code, 1, ch, ch, ch, 0); __lordKey(code, 0, ch, '', ch, 0); await flush(); };
const word = async (s) => { for (const ch of s) await type(30 + (ch.charCodeAt(0) % 20), ch); };

// 1. framed: the Terminal in front
sent.length = 0;
await word('secret');
check('framed: the Terminal gets what is typed in it', typedIn.term.join('') === 'secret', typedIn.term.join(''));
check('framed: the machine is not focused', focusOf.vm === false && focusOf.term === true, JSON.stringify(focusOf));
check('framed: nothing typed in the Terminal reaches the guest', !sent.some((x) => x[0] === 'key'), JSON.stringify(sent));

// 2. the machine in front: its keys go in
wm.focus('vm');
await flush();
sent.length = 0;
__lordKey(42, 1, 'Shift', '', '', 1);    // Shift held in the guest
await flush();
check('machine in front: a key goes to the guest', sent.length === 1 && sent[0][1] === 42 && sent[0][2] === 1, JSON.stringify(sent));
// focus moves to the Terminal; the machine's listener may still be there for this key (no render yet)
sent.length = 0;
wm.focus('term');
__lordKey(25, 1, 'p', 'p', 'p', 0);
__lordKey(25, 0, 'p', '', 'p', 0);
await flush();
check('focus moved: the key typed next does not reach the guest', !sent.some((x) => x[0] === 'key'), JSON.stringify(sent));
check('focus moved: what was held in the guest is let go of', sent.some((x) => x[0] === 'release'), JSON.stringify(sent));

// 3. full screen: the machine full screen, then the Terminal brought forward (another display): before, both were
// "focused" (FloatingWindows gave every full-screen window of the desktop the keys) and a password reached the guest
wm.focus('vm');
wm.setFullscreen('vm', true);
await flush();
check('full screen: the machine has the keyboard', focusOf.vm === true && focusOf.term === false && wm.hasKeyboard('vm'), JSON.stringify(focusOf));
wm.focus('term');
await flush();
sent.length = 0;
typedIn.term.length = 0;
await word('hunter2');
check('full screen behind: the Terminal gets the password', typedIn.term.join('') === 'hunter2', typedIn.term.join(''));
check('full screen behind: the machine is not focused', focusOf.vm === false && focusOf.term === true, JSON.stringify(focusOf));
check('full screen behind: no key reaches the guest', !sent.some((x) => x[0] === 'key'), JSON.stringify(sent));

// 4. a system prompt (password, keyring) over the machine in front: its keys are LordOS's
wm.focus('vm');
await flush();
setPrompt(true);
await flush();
sent.length = 0;
await word('adminpw');
check('system prompt: wm.hasKeyboard is false for the machine', rt.shellHasKeyboard() && !wm.hasKeyboard('vm'), String(prompt));
check('system prompt: no key typed into it reaches the guest', !sent.some((x) => x[0] === 'key'), JSON.stringify(sent));
setPrompt(false);
await flush();
sent.length = 0;
await type(30, 'a');
check('prompt closed: the machine has its keys again', sent.some((x) => x[0] === 'key' && x[1] === 30), JSON.stringify(sent));
