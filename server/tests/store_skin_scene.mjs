// store_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Store with both regions a theme may draw
// (sidebar and toolbar; docs/THEMES-GUIDE.md "App skins → Store") replaced by a minimal stub. The stubs get the pages;
// go() opens only a page the Store lists and not one marked "soon", the search reaches the Store, and layouts off brings
// the Store's own sidebar back. The stub lord has no catalogs: texts are their keys. Prints "ok <name>" or "FAIL …".
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';
import Store from '../../apps/store/index.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));
globalThis.lord.getLayout = () => [0, 0, 1000, 650];

const seen = {};
const stub = (region) => function Stub({ model }) { seen[region] = model; return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`)); };
check('the Store names two regions', JSON.stringify(APP_REGIONS.store) === '["sidebar","toolbar"]');
registerAppSkins({ store: { sidebar: stub('sidebar'), toolbar: stub('toolbar') } });
rt.render(h(rt.View, { style: { width: 1000, height: 650 } }, h(AppWindowContext.Provider, { value: { id: 'store-1', args: {} } }, h(Store))));
await flush();

const m = () => seen.sidebar;
check('both stubs are drawn', shown('stub-sidebar') && shown('stub-toolbar'));
check("the Store's own sidebar is not drawn", !shown('store.section.origin'));
check('the pages, Discover selected', m() && m().pages.some((p) => p.id === 'discover' && p.selected) && m().pages.some((p) => p.group === 'origin'), JSON.stringify(m() && m().pages.map((p) => p.id)));
check('the model is frozen', Object.isFrozen(m()) && Object.isFrozen(m().pages[0]) && Object.isFrozen(m().actions));
check('no install action', JSON.stringify(Object.keys(m().actions)) === '["go","back","search","refresh"]');
m().actions.go('installed');
await flush();
check('go: the Installed page', m().page === 'installed' && m().pages.find((p) => p.id === 'installed').selected);
m().actions.go('wine');
m().actions.go('nosuchpage');
await flush();
check('go: a page marked soon or unknown is refused', m().page === 'installed');
m().actions.go('discover');
await flush();
m().actions.search('vlc');
await flush();
check('search: the Store has the query', seen.toolbar.query === 'vlc', String(seen.toolbar.query));
settings.set({ themeParts: { layouts: false } });
await flush();
check("layouts off: the Store's own sidebar", shown('store.section.origin') && !shown('stub-sidebar'));
settings.set({ themeParts: { layouts: true } });
await flush();
check('layouts on again: the stubs come back', shown('stub-sidebar'));
process.exit(0);
