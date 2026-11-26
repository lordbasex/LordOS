// files_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Files with every region a theme may draw
// (toolbar, addressBar, sidebar, itemView, statusBar, contextMenu; docs/THEMES-GUIDE.md "App skins → Files") replaced by
// a minimal stub. Each stub gets the view model; the actions it asks for are done by Files with its own checks (a folder
// Files only browses refuses a new folder; Move to Trash goes through lord.fs.trash); a stub that throws gives the region
// back to Files; and the person's switch (Settings → Appearance, layouts off) brings Files' own regions back.
// The stub lord has no catalogs: texts are their keys ('files.favorites'). Prints "ok <name>" or "FAIL <name> <detail>".
import { fsLog } from './files_skin_scene_lord.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';
import Files from '../../apps/files/index.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));

const seen = {};
const renders = {};
let throwItems = false;
const stub = (region) => function Stub({ model }) {
  seen[region] = model;
  renders[region] = (renders[region] || 0) + 1;
  if (region === 'itemView' && throwItems) throw new Error('a broken skin');
  return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`));
};
const regions = APP_REGIONS.files;
registerAppSkins({ files: Object.fromEntries(regions.map((r) => [r, stub(r)])), nosuchapp: { toolbar: stub('x') } });

rt.render(h(rt.View, { style: { width: 1100, height: 700 } },
  h(AppWindowContext.Provider, { value: { id: 'files-1', args: { path: '/home/test' } } }, h(Files))));
await flush();

for (const r of regions.filter((x) => x !== 'contextMenu')) check(`the ${r} stub is drawn`, shown(`stub-${r}`));
check("Files' own sidebar is not drawn", !shown('files.favorites'));
check("Files' own items are not drawn", !shown('notes.txt'));
const m = () => seen.itemView;
check('the stub gets the items', m() && m().items.map((x) => x.name).join(',') === 'Desktop,Documents,notes.txt,photo.png', JSON.stringify(m() && m().items.map((x) => x.name)));
check('the model is frozen', Object.isFrozen(m()) && Object.isFrozen(m().items) && Object.isFrozen(m().items[0]) && Object.isFrozen(m().actions));
check('the model carries no lord.fs', !Object.values(m().actions).includes(globalThis.lord.fs.trash) && !JSON.stringify(Object.keys(m())).includes('fs'));
check('the location is the home folder', m().location.path === '/home/test' && m().location.writable === true, JSON.stringify(m().location));
check('the places are offered', ['home', 'desktop', 'documents', 'computer', 'trash'].every((id) => m().places.some((p) => p.id === id)), JSON.stringify(m().places.map((p) => p.id)));

// open a folder, select, back
const key = (name) => m().items.find((x) => x.name === name).key;
m().actions.open(key('Documents'));
await flush();
check('open: Files goes into the folder', m().location.path === '/home/test/Documents' && m().items.map((x) => x.name).join() === 'report.pdf', m().location.path);
check('history: Back is offered', m().history.back === true);
m().actions.back();
await flush();
check('back: Files is home again', m().location.path === '/home/test');
m().actions.select(key('notes.txt'));
await flush();
check('select: one item', m().selection.count === 1 && m().selection.first.name === 'notes.txt' && m().details.name === 'notes.txt', JSON.stringify(m().selection));
m().actions.select(key('photo.png'), { add: true });
await flush();
check('select with add: two items', m().selection.count === 2);

// Move to Trash: Files' own path (lord.fs.trash), never the skin's
m().actions.trash();
await flush();
check('trash: Files moved both to the Trash through lord.fs', fsLog.includes('trash /home/test/notes.txt') && fsLog.includes('trash /home/test/photo.png'), JSON.stringify(fsLog));

// a folder Files only browses: a new folder there is refused with Files' notice, no mkdir
m().actions.goPlace('computer');
await flush();
check('goPlace: This Computer', m().location.path === '/', m().location.path);
const before = fsLog.length;
m().actions.newFolder();
await flush();
check('newFolder in a read-only folder: Files refuses it', fsLog.slice(before).every((l) => !l.startsWith('mkdir')), JSON.stringify(fsLog.slice(before)));
check('the read-only folder says so in the model', m().location.writable === false && m().location.readOnly === true);

// the address bar: a folder that exists, then one that doesn't
check('goAddress: a folder', m().actions.goAddress('~/Documents') === true);
await flush();
check('goAddress went there', m().location.path === '/home/test/Documents');
check('goAddress: nothing there', m().actions.goAddress('/no/such/place') === false);
await flush();
check("goAddress: the status bar says it isn't there", /fav\.gone|no\/such\/place/.test(seen.statusBar.status.text), seen.statusBar.status.text);

// the context menu: Files builds the items, the stub draws them
m().actions.goPlace('home');
await flush();
m().actions.menu(key('notes.txt'), { x: 10, y: 10, local: () => ({ x: 10, y: 10 }) });
await flush();
const cm = seen.contextMenu;
check('menu: the contextMenu stub gets Files\' items', cm && cm.items.some((i) => i && i.label === 'files.menu.trash') && typeof cm.close === 'function', JSON.stringify(cm && cm.items.map((i) => i.label || i)));
cm.close();
await flush();

// the skin ran into an error: Files' own items, for the rest of the session
throwItems = true;
m().actions.goPlace('documents');
await flush();
check('a skin that throws gives the region back to Files', shown('report.pdf'));
check('the other regions stay skinned', shown('stub-toolbar') && shown('stub-sidebar'));

// the person turns the theme's layouts off: Files' own regions
settings.set({ themeParts: { layouts: false } });
await flush();
check("layouts off: Files' own sidebar", shown('files.favorites'));
check('layouts off: no stub is drawn', regions.every((r) => !shown(`stub-${r}`)));
settings.set({ themeParts: { layouts: true } });
await flush();
check('layouts on again: the stubs come back', shown('stub-toolbar') && shown('stub-statusBar'));
process.exit(0);
