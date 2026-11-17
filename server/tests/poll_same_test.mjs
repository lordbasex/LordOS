// sameValue (sdk/samevalue.js): a poll that reads the same data again keeps the old value, so nothing re-renders.
import assert from 'node:assert/strict';
import { sameValue } from '../../sdk/samevalue.js';

assert.ok(sameValue(1, 1));
assert.ok(sameValue(NaN, NaN));
assert.ok(!sameValue(0.5, 0.51));
assert.ok(sameValue(null, null));
assert.ok(!sameValue(null, {}));
assert.ok(sameValue({ a: 1, b: 'x', c: [1, 2, { d: true }] }, { b: 'x', a: 1, c: [1, 2, { d: true }] }));
assert.ok(!sameValue({ a: 1 }, { a: 1, b: undefined }));
assert.ok(!sameValue({ a: [1, 2] }, { a: [1, 2, 3] }));
assert.ok(!sameValue([1], { 0: 1 }));
assert.ok(!sameValue({ memAvailKB: 100, uptime: 5 }, { memAvailKB: 100, uptime: 10 }));
console.log('poll same: ok');
