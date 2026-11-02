// The app switcher's rules: node tests/switcher_model_test.mjs
import assert from 'node:assert/strict';
import { switcherPrefs, switcherMods, switcherGrabName, groupWindows, digitOf, stepWindow, validColor, skinColors, fitScale, switcherBits } from '../sdk/switchermodel.js';

assert.equal(switcherPrefs(null).keys, 'theme');
assert.equal(switcherPrefs({ keys: 'x', scope: 'x', size: 'x', skin: 'x', excluded: 1 }).scope, 'all');
assert.deepEqual(switcherPrefs({ excluded: 1 }).excluded, []);
assert.deepEqual(switcherMods('theme', 'meta'), ['meta']);
assert.deepEqual(switcherMods('theme', 'alt'), ['alt']);
assert.deepEqual(switcherMods('both', 'alt'), ['meta', 'alt']);
assert.equal(switcherGrabName('flow', 'theme'), 'flow');
assert.equal(switcherGrabName('classic', 'both'), 'classic+tab-both');
assert.equal(switcherBits({ mods: 8 }), 8);
assert.equal(switcherBits({ mods: 2 }), 2);

const a = { id: 'a', d: 1, s: 1 }, b = { id: 'b', d: 2, s: 2, minimized: true };
const g = { key: 'k', windows: [a, b] };
const ctx = { inDesktop: (w) => w.d === 1, inDisplay: (w) => w.s === 2 };
assert.equal(groupWindows(g, switcherPrefs({}), ctx).length, 2);
assert.deepEqual(groupWindows(g, switcherPrefs({ scope: 'desktop' }), ctx), [a]);
assert.deepEqual(groupWindows(g, switcherPrefs({ scope: 'display' }), ctx), [b]);
assert.deepEqual(groupWindows(g, switcherPrefs({ excluded: ['k'] }), ctx), []);
assert.deepEqual(groupWindows({ key: 'k', windows: [b] }, switcherPrefs({ hideMinimized: true }), ctx), []);
assert.deepEqual(groupWindows(g, switcherPrefs({ scope: 'desktop', hideMinimized: true }), { ...ctx, inDesktop: (w) => w.d === 2 }), []);

assert.equal(digitOf(2), 0); assert.equal(digitOf(10), 8); assert.equal(digitOf(11), -1); assert.equal(digitOf(1), -1);
assert.equal(stepWindow(0, 3, -1), 2); assert.equal(stepWindow(2, 3, 1), 0); assert.equal(stepWindow(0, 0, 1), 0);
assert.ok(validColor('#a1B2c3')); assert.ok(!validColor('red')); assert.ok(!validColor('#123'));
const th = { mode: 'dark', accent: '#111111', text: '#fff', separator: '#222' };
assert.equal(skinColors(switcherPrefs({ skin: 'custom', customBg: 'bad' }), th).bg, '#20202a');
assert.equal(skinColors(switcherPrefs({ skin: 'contrast' }), th).accent, '#ffffff');
assert.equal(fitScale(1000, 500, 100, 100), 0.1);
console.log('switcher model: ok');
