// screenshots_overlay_close_scene.mjs — a scene of tests/textfield_menu_test.mjs (ws170 QA BUG-28; not run by itself):
// Screenshots' area selection (apps/screenshots/index.jsx capture.area) on two displays, its overlays then closed from
// elsewhere (an agent's close_window, wm.close). The whole selection must end: its keys (Space, C, T) stop being
// shortcuts, and a text field typed in afterwards gets every key ("c t ca to", not "a o"). While the overlays are up
// those keys are the selection's. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import '@lordos/sdk';                                  // its stubs first (the shim), then the modules below
import { enableShellLayers } from '../../sdk/layers.js';
import { TextInput } from '../../sdk/components.jsx';
import { windows, configureWindows, wm } from '../../sdk/windows.js';
import { resetShortcuts } from '../../sdk/shortcuts.js';
import { capture } from '../../apps/screenshots/index.jsx';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};

lord.perms = { get: () => 'allow', names: () => [] };
lord.wayland = new Proxy({}, { get: () => () => undefined });
lord.fs = new Proxy({ home: () => '/home/test', stat: () => { throw new Error('ENOENT'); } }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
let nextHandle = 100;
lord.media = new Proxy({ screenshot: () => nextHandle++, info: () => ({ width: 1920, height: 1080 }) },
  { get: (o, k) => (k in o ? o[k] : () => undefined) });
lord.screen.displays = [{ id: 1, x: 0, y: 0, w: 1920, h: 1080, main: true, scale: 100 }, { id: 2, x: 1920, y: 0, w: 1920, h: 1080, scale: 100 }];
lord.screen.width = 3840; lord.screen.height = 1080; lord.screen.mirror = false;
globalThis.__lordScreen();
configureWindows({ apps: [{ id: 'screenshots', name: 'Screenshots', size: [440, 420] }] });
enableShellLayers();
resetShortcuts();

let value = '';
function Field() {
  const [v, setV] = useState('');
  value = v;
  return h(TextInput, { value: v, onChange: setV, autoFocus: true });
}
const CODES = { a: 30, c: 46, o: 24, t: 20, ' ': 57 };
const type = async (s) => {
  for (const ch of s) { __lordKey(CODES[ch], 1, ch, ch, ch, 0); __lordKey(CODES[ch], 0, ch, '', ch, 0); await flush(); }
};
const overlays = () => windows.get().filter((w) => w.app === 'screenshots' && String(w.instance || w.id).includes('overlay'));

rt.render(h(Field));
await flush();
capture.area('area');
await sleep(400);
await flush();
check('two overlays, one per display', overlays().length, 2);
await type('c');
check('while selecting, C is the selection\'s (not typed)', value, '');
for (const w of overlays()) wm.close(w.id, true);      // what an agent's close_window does
await flush();
await type('c t ca to');
check('overlays closed from elsewhere: every key reaches the field', value, 'c t ca to');
capture.area('area');
await sleep(400);
await flush();
wm.close(overlays()[0].id, true);                     // one of them: the whole selection ends
await flush();
check('one overlay closed: the others go too', overlays().length, 0);
await type('ta');
check('and the keys are the field\'s again', value, 'c t ca tota');
process.exit(0);
