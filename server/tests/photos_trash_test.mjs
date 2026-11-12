// photos_trash_test.mjs — Move to Trash in Photos (apps/photos/trash.js, GitHub #119): where each picture goes, the
// library's selection and what the viewer shows after its picture leaves.
import { trashPlan, clickSelect, menuTargets, keepListed, afterRemove } from '../../apps/photos/trash.js';

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${what}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } };
const H = '/home/u/Pictures', M = '/home/u/Cloud';
const accounts = [{ id: 'g', name: 'Google', kind: 'drive', mount: `${M}/Google` }];

// where each picture goes
const plan = trashPlan([`${H}/a.jpg`, `${M}/Google/Photos/b.jpg`, '/media/share/c.png', `${M}/Google/d.png`, `${M}/Google`], accounts, (p) => p.startsWith('/media/share'));
eq('local pictures go to the Trash', plan.local, [`${H}/a.jpg`]);
eq('a slow folder goes off the thread', plan.slow, ['/media/share/c.png']);
eq('cloud pictures are grouped by account', plan.cloud.map((g) => [g.acc.id, g.paths]), [['g', [`${M}/Google/Photos/b.jpg`, `${M}/Google/d.png`]]]);
eq('never the account folder itself', plan.cloud[0].paths.includes(`${M}/Google`), false);
eq('nothing given', trashPlan([], accounts), { local: [], slow: [], cloud: [] });

// selection
const order = ['a', 'b', 'c', 'd', 'e'];
let r = clickSelect(new Set(), order, null, 'b');
eq('a plain click selects one', [[...r.sel], r.anchor], [['b'], 'b']);
r = clickSelect(r.sel, order, r.anchor, 'd', { shift: true });
eq('shift selects from the anchor', [[...r.sel], r.anchor], [['b', 'c', 'd'], 'b']);
r = clickSelect(r.sel, order, 'd', 'a', { shift: true });
eq('shift backwards', [...r.sel], ['a', 'b', 'c', 'd']);
r = clickSelect(new Set(['a', 'c']), order, 'a', 'c', { ctrl: true });
eq('ctrl takes one away', [[...r.sel], r.anchor], [['a'], 'c']);
r = clickSelect(new Set(['a']), order, 'a', 'e', { meta: true });
eq('meta adds one', [...r.sel], ['a', 'e']);
r = clickSelect(new Set(['a', 'b']), order, 'a', 'c');
eq('a plain click on another leaves only it', [...r.sel], ['c']);
eq('shift with an anchor no longer listed: only it', [...clickSelect(new Set(), order, 'zz', 'c', { shift: true }).sel], ['c']);
eq('the menu on a selected picture acts on the selection', menuTargets(new Set(['a', 'b']), 'b'), ['a', 'b']);
eq('the menu on another acts on it alone', menuTargets(new Set(['a', 'b']), 'c'), ['c']);
const s0 = new Set(['a', 'b']);
eq('keepListed keeps the same set when all are there', keepListed(s0, [{ path: 'a' }, { path: 'b' }]) === s0, true);
eq('keepListed drops what left', [...keepListed(s0, [{ path: 'b' }])], ['b']);

// the viewer after a removal
const list = order.map((p) => ({ path: p }));
eq('the next picture after the one shown', afterRemove(list, 2, ['c']), { list: [{ path: 'a' }, { path: 'b' }, { path: 'd' }, { path: 'e' }], index: 2 });
eq('the one before when it was the last', afterRemove(list, 4, ['e']).index, 3);
eq('the same picture when another left', afterRemove(list, 3, ['a']), { list: [{ path: 'b' }, { path: 'c' }, { path: 'd' }, { path: 'e' }], index: 2 });
eq('nothing left: back to the library', afterRemove([{ path: 'a' }], 0, ['a']), null);
console.log(`photos_trash_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
