// password_protect_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): every password surface LordOS
// draws is a protected node (lord.setProp(id, 'protected', true)), which lord-server paints black in every capture of the
// screen or an area, recordings, the remote desktop and shared screens, and refuses captured alone (engine-test's
// test_capture_protected checks the C side). The maintainer's rule (2026-11-22): a password is never captured.
//   - any password TextInput (secure) is protected, and stops being so when it stops being a password field; a plain field
//     never is (so the Wi-Fi, VPN, SSH, Cloud, Mail, Users, remote desktop and login fields, all secure TextInputs);
//   - the keyring's unlock prompt (sdk/vault.js KeyringPrompt) is protected whole;
//   - the administrator prompt for a Linux app (polkit, sdk/hardware.jsx PolkitPrompt) is protected whole, dots included.
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import './password_protect_scene_stub.js';
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { TextInput } from '../../sdk/components.jsx';
import { enableShellLayers } from '../../sdk/layers.js';
import { KeyringPrompt, askKeyringUnlock } from '../../sdk/vault.js';
import { PolkitPrompt, polkitPrompt } from '../../sdk/hardware.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const prot = new Map();
lord.setProp = (id, k, v) => { if (k === 'protected') prot.set(id, !!v); };
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (pred) => [...texts].find(([id, x]) => pred(x) && inTree(id))?.[0] || null;
// the text is inside a protected node (it or an ancestor), as lord-server's paint_node_protected sees it
const covered = (id) => { for (let n = id; n; n = parents.get(n)) if (prot.get(n)) return true; return false; };

const set = {};
function Fields() {
  const [pw, setPw] = useState('hunter2');
  const [plain, setPlain] = useState('visible text');
  const [secure, setSecure] = useState(true);
  set.secure = setSecure;
  return h(rt.View, null,
    h(TextInput, { value: pw, onChange: setPw, secure }),
    h(TextInput, { value: plain, onChange: setPlain }));
}
enableShellLayers();
rt.render(h(rt.View, null, h(Fields), h(KeyringPrompt), h(PolkitPrompt)));
await flush();

const dots = textNode((x) => x === '•••••••');
check('a password field shows its dots', !!dots, JSON.stringify([...texts.values()].slice(0, 20)));
check('a password field is protected', dots && covered(dots));
const plain = textNode((x) => x === 'visible text');
check('a plain field is not', plain && !covered(plain));
set.secure(false);
await flush();
const shown = textNode((x) => x === 'hunter2');
check('no longer a password field: no longer protected', shown && !covered(shown));
set.secure(true);
await flush();
check('a password field again: protected again', covered(textNode((x) => x === '•••••••')));

askKeyringUnlock();
await flush();
const kt = textNode((x) => /keyring/i.test(x));
check('the keyring prompt shows', !!kt, JSON.stringify([...texts.values()].filter((x) => /eyring|unlock/i.test(x))));
check('the keyring prompt is protected whole (its title too)', kt && covered(kt));

polkitPrompt.set({ id: 7, state: 'ask', local: true, count: 5, message: 'Authentication is needed to mount the disk', action: 'org.example.mount', account: 'ana', comm: 'udisksctl' });
await flush();
const pm = textNode((x) => x === 'Authentication is needed to mount the disk');
const pd = textNode((x) => x === '•••••');
check('the administrator prompt shows its dots', !!pm && !!pd);
check('the administrator prompt is protected whole', pm && pd && covered(pm) && covered(pd));
process.exit(0);
