// avatar_model_test.mjs — the square an avatar is cut from (apps/settings/avatarmodel.js).
import assert from 'node:assert/strict';
import { avatarCrop } from '../../apps/settings/avatarmodel.js';

assert.deepEqual(avatarCrop(4000, 3000, 0, null), { x: 500, y: 0, size: 3000 }, 'no zoom: the short side, centered');
assert.deepEqual(avatarCrop(4000, 3000, 1, { x: 2000, y: 1500 }), { x: 1625, y: 1125, size: 750 }, 'full zoom: a quarter of it');
assert.deepEqual(avatarCrop(4000, 3000, 1, { x: 0, y: 0 }), { x: 0, y: 0, size: 750 }, 'moved past a corner: held in the picture');
assert.deepEqual(avatarCrop(4000, 3000, 1, { x: 9999, y: 9999 }), { x: 3250, y: 2250, size: 750 }, 'and the other corner');
assert.equal(avatarCrop(100, 80, 1, null).size, 64, 'never under 64 px');
assert.equal(avatarCrop(40, 30, 1, null).size, 30, 'a tiny picture: all of it');
assert.equal(avatarCrop(0, 0, 0, null), null, 'no picture');
assert.deepEqual(avatarCrop(1000, 1000, 5, { x: 'a', y: NaN }).size, 250, 'bad values: clamped, no NaN');
console.log('avatar model: ok');
