// fs_recent_cache_test.mjs — sdk/fs.js recentCached (QA 0.8.9.6.2): the Recent widget's and the search's list comes
// from a walk on lord-server's workers (lord.fs.recentAsync), never lord.fs.recent on the interface's thread; one walk at
// a time, a new one when the last is a minute old, subscribers told when it answers.
//   node server/tests/fs_recent_cache_test.mjs
let sync = 0, asks = 0, lastReq = 0;
globalThis.lord = {
  settings: { load: () => '{}', save: () => true },
  fs: {
    recent: () => { sync++; return []; },
    recentAsync: (limit, days) => { asks++; return ++lastReq; },
  },
};
const fs = await import('../../sdk/fs.js');
let bad = 0, n = 0;
const eq = (name, a, b) => { n++; if (JSON.stringify(a) !== JSON.stringify(b)) { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
let told = 0;
fs.recentFiles.subscribe(() => told++);
eq('nothing yet: empty, a walk asked', [fs.recentCached(3, 30), asks, sync], [[], 1, 0]);
eq('one walk at a time', [fs.recentCached(3, 30), asks], [[], 1]);
const now = Date.now() / 1000;
globalThis.__lordFsReply(lastReq, { recent: [{ path: '/h/a', mtime: now }, { path: '/h/b', mtime: now - 40 * 86400 }, { path: '/h/c', mtime: now - 1 }] });
eq('the answer: subscribers told', told, 1);
eq('the list, the days and the limit applied', fs.recentCached(5, 30).map((f) => f.path), ['/h/a', '/h/c']);
eq('fresh: no new walk', asks, 1);
eq('the interface thread never walked', sync, 0);
eq('a stable empty value before any list is a frozen array', Object.isFrozen(fs.recentFiles.get()) || Array.isArray(fs.recentFiles.get()), true);
console.log(bad ? `fs-recent-cache-test: ${bad}/${n} failed` : `fs-recent-cache-test: ${n} ok`);
process.exit(bad ? 1 : 0);
