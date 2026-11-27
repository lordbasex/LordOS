// ime_commit_scene.mjs — the scene of `node tests/focus_scene_test.mjs ime_commit_scene.mjs` (not run by itself): the
// SDK's TextInput, a controlled field (its value in the parent's state), receiving keys as lord-server
// sends them while an input method composes (server/src/ui/js.c js_key). A key that ends a Korean syllable (Space,
// punctuation, Enter) arrives as two __lordKey calls in one turn: the committed syllable (code 0) and then the key
// itself. Both must land: 가 then Space is "가 ", never " " (ws170 QA BUG-20). Prints "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { TextInput } from '../../sdk/components.jsx';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => console.log(got === want ? `ok ${name}` : `FAIL ${name} got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);

let value = '';
function Scene() {
  const [v, setV] = useState('');
  value = v;
  return h(TextInput, { value: v, onChange: setV, autoFocus: true });
}
// the committed text (code 0, as ime_commit sends it) and the key that ended the syllable, without a turn between them
const commitThen = (text, key, chr, code) => {
  __lordKey(0, 1, text, text, '', 0);
  __lordKey(code, 1, key, chr, chr || key.toLowerCase(), 0);
  __lordKey(code, 0, key, '', '', 0);
};
rt.render(h(Scene));
await flush();
commitThen('가', ' ', ' ', 57);
await flush();
check('TextInput: 가 then Space', value, '가 ');
commitThen('빠', ' ', ' ', 57);
await flush();
check('TextInput: 빠 then Space after it', value, '가 빠 ');
commitThen('아', '.', '.', 52);
await flush();
check('TextInput: 아 then a full stop', value, '가 빠 아.');
commitThen('다', 'Enter', '', 28);
await flush();
check('TextInput: 다 then Enter', value, '가 빠 아.다');

process.exit(0);
