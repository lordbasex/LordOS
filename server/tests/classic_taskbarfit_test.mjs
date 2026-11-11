// classic_taskbarfit_test.mjs — themes/classic/taskbarfit.js (GitHub #111): with many apps on Lord Classic's taskbar
// the app buttons shrink, then the rest move into "more apps", and the row never reaches the tray; centered, the
// row stays in the middle of the display while there is room.
//
//   node tests/classic_taskbarfit_test.mjs        (from server/; run_suites.sh runs it when node exists)
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { taskbarFit, rowLength, BTN, MIN, PAD } = await import(path.join(here, '../../themes/classic/taskbarfit.js'));

const end = (f, len) => PAD + f.spacer + rowLength(f.shown, f.size, f.more);   // where the row ends

// a few apps: full buttons, all shown, centered on the display
let f = taskbarFit({ length: 1920, tray: 260, count: 6, center: true });
assert.deepEqual([f.size, f.shown, f.more], [BTN, 6, false]);
assert.equal(Math.abs(PAD + f.spacer + rowLength(6, BTN) / 2 - 960) <= 1, true);
assert.equal(taskbarFit({ length: 1920, tray: 260, count: 6, center: false }).spacer, 0);

// for every count up to 120 apps, both alignments and a few display sizes: the row ends before the tray, every
// app is either shown or in "more apps", and a button is never smaller than MIN
for (const length of [800, 1024, 1366, 1920, 2560]) {
  for (const tray of [180, 260, 420]) {
    for (const center of [true, false]) {
      for (let count = 0; count <= 120; count++) {
        const g = taskbarFit({ length, tray, count, center });
        const at = `${length}/${tray}/${center}/${count}`;
        assert.ok(end(g) <= length - PAD - tray, `row over the tray at ${at}`);
        assert.ok(g.size >= MIN && g.size <= BTN, `size ${g.size} at ${at}`);
        assert.equal(g.more, g.shown < count, `more apps at ${at}`);
        assert.ok(g.spacer >= 0, `spacer at ${at}`);
        if (!center) assert.equal(g.spacer, 0);
      }
    }
  }
}

// the report: a 1366 px display, a centered bar and 40 apps: smaller buttons first, then "more apps"
f = taskbarFit({ length: 1366, tray: 300, count: 24, center: true });
assert.ok(f.size < BTN && f.size >= MIN && !f.more, JSON.stringify(f));
f = taskbarFit({ length: 1366, tray: 300, count: 40, center: true });
assert.deepEqual([f.size, f.more], [MIN, true]);
assert.ok(f.shown > 20 && f.shown < 40, JSON.stringify(f));
// the row fills the room: one more button would not fit
assert.ok(PAD + rowLength(f.shown + 1, MIN, true) > 1366 - PAD - 300);
// centered but full: it moves toward the start so the tray keeps its place
assert.equal(f.spacer, 1366 - 2 * PAD - 300 - rowLength(f.shown, MIN, true));

// a bar on the side: its length is the display's height
f = taskbarFit({ length: 1080, tray: 150, count: 30, center: true });
assert.ok(end(f) <= 1080 - PAD - 150);

console.log('classic-taskbarfit: ok');
