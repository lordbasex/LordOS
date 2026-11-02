// remotecert_test.mjs — a known certificate passes by itself, a changed one is never treated like a new one.
import assert from 'node:assert';
import { certCheck } from '../../sdk/remotecert.js';
const t = { 'h:0': 'AA' };
assert.equal(certCheck(t, 'h:0', 'AA').kind, 'known');
assert.deepEqual(certCheck(t, 'h:0', 'BB'), { kind: 'changed', old: 'AA' });
assert.equal(certCheck(t, 'other:0', 'BB').kind, 'new');
console.log('remotecert-test: ok');
