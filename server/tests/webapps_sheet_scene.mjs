// webapps_sheet_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the Store's "Add from an
// address" sheet (apps/store/addweb.js) over a stand-in lord.webapps: an http: address is refused before any probe,
// Check probes the https: address, the sheet shows the origin lord-web read (not what was typed), the name and whether
// the page has a manifest, and Install sends only the probe's id, the name and the profile (never a URL). Prints one
// line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { AddFromAddress } from '../../apps/store/addweb.js';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

// lord-server's side, as lord.webapps answers it
const calls = { probe: [], install: [] };
let answer = null;
lord.webapps = {
  available: () => true,
  list: () => [],
  probe: (url) => { calls.probe.push(url); return 7; },
  probeResult: (id) => (id === 7 ? answer : null),
  install: (entry) => { calls.install.push(entry); return 'notes-online'; },
};

const ids = {};
enableShellLayers();
rt.render(h(rt.View, { ref: (r) => { if (r) ids.root = r.id; } }, h(AddFromAddress)));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => x === s && inTree(id))?.[0];
const shows = (s) => !!textNode(s);
const click = async (id, x = 5, y = 5) => { __lordEvent('down', id, x, y, 0); __lordEvent('up', id, x, y, 0); await flush(); };
const button = (label) => parents.get(textNode(label));
const key = async (name, chr = '') => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), 0); __lordKey(0, 0, name, '', '', 0); await flush(); };
const type = async (s) => { for (const c of s) await key(c, c); };
const childOf = (p) => [...parents].find(([c, q]) => q === p && inTree(c))?.[0];
// the address field: the TextInput in the left-to-right box of the address row
const field = () => childOf([...styles].find(([id, st]) => st.direction === 'ltr' && st.flexGrow === 1 && st.minWidth === 0 && inTree(id))?.[0]);

check('the sheet is there', shows('webapps.store.addTitle'));
check('it has a field', !!field());
await click(field(), 10);
await type('http://notes.example');
await click(button('webapps.store.check'));
check('an http address is refused before any probe', calls.probe.length === 0 && shows('webapps.store.address.scheme'), JSON.stringify(calls.probe));

await click(field(), 10);
await key('End');
for (let i = 0; i < 25; i++) await key('Backspace');
await type('notes.example');
await click(button('webapps.store.check'));
check('Check probes the https address', calls.probe.length === 1 && calls.probe[0] === 'https://notes.example/', JSON.stringify(calls.probe));
check('it says it is checking', shows('webapps.store.checking'));
answer = { done: true, ok: true, name: 'Notes Online', startUrl: 'https://app.notes.example/', scope: 'https://app.notes.example/',
  origin: 'https://app.notes.example', themeColor: '', manifest: true, iconPath: '', iconKind: '' };
await sleep(400);
await flush();
check('the sheet shows the origin lord-web read', shows('https://app.notes.example'));
check('it says the page has a manifest', shows('webapps.store.manifestYes'));
check('nothing is installed before Install', calls.install.length === 0);
await click(button('store.install'));
const sent = calls.install[0];
check('Install sends the probe, the name and the profile, and no URL',
  sent && sent.source === 'url' && sent.probe === 7 && sent.name === 'Notes Online' && !('startUrl' in sent) && !('origin' in sent) && Object.keys(sent).length === 4,
  JSON.stringify(sent));
check('then it says it is installed', shows('webapps.store.installed'));
process.exit(0);
