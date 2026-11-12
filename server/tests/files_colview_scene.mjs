// files_colview_scene.mjs — the scene of tests/menubar_test.mjs files_colview_scene.mjs (GitHub #99; not run by
// itself): Files' column view (apps/files/views.jsx ColumnView) shows a tagged item's tag dots right after its name,
// as the list view does. Before, only a folder's icon took the tag's tint, so a tagged file showed nothing.
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { ColumnView } from '../../apps/files/views.jsx';
import { tagStore } from '../../apps/files/ops.js';
import { TAG_COLORS } from '../../sdk/fileglyphs.jsx';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const tree = {
  '/home/u': [{ name: 'Pictures', dir: true }, { name: 'notes.txt', dir: false, size: 4 }, { name: 'plain.txt', dir: false, size: 4 }],
};
const entriesOf = (d) => (tree[d] || []).map((e) => ({ ...e, path: `${d}/${e.name}`, mtime: 0 }));
tagStore.set({ '/home/u/notes.txt': ['red', 'blue'], '/home/u/Pictures': ['green'] });
const ctx = {
  isDir: (p) => !!tree[p], entryAt: (p) => entriesOf('/home/u').find((e) => e.path === p) || null,
  selected: new Set(), cut: new Set(), renaming: null, dropTarget: null, sortOpts: {},
  onItemDown: () => {}, onItemOpen: () => {}, onContextMenu: () => {},
};
rt.render(h(rt.View, { style: { width: 900, height: 500 } },
  h(ColumnView, { root: '/home/u', chain: [], setChain: () => {}, entriesOf, ctx, sort: { key: 'name', asc: true } })));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const rowOf = (label) => {   // the item's row: the name's Text → its wrapper View → the row
  const raw = [...texts].find(([id, s]) => s === label && inTree(id))?.[0];
  let n = raw;
  for (let i = 0; n && i < 4; i++, n = parents.get(n)) if (styles.get(n)?.height === 24) return n;
  return null;
};
// the dots: nodes inside a row whose background is one of the tag colors (the runtime sends colors as 0xAARRGGBB)
const argb = (hex) => parseInt(`ff${hex.slice(1)}`, 16);
const dotsIn = (row) => [...styles].filter(([id, s]) => inTree(id) && Object.values(TAG_COLORS).map(argb).includes(s.backgroundColor) && s.width === 7
  && (() => { for (let n = parents.get(id); n; n = parents.get(n)) if (n === row) return true; return false; })()).map(([, s]) => s.backgroundColor);

const file = rowOf('notes.txt'), folder = rowOf('Pictures'), plain = rowOf('plain.txt');
check('the rows are there', file && folder && plain, JSON.stringify([file, folder, plain]));
check('a tagged file shows its two dots', JSON.stringify(dotsIn(file)) === JSON.stringify([TAG_COLORS.red, TAG_COLORS.blue].map(argb)), JSON.stringify(dotsIn(file)));
check('a tagged folder shows its dot', JSON.stringify(dotsIn(folder)) === JSON.stringify([TAG_COLORS.green].map(argb)), JSON.stringify(dotsIn(folder)));
check('an untagged file shows none', dotsIn(plain).length === 0, JSON.stringify(dotsIn(plain)));
