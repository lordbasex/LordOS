// store_trademark_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the trademark notice of the
// Store's Web apps section and of each web app's page (apps/store/trademark.js, 0.8.8.9) over the English webapps
// catalog: it shows the catalog's text, which says the names, logos and icons belong to their owners and only identify
// the services, that LordOS is not affiliated with, endorsed or sponsored by them, and that it runs the sites unmodified;
// it follows the reading direction (textAlign 'start'). Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { TrademarkNote } from '../../apps/store/trademark.js';
import webappsEn from '../../assets/locales/en/webapps.json';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'webapps' ? JSON.stringify(webappsEn) : shellCatalog(lang, ns));

enableShellLayers();
rt.render(h(rt.View, null, h(TrademarkNote)));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const want = webappsEn['store.trademark'] || '';
const node = [...texts].find(([id, x]) => x === want && inTree(id))?.[0];
check('the catalog has the notice', want.length > 0);
check('the notice shows the catalog\'s text', !!node, JSON.stringify([...texts.values()]));
for (const [what, re] of [['trademarks of their owners', /trademarks of their respective owners/], ['only to identify the services', /only to identify the services/],
  ['not affiliated, endorsed or sponsored', /not affiliated with, endorsed or sponsored by them/], ['the sites unmodified', /doesn't change the sites/]]) {
  check(`it says: ${what}`, re.test(want), want);
}
const box = node && (styles.get(node).textAlign ? node : parents.get(node));
check('it follows the reading direction', !!box && styles.get(box).textAlign === 'start', box && JSON.stringify(styles.get(box)));
process.exit(0);
