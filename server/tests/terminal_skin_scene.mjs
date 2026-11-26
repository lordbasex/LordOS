// terminal_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Terminal with every region a
// theme may draw (tabBar, statusBar, contextMenu; docs/THEMES-GUIDE.md "App skins → Terminal") replaced by a minimal
// stub. The stubs get the tabs; the commands a skin may ask for are the Terminal's own (a new tab opens a session),
// anything else is refused, no action types into a session, and layouts off brings the Terminal's own tab bar back.
// The stub lord has no catalogs: texts are their keys. Prints "ok <name>" or "FAIL <name> <detail>".
import { termLog } from './terminal_skin_scene_lord.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';
import Terminal from '../../apps/terminal/index.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));
const errs = [];
const ce = console.error;
console.error = (...a) => { errs.push(a.map(String).join(' ').slice(0, 300)); ce(...a); };

const seen = {};
const stub = (region) => function Stub({ model }) { seen[region] = model; return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`)); };
check('the Terminal names three regions', JSON.stringify(APP_REGIONS.terminal) === '["tabBar","statusBar","contextMenu"]');
registerAppSkins({ terminal: { tabBar: stub('tabBar'), statusBar: stub('statusBar'), contextMenu: stub('contextMenu') } });

rt.render(h(rt.View, { style: { width: 900, height: 600 } },
  h(AppWindowContext.Provider, { value: { id: 'terminal-1', args: {} } }, h(Terminal))));
await flush();

const m = () => seen.tabBar;
check('the tabBar stub is drawn', shown('stub-tabBar'));
check('one tab, active', m() && m().tabs.length === 1 && m().tabs[0].active, JSON.stringify(m() && m().tabs));
check('the model is frozen', Object.isFrozen(m()) && Object.isFrozen(m().tabs[0]) && Object.isFrozen(m().actions));
check('the actions write nothing', JSON.stringify(Object.keys(m().actions)) === '["selectTab","closeTab","tabMenu","command"]');
const writes = () => termLog.filter((l) => /^(write|key|paste)/.test(l)).length;
const w0 = writes();
m().actions.command('newTab');
await flush();
check('command newTab: the Terminal opens a second tab', m().tabs.length === 2 && m().tabs[1].active, JSON.stringify(m().tabs.map((x) => x.active)));
m().actions.selectTab(m().tabs[0].key);
await flush();
check('selectTab: the first tab is in front', m().tabs[0].active && !m().tabs[1].active);
m().actions.command('rm -rf /');
m().actions.command('writeText');
await flush();
check('a command not on the list does nothing', m().tabs.length === 2);
m().actions.closeTab(m().tabs[1].key);
await flush();
check('closeTab: back to one tab', m().tabs.length === 1);
m().actions.tabMenu(m().tabs[0].key, { x: 5, y: 5 });
await flush();
check("tabMenu: the contextMenu stub gets the Terminal's items", seen.contextMenu && seen.contextMenu.items.some((i) => i && i.label === 'terminal.files.showServerFiles'), JSON.stringify(seen.contextMenu && seen.contextMenu.items.map((i) => i.label || i)));
check('no skin action typed into a session', writes() === w0, JSON.stringify(termLog));

settings.set({ themeParts: { layouts: false } });
await flush();
check("layouts off: the Terminal's own tab bar", !shown('stub-tabBar') && shown('❖1'));
settings.set({ themeParts: { layouts: true } });
await flush();
check('layouts on again: the stub comes back', shown('stub-tabBar'));
check('no React errors', !errs.length, JSON.stringify(errs.slice(0, 2)));
process.exit(0);
