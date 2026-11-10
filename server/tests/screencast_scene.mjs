// screencast_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the ScreenCast chooser
// (sdk/screencast.jsx) rendered through runtime/index.js with the same stub `lord`, fed the hw events portal.c sends:
// the app's verified name and the displays show, a sender lord-server couldn't verify is an unknown program with a
// warning and never the name its process gives itself, a picture is asked for each only while it is up, Share stays off until
// something is picked, Share sends what was picked, Cancel sends nothing, a request closed by the app takes the chooser
// away, and every picture is closed with it. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { ScreenCastPrompt, castEvent, castRuns, castNames } from '../../sdk/screencast.jsx';
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

// as sdk/hardware.jsx names a verified app (its launcher's scope)
castNames({ app: (p) => (p.who === 'desktop:meetclient' ? 'Meet Client' : p.who) });
enableShellLayers();
rt.render(h(ScreenCastPrompt));
await flush();
check('nothing before a request', !shows('Share'));

castEvent('portal-cast', { id: 7, monitor: true, window: false, multiple: false, who: 'desktop:meetclient' });
await flush();
check('the verified app is named', shows('“Meet Client” wants to see your screen'), [...texts.values()].join(' | '));
check('no warning for a verified app', !shows('The system can’t tell which program is asking. Share only if you just asked a program to share your screen.'));
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

castEvent('portal-cast', { id: 8, monitor: true, window: true, multiple: true, who: '', comm: 'Meet Client' });
await flush();
check('an unverified sender is an unknown program', shows('An unknown program wants to see your screen'), [...texts.values()].join(' | '));
check('with the warning', shows('The system can’t tell which program is asking. Share only if you just asked a program to share your screen.'));
check('never the name its process gives itself', ![...texts].some(([id, x]) => inTree(id) && x.includes('Meet Client')));
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

castEvent('portal-casts', [{ id: 9, who: 'desktop:meetclient' }, { id: 10, who: '', comm: 'recorder' }]);
check('the indicator\'s list, with the verified name', castRuns.get().length === 2 && castRuns.get()[0].name === 'Meet Client', JSON.stringify(castRuns.get()));
check('and an unknown program for the other, not its process name', castRuns.get()[1].name === 'An unknown program', JSON.stringify(castRuns.get()));
castEvent('portal-casts', null);
check('an empty list', castRuns.get().length === 0);
