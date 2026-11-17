// files_colview_test.mjs — the column view's items as data: what is picked in a deeper column is part of the selection
// (apps/files/colview.js, GitHub #100); every column scrolls instead of being dropped, and Back steps through the
// columns (#133); Select All takes the selection's column (#170); where New Folder puts the folder (#134); why a folder is only browsed (#139).
import { columnEntries, columnRange, columnAll, byFolder, folderReadOnly, COL_W, COL_MIN, COL_MAX, COL_EXTRA, MAX_COL_WIDTHS, clampCol, colWidth, colWidthsWith, fitCol,
  PREVIEW_MIN, colStrip, chainFolder, chainStep, historyStep, newFolderDir, listEntries, readOnlyWhy } from '../../apps/files/colview.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const tree = {
  '/home/u': [{ name: 'Pictures', dir: true }, { name: 'notes.txt', dir: false }],
  '/home/u/Pictures': [{ name: 'Screenshots', dir: true }, { name: 'a.png', dir: false }],
  '/home/u/Pictures/Screenshots': [{ name: 's1.png', dir: false }, { name: 's2.png', dir: false }, { name: 's3.png', dir: false }],
};
const itemsOf = (d) => (tree[d] || []).map((e) => ({ ...e, path: `${d}/${e.name}` }));
const paths = (l) => l.map((e) => e.path);

eq('no chain: the first column', paths(columnEntries('/home/u', [], itemsOf)), ['/home/u/Pictures', '/home/u/notes.txt']);
// Pictures → Screenshots → s2.png: four columns, and s2.png is among the items (it was not: Copy had nothing)
const pool = columnEntries('/home/u', ['/home/u/Pictures', '/home/u/Pictures/Screenshots', '/home/u/Pictures/Screenshots/s2.png'], itemsOf);
eq('deep chain: every column', paths(pool), ['/home/u/Pictures', '/home/u/notes.txt', '/home/u/Pictures/Screenshots', '/home/u/Pictures/a.png',
  '/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s2.png', '/home/u/Pictures/Screenshots/s3.png']);
const selected = new Set(['/home/u/Pictures/Screenshots/s2.png']);
eq('deep pick: selected', paths(pool.filter((e) => selected.has(e.path))), ['/home/u/Pictures/Screenshots/s2.png']);
eq('a file in the chain ends it', paths(columnEntries('/home/u', ['/home/u/notes.txt', '/home/u/x'], itemsOf)), ['/home/u/Pictures', '/home/u/notes.txt']);
eq('a chain item that is gone ends it', paths(columnEntries('/home/u', ['/home/u/Gone'], itemsOf)), ['/home/u/Pictures', '/home/u/notes.txt']);
eq('an unreadable folder', paths(columnEntries('/nope', [], () => null)), []);

// Shift: a range inside the clicked one's column
eq('range in a deep column', columnRange(pool, '/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s3.png'),
  ['/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s2.png', '/home/u/Pictures/Screenshots/s3.png']);
eq('range backwards', columnRange(pool, '/home/u/Pictures/Screenshots/s3.png', '/home/u/Pictures/Screenshots/s2.png'),
  ['/home/u/Pictures/Screenshots/s2.png', '/home/u/Pictures/Screenshots/s3.png']);
eq('anchor in another column: just the clicked one', columnRange(pool, '/home/u/notes.txt', '/home/u/Pictures/Screenshots/s3.png'), ['/home/u/Pictures/Screenshots/s3.png']);
eq('anchor gone', columnRange(pool, '/home/u/zzz', '/home/u/Pictures/a.png'), ['/home/u/Pictures/a.png']);

// Select All (GitHub #170): the column the selection is in, not the first one
const shots = ['/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s2.png', '/home/u/Pictures/Screenshots/s3.png'];
eq('all: a file picked in the third column', paths(columnAll(pool, '/home/u/Pictures/Screenshots/s2.png', selected)), shots);
eq('all: the anchor wins over a deeper selection', paths(columnAll(pool, '/home/u/Pictures/a.png', new Set(['/home/u/Pictures/a.png', shots[0]]))),
  ['/home/u/Pictures/Screenshots', '/home/u/Pictures/a.png']);
eq('all: anchor not selected, the deepest selected', paths(columnAll(pool, '/home/u/notes.txt', new Set(['/home/u/Pictures', shots[2]]))), shots);
eq('all: anchor gone from the columns', paths(columnAll(pool, '/home/u/zzz', new Set(['/home/u/zzz', '/home/u/Pictures/a.png']))),
  ['/home/u/Pictures/Screenshots', '/home/u/Pictures/a.png']);
eq('all: nothing selected, the first column', paths(columnAll(pool, null, new Set())), ['/home/u/Pictures', '/home/u/notes.txt']);
eq('all: the root folder', paths(columnAll([{ path: '/bin' }, { path: '/etc' }, { path: '/etc/x' }], null, new Set())), ['/bin', '/etc']);
eq('all: empty', columnAll([], null, new Set()), []);

// Duplicate: next to each item
eq('by folder', byFolder(['/home/u/notes.txt', '/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s2.png']),
  [{ dir: '/home/u', paths: ['/home/u/notes.txt'] }, { dir: '/home/u/Pictures/Screenshots', paths: ['/home/u/Pictures/Screenshots/s1.png', '/home/u/Pictures/Screenshots/s2.png'] }]);
eq('by folder: the root', byFolder(['/etc']), [{ dir: '/', paths: ['/etc'] }]);

// the folder shown: Home's own stat says read-only (its parent is outside the home), but what is in it takes changes
const stats = { '/home/u': { writable: false }, '/home/u/Pictures': { writable: true }, '/usr': { writable: false } };
const stat = (p) => { if (!stats[p]) throw new Error('gone'); return stats[p]; };
eq('Home takes changes', folderReadOnly('/home/u', '/home/u', stat), false);
eq('a folder in it', folderReadOnly('/home/u/Pictures', '/home/u', stat), false);
eq('a system folder is only browsed', folderReadOnly('/usr', '/home/u', stat), true);
eq('a folder that is gone', folderReadOnly('/home/u/gone', '/home/u', stat), true);
eq('no folder (Recents…)', folderReadOnly(null, '/home/u', stat), false);

// ---- widths (GitHub #99) ----
eq('no stored width', colWidth({}, '/a'), COL_W);
eq('stored width', colWidth({ '/a': 300 }, '/a'), 300);
eq('stored junk', colWidth({ '/a': 'x' }, '/a'), COL_W);
eq('not an object', colWidth(null, '/a'), COL_W);
eq('clamped low', colWidth({ '/a': 10 }, '/a'), COL_MIN);
eq('clamped high', clampCol(5000), COL_MAX);
eq('rounded', clampCol(250.6), 251);
eq('with: set', colWidthsWith({ '/a': 300 }, '/b', 260), { '/a': 300, '/b': 260 });
eq('with: newest last', Object.keys(colWidthsWith({ '/a': 300, '/b': 260 }, '/a', 310)), ['/b', '/a']);
eq('with: forget', colWidthsWith({ '/a': 300, '/b': 260 }, '/a', null), { '/b': 260 });
eq('with: junk dropped', colWidthsWith({ '/a': 'x', '/b': 1e9 }, '/c', 200), { '/b': COL_MAX, '/c': 200 });
const lots = {};
for (let i = 0; i < MAX_COL_WIDTHS + 20; i++) lots[`/d${i}`] = 200;
const capped = colWidthsWith(lots, '/new', 300);
eq('with: capped', Object.keys(capped).length, MAX_COL_WIDTHS);
eq('with: the oldest go', '/d0' in capped || !('/new' in capped), false);
eq('fit: longest name', fitCol([40, 180.2, 90]), 181 + COL_EXTRA);
eq('fit: tags count', fitCol([100, 90], [0, 30]), 120 + COL_EXTRA);
eq('fit: short names keep the minimum', fitCol([10]), COL_MIN);
eq('fit: empty folder', fitCol([]), COL_MIN);
eq('fit: very long names', fitCol([2000]), COL_MAX);
// ---- every column, scrolled (GitHub #133) ----
// #99 kept only the last columns that fit: widening one, or opening a subfolder, dropped the first ones for good
eq('strip: fits, the preview takes the rest', colStrip([220, 220], 900), { preview: 460, total: 900, over: 0 });
eq('strip: four columns in 700 scroll, none dropped', colStrip([220, 220, 220, 220], 700), { preview: PREVIEW_MIN, total: 1080, over: 380 });
eq('strip: widening the first one only adds to the scroll', colStrip([500, 220, 220, 220], 700).over - colStrip([220, 220, 220, 220], 700).over, 280);
eq('strip: not laid out yet', colStrip([220, 220, 220], 0), { preview: PREVIEW_MIN, total: 860, over: 0 });
eq('strip: no columns', colStrip([], 600), { preview: 600, total: 600, over: 0 });
const isDirT = (p) => !/\.\w+$/.test(p);
eq('deepest: a file ends in its folder', chainFolder(['/h/Documents', '/h/Documents/Test', '/h/Documents/Test/a.txt'], isDirT), '/h/Documents/Test');
eq('deepest: none picked', chainFolder([], isDirT), null);

// ---- Back and Forward through the columns (GitHub #133) ----
const sameT = (a, b) => a.type === b.type && a.path === b.path;
const viewOfT = (l) => ({ view: l.path === '/h' ? 'icons' : 'list' });
let tab = { loc: { type: 'dir', path: '/h/Documents' }, chain: [], back: [{ type: 'dir', path: '/h' }], fwd: [], view: 'columns' };
const apply = (p) => { tab = { ...tab, ...p }; };
apply(chainStep(tab, ['/h/Documents/Test'], isDirT));
eq('a folder opened in a column is a step', tab.back.length, 2);
eq('the step keeps the columns it left', tab.back[1], { type: 'dir', path: '/h/Documents', chain: [] });
apply(chainStep(tab, ['/h/Documents/Test', '/h/Documents/Test/a.txt'], isDirT));
eq('a file picked is no step', tab.back.length, 2);
apply(chainStep(tab, ['/h/Documents/Test', '/h/Documents/Test/Sub'], isDirT));
eq('a deeper folder is another step', tab.back.length, 3);
apply(historyStep(tab, -1, viewOfT, sameT));
eq('Back: the columns before, still in columns', [tab.chain, tab.view, tab.loc], [['/h/Documents/Test', '/h/Documents/Test/a.txt'], 'columns', { type: 'dir', path: '/h/Documents' }]);
apply(historyStep(tab, -1, viewOfT, sameT));
eq('Back again: the first column alone', [tab.chain, tab.view], [[], 'columns']);
apply(historyStep(tab, 1, viewOfT, sameT));
eq('Forward: the column again', tab.chain, ['/h/Documents/Test', '/h/Documents/Test/a.txt']);
apply(historyStep(tab, -1, viewOfT, sameT));
apply(historyStep(tab, -1, viewOfT, sameT));
eq('Back to another place: its own view, no chain on the place', [tab.loc, tab.view, tab.chain], [{ type: 'dir', path: '/h' }, 'icons', []]);
eq('nothing left', historyStep(tab, -1, viewOfT, sameT), null);
eq('Forward from there', historyStep(tab, 1, viewOfT, sameT).loc, { type: 'dir', path: '/h/Documents' });
const capT = { loc: { type: 'dir', path: '/a' }, chain: [], back: Array.from({ length: 50 }, (_, i) => ({ type: 'dir', path: `/p${i}` })), fwd: [{ type: 'dir', path: '/f' }] };
const stepped = chainStep(capT, ['/a/b'], isDirT);
eq('steps: at most 50, Forward cleared', [stepped.back.length, stepped.fwd], [50, []]);

// ---- where New Folder goes (GitHub #134) ----
const nf = (o) => newFolderDir({ dir: '/h', isDir: isDirT, ...o });
eq('columns: the last column\'s folder', nf({ view: 'columns', chain: ['/h/Documents', '/h/Documents/Test1'] }), '/h/Documents/Test1');
eq('columns: a file picked, its folder', nf({ view: 'columns', chain: ['/h/Documents', '/h/Documents/a.txt'] }), '/h/Documents');
eq('columns: nothing picked, the folder shown', nf({ view: 'columns', chain: [] }), '/h');
eq('list: a selected folder', nf({ view: 'list', selected: ['/h/Documents'] }), '/h/Documents');
eq('list: a selected folder in an unfolded one', nf({ view: 'list', selected: ['/h/Documents/Test1/sub-test-2'] }), '/h/Documents/Test1/sub-test-2');
eq('list: a file in an unfolded folder', nf({ view: 'list', selected: ['/h/Documents/Test1/a.txt'] }), '/h/Documents/Test1');
eq('list: several in one folder', nf({ view: 'list', selected: ['/h/Documents/a.txt', '/h/Documents/Test1'] }), '/h/Documents');
eq('list: several in different folders', nf({ view: 'list', selected: ['/h/Documents/a.txt', '/h/b.txt'] }), '/h');
eq('list: nothing selected', nf({ view: 'list', selected: [] }), '/h');
eq('icons: always the folder shown', nf({ view: 'icons', selected: ['/h/Documents'] }), '/h');
eq('gallery: always the folder shown', nf({ view: 'gallery', selected: ['/h/Documents'] }), '/h');
eq('never outside the folder shown', nf({ view: 'list', selected: ['/etc'] }), '/h');
eq('no prefix tricks', nf({ view: 'list', selected: ['/home2'], dir: '/home' }), '/home');
eq('no folder shown (Recents)', newFolderDir({ view: 'list', dir: null, selected: ['/h/x'], isDir: isDirT }), null);
eq('the root: a folder picked in it', newFolderDir({ view: 'columns', dir: '/', chain: ['/opt'], isDir: isDirT }), '/opt');
eq('the root: a file picked in it', newFolderDir({ view: 'columns', dir: '/', chain: ['/a.txt'], isDir: isDirT }), '/');

// ---- the list's unfolded folders (GitHub #134) ----
const lt = { '/h/Documents': [{ path: '/h/Documents/Test1', dir: true }, { path: '/h/Documents/a.txt', dir: false }],
  '/h/Documents/Test1': [{ path: '/h/Documents/Test1/sub-test-2', dir: true }] };
const top = [{ path: '/h/Documents', dir: true }, { path: '/h/Music', dir: true }];
const lp = (exp) => listEntries(top, new Set(exp), (e) => lt[e.path] || []).map((e) => e.path);
eq('list: nothing unfolded', lp([]), ['/h/Documents', '/h/Music']);
eq('list: unfolded, in row order', lp(['/h/Documents', '/h/Documents/Test1']),
  ['/h/Documents', '/h/Documents/Test1', '/h/Documents/Test1/sub-test-2', '/h/Documents/a.txt', '/h/Music']);
eq('list: a folded parent hides an unfolded child', lp(['/h/Documents/Test1']), ['/h/Documents', '/h/Music']);
eq('list: unreadable', listEntries(top, new Set(['/h/Music']), () => null).length, 2);

// ---- why a folder is only browsed (GitHub #139) ----
eq('ro: /opt/lampp/htdocs is outside, whoever owns it', readOnlyWhy('/opt/lampp/htdocs', true, '/home/u', 'u'), 'outside');
eq('ro: the root', readOnlyWhy('/', true, '/home/u', 'u'), 'outside');
eq('ro: a folder in the home without permission', readOnlyWhy('/home/u/root-owned', true, '/home/u', 'u'), 'denied');
eq('ro: a read-only disk', readOnlyWhy('/media/u/CDROM', true, '/home/u', 'u'), 'denied');
eq('ro: /run/media too', readOnlyWhy('/run/media/u/X', true, '/home/u', 'u'), 'denied');
eq('ro: another user\'s disk is outside', readOnlyWhy('/media/v/X', true, '/home/u', 'u'), 'outside');
eq('ro: no prefix tricks', readOnlyWhy('/home/user2', true, '/home/u', 'u'), 'outside');
eq('ro: no user known', readOnlyWhy('/media/u/X', true, '/home/u', ''), 'outside');
eq('ro: takes changes', readOnlyWhy('/home/u/Documents', false, '/home/u', 'u'), null);
eq('ro: no folder', readOnlyWhy(null, true, '/home/u', 'u'), null);

console.log(`files-colview-test: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
