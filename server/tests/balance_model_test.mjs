// node server/tests/balance_model_test.mjs — Settings → Sound balance math (apps/settings/balance.js).
import assert from 'node:assert/strict';
import { balanceOf, volumesFor, snapBalance } from '../../apps/settings/balance.js';
const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-9, `${a} != ${b}`);
near(balanceOf([0.5, 0.5]), 0);
near(balanceOf([0.25, 0.5]), 0.5);
near(balanceOf([0.5, 0.25]), -0.5);
near(balanceOf([0.5]), 0);
near(balanceOf(undefined), 0);
assert.deepEqual(volumesFor(0.8, 0), [0.8, 0.8]);
assert.deepEqual(volumesFor(0.8, 1), [0, 0.8]);
assert.deepEqual(volumesFor(0.8, -1), [0.8, 0]);
for (const b of [-0.7, -0.2, 0.3, 0.9]) near(balanceOf(volumesFor(0.6, b)), b);
assert.equal(snapBalance(0.03), 0);
assert.equal(snapBalance(-0.2), -0.2);
near(Math.max(...volumesFor(0.6, 0.4)), 0.6);   // moving the balance keeps the level
console.log('balance_model_test: ok');
