// store_endless_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the Store's long lists load
// their next page near the end of the scroll (apps/store/endless.js): a page per content height, none while far from
// the end, a list that grew asks again once laid out (a tall window fills itself), back to the first page when the list
// changes, as long as it was when it is shown again (back from an app's page), and the light "Show more" with what is
// left. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { EndsContext, MoreButton, PAGE, makeEnds, usePaged } from '../../apps/store/endless.js';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

// the content's height and the offset the stub ScrollView reports
let content = 5000, offset = 0;
lord.getLayout = () => [0, 0, 800, content];
const scrollRef = { current: { inner: { current: { id: 1 } }, offset: () => offset } };
const ends = makeEnds(scrollRef);

let shown = null;
function List({ total, k }) {
  const { limit, more } = usePaged(total, k);
  shown = { limit, more };
  return h(rt.View, null, h(MoreButton, { left: total - limit, onMore: more }));
}
const render = (props) => rt.render(h(EndsContext.Provider, { value: ends }, props ? h(List, props) : null));
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const label = () => [...texts].filter(([id]) => inTree(id)).map(([, x]) => x).find((x) => x.startsWith('flathub.showMore')) || '';

render({ total: 200, k: 'a' });
await flush();
check('first page', shown.limit === PAGE, shown.limit);
check('the light Show more says what is left', label() === 'flathub.showMore:{"count":140}', label());
ends.check(0, 800);
await flush();
check('far from the end: no page', shown.limit === PAGE, shown.limit);
ends.check(4000, 800);
await flush();
check('near the end: one more page', shown.limit === 2 * PAGE, shown.limit);
ends.check(4100, 800);
await flush();
check('one page per content height', shown.limit === 2 * PAGE, shown.limit);
content = 9000; offset = 8300;
await wait(120);
await flush();
check('grown and still near the end: it asks again', shown.limit === 3 * PAGE, shown.limit);
content = 20000;
await wait(120);
await flush();
check('grown and far from the end: it stops', shown.limit === 3 * PAGE, shown.limit);
shown.more();
await flush();
check('Show more adds a page', shown.limit === 4 * PAGE, shown.limit);
check('nothing left: no Show more', label() === '', label());
ends.check(19500, 800);
await flush();
check('all shown: no more pages', shown.limit === 4 * PAGE, shown.limit);

render(null);
await flush();
render({ total: 200, k: 'a' });
await flush();
check('shown again: as long as it was', shown.limit === 4 * PAGE, shown.limit);
render({ total: 500, k: 'b' });
await flush();
check('another list: the first page', shown.limit === PAGE, shown.limit);
ends.limits.clear();
render(null);
await flush();
render({ total: 200, k: 'a' });
await flush();
check('cleared (the list changed): the first page', shown.limit === PAGE, shown.limit);
ends.reset(); content = 5000;
ends.check(4500, 800);
await flush();
check('after reset the same height pages again', shown.limit === 2 * PAGE, shown.limit);
process.exit(0);
