// files_drag_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself), GitHub #218: dragging files in a folder
// of 2000 entries. Each pointer move of a drag used to set the Files window's own state, so the whole window (every
// card in sight, the sidebar, the toolbar) drew again on every move. Now the ghost and the drop highlights live in
// apps/files/dnd.jsx's stores: a move draws only the ghost, and a place's highlight only when the place under the
// pointer changes. Counted by the renders of Files' main component (it reads lord.fs.home while it renders, see
// files_drag_scene_lord.js). Prints "ok <name>" or "FAIL <name> <detail>".
import { fsCalls } from './files_drag_scene_lord.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import Files from '../../apps/files/index.jsx';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

rt.render(h(rt.View, { style: { width: 1100, height: 700 } },
  h(AppWindowContext.Provider, { value: { id: 'files-1', args: {} } }, h(Files))));
await flush();
await new Promise((res) => setTimeout(res, 80));        // the icon view measures its grid 50 ms after drawing
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// a name's Text node (the raw text's parent: events go to the nodes React made)
const nodeOf = (label) => parents.get([...texts].find(([id, s]) => s === label && inTree(id))?.[0]);
check('the folder is listed', ['file-0000.txt', 'file-0003.txt', 'Folder 05'].every(nodeOf), JSON.stringify([...texts.values()].slice(0, 40)));
const drawn = [...texts].filter(([id, s]) => /^file-/.test(s) && inTree(id)).length;
check(`a big folder draws only what is in sight (${drawn} of 2000 files)`, drawn > 20 && drawn < 600, String(drawn));

// press on a file, start the drag, then move over other files and over a folder
const file = nodeOf('file-0000.txt'), folder = nodeOf('Folder 05'), others = ['file-0001.txt', 'file-0002.txt', 'file-0003.txt'].map(nodeOf);
const ev = globalThis.__lordEvent;
ev('down', file, 300, 200, 0);
await flush();
ev('move', file, 320, 220, 0);             // past the 6 px that start a drag
await flush();
const ghostShown = () => [...styles].some(([id, s]) => inTree(id) && s.opacity === 0.85 && s.pointerEvents === 'none');
check('the ghost is shown once the drag starts', ghostShown());

// a move after which Files' main component read lord.fs.home is a move that drew the whole window again
const MOVES = 30;
let overFiles = 0;
for (let i = 0; i < MOVES; i++) {
  const n0 = fsCalls.home;
  ev('move', others[i % others.length], 330 + i, 230 + i, 0);
  await flush();
  if (fsCalls.home > n0) overFiles++;
}
// the ghost follows the pointer: its left edge is the last move's x - 20 (the stub's layout puts the window at 0, 0)
const ghostAt = [...styles].filter(([id, s]) => inTree(id) && s.opacity === 0.85 && s.pointerEvents === 'none').map(([, s]) => s.left);
check('the ghost follows the pointer', ghostAt.includes(330 + MOVES - 1 - 20) || ghostAt.includes(330 + MOVES - 1), JSON.stringify(ghostAt));

// over a folder: its card is lit, and only that card draws again. The card is the name's ancestor with the card's
// padding; its icon box (radius 7) takes the accent color.
const cardOf = (n) => { for (let i = 0; n && i < 6; i++, n = parents.get(n)) if (styles.get(n)?.paddingTop === 6 && styles.get(n)?.gap === 5) return n; return null; };
const card = cardOf(folder);
const bgOf = () => { const box = [...parents].find(([c, p]) => p === card && styles.get(c)?.borderRadius === 7)?.[0]; return box ? styles.get(box).backgroundColor : undefined; };
const bg0 = bgOf();
let overFolder = 0;
for (let i = 0; i < 5; i++) { const n0 = fsCalls.home; ev('move', folder, 400 + i, 300, 0); await flush(); if (fsCalls.home > n0) overFolder++; }
check('the folder under the pointer is lit', !!card && bgOf() !== bg0 && !!bgOf(), `${card} ${bg0} -> ${bgOf()}`);

console.log(`ok measure: the whole window drew again on ${overFiles} of ${MOVES} moves over files, ${overFolder} of 5 over a folder`);
check('a move over files draws only the ghost', overFiles === 0, `${overFiles} of ${MOVES}`);
check('a move onto a folder draws only the ghost and the highlights', overFolder === 0, `${overFolder} of 5`);

ev('up', folder, 404, 300, 0);
await flush();
check('the ghost goes away on release', !ghostShown());
process.exit(0);
