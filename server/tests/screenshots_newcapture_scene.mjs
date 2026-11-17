// screenshots_newcapture_scene.mjs — a scene of tests/textfield_menu_test.mjs (GitHub #166; not run by itself): the
// Screenshots markup editor (apps/screenshots/editor.jsx) after a capture has New Screenshot, and it and Ctrl+N bring
// the capture panel back while the editor stays open; without onNewCapture (no capture panel to go back to) the button
// isn't there. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { Editor } from '../../apps/screenshots/editor.jsx';
import shotsEn from '../../assets/locales/en/screenshots.json';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'screenshots' ? JSON.stringify(shotsEn) : shellCatalog(lang, ns));
// a capture of 800 × 600 in lord-server: the editor's media calls answer as for a real one
lord.media = new Proxy({ info: () => ({ width: 800, height: 600 }), annotate: () => 2, view: () => true }, { get: (o, k) => (k in o ? o[k] : () => undefined) });

const label = shotsEn.newCapture;
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const button = () => { const raw = [...texts].find(([id, s]) => s === label && inTree(id))?.[0]; return raw ? parents.get(parents.get(raw)) : null; };
const click = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };
const key = async (name, chr, flags) => { __lordKey(0, 1, name, chr, chr, flags); __lordKey(0, 0, name, '', '', flags); await flush(); };

let asked = 0;
let withPanel = true, setWithPanel = null;
function Scene() {
  const [on, set] = React.useState(withPanel);
  setWithPanel = set;
  return h(AppWindowContext.Provider, { value: { id: 'screenshots#edit1', focused: true, args: null } },
    h(rt.View, { style: { width: 1000, height: 680 } },
      h(Editor, { handle: 1, app: 'screenshots', newCapturePath: () => '/tmp/x.png', ...(on ? { onNewCapture: () => { asked++; } } : null) })));
}
enableShellLayers();
rt.render(h(Scene));
await flush();

check('the English label is "New Screenshot"', label === 'New Screenshot', label);
check('after a capture the toolbar has New Screenshot', !!button());
let b = button();
// a press reaches the Button's own node or the text inside it
for (let n = b, i = 0; n && i < 3 && !asked; n = parents.get(n), i++) await click(n);
check('New Screenshot asks for the panel', asked === 1, `asked ${asked}`);
check('the editor is still there after it', !!button());
asked = 0;
await key('n', 'n', 2);
check('Ctrl+N asks for the panel too', asked === 1, `asked ${asked}`);
asked = 0;
await key('n', 'n', 0);
check('n alone does not', asked === 0, `asked ${asked}`);
setWithPanel(false);
await flush();
check('no onNewCapture: no button', !button());
await key('n', 'n', 2);
check('no onNewCapture: Ctrl+N does nothing', asked === 0, `asked ${asked}`);
process.exit(0);
