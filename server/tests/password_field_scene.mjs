// password_field_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): a password TextInput rendered
// through runtime/index.js with the same stub `lord`, driven with the server's own events (__lordEvent, __lordKey): the
// eye shows the text and keeps the focus, nothing is copied or cut while it is shown, a click elsewhere hides it again,
// and ⇪ with "Caps Lock is on" follows the Caps Lock bit of the keys while the field has focus. Prints one line per
// check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { TextInput } from '../../sdk/components.jsx';
import { enableShellLayers } from '../../sdk/layers.js';

const { styles, parents, texts, removed, clip } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const ids = {};
const now = { pw: 'abc', own: 'xyz' };
function Scene() {
  const [v, setV] = useState(now.pw);
  const [o, setO] = useState(now.own);
  now.pw = v; now.own = o;
  return h(rt.View, { ref: (r) => { if (r) ids.root = r.id; } },
    h(rt.View, { ref: (r) => { if (r) ids.pwBox = r.id; } }, h(TextInput, { value: v, onChange: setV, secure: true })),
    h(rt.View, { ref: (r) => { if (r) ids.ownBox = r.id; } }, h(TextInput, { value: o, onChange: setO, secure: true, eye: false })));
}
enableShellLayers();
rt.render(h(Scene));
await flush();

const childOf = (p) => [...parents].find(([, q]) => q === p)?.[0];
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const under = (id, top) => { for (let n = id; n; n = parents.get(n)) if (n === top) return true; return false; };
const click = async (id, x, y = 5, button = 0) => { __lordEvent('down', id, x, y, button); __lordEvent('up', id, x, y, button); await flush(); };
// a key as lord-server sends it with libxkbcommon: flags 2 Ctrl, 32 Caps Lock
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const shows = (s, top) => [...texts].some(([id, x]) => x === s && inTree(id) && under(id, top));
// the eye: the 20 × 20 button at the field's end
const eyeOf = (top) => [...styles].find(([id, st]) => st.width === 20 && st.height === 20 && st.borderRadius === 4 && inTree(id) && under(id, top))?.[0];
const ctrl = 2, caps = 32;

const pw = childOf(ids.pwBox), own = childOf(ids.ownBox);
check('found the fields', !!pw && !!own);
check('a password field draws dots', shows('•••', ids.pwBox));
check('it has the eye', !!eyeOf(ids.pwBox));
check('a field with its own Show has none', !eyeOf(ids.ownBox));

await click(pw, 500);
await key('d', 'd');
check('typing in it', now.pw === 'abcd' && shows('••••', ids.pwBox), now.pw);
await click(eyeOf(ids.pwBox), 0);
check('the eye shows the text', shows('abcd', ids.pwBox));
await key('e', 'e');
check('the field keeps the focus after the eye', now.pw === 'abcde' && shows('abcde', ids.pwBox), now.pw);
clip.text = 'before';
await key('a', '', ctrl);
await key('c', '', ctrl);
check('nothing is copied while it is shown', clip.text === 'before', clip.text);
await key('x', '', ctrl);
check('nothing is cut while it is shown', now.pw === 'abcde' && clip.text === 'before', `${now.pw} / ${clip.text}`);
await key('End');
await click(eyeOf(ids.pwBox), 0);
check('a second click hides it', shows('•••••', ids.pwBox));
await click(eyeOf(ids.pwBox), 0);
await click(ids.root, 1900);
check('a click elsewhere hides it again', shows('•••••', ids.pwBox) && !shows('abcde', ids.pwBox));

// Caps Lock: the bit every key carries
await click(pw, 500);
await key('A', 'A', caps);
check('Caps Lock on: ⇪ and the words', shows('⇪', ids.pwBox) && shows('Caps Lock is on', ids.pwBox));
await key('CapsLock', '', 0);
check('Caps Lock off: no hint', !shows('⇪', ids.pwBox));
await key('B', 'B', caps);
await click(ids.root, 1900);
check('no hint without focus', !shows('⇪', ids.pwBox));
await click(pw, 500);
check('back in the field with Caps Lock still on: the hint again', shows('⇪', ids.pwBox));
await click(own, 500);
check('the other password field shows it too, and the first one no longer', shows('⇪', ids.ownBox) && !shows('⇪', ids.pwBox));
process.exit(0);
