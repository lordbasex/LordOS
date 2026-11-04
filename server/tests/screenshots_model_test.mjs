// screenshots_model_test.mjs — Screenshots' selection → captured image math (apps/screenshots/geometry.js):
// a window or area picked in logical pixels lands whole in a capture taken at the display's
// physical resolution (issue #19: at 200 % the window came out cropped).
//
//   node tests/screenshots_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { captureView, captureRect } = await import(path.join(here, '../../apps/screenshots/geometry.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const one = (scale, w, h, pw, ph) => ({ width: w, height: h, displays: [{ id: 1, x: 0, y: 0, w, h, pw, ph, scale, main: true }] });

// ---- 100 %: unchanged ----
const s100 = one(100, 1920, 1080, 1920, 1080);
eq('100 %: window as is', captureRect({ x: 100, y: 50, w: 800, h: 600 }, 1920, 1080, s100), { x: 100, y: 50, w: 800, h: 600 });

// ---- 200 % (the report: 1440 × 900 logical, 2880 × 1800 image) ----
const s200 = one(200, 1440, 900, 2880, 1800);
eq('200 %: view', captureView(2880, 1800, s200), { x: 0, y: 0, k: 2 });
eq('200 %: maximized window below the menu bar, whole', captureRect({ x: 0, y: 28, w: 1440, h: 872 }, 2880, 1800, s200), { x: 0, y: 56, w: 2880, h: 1744 });
eq('200 %: window in the middle', captureRect({ x: 300, y: 120, w: 640, h: 480 }, 2880, 1800, s200), { x: 600, y: 240, w: 1280, h: 960 });
eq('200 %: half pixels rounded outwards', captureRect({ x: 10.5, y: 20.25, w: 100, h: 50 }, 2880, 1800, s200), { x: 21, y: 40, w: 200, h: 101 });

// ---- 175 %: the image is 2880 wide, the logical width rounds to 1646 ----
const s175 = one(175, 1646, 1029, 2880, 1800);
eq('175 %: whole screen stays inside the image', captureRect({ x: 0, y: 0, w: 1646, h: 1029 }, 2880, 1800, s175), { x: 0, y: 0, w: 2880, h: 1800 });
eq('175 %: window', captureRect({ x: 100, y: 100, w: 400, h: 300 }, 2880, 1800, s175), { x: 175, y: 175, w: 700, h: 525 });

// ---- clamping ----
eq('partly off screen: clamped', captureRect({ x: -50, y: 800, w: 200, h: 300 }, 2880, 1800, s200), { x: 0, y: 1600, w: 300, h: 200 });
eq('fully off screen: nothing', captureRect({ x: 1500, y: 0, w: 100, h: 100 }, 2880, 1800, s200), null);
eq('no rect', captureRect(null, 2880, 1800, s200), null);

// ---- several displays ----
// main display on the right of a 100 % one: the capture is the main display only
const two = { width: 1280 + 1440, height: 900, displays: [
  { id: 1, x: 0, y: 0, w: 1280, h: 800, pw: 1280, ph: 800, scale: 100, main: false },
  { id: 2, x: 1280, y: 0, w: 1440, h: 900, pw: 2880, ph: 1800, scale: 200, main: true },
] };
eq('main display at an offset', captureRect({ x: 1380, y: 28, w: 500, h: 400 }, 2880, 1800, two), { x: 200, y: 56, w: 1000, h: 800 });
// "all displays": the whole logical space at 100 % (the image does not match the main display)
eq('all displays: 1:1 in the global space', captureRect({ x: 1380, y: 28, w: 500, h: 400 }, 2720, 900, two), { x: 1380, y: 28, w: 500, h: 400 });

// ---- no display list (headless, older server) ----
eq('no displays: from the screen width', captureRect({ x: 10, y: 10, w: 100, h: 100 }, 1600, 1000, { width: 800, height: 500 }), { x: 20, y: 20, w: 200, h: 200 });

console.log(`${n - bad}/${n} screenshots geometry checks passed`);
process.exit(bad ? 1 : 0);
