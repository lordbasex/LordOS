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
import { ThemeProvider, monthName, setBacklight } from '../../sdk/index.js';

const { styles, texts, parents, removed, calls } = globalThis.__lordTest;
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

// (issue #114) Do Not Disturb, Night Light, the brightness slider (there is a backlight) and Log Out, all by default
check('cc: do not disturb tile', shown('shell.classic.dnd'), true);
check('cc: night light tile', shown('shell.classic.nightLight'), true);
check('cc: brightness with a backlight', shown('prefs.display.brightness'), true);
check('cc: the slider shows the backlight level', shown('40 %'), true);
check('cc: log out by default', shown('shell.menu.logout'), true);
// a switch tile turns on and off with one click anywhere on it, and its color says which
const tileOf = (s) => parents.get(parents.get(parents.get(textNode(s))));
const dndTile = tileOf('shell.classic.dnd');
const accent = parseInt(`ff${t.accent.slice(1)}`, 16);      // lord.setStyle gets colors as 0xAARRGGBB
check('cc: do not disturb off: grey', styles.get(dndTile)?.backgroundColor === accent, false);
await click(dndTile);
check('cc: one click turns do not disturb on', settings.get().notifications?.dnd, true);
check('cc: do not disturb on: the accent', styles.get(tileOf('shell.classic.dnd'))?.backgroundColor, accent);
await click(tileOf('shell.classic.dnd'));
check('cc: another click turns it off', settings.get().notifications?.dnd, false);
await click(tileOf('shell.classic.nightLight'));
check('cc: one click turns night light on', settings.get().night?.manual?.on, true);
await click(tileOf('shell.cc.dark'));
check('cc: one click on dark mode', settings.get().mode, 'light');
await click(tileOf('shell.cc.dark'));
check('cc: and back', settings.get().mode, 'dark');
// the brightness slider's writes: the closed admin action, at most one per 120 ms, with the last position
calls.length = 0;
setBacklight('intel_backlight', 300); setBacklight('intel_backlight', 500); setBacklight('intel_backlight', 650);
await new Promise((res) => setTimeout(res, 200)); await flush();
check('cc: brightness sends its last position once', calls.filter((c) => c.startsWith('admin:')).join(','), 'admin:display-brightness:intel_backlight 650');
// Log Out and the new tiles can be hidden like the metrics; Settings stays
settings.set({ menuBar: { ccLogout: false, ccDnd: false, ccNight: false, ccBrightness: false } });
await flush();
check('cc: log out hidden', shown('shell.menu.logout'), false);
check('cc: settings stays', shown('shell.menu.settings'), true);
check('cc: do not disturb hidden', shown('shell.classic.dnd'), false);
check('cc: night light hidden', shown('shell.classic.nightLight'), false);
check('cc: brightness hidden', shown('prefs.display.brightness'), false);
check('cc: memory stays when log out is hidden', shown('shell.cc.memory'), true);
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
// #197: each week is its own row of seven equal cells; a wrapping row of W / 7 wide cells lost the seventh day to
// the next line (the panel's border made the row 2 px narrower than seven cells)
{
  const day = textNode('15');                       // the <Text>, in its circle, in its cell, in its week
  const cell = parents.get(parents.get(day)), row = parents.get(cell);
  const kids = [...parents].filter(([c, p]) => p === row && attached(c)).length;
  check('clock: a week is a row of seven', kids, 7);
  check('clock: the week row does not wrap', styles.get(row).flexWrap === undefined || styles.get(row).flexWrap === 'nowrap', true);
  check('clock: a day cell takes a seventh (flex), not a computed width', styles.get(cell).flexGrow === 1 && styles.get(cell).width === undefined, true);
}
settings.set({ menuBar: { calendar: false } });
await flush();
check('clock: calendar hidden', shown(month), false);
check('clock: notifications stay', shown('shell.classic.notifications'), true);
process.exit(0);
