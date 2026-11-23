// store_origin_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): an origin's second column in
// the Store (apps/store/origincol.js, 2026-11-23). "Installed" comes first, with how many of that system's apps are
// installed, then the categories: All (the shop's home) and each category with apps; choosing Installed selects it and
// hands the Store installedmodel.js's MINE. The narrow window's chips put Installed first too, and the general
// Installed page's filter chips show each filter with its count. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { FilterChips, OriginChips, OriginColumn } from '../../apps/store/origincol.js';
import { MINE } from '../../apps/store/installedmodel.js';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// the texts shown, in the order they were made (siblings left to right)
const shown = () => [...texts].filter(([id]) => inTree(id)).sort((a, b) => a[0] - b[0]).map(([, s]) => s);
const nodeOf = (s) => [...texts].filter(([id, x]) => x === s && inTree(id)).map(([id]) => parents.get(id)).pop();
const click = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };

const CATS = ['AudioVideo', 'Graphics', 'Network', 'Office', 'Utility'];
const counts = { AudioVideo: 4, Graphics: 2, Network: 0, Office: 3, Utility: 1 };
let picked = 'none';
let setSub = null;
function Host() {
  const [sub, set] = React.useState(null);
  setSub = set;
  return h(OriginColumn, { sub, cats: CATS, counts, total: 10, mine: 3, onPick: (v) => { picked = v; set(v); } });
}
enableShellLayers();
rt.render(h(Host));
await flush();

const order = shown();
const at = (s) => order.indexOf(s);
check('Installed is shown', at('store.page.installed') >= 0, JSON.stringify(order));
check('Installed comes first, over the categories', at('store.page.installed') === 0 && at('store.page.installed') < at('store.section.categories'), JSON.stringify(order));
check('Installed shows how many are installed', order[1] === '3', JSON.stringify(order));
check('then Categories, All and its count', at('store.section.categories') < at('store.sub.all') && order[at('store.sub.all') + 1] === '10', JSON.stringify(order));
check('the categories with apps, in order', JSON.stringify(order.filter((s) => s.startsWith('store.cat.'))) ===
  JSON.stringify(['store.cat.AudioVideo', 'store.cat.Graphics', 'store.cat.Office', 'store.cat.Utility']), JSON.stringify(order));
// the stub SidebarItem marks its View when selected: the text's node or one of its two parents
const selected = (s) => { for (let n = nodeOf(s), i = 0; n && i < 3; n = parents.get(n), i++) if ((styles.get(n) || {}).opacity === 0.99) return true; return false; };
check('All is selected at first (the shop comes first)', selected('store.sub.all') && !selected('store.page.installed'));
await click(nodeOf('store.page.installed'));
check('choosing Installed hands the Store MINE', picked === MINE, String(picked));
check('and selects it, not All', selected('store.page.installed') && !selected('store.sub.all'));
await click(nodeOf('store.cat.Graphics'));
check('a category after it', picked === 'Graphics' && selected('store.cat.Graphics') && !selected('store.page.installed'), String(picked));
setSub(null);
await flush();

// the narrow window's chips, and the general Installed page's filters
let chip = 'none', filt = 'none';
rt.render(h(rt.View, null,
  h(OriginChips, { sub: null, cats: CATS, counts, onPick: (v) => { chip = v; } }),
  h(FilterChips, { value: 'all', filters: [['all', 9], ['lordos', 3], ['debian', 2], ['flathub', 1], ['web', 2], ['off', 1]], onChange: (v) => { filt = v; } })));
await flush();
const o2 = shown();
const chips = o2.slice(o2.lastIndexOf('store.page.installed'));
check('chips: Installed first, then All and the categories', JSON.stringify(chips.slice(0, 6)) ===
  JSON.stringify(['store.page.installed', 'store.sub.all', 'store.cat.AudioVideo', 'store.cat.Graphics', 'store.cat.Office', 'store.cat.Utility']), JSON.stringify(chips));
const lastNode = (s) => [...texts].filter(([id, x]) => x === s && inTree(id)).map(([id]) => parents.get(id)).pop();
await click(lastNode('store.page.installed'));
check('the Installed chip hands MINE', chip === MINE, String(chip));
const f = o2.slice(o2.lastIndexOf('store.off.filterAll'));
check('filters: each with its label and count, in order', JSON.stringify(f) === JSON.stringify(['store.off.filterAll', '9', 'store.page.lordos', '3', 'store.src.debian', '2',
  'store.page.flathub', '1', 'store.page.webapps', '2', 'store.off.filterOff', '1']), JSON.stringify(f));
await click(lastNode('store.page.flathub'));
check('a filter chip hands its key', filt === 'flathub', String(filt));
process.exit(0);
