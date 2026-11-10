// screencast_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the ScreenCast chooser
// (sdk/screencast.jsx) rendered through runtime/index.js with the same stub `lord`, fed the hw events portal.c sends:
// the app's name and the displays show, a picture is asked for each only while it is up, Share stays off until
// something is picked, Share sends what was picked, Cancel sends nothing, a request closed by the app takes the chooser
// away, and every picture is closed with it. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { ScreenCastPrompt, castEvent, castRuns } from '../../sdk/screencast.jsx';
import { enableShellLayers } from '../../sdk/layers.js';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const calls = { cast: [], thumbs: [], closed: [] };
let thumbId = 100;
lord.portal = {
  cast: (id, sources) => { calls.cast.push([id, sources]); },
  castThumb: (id, src) => { calls.thumbs.push([id, src]); return ++thumbId; },
  castShown: () => {},
  castStop: () => {},
};
lord.media = { close: (id) => { calls.closed.push(id); } };

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textId = (s) => [...texts].find(([id, x]) => x === s && inTree(id))?.[0];
const shows = (s) => !!textId(s);
// lord-server hits the <Text>, not its raw text node
const click = async (id) => { const t = parents.get(id) || id; __lordEvent('down', t, 5, 5, 0); __lordEvent('up', t, 5, 5, 0); await flush(); };

enableShellLayers();
rt.render(h(ScreenCastPrompt));
await flush();
check('nothing before a request', !shows('Share'));

castEvent('portal-cast', { id: 7, monitor: true, window: false, multiple: false, who: '', comm: 'meetclient' });
await flush();
check('the app is named', shows('“meetclient” wants to see your screen'), [...texts.values()].join(' | '));
check('the display is offered', shows('Screen 1'));
check('no window section when only screens were asked', !shows('Open windows'));
check('a picture asked for the display, for this request', calls.thumbs.length === 1 && calls.thumbs[0][0] === 7 && calls.thumbs[0][1].display === 0,
  JSON.stringify(calls.thumbs));
await click(textId('Share'));
check('Share does nothing with nothing picked', calls.cast.length === 0, JSON.stringify(calls.cast));
await click(textId('Screen 1'));
await click(textId('Share'));
check('Share sends the display picked', calls.cast.length === 1 && calls.cast[0][0] === 7 && JSON.stringify(calls.cast[0][1]) === '[{"display":0}]',
  JSON.stringify(calls.cast));
check('the chooser goes after the answer', !shows('Share'));
check('its pictures are closed', calls.closed.includes(101), JSON.stringify(calls.closed));

castEvent('portal-cast', { id: 8, monitor: true, window: true, multiple: true, who: '', comm: '' });
await flush();
check('an unknown app', shows('“An app” wants to see your screen'));
check('windows asked: the section, empty', shows('Open windows') && shows('No windows are open.'));
check('several: the other text', shows('Pick the screens and windows to share. The app sees only those, until you stop sharing.'));
await click(textId('Cancel'));
check('Cancel answers with nothing', calls.cast.length === 2 && calls.cast[1][0] === 8 && calls.cast[1][1] === null, JSON.stringify(calls.cast));

castEvent('portal-cast', { id: 9, monitor: true, window: false, multiple: false, who: '', comm: 'x' });
await flush();
castEvent('portal-cast-gone', { id: 3 });
await flush();
check('another request\'s close leaves it', shows('Share'));
castEvent('portal-cast-gone', { id: 9 });
await flush();
check('the app closed its request: the chooser goes', !shows('Share'));
check('no answer for it', calls.cast.length === 2);

castEvent('portal-casts', [{ id: 9, who: '', comm: 'recorder' }]);
check('the indicator\'s list, with the name', castRuns.get().length === 1 && castRuns.get()[0].name === 'recorder', JSON.stringify(castRuns.get()));
castEvent('portal-casts', null);
check('an empty list', castRuns.get().length === 0);
