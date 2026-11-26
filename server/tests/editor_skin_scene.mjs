// editor_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Text Editor with every region a theme
// may draw (toolbar, tabBar, statusBar, contextMenu; docs/THEMES-GUIDE.md "App skins → Text Editor") replaced by a minimal
// stub. The stubs get the tabs and the status but never the text; the commands a skin may ask for are the editor's own
// (New opens a tab), anything else is refused, and layouts off brings the editor's own regions back. The stub lord has no
// catalogs: texts are their keys. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';
import Editor from '../../apps/editor/index.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));
const errs = [];
const ce = console.error;
console.error = (...a) => { errs.push(a.map(String).join(' ').slice(0, 300)); ce(...a); };
globalThis.lord.getLayout = () => [0, 0, 900, 600];
globalThis.lord.fs = new Proxy({ home: () => '/home/test' }, { get: (o, k) => (k in o ? o[k] : () => undefined) });

const seen = {};
const regions = APP_REGIONS.editor;
const stub = (region) => function Stub({ model }) { seen[region] = model; return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`)); };
check('the Text Editor names four regions', JSON.stringify(regions) === '["toolbar","tabBar","statusBar","contextMenu"]');
registerAppSkins({ editor: Object.fromEntries(regions.map((r) => [r, stub(r)])) });

rt.render(h(rt.View, { style: { width: 900, height: 600 } },
  h(AppWindowContext.Provider, { value: { id: 'editor-1', args: {} } }, h(Editor))));
await flush();

const m = () => seen.tabBar;
for (const r of ['toolbar', 'tabBar', 'statusBar']) check(`the ${r} stub is drawn`, shown(`stub-${r}`));
check('one untitled tab', m() && m().tabs.length === 1 && m().tabs[0].name === 'editor.untitled' && m().tabs[0].active, JSON.stringify(m() && m().tabs));
check('the status: line 1, column 1', m().status.line === 1 && m().status.col === 1, JSON.stringify(m().status));
check('the model carries no text', !JSON.stringify(m()).includes('"text"') && Object.keys(m()).join() === 'app,tabs,status,commands,actions');
check('the model is frozen', Object.isFrozen(m()) && Object.isFrozen(m().tabs[0]) && Object.isFrozen(m().actions));
m().actions.command('new');
await flush();
check('command new: a second tab, in front', m().tabs.length === 2 && m().tabs[1].active);
m().actions.selectTab(m().tabs[0].key);
await flush();
check('selectTab: the first tab', m().tabs[0].active);
m().actions.command('eval');
m().actions.command('insertText');
await flush();
check('a command not on the list does nothing', m().tabs.length === 2 && m().tabs[0].active);
m().actions.closeTab(m().tabs[1].key);
await flush();
check('closeTab: an unchanged tab closes', m().tabs.length === 1);
m().actions.menu({ x: 4, y: 4, local: () => ({ x: 4, y: 4 }) });
await flush();
check("menu: the contextMenu stub gets the editor's items", seen.contextMenu && seen.contextMenu.items.some((i) => i && i.label === 'editor.cmd.paste'), JSON.stringify(seen.contextMenu && seen.contextMenu.items.map((i) => i.label || i)));

settings.set({ themeParts: { layouts: false } });
await flush();
check("layouts off: the editor's own tabs and status", !shown('stub-tabBar') && !shown('stub-toolbar') && shown('editor.untitled'));
settings.set({ themeParts: { layouts: true } });
await flush();
check('layouts on again: the stubs come back', shown('stub-tabBar') && shown('stub-statusBar'));
check('no React errors', !errs.length, JSON.stringify(errs.slice(0, 2)));
process.exit(0);
