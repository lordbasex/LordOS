// window_sheet_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): an app window's sheet
// (<WindowSheet>, sdk/layers.js) is drawn in its window's own overlay host, never in the shell's modal layer. QA
// 0.8.9.6.2 (BUG-D): Settings → AI's model sheet, in the modal layer, stayed drawn over Thunderbird and the launcher
// opened after it. Outside a window (a page rendered alone) it falls back to the modal layer. Prints "ok <name>" or
// "FAIL <name> <detail>".
import React, { useState } from 'react';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import * as rt from '@lordos/runtime';
import { enableShellLayers, WindowSheet, Layer, LAYERS } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { ConfirmDialog } from '../../sdk/controls.jsx';
import { boxAt } from '../../sdk/contextmenu.jsx';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => String(x) === s && inTree(id))?.[0];
const under = (id, anc) => { for (let n = id; n; n = parents.get(n)) if (n === anc) return true; return false; };

enableShellLayers();
const hostIds = {};
// a stand-in for AppHost: the window's content, then its overlay host over it
function Win({ name, children }) {
  const [host, setHost] = useState(null);
  if (host) hostIds[name] = host.id;
  return h(AppWindowContext.Provider, { value: { id: 7, args: null, focused: true, overlayHost: host } },
    h(rt.View, { style: { flex: 1 } }, children, h(rt.View, { ref: setHost, style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 } })));
}

rt.render(h(rt.View, null,
  h(Win, { name: 'a' }, h(WindowSheet, null, (a) => h(rt.Text, null, `sheet ${a.x},${a.y} ${a.w}x${a.h}`))),
  h(Win, { name: 'b' }, h(Layer, { name: 'modal' }, h(rt.Text, null, 'old modal'))),
  h(WindowSheet, null, h(rt.Text, null, 'no window')),
  h(ConfirmDialog, { title: 'Trust it?', okLabel: 'Trust', onOk: () => {}, onCancel: () => {} })));
await flush();
await flush();
const modal = rt.layerContainer(LAYERS.modal);
const sheet = textNode('sheet 0,0 1920x1080');
check('the sheet is drawn, its area from the host (0, 0, its size)', !!sheet, [...texts.values()].join(' | '));
check('in its window\'s overlay host', !!sheet && hostIds.a > 0 && under(sheet, hostIds.a));
check('never in the shell\'s modal layer', !!sheet && !!modal && !under(sheet, modal.id));
const old = textNode('old modal');
check('(a <Layer name="modal"> is in the modal layer, above every window)', !!old && under(old, modal.id));
const alone = textNode('no window');
check('outside a window: the modal layer', !!alone && under(alone, modal.id));
// a dialog given no cancelLabel still says Cancel (QA 0.8.9.6.2: the community theme trust dialog's button was empty)
check('ConfirmDialog without cancelLabel: "Cancel"', !!textNode('Cancel'), [...texts.values()].join(' | '));
// a text field's menu near the bottom of the work area goes up, above the field, instead of being cut (QA 0.8.9.6.2)
{
  const items = Array.from({ length: 9 }, (_, i) => ({ label: `item ${i}` }));
  const b = boxAt(items, 10, 20, 200, { left: -300, top: -700, w: 900, h: 60 });
  check('a field\'s menu near the bottom opens upward', b.top < 0 && b.top + 10 + 9 * 26 <= 60 + 30, JSON.stringify(b));
  const plain = boxAt(items, 10, 20, 200, { w: 4000, h: 4000 });
  check('with room below it stays where it was asked', plain.top === 20 && plain.left === 10, JSON.stringify(plain));
}
// and no app draws a window's sheet in the modal layer any more (run from server/, as run_suites.sh does)
const found = [];
const walk = (d) => {
  for (const f of readdirSync(d)) {
    const p = path.join(d, f);
    if (statSync(p).isDirectory()) { if (f !== 'node_modules') walk(p); }
    else if (/\.(jsx?|mjs)$/.test(f) && /<Layer name="modal"|Layer, \{ name: 'modal'/.test(readFileSync(p, 'utf8'))) found.push(p);
  }
};
walk(path.resolve(process.cwd(), '..', 'apps'));
check('no app uses <Layer name="modal"> for its window\'s sheets', !found.length, found.join(' '));
process.exit(0);
