// textinput_select_scene.mjs — the scene of `node tests/focus_scene_test.mjs textinput_select_scene.mjs` (not run by
// itself): the SDK's TextInput with select (Files' rename: the name without its extension selected first; ws170 QA
// MEJORA-1). Typing replaces the name and keeps ".txt"; a folder's whole name is replaced. Prints "ok <name>" or
// "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { TextInput } from '../../sdk/components.jsx';
import { renameSelection } from '../../apps/files/names.js';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => console.log(got === want ? `ok ${name}` : `FAIL ${name} got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
const CODES = { n: 49, u: 22, e: 18, v: 47, o: 24 };
const type = async (s) => { for (const ch of s) { __lordKey(CODES[ch], 1, ch, ch, ch, 0); __lordKey(CODES[ch], 0, ch, '', ch, 0); await flush(); } };

let value = '', show = null;
function Field({ name, dir }) {
  const [v, setV] = useState(name);
  value = v;
  return h(TextInput, { value: v, onChange: setV, autoFocus: true, selectAll: true, select: renameSelection(name, dir) });
}
function Scene() {
  const [which, set] = useState('file');
  show = set;
  return which === 'file' ? h(Field, { key: 'f', name: 'archivo-3.txt' }) : h(Field, { key: 'd', name: 'fotos.2024', dir: true });
}
rt.render(h(Scene));
await flush();
await type('nuevo');
check('a file: the name replaced, the extension kept', value, 'nuevo.txt');
show('dir');
await flush();
await type('nuevo');
check('a folder: its whole name replaced', value, 'nuevo');
process.exit(0);
