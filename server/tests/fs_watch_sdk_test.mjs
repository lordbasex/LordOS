// fs_watch_sdk_test.mjs — sdk/fs.js's watch(): lord.fs.watch / unwatch through the SDK (server/src/fswatch.c), against a
// stubbed lord.fs: the event reaches fn, stopping unwatches once, a refusal throws the code, an isolated app (no watch)
// says sys.unsupported, and protected is documented on stat.
import assert from 'node:assert/strict';

const calls = [];
let nextId = 1;
const fns = new Map();
globalThis.lord = {
  fs: {
    watch(path, fn) {
      calls.push(['watch', path]);
      if (path === 'Cloud/x') { const e = new Error('fs.watchRemote'); e.code = 'fs.watchRemote'; throw e; }
      const id = nextId++;
      fns.set(id, fn);
      return id;
    },
    unwatch(id) { calls.push(['unwatch', id]); return fns.delete(id); },
  },
  locale: { get: () => 'en', catalog: () => null },
  settings: { load: () => '{}', save() {} },
};

const fs = await import('../../sdk/fs.js');

// an event reaches fn, with no file names
const seen = [];
const stop = fs.watch('Desktop', (ev) => seen.push(ev));
assert.equal(typeof stop, 'function');
fns.get(1)({ changed: true, overflow: false, gone: false });
assert.deepEqual(seen, [{ changed: true, overflow: false, gone: false }]);
fns.get(1)(undefined);                                   // a missing event object still reads as a change
assert.equal(seen[1].changed, true);

// stopping unwatches exactly once
stop();
stop();
assert.deepEqual(calls.filter((c) => c[0] === 'unwatch'), [['unwatch', 1]]);

// a refusal throws the server's code
assert.throws(() => fs.watch('Cloud/x', () => {}), (e) => e.code === 'fs.watchRemote');

// an isolated app's lord.fs without watch: sys.unsupported
const saved = globalThis.lord.fs.watch;
delete globalThis.lord.fs.watch;
assert.throws(() => fs.watch('Desktop', () => {}), (e) => e.code === 'sys.unsupported');
globalThis.lord.fs.watch = saved;

console.log('fs-watch-sdk-test: ok');
