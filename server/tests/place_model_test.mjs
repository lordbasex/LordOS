// place_model_test.mjs — a window comes back on the display it was on, or somewhere visible when that one is gone.
import assert from 'node:assert';
import { displayKey, findDisplay, placeOf, cleanPlace, restoreFrame, putPlace, MAX_PLACES } from '../../sdk/placemodel.js';

const A = { id: 1, name: 'Dell U2415', connector: 'HDMI-A-1', x: 0, y: 0, w: 1920, h: 1080, main: true };
const B = { id: 2, name: 'LG 27', connector: 'DP-1', x: 1920, y: 0, w: 2560, h: 1440 };
const area = (d) => ({ x: d.x, y: d.y + 28, w: d.w, h: d.h - 28 });
const both = [A, B];

// known by what it is, not by its index or place in the list
const p = placeOf({ x: 2020, y: 128, w: 800, h: 600 }, B);
assert.deepEqual(p, { d: displayKey(B), x: 100, y: 128, w: 800, h: 600, max: false });
assert.equal(findDisplay(p.d, [B, A]), B);
const moved = { ...B, x: -2560, id: 9 };                       // arranged on the other side, a new id
assert.deepEqual(restoreFrame(p, [A, moved], area, { main: A }).x, -2460);
assert.equal(findDisplay(displayKey(B), [{ ...B, connector: 'DP-2' }]).connector, 'DP-2');   // same monitor, another port
assert.equal(findDisplay(displayKey(B), [{ ...B, connector: 'DP-2' }, { ...B, connector: 'DP-3' }]), null);   // two alike: can't tell
assert.equal(findDisplay(displayKey(B), [{ ...A, connector: 'DP-1' }]), null);               // another monitor on that port

// the display is gone, or the place is off the screen: a visible place on one that exists
const gone = restoreFrame(p, [A], area, { main: A });
assert.ok(gone.x >= 0 && gone.x + gone.w <= 1920 && gone.y >= 28 && gone.y + gone.h <= 1080);
const off = restoreFrame({ ...p, x: 9000, y: 9000 }, both, area, { main: A });
assert.ok(off.x + off.w <= B.x + B.w && off.y + off.h <= B.h);
const big = restoreFrame({ ...p, w: 9000, h: 9000 }, [A], area, { main: A });
assert.ok(big.w <= 1920 && big.h <= 1052);

// the Dock's assignment chooses the display; a fixed app keeps its size; a twin doesn't land on the first
assert.ok(restoreFrame(p, both, area, { main: A, assigned: A }).x < 1920);
const fx = restoreFrame({ ...p, w: 1000, h: 900, max: true }, both, area, { main: A, fixed: true, size: [400, 300] });
assert.deepEqual([fx.w, fx.h, fx.maximized], [400, 300, false]);
assert.equal(restoreFrame(p, both, area, { main: A, shift: 30 }).x, 2050);

// maximized, snapped, lifted by the keyboard and fullscreen
assert.equal(placeOf({ x: 0, y: 0, w: 900, h: 700, maximized: true }, A).max, true);
assert.equal(placeOf({ x: 0, y: 0, w: 960, h: 1080, snapped: 'left', restore: { x: 50, y: 60, w: 500, h: 400 } }, A).w, 500);
assert.equal(placeOf({ x: 0, y: 0, w: 960, h: 300, oskFrame: { x: 5, y: 6, w: 500, h: 700, maximized: true } }, A).h, 700);
assert.equal(placeOf({ x: 0, y: 0, w: 960, h: 300, fullscreen: true }, A), null);
assert.equal(placeOf({ x: 0, y: 0, w: 200, h: 100, pip: {} }, A), null);

// settings are data: nothing odd gets through
for (const v of [null, 'x', {}, { d: 'a', x: 'y', y: 0, w: 100, h: 100 }, { d: 'a', x: 0, y: 0, w: 1, h: 100 }, { d: 'a', x: 0, y: 0, w: 1e9, h: 100 }]) assert.equal(cleanPlace(v), null);
assert.equal(restoreFrame({ d: 'a' }, both, area, { main: A }), null);

let all = {};
for (let i = 0; i < MAX_PLACES + 5; i++) all = putPlace(all, `a${i}`, p);
assert.equal(Object.keys(all).length, MAX_PLACES);
assert.ok(!all.a0 && all[`a${MAX_PLACES + 4}`]);
console.log('place-model-test: ok');
