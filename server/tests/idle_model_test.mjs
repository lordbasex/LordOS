// node server/tests/idle_model_test.mjs — the lock around a system sleep (sdk/idlemodel.js, issue #36).
import assert from 'node:assert/strict';
import { lockBeforeSleep, lockAfterWake } from '../../sdk/idlemodel.js';

// Immediately (0, also the default when the setting is missing): lock before sleeping
assert.equal(lockBeforeSleep(0), true);
assert.equal(lockBeforeSleep(undefined), true);
assert.equal(lockAfterWake(0, 3600), false);          // already locked before sleeping
// Never
assert.equal(lockBeforeSleep(-1), false);
assert.equal(lockAfterWake(-1, 86400), false);
// a delay: not before sleeping, on waking only after sleeping at least that long
for (const d of [5, 60, 300, 900, 3600]) {
  assert.equal(lockBeforeSleep(d), false);
  assert.equal(lockAfterWake(d, d - 1), false);
  assert.equal(lockAfterWake(d, d), true);
  assert.equal(lockAfterWake(d, 6 * 3600), true);
}
assert.equal(lockAfterWake(60, undefined), false);
console.log('idle_model_test: ok');
