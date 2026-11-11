// webapps_get_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Get of a catalog web app in the
// Store (apps/store/getweb.js) over a stand-in lord.webapps, lord.pkg and package job. Without lordos-web, Install shows
// the engine step (its download and disk size from lord.pkg.plan({ install: ['lordos-web'] })) and installs nothing;
// without an administrator it can't go on; the engine's Install runs the Store's package job for lordos-web only and,
// when it ends well, installs the app with no other click (a catalog entry: no URL); a failed job installs nothing.
// The second app, with the engine there, has no engine step. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { adminState } from '@lordos/sdk';
import { GetSteps, useGetWebApp } from '../../apps/store/getweb.js';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

let engine = false;
const calls = { plan: [], install: [], jobs: [] };
lord.webapps = {
  available: () => engine,
  list: () => [],
  install: (entry) => { calls.install.push(entry); return entry.id; },
};
lord.pkg = { plan: (req) => { calls.plan.push(req); return { ok: true, download: 52428800, installedDelta: 209715200,
  items: [{ name: 'lordos-web', action: 'install', requested: true }, { name: 'libwpewebkit-2.0-1', action: 'install' }] }; } };
let finish = null;
const Progress = () => h(rt.Text, null, 'job running');
const pkg = { Progress, runJob: (action, arg, title) => { calls.jobs.push([action, arg, title]); return new Promise((res) => { finish = res; }); } };

function Get({ a }) {
  const g = useGetWebApp(a, pkg);
  return h(rt.View, null,
    g.s.step === 'idle' ? h(rt.View, { onClick: g.start }, h(rt.Text, null, `get ${a.id}`)) : null,
    h(GetSteps, { a, g, pkg }));
}
const chat = { id: 'chat', name: 'Chat' };
const mail = { id: 'mail', name: 'Mail' };
enableShellLayers();
rt.render(h(rt.View, null, h(Get, { a: chat }), h(Get, { a: mail })));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => x === s && inTree(id))?.[0];
const shows = (s) => !!textNode(s);
const click = async (id, x = 5, y = 5) => { __lordEvent('down', id, x, y, 0); __lordEvent('up', id, x, y, 0); await flush(); };
const button = (label) => parents.get(textNode(label));

await click(button('get chat'));
check('without lordos-web: the engine step', shows('webapps.get.engineTitle'));
check('it asks the package engine for lordos-web\'s plan', calls.plan.length === 1 && JSON.stringify(calls.plan[0]) === '{"install":["lordos-web"]}', JSON.stringify(calls.plan));
check('it shows the download and the disk size', shows('webapps.get.engineSizes'));
check('nothing installed, no job yet', calls.install.length === 0 && calls.jobs.length === 0);

adminState.on = false;
await click(button('store.cancel'));
await click(button('get chat'));
check('without an administrator it says so', shows('store.debian.needAdmin'));
await click(button('webapps.get.installBoth'));
check('… and runs no job', calls.jobs.length === 0);
adminState.on = true;
await click(button('store.cancel'));
await click(button('get chat'));

// a job that fails installs nothing; Try Again goes back to the engine step
await click(button('webapps.get.installBoth'));
finish(false);
await flush();
check('a failed job installs nothing and offers Try Again', calls.install.length === 0 && shows('webapps.get.retry') && shows('job running'));
await click(button('webapps.get.retry'));
check('Try Again shows the engine step again', shows('webapps.get.engineTitle'));
calls.jobs.length = 0;

await click(button('webapps.get.installBoth'));
check('Install runs the package job for lordos-web only', calls.jobs.length === 1 && calls.jobs[0][0] === 'pkg-install' && calls.jobs[0][1] === 'lordos-web', JSON.stringify(calls.jobs));
check('the job\'s progress shows', shows('job running') && shows('webapps.get.thenApp'));
check('the app waits for the job', calls.install.length === 0);
engine = true;
finish(true);
await flush();
const sent = calls.install[0];
check('when the job ends well the app is installed with no other click', calls.install.length === 1 && sent.source === 'catalog' && sent.id === 'chat' && Object.keys(sent).length === 3, JSON.stringify(calls.install));
check('then it says it is installed', shows('webapps.store.installed'));

const plans = calls.plan.length;
await click(button('get mail'));
check('the second app: no engine step, no plan, no job', calls.plan.length === plans && !shows('webapps.get.engineTitle') && calls.jobs.length === 1 && calls.install.length === 2 && calls.install[1].id === 'mail',
  JSON.stringify({ plan: calls.plan.length, jobs: calls.jobs.length, install: calls.install }));
process.exit(0);
