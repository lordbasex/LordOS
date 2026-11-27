// webapp_menu_orphan_scene.mjs — a scene of tests/textfield_menu_test.mjs (ws170 QA BUG-31; not run by itself): a web
// app's right-click menu (sdk/hardware.jsx WebAppMenu, in the system layer) closes when the last window of that web app
// closes, answering "" to lord-web, even though lord-web keeps running (it never says the menu is gone). A window of
// another web app closing leaves it open. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import '@lordos/sdk';
import { WebAppMenu } from '../../sdk/hardware.jsx';
import { webMenu } from '../../sdk/webapps.js';
import { windows } from '../../sdk/windows.js';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const answers = [];
lord.wayland = new Proxy({}, { get: () => () => undefined });
lord.webapps = new Proxy({ menuAnswer: (id, item) => answers.push([id, item]), list: () => [] }, { get: (o, k) => (k in o ? o[k] : () => undefined) });

const web = (id, app) => ({ id, app: 'linux', args: { surface: 5, web: { app, profile: 'default' } }, x: 0, y: 0, w: 800, h: 600 });
windows.set([web('linux#1', 'whatsapp'), web('linux#2', 'gmail')]);
rt.render(h(WebAppMenu));
await flush();
const open = (id) => { globalThis.__lordHw('webapps-menu', { id, app: 'whatsapp', profile: 'default', items: ['copy'] }); };
open(7);
await flush();
check('the menu is open', webMenu.get()?.id, 7);
windows.set([web('linux#1', 'whatsapp')]);                 // another web app's window closes
await flush();
check('another app\'s window closing leaves it', webMenu.get()?.id, 7);
windows.set([]);                                           // its own window closes (close_window): lord-web still runs
await flush();
check('its window closed: the menu closes', webMenu.get(), null);
check('and lord-web is told "" once', answers, [[7, '']]);
process.exit(0);
