// classic_startmenu_power_scene.mjs — the scene of tests/classic_startmenu_power_test.mjs (not run by
// itself): the Lord Classic start menu (themes/classic/components/StartMenu.jsx) rendered through
// runtime/index.js and the real SDK with the shell's layers on, over a stub `lord` (the bundle's banner).
// It opens the power menu and clicks a row the way lord-server sends a click (down, then up on the same
// node). Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { StartMenu } from '../../themes/classic/components/StartMenu.jsx';
import { makeTheme } from '../../themes/classic/tokens.js';

const { styles, texts, parents, calls } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => console.log(got === want ? `ok ${name}` : `FAIL ${name} got ${got}, want ${want}`);
const click = async (id) => {
  globalThis.__lordEvent('down', id, 10, 10, 0); await flush();
  globalThis.__lordEvent('up', id, 10, 10, 0); await flush();
};
// the <Text> that shows s now (lord-server hits it, not its raw text node; the newest one: a raw node
// of a closed menu is not destroyed here)
const textNode = (s) => { let r = 0; for (const [id, v] of texts) if (v === s) r = parents.get(id) || 0; return r; };
// the power button: the 40 × 40 rounded square at the end of the footer
const powerButton = () => {
  for (const [id, s] of styles) if (s.width === 40 && s.height === 40 && s.borderRadius === 6) return id;
  return 0;
};

enableShellLayers();
const t = makeTheme({ mode: 'light' });
let open = true, closes = 0, rerender = null;
function Scene() {
  const [, set] = React.useState(0);
  rerender = () => set((v) => v + 1);
  return open ? h(StartMenu, { t, box: { left: 100, top: 100, width: 640, height: 700 },
    onClose: () => { closes++; open = false; rerender(); }, isOnBar: () => false }) : null;
}
rt.render(h(Scene));
await flush();

for (const [label, call] of [['shell.menu.lock', 'lock'], ['shell.menu.switchUser', 'switchUser']]) {
  open = true; closes = 0; calls.length = 0; rerender(); await flush();
  const btn = powerButton();
  check(`${call}: the power button is there`, !!btn, true);
  await click(btn);
  const row = textNode(label);
  check(`${call}: the power menu shows the row`, !!row, true);
  await click(row);
  check(`${call}: the click on the row reaches lord.session.${call}`, calls.join(','), call);
  check(`${call}: the start menu closes once`, closes, 1);
}
// a press outside the start menu and its power menu still closes both
open = true; closes = 0; rerender(); await flush();
await click(powerButton());
globalThis.__lordEvent('down', 999999, 5, 5, 0); await flush();
check('a press outside closes the start menu', closes, 1);
process.exit(0);
