// hwkeys_test.mjs — the steps of a keyboard's volume, brightness and keyboard-light keys.
//   node tests/hwkeys_test.mjs
import assert from 'node:assert/strict';
import { nextVolume, nextBacklight, nextKeyLight, levelFraction, litSegments, speakerWaves, hudOpacity, LEVEL_HUD_MS, LEVEL_HUD_FADE } from '../sdk/hwkeymodel.js';

assert.equal(nextVolume(0.5, 1), 0.55);
assert.equal(nextVolume(0.5, -1), 0.45);
assert.equal(nextVolume(0.52, 1), 0.55);       // lands on a step
assert.equal(nextVolume(0.52, -1), 0.5);
assert.equal(nextVolume(0.57999, 1), 0.6);
assert.equal(nextVolume(1, 1), 1);             // the key stops at 100 %
assert.equal(nextVolume(0, -1), 0);
assert.equal(nextVolume(1.3, 1), 1.3);         // above 100 % from Settings: not cut
assert.equal(nextVolume(1.3, -1), 1.25);
let v = 1; for (let i = 0; i < 20; i++) v = nextVolume(v, -1);
assert.equal(v, 0);
for (let i = 0; i < 20; i++) v = nextVolume(v, 1);
assert.equal(v, 1);

assert.equal(nextBacklight(800, 1600, 1), 900);
assert.equal(nextBacklight(800, 1600, -1), 700);
assert.equal(nextBacklight(1600, 1600, 1), 1600);
assert.equal(nextBacklight(50, 1600, -1), 1);  // never fully off
assert.equal(nextBacklight(3, 7, 1), 4);       // few levels: one at a time

assert.equal(nextKeyLight(0, 255, 1), 32);
assert.equal(nextKeyLight(32, 255, -1), 0);    // off
assert.equal(nextKeyLight(250, 255, 1), 255);
assert.equal(nextKeyLight(0, 2, 1), 1);

// the level indicator
assert.equal(levelFraction(0.5), 0.5);
assert.equal(levelFraction(1.3), 1);           // above 100 %: a full bar
assert.equal(levelFraction(800, 1600), 0.5);
assert.equal(levelFraction(5, 0), 0);
assert.equal(levelFraction(NaN), 0);
assert.equal(litSegments(0), 0);
assert.equal(litSegments(1), 16);
assert.equal(litSegments(0.5), 8);
assert.equal(litSegments(0.01), 1);            // on, however low: one segment
assert.equal(litSegments(0.99), 15);           // only a full level fills the bar
assert.equal(litSegments(1 / 1600), 1);        // the screen's lowest step
assert.equal(speakerWaves(0, false), 0);
assert.equal(speakerWaves(0.2, false), 1);
assert.equal(speakerWaves(0.5, false), 2);
assert.equal(speakerWaves(1, false), 3);
assert.equal(speakerWaves(1, true), 0);
assert.equal(hudOpacity(0, false), 1);
assert.equal(hudOpacity(LEVEL_HUD_MS - 1, false), 1);
assert.equal(hudOpacity(LEVEL_HUD_MS + LEVEL_HUD_FADE / 2, false), 0.5);
assert.equal(hudOpacity(LEVEL_HUD_MS + LEVEL_HUD_FADE, false), 0);
assert.equal(hudOpacity(LEVEL_HUD_MS, true), 0);   // Reduce motion: no fade

console.log('hwkeys: ok');
