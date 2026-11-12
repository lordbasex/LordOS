// files_colview_test.mjs — the column view's items as data: what is picked in a deeper column is part of the selection
// (apps/files/colview.js, GitHub #100).
import { columnEntries, columnRange, byFolder, folderReadOnly, COL_W, COL_MIN, COL_MAX, COL_EXTRA, MAX_COL_WIDTHS, clampCol, colWidth, colWidthsWith, fitCol, colsThatFit } from '../../apps/files/colview.js';

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
eq('fit: unknown avail, the last three', colsThatFit([220, 220, 220, 220], 0), 3);
eq('fit: two of four', colsThatFit([220, 220, 220, 220], 700), 2);
eq('fit: narrow columns, more', colsThatFit([150, 150, 150, 150], 800), 4);
eq('fit: one too wide still shows', colsThatFit([220, 600], 500), 1);
eq('fit: none', colsThatFit([], 900), 1);

console.log(`files-colview-test: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
