// webapps_icon_settings_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Settings → Web apps →
// an app → Refresh icon (apps/settings/WebApps.jsx, sdk/webapps.js) over a stand-in lord.webapps. Refresh icon must
// never fail silently (2026-11-26, WhatsApp kept the letter tile and the button did nothing visible): a spinner while
// the probe runs, then "Icon updated" and the site's icon, or the reason there is none (the runtime's code, or
// webapps.noIcon), and each failure is one line in the shell's log with the app's id and a closed code only.
// Its tests/webapps_icon_settings_scene_sdk.js is the SDK. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { WebAppsSettings } from '../../apps/settings/WebApps.jsx';
import { wantSiteIcon } from '../../sdk/webapps.js';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const entry = (site) => ({
  id: 'whatsapp', name: 'WhatsApp', origin: 'https://web.whatsapp.com', startUrl: 'https://web.whatsapp.com/', source: 'catalog',
  profiles: [{ id: 'personal', name: 'Personal' }], permissions: {}, background: true,
  ...(site ? { siteIcon: true, iconPath: '/home/ana/.cache/lordos/web/icons/whatsapp.png' } : {}),
});
let kept = false, answer = null, fetches = 0;
const manualArgs = [];
lord.webapps = {
  available: () => true,
  list: () => [entry(kept)],
  icons: () => (kept ? { whatsapp: '/home/ana/.cache/lordos/web/icons/whatsapp.png' } : {}),
  running: () => [], badges: () => [], developerMode: () => false,
  fetchIcon: (app, manual) => { fetches++; manualArgs.push([app, manual]); return app === 'whatsapp' ? 5 : app === 'telegram' ? 6 : 0; },
  // null while the probe runs; then the answer the scene set (the Store's background one never ends here)
  iconResult: (app) => (app === 'whatsapp' ? answer : null),
};
lord.fs = { ...(lord.fs || {}), home: () => '/home/ana', duAsync: () => new Promise(() => {}), read: () => { throw new Error('none'); }, list: () => [] };
const warned = [];
const warn0 = console.warn;
console.warn = (...a) => warned.push(a.join(' '));

enableShellLayers();
rt.render(h(rt.View, null, h(WebAppsSettings, {})));
await flush();
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => String(x) === s && inTree(id))?.[0];
const shows = (s) => !!textNode(s);
const click = async (id, x = 5, y = 5) => { __lordEvent('down', id, x, y, 0); __lordEvent('up', id, x, y, 0); await flush(); };

await click(parents.get(textNode('WhatsApp')));
check('the app\'s page: the letter tile', shows('webapps.settings.iconTile'));
check('Refresh icon is there', shows('webapps.settings.iconRefresh'));

// the Store's background queue is busy with another app's icon (QA 0.8.9.6.2, BUG-E): Refresh icon doesn't wait for it
wantSiteIcon('telegram');
wantSiteIcon('whatsapp');
check('a background fetch runs, not manual', fetches === 1 && manualArgs[0][0] === 'telegram' && manualArgs[0][1] === false, JSON.stringify(manualArgs));
fetches = 0;
manualArgs.length = 0;
// a failure: busy first, then the runtime's own code, said and logged
await click(parents.get(textNode('webapps.settings.iconRefresh')));
check('a probe started', fetches === 1, `fetches ${fetches}`);
check('at once, as a manual fetch, ahead of the queue', manualArgs.length === 1 && manualArgs[0][0] === 'whatsapp' && manualArgs[0][1] === true,
  JSON.stringify(manualArgs));
check('busy: "Refreshing…"', shows('webapps.settings.iconRefreshing'));
answer = { done: true, ok: false, error: 'web.probe.iconType' };
await wait(700);
await flush();
check('the reason is shown', [...texts].some(([id, x]) => /web\.probe\.iconType/.test(String(x)) && inTree(id)),
  [...texts.values()].filter((x) => /probe|noIcon|icon/i.test(String(x))).join(' | '));
check('not busy any more', shows('webapps.settings.iconRefresh') && !shows('webapps.settings.iconRefreshing'));
check('one line in the log, closed words only', warned.length === 1 && /whatsapp/.test(warned[0]) && /web\.probe\.iconType/.test(warned[0]) && !/https?:/.test(warned[0]),
  JSON.stringify(warned));

// no icon at all and no code: webapps.noIcon
answer = null;
await click(parents.get(textNode('webapps.settings.iconRefresh')));
answer = { done: true, ok: false, error: 'webapps.noIcon' };
await wait(700);
await flush();
check('"no icon" is shown', [...texts].some(([id, x]) => /webapps\.noIcon/.test(String(x)) && inTree(id)));
check('a second log line', warned.length === 2 && /webapps\.noIcon/.test(warned[1]), JSON.stringify(warned));

// a page's text never reaches the message or the log
answer = null;
await click(parents.get(textNode('webapps.settings.iconRefresh')));
answer = { done: true, ok: false, error: 'https://evil.example/<b>' };
await wait(700);
await flush();
check('an unknown error is a closed code', warned.length === 3 && /webapps\.probeFailed/.test(warned[2]) && !/evil/.test(warned[2]) &&
  ![...texts.values()].some((x) => /evil/.test(String(x))), JSON.stringify(warned));

// success: the site's icon and "Icon updated"
answer = null;
await click(parents.get(textNode('webapps.settings.iconRefresh')));
answer = { done: true, ok: true, iconPath: '/home/ana/.cache/lordos/web/icons/whatsapp.png' };
kept = true;
await wait(700);
await flush();
check('"Icon updated"', shows('webapps.settings.iconUpdated'));
check('the site\'s icon is now the app\'s', shows('webapps.settings.iconSite'));
check('no log line for a success', warned.length === 3, JSON.stringify(warned));
console.warn = warn0;
process.exit(0);
