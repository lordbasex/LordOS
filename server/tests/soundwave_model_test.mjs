// node server/tests/soundwave_model_test.mjs — the sound wave widget's choices (sdk/soundwavemodel.js) and the
// bottom-centre pin of any widget (sdk/widgetgrid.js pinnedRect).
import assert from 'node:assert/strict';
import { sourceOf, appsPlaying, resolve, colorsOf, gainOf, PALETTES } from '../../sdk/soundwavemodel.js';
import { pinnedRect, MARGIN } from '../../sdk/widgetgrid.js';

// the source: the desktop unless the options say otherwise; nonsense falls back to the desktop
assert.deepEqual(sourceOf(undefined), { kind: 'desktop' });
assert.deepEqual(sourceOf({ source: { kind: 'mic' } }), { kind: 'mic' });
assert.deepEqual(sourceOf({ source: { kind: 'app', app: 'chromium', label: 'Chromium' } }), { kind: 'app', app: 'chromium', label: 'Chromium' });
assert.deepEqual(sourceOf({ source: { kind: 'app' } }), { kind: 'desktop' });
assert.deepEqual(sourceOf({ source: { kind: 'rm -rf' } }), { kind: 'desktop' });

const st = { inputs: [
  { index: 40, app: 'Chromium', appId: '', binary: 'chromium', corked: true },
  { index: 52, app: 'Chromium', appId: '', binary: 'chromium', corked: false },
  { index: 47, app: 'Videos', appId: 'org.lordos.Videos', binary: 'lord-media', corked: false },
] };
// one entry per app
assert.deepEqual(appsPlaying(st), [{ key: 'chromium', label: 'Chromium' }, { key: 'org.lordos.Videos', label: 'Videos' }]);
assert.deepEqual(appsPlaying(null), []);
// the desktop and the microphone are the default output and input
assert.deepEqual(resolve(st, { kind: 'desktop' }), ['sink', -1]);
assert.deepEqual(resolve(st, { kind: 'mic' }), ['source', -1]);
// an app: its stream that is playing (not the paused tab), the newest if several
assert.deepEqual(resolve(st, { kind: 'app', app: 'chromium' }), ['input', 52]);
assert.deepEqual(resolve({ inputs: [st.inputs[0]] }, { kind: 'app', app: 'chromium' }), ['input', 40]);
// not playing now: nothing is listened to
assert.equal(resolve(st, { kind: 'app', app: 'spotify' }), null);

// colours: three numbers, the accent's own in the middle; an unknown palette is neon
assert.deepEqual(colorsOf('fire'), PALETTES.fire);
assert.deepEqual(colorsOf('nope'), PALETTES.neon);
assert.equal(colorsOf('accent', '#7C5CFF')[1], 0x7c5cff);
assert.deepEqual(colorsOf('accent', 'red'), PALETTES.neon);
for (const c of colorsOf('accent', '#ff0000')) assert.ok(Number.isInteger(c) && c >= 0 && c <= 0xffffff);
assert.equal(gainOf('low'), 0.6);
assert.equal(gainOf(undefined), 1);
assert.equal(gainOf('high'), 1.7);

// pinned at the bottom centre of a display's work area (DP-3 at x 3840 on three 1920 × 1080 displays, a top bar)
const area = { x: 3840, y: 32, w: 1920, h: 1048 };
const r = pinnedRect(area, 'w', 'bottom-center');
assert.deepEqual(r, { x: 3840 + (1920 - 728) / 2, y: 32 + 1048 - MARGIN - 170, w: 728, h: 170 });
assert.equal(pinnedRect(area, 'w', 'top-left'), null);
console.log('soundwave-model-test: ok');
