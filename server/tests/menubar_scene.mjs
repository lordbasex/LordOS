// menubar_scene.mjs — the scene of tests/menubar_test.mjs (not run by itself), issue #58: Lord Flow's Control Center
// and clock panel and the shared output list, rendered through runtime/index.js and the real SDK with the shell's
// layers on, over a stub `lord` (the bundle's banner) with two sound outputs. It turns the choices of Settings →
// Desktop & Dock → Menu Bar / Control Center on and off and reads what is drawn. Prints one line per check: "ok
// <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { settings } from '../../sdk/settings.js';
import { SoundOutputList } from '../../sdk/soundoutputs.js';
import { ControlCenter } from '../../themes/default/components/ControlCenter.jsx';
import { NotificationCenter } from '../../themes/default/components/NotificationCenter.jsx';
import { makeTheme } from '../../themes/default/tokens.js';
import { ThemeProvider, monthName } from '../../sdk/index.js';

const { texts, parents, removed, calls } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => console.log(got === want ? `ok ${name}` : `FAIL ${name} got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
const click = async (id) => {
  globalThis.__lordEvent('down', id, 10, 10, 0); await flush();
  globalThis.__lordEvent('up', id, 10, 10, 0); await flush();
};
// a node is on screen when neither it nor any node above it left the tree
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const shown = (s) => { for (const [id, v] of texts) if (v === s && attached(id)) return true; return false; };
// the <Text> that shows s now (lord-server hits it, not its raw text node)
const textNode = (s) => { let r = 0; for (const [id, v] of texts) if (v === s && attached(id)) r = parents.get(id) || 0; return r; };

enableShellLayers();
const t = makeTheme({ mode: 'dark', accent: 'azul', effects: true });
const area = { x: 0, y: 0, w: 1920, h: 1080 };
let which = 'cc';
let rerender = null;
function Scene() {
  const [, set] = React.useState(0);
  rerender = () => set((v) => v + 1);
  const body = which === 'cc' ? h(ControlCenter, { t, top: 28, area, onClose: () => {} })
    : which === 'clock' ? h(NotificationCenter, { t, top: 28, area, onClose: () => {} })
      : h(SoundOutputList, { onPick: () => {} });
  return h(ThemeProvider, { tokens: t }, body);
}
rt.render(h(Scene));
await flush();

// the Control Center: everything on by default (a settings.json without "menuBar")
check('cc: memory shown by default', shown('shell.cc.memory'), true);
check('cc: network shown by default', shown('shell.cc.network'), true);
check('cc: bluetooth tile with an adapter', shown('shell.cc.bluetooth'), true);
check('cc: wi-fi tile with a radio', shown('shell.cc.wifi'), true);
settings.set({ menuBar: { ccSystem: false, ccNetwork: false, ccWifi: false, ccBluetooth: false } });
await flush();
check('cc: memory hidden', shown('shell.cc.memory'), false);
check('cc: network hidden', shown('shell.cc.network'), false);
check('cc: bluetooth hidden', shown('shell.cc.bluetooth'), false);
check('cc: wi-fi hidden', shown('shell.cc.wifi'), false);
check('cc: sound stays', shown('shell.cc.sound'), true);

// the output list: both outputs, a click on the other one makes it the default
which = 'outputs'; rerender(); await flush();
check('outputs: the title', shown('shell.cc.output'), true);
check('outputs: built-in', shown('Built-in Audio'), true);
check('outputs: headset', shown('Headset'), true);
calls.length = 0;
await click(textNode('Headset'));
check('outputs: a click sets the default output', calls.join(','), 'setDefault:sink:bluez_output.AA_BB');
calls.length = 0;
await click(textNode('Built-in Audio'));
check('outputs: the default one is not set again', calls.join(','), '');

// the clock panel: the calendar unless it was turned off
which = 'clock'; settings.set({ menuBar: {} }); rerender(); await flush();
const month = `${monthName(new Date().getMonth())} ${new Date().getFullYear()}`;
check('clock: notifications title', shown('shell.classic.notifications'), true);
check('clock: calendar shown', shown(month), true);
settings.set({ menuBar: { calendar: false } });
await flush();
check('clock: calendar hidden', shown(month), false);
check('clock: notifications stay', shown('shell.classic.notifications'), true);
process.exit(0);
