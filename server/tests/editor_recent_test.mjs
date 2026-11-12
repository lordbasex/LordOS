// Text Editor's File → Open Recent (#113), from server/: node tests/editor_recent_test.mjs
import assert from 'node:assert/strict';
import { addRecent, removeRecent, recentRows, RECENT_MAX } from '../../apps/editor/recent.js';

// newest first, no duplicates, at most five
let l = [];
for (const p of ['/h/a.txt', '/h/b.txt', '/h/c.txt', '/h/a.txt']) l = addRecent(l, p);
assert.deepEqual(l, ['/h/a.txt', '/h/c.txt', '/h/b.txt']);
for (const p of ['/h/d', '/h/e', '/h/f']) l = addRecent(l, p);
assert.equal(RECENT_MAX, 5);
assert.deepEqual(l, ['/h/f', '/h/e', '/h/d', '/h/a.txt', '/h/c.txt']);
// garbage in the saved value is dropped; a relative or empty path is never added
assert.deepEqual(addRecent(['/h/x', 3, null, 'rel/y', ''], '/h/z'), ['/h/z', '/h/x']);
assert.deepEqual(addRecent('nonsense', 'rel'), []);
assert.deepEqual(addRecent(undefined, '/h/one'), ['/h/one']);
assert.deepEqual(removeRecent(['/h/a', '/h/b'], '/h/a'), ['/h/b']);
assert.deepEqual(removeRecent(null, '/h/a'), []);

// rows: names, a folder only where two share a name, missing files greyed (kept)
const rows = recentRows(['/home/u/p1/index.js', '/home/u/notes.md', '/srv/p2/index.js', '/gone.txt'],
  { home: '/home/u', exists: (p) => p !== '/gone.txt' });
assert.deepEqual(rows.map((r) => r.label), ['index.js — ~/p1', 'notes.md', 'index.js — /srv/p2', 'gone.txt']);
assert.deepEqual(rows.map((r) => r.missing), [false, false, false, true]);
// an exists() that throws counts as missing; never more than five rows
assert.equal(recentRows(['/a'], { exists: () => { throw new Error('x'); } })[0].missing, true);
assert.equal(recentRows(['/1', '/2', '/3', '/4', '/5', '/6']).length, 5);
assert.deepEqual(recentRows(undefined), []);

console.log('editor-recent-test: ok');
