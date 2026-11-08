// screenshots_model_test.mjs — Screenshots' selection → captured image math (apps/screenshots/geometry.js):
// a window or area picked in logical pixels lands whole in a capture taken at the display's
// physical resolution (issue #19: at 200 % the window came out cropped).
//
//   node tests/screenshots_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { captureView, captureRect, displaysToCapture, displayAt, dragRect, capturePixel, magnify, middleColor, LOUPE_PX, LOUPE_ZOOM } = await import(path.join(here, '../../apps/screenshots/geometry.js'));

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

// ---- several displays (0.8.8.6: captures were of the main display only) ----
const three = { width: 5760, height: 1080, mirror: false, displays: [
  { id: 94, x: 1920, y: 0, w: 1920, h: 1080, pw: 1920, ph: 1080, scale: 100, main: true },
  { id: 83, x: 0, y: 0, w: 1920, h: 1080, pw: 1920, ph: 1080, scale: 100 },
  { id: 88, x: 3840, y: 0, w: 1920, h: 1080, pw: 3840, ph: 2160, scale: 200 }] };
eq('three displays: each captured', displaysToCapture(three).map((d) => d.id), [94, 83, 88]);
eq('mirrored: Settings\' choice', displaysToCapture({ ...three, mirror: true }), [null]);
eq('one display: Settings\' choice', displaysToCapture(one(100, 1920, 1080, 1920, 1080)), [null]);
eq('the point on the left display', displayAt(three.displays, 100, 100).id, 83);
eq('the point on the right display', displayAt(three.displays, 4000, 10).id, 88);
eq('outside every display', displayAt(three.displays, 9000, 10), null);
eq('a selection on the left display lands in its capture', captureRect({ x: 100, y: 50, w: 200, h: 100 }, 1920, 1080, three, three.displays[1]), { x: 100, y: 50, w: 200, h: 100 });
eq('a selection on the 200 % right display, at its resolution', captureRect({ x: 3940, y: 50, w: 200, h: 100 }, 3840, 2160, three, three.displays[2]), { x: 200, y: 100, w: 400, h: 200 });
eq('a drag stays inside the display it began on', dragRect({ x: 1800, y: 100 }, 2500, 300, three.displays[1]), { x: 1800, y: 100, w: 120, h: 200 });
eq('a drag up and left', dragRect({ x: 500, y: 500 }, 100, 50, three.displays[1]), { x: 100, y: 50, w: 400, h: 450 });

// ---- the magnifier and the color picker (0.8.8.6) ----
eq('the capture pixel on a 200 % display', capturePixel(3940.5, 10.7, 3840, 2160, three, three.displays[2]), { x: 201, y: 21 });
eq('the capture pixel at 100 %', capturePixel(100.2, 50.9, 1920, 1080, three, three.displays[1]), { x: 100, y: 50 });
const block = new Uint8Array(LOUPE_PX * LOUPE_PX * 4);
const mid = ((LOUPE_PX >> 1) * LOUPE_PX + (LOUPE_PX >> 1)) * 4;
block.set([0x1e, 0x90, 0xff, 255], mid);
eq('the middle pixel\'s color', middleColor(block), { hex: '#1E90FF', r: 30, g: 144, b: 255 });
const big = magnify(block);
eq('the magnifier is the block enlarged', big.length, (LOUPE_PX * LOUPE_ZOOM) ** 2 * 4);
const at = (x, y) => Array.from(big.slice((y * LOUPE_PX * LOUPE_ZOOM + x) * 4, (y * LOUPE_PX * LOUPE_ZOOM + x) * 4 + 4));
const c = (LOUPE_PX >> 1) * LOUPE_ZOOM;
eq('inside the middle pixel: its color', at(c + 3, c + 3), [0x1e, 0x90, 0xff, 255]);
eq('the middle pixel is outlined', at(c, c + 3), [255, 255, 255, 255]);
eq('outside the capture: dark, opaque', at(1, 1), [0, 0, 0, 255]);

// ---- copy the text (OCR) ----
const { ocrLangs, cleanText } = await import(path.join(here, '../../apps/screenshots/ocr.js'));
eq('the interface languages first, then the shipped ones', ocrLangs(['pt-BR']), 'por+eng+spa+fra+ita');
eq('each once', ocrLangs(['es', 'en', 'es']), 'spa+eng+por+fra+ita');
eq('another installed language goes first', ocrLangs(['de']), 'deu+eng+spa+por+fra+ita');
eq('an unknown language is left out', ocrLangs(['ja']), 'eng+spa+por+fra+ita');
eq('no languages: the shipped ones', ocrLangs(undefined), 'eng+spa+por+fra+ita');
eq('cleaned text', cleanText('ls -la  \n\n\n\ntotal 4\f\n'), 'ls -la\n\ntotal 4');

console.log(`${n - bad}/${n} screenshots geometry checks passed`);
process.exit(bad ? 1 : 0);
