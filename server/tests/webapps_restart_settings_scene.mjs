// webapps_restart_settings_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Settings → Web apps,
// Restart and Force Quit of a running profile and "Restart web apps engine" (apps/settings/WebApps.jsx, sdk/webapps.js;
// docs/WEBAPPS.md, "When a web app stops answering") over a stand-in lord.webapps. Each asks first and only the confirm
// reaches lord.webapps (restart, quit, restartAll); Cancel calls nothing; a profile that isn't running has neither button;
// with nothing running the engine's button is off; an error from lord-server is shown. Its
// tests/webapps_restart_settings_scene_sdk.js is the SDK. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { WebAppsSettings } from '../../apps/settings/WebApps.jsx';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const calls = [];
let running = [{ app: 'whatsapp', profile: 'personal', pid: 41, inspect: false }];
let fail = null;
lord.webapps = {
  available: () => true,
  list: () => [{ id: 'whatsapp', name: 'WhatsApp', origin: 'https://web.whatsapp.com', startUrl: 'https://web.whatsapp.com/', source: 'catalog',
    profiles: [{ id: 'personal', name: 'Personal' }, { id: 'work', name: 'Work' }], permissions: {}, background: true }],
  icons: () => ({}), badges: () => [], developerMode: () => false,
  running: () => running,
  restart: (app, profile) => { calls.push(['restart', app, profile]); if (fail) throw Object.assign(new Error(fail), { code: fail }); return 'restarted'; },
  quit: (app, profile) => { calls.push(['quit', app, profile]); return true; },
  restartAll: () => { calls.push(['restartAll']); return running.length; },
};
lord.fs = { ...(lord.fs || {}), home: () => '/home/ana', duAsync: () => new Promise(() => {}), read: () => { throw new Error('none'); }, list: () => [] };

enableShellLayers();
rt.render(h(rt.View, null, h(WebAppsSettings, {})));
await flush();
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const nodes = (s) => [...texts].filter(([id, x]) => String(x) === s && inTree(id)).map(([id]) => id);
const textNode = (s) => nodes(s)[0];
const shows = (s) => !!textNode(s);
const click = async (id, x = 5, y = 5) => { __lordEvent('down', id, x, y, 0); __lordEvent('up', id, x, y, 0); await flush(); };
// the dialog's buttons are the last ones with that label (the page's own come first)
const clickLast = async (s) => { const l = nodes(s); await click(parents.get(l[l.length - 1])); };

check('the engine section is there', shows('webapps.settings.restartEngine') && shows('webapps.settings.engineTitle'));
const engineNode = textNode('webapps.restart.ok');          // the engine's button: on the page before any app is opened
const profileRestart = () => nodes('webapps.restart.ok').find((id) => id !== engineNode);
await click(parents.get(textNode('WhatsApp')));
check('a running profile has Restart and Force Quit (one of each: work isn\'t running)',
  nodes('webapps.restart.ok').length === 2 && nodes('webapps.quit.ok').length === 1, `${nodes('webapps.restart.ok').length} ${nodes('webapps.quit.ok').length}`);

// Restart: asked first, Cancel calls nothing
await click(parents.get(profileRestart()));
check('Restart asks first', shows('webapps.restart.title') && shows('webapps.restart.detail') && calls.length === 0, JSON.stringify(calls));
await clickLast('webapps.settings.cancel');
check('Cancel: nothing restarted, the question gone', calls.length === 0 && !shows('webapps.restart.title'), JSON.stringify(calls));
await click(parents.get(profileRestart()));
await clickLast('webapps.restart.ok');
check('the confirm restarts that profile only', JSON.stringify(calls) === JSON.stringify([['restart', 'whatsapp', 'personal']]), JSON.stringify(calls));

// Force Quit: asked first, then quit
await click(parents.get(textNode('webapps.quit.ok')));
check('Force Quit asks first', shows('webapps.quit.title') && shows('webapps.quit.detail') && calls.length === 1);
await clickLast('webapps.quit.ok');
check('the confirm force quits it', JSON.stringify(calls[1]) === JSON.stringify(['quit', 'whatsapp', 'personal']), JSON.stringify(calls));

// an error from lord-server is shown, not swallowed
fail = 'webapps.noRuntime';
await click(parents.get(profileRestart()));
await clickLast('webapps.restart.ok');
check('an error is shown', [...texts].some(([id, x]) => /webapps\.noRuntime/.test(String(x)) && inTree(id)));
fail = null;

// the engine: every running web app, after a question
const before = calls.length;
await click(parents.get(engineNode));
check('the engine asks first', shows('webapps.settings.restartEngineTitle') && shows('webapps.settings.restartEngineDetail') && calls.length === before);
await clickLast('webapps.restart.ok');
check('restart all, once', calls.length === before + 1 && calls.at(-1)[0] === 'restartAll', JSON.stringify(calls));
check('"Web apps restarted"', shows('webapps.settings.restartEngineDone'));
process.exit(0);
