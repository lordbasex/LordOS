// imageedit_model_test.mjs — the image editor's math (sdk/imageeditmodel.js, issue #30): aspect presets,
// crop rectangles from a drag, a handle or a move (always inside the picture, the ratio kept), view ↔
// image pixels, the new size with the proportions kept, the pixelate block, redaction ops and where a
// save goes (in place only for PNG and JPEG, copies next to the original, never over another file).
//
//   node tests/imageedit_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/imageeditmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, cond) => eq(name, !!cond, true);
const near = (a, b, e = 1e-6) => Math.abs(a - b) <= e;
const rnd = (r) => ({ x: Math.round(r.x * 1000) / 1000, y: Math.round(r.y * 1000) / 1000, w: Math.round(r.w * 1000) / 1000, h: Math.round(r.h * 1000) / 1000 });
const inside = (r, b) => r.x >= b.x - 1e-9 && r.y >= b.y - 1e-9 && r.x + r.w <= b.x + b.w + 1e-9 && r.y + r.h <= b.y + b.h + 1e-9;

// ---- presets ----
eq('free has no ratio', M.presetRatio('free', 1920, 1080), 0);
eq('original = the picture', M.presetRatio('original', 1920, 1080), 1920 / 1080);
eq('square', M.presetRatio('square', 1920, 1080), 1);
eq('16:9 on a landscape picture', M.presetRatio('16:9', 1920, 1080), 16 / 9);
eq('16:9 on a portrait picture is 9:16', M.presetRatio('16:9', 1080, 1920), 9 / 16);
eq('4:3 on a portrait picture is 3:4', M.presetRatio('4:3', 600, 800), 3 / 4);
eq('no size, no ratio', M.presetRatio('square', 0, 10), 0);
eq('every preset listed', M.CROP_PRESETS, ['free', 'original', 'square', '16:9', '4:3']);

// ---- fitting a ratio ----
const B = { x: 0, y: 0, w: 1000, h: 500 };
eq('square in a wide picture: centered, full height', M.fitRatio(B, 1), { x: 250, y: 0, w: 500, h: 500 });
eq('16:9 in a 2:1 picture', rnd(M.fitRatio(B, 16 / 9)), { x: 55.556, y: 0, w: 888.889, h: 500 });
eq('free: all of it', M.fitRatio(B, 0), B);
eq('3:4 in a wide box', rnd(M.fitRatio({ x: 10, y: 20, w: 400, h: 300 }, 3 / 4)), { x: 97.5, y: 20, w: 225, h: 300 });

// ---- dragging a new crop ----
eq('free drag', M.dragRect(100, 100, 300, 250, 0, B), { x: 100, y: 100, w: 200, h: 150 });
eq('free drag up and left', M.dragRect(300, 250, 100, 100, 0, B), { x: 100, y: 100, w: 200, h: 150 });
eq('free drag past the edge stops at it', M.dragRect(900, 400, 1300, 900, 0, B), { x: 900, y: 400, w: 100, h: 100 });
const sq = M.dragRect(100, 100, 300, 180, 1, B);
eq('square drag: the larger side leads', sq, { x: 100, y: 100, w: 200, h: 200 });
const wide = M.dragRect(800, 100, 1200, 400, 16 / 9, B);
ok('16:9 drag past the edge keeps the ratio', near(wide.w / wide.h, 16 / 9) && inside(wide, B) && near(wide.x, 800) && near(wide.x + wide.w, 1000));
const anchorOut = M.dragRect(-50, -50, 200, 200, 0, B);
eq('an anchor outside is brought in', anchorOut, { x: 0, y: 0, w: 200, h: 200 });
// fuzz: any drag, any preset, always inside and with its ratio
let fuzzBad = 0;
let seed = 7;
const r01 = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
for (let i = 0; i < 2000; i++) {
  const ratio = [0, 1, 16 / 9, 4 / 3, 9 / 16, 1.5][i % 6];
  const r = M.dragRect(r01() * 1200 - 100, r01() * 700 - 100, r01() * 1400 - 200, r01() * 900 - 200, ratio, B);
  if (!inside(r, B) || r.w < 0 || r.h < 0 || (ratio && r.h > 1e-6 && !near(r.w / r.h, ratio, 1e-6))) fuzzBad++;
}
eq('2000 random drags: inside and with the ratio', fuzzBad, 0);

// ---- handles ----
const R = { x: 100, y: 100, w: 400, h: 200 };
eq('corner nw', M.handleAt(R, 102, 98, 10), 'nw');
eq('corner se', M.handleAt(R, 505, 305, 10), 'se');
eq('edge n (free)', M.handleAt(R, 300, 101, 10), 'n');
eq('edge e (free)', M.handleAt(R, 499, 200, 10), 'e');
eq('no edges with a ratio: inside is move', M.handleAt(R, 300, 101, 10, 2), 'move');
eq('inside: move', M.handleAt(R, 300, 200, 10), 'move');
eq('outside: nothing', M.handleAt(R, 700, 200, 10), null);
eq('no rect', M.handleAt(null, 1, 1), null);
eq('drag se corner (free)', M.resizeRect(R, 'se', 600, 400, 0, B), { x: 100, y: 100, w: 500, h: 300 });
eq('drag nw corner (free)', M.resizeRect(R, 'nw', 50, 60, 0, B), { x: 50, y: 60, w: 450, h: 240 });
const kept = M.resizeRect(R, 'se', 700, 320, 2, B);
ok('drag a corner with a ratio keeps it and the opposite corner', near(kept.w / kept.h, 2) && kept.x === 100 && kept.y === 100);
eq('drag the n edge', M.resizeRect(R, 'n', 999, 40, 0, B), { x: 100, y: 40, w: 400, h: 260 });
eq('the n edge never passes the s edge', M.resizeRect(R, 'n', 0, 900, 0, B).h, 1);
eq('drag the w edge out of the picture stops at it', M.resizeRect(R, 'w', -80, 0, 0, B), { x: 0, y: 100, w: 500, h: 200 });
eq('move', M.moveRect(R, 50, -20, B), { x: 150, y: 80, w: 400, h: 200 });
eq('move stops at the edges', M.moveRect(R, 900, 900, B), { x: 600, y: 300, w: 400, h: 200 });

// ---- view ↔ image ----
const v = M.fitView(2000, 1000, 1000, 800);       // drawn at half size, centered vertically
eq('fit view', v, { ox: 0, oy: 150, s: 0.5 });
eq('pointer → image', M.toImagePoint(500, 400, v), { x: 1000, y: 500 });
eq('image rect → view', M.toViewRect({ x: 200, y: 100, w: 400, h: 200 }, v), { x: 100, y: 200, w: 200, h: 100 });
eq('image rect: whole pixels', M.imageRect({ x: 10.4, y: 9.6, w: 100.3, h: 50.2 }, 2000, 1000), { x: 10, y: 10, w: 101, h: 50 });
eq('image rect: clamped', M.imageRect({ x: -20, y: 990, w: 100, h: 40 }, 2000, 1000), { x: 0, y: 990, w: 80, h: 10 });
eq('image rect: under a pixel is nothing', M.imageRect({ x: 5, y: 5, w: 0.2, h: 10 }, 100, 100), null);
eq('image rect: NaN is nothing', M.imageRect({ x: NaN, y: 0, w: 5, h: 5 }, 100, 100), null);
eq('image rect: negative size flips', M.imageRect({ x: 50, y: 50, w: -10, h: -20 }, 100, 100), { x: 40, y: 30, w: 10, h: 20 });

// ---- size ----
eq('side: a number', M.parseSide(' 1280 '), 1280);
eq('side: zero refused', M.parseSide('0'), null);
eq('side: over the limit refused', M.parseSide('9000'), null);
eq('side: not a whole number', M.parseSide('12.5'), null);
eq('side: text', M.parseSide('abc'), null);
eq('side: negative', M.parseSide('-4'), null);
eq('keep ratio: width typed', M.resizeSize(1920, 1080, 'width', 1280, true), { width: 1280, height: 720 });
eq('keep ratio: height typed', M.resizeSize(1920, 1080, 'height', 540, true), { width: 960, height: 540 });
eq('keep ratio: never under 1', M.resizeSize(4000, 10, 'width', 100, true), { width: 100, height: 1 });
eq('keep ratio: the other side over the limit lowers both', M.resizeSize(100, 1000, 'width', 1000, true), { width: 819, height: 8192 });
eq('free: only the typed side', M.resizeSize(1920, 1080, 'width', 500, false), { width: 500, height: 1080 });

// ---- redaction ----
eq('pixelate block: about 1/60 of the larger side', M.pixelBlock(2880, 1800), 48);
eq('pixelate block: at least 8', M.pixelBlock(200, 100), 8);
eq('pixelate block: at most 200', M.pixelBlock(8192 * 2, 10), 200);
eq('fill op (corners in order)', M.redactOp('fill', 50, 80, 10, 20, 100, 100, '#000000'), { type: 'fill', x1: 10, y1: 20, x2: 50, y2: 80, color: '#000000' });
eq('pixelate op carries its block', M.redactOp('pixelate', 0, 0, 10, 10, 1200, 600).size, 20);
eq('an unknown redaction is a fill', M.redactOp('weird', 0, 0, 1, 1, 10, 10).type, 'fill');

// ---- saving ----
const png = M.savePlan('/home/u/Pictures/Screenshots/Screenshot 1.png', 'edited', 42);
eq('PNG: saved in place', png.replace, true);
eq('PNG: format', png.format, 'png');
eq('PNG: copy name', png.copy(), '/home/u/Pictures/Screenshots/Screenshot 1 (edited).png');
eq('PNG: second copy', png.copy(2), '/home/u/Pictures/Screenshots/Screenshot 1 (edited 2).png');
eq('PNG: hidden temporary file next to it', png.temp, '/home/u/Pictures/Screenshots/.Screenshot 1.lordos-edit-42.png');
const jpg = M.savePlan('/home/u/a.b.JPG', 'edited', 1);
eq('JPEG: in place, as JPEG', [jpg.replace, jpg.format, jpg.copy()], [true, 'jpeg', '/home/u/a.b (edited).jpg']);
eq('.jpeg keeps its extension', M.savePlan('/x/p.jpeg', 'e').copy(), '/x/p (e).jpeg');
const webp = M.savePlan('/home/u/pic.webp', 'edited');
eq('WebP: only as a PNG copy', [webp.replace, webp.format, webp.copy()], [false, 'png', '/home/u/pic (edited).png']);
eq('a capture never saved: no file to replace', M.savePlan(null, 'edited').replace, false);
const taken = new Set([png.copy(1), png.copy(2)]);
eq('free copy skips names that exist', M.freeCopy(png, (p) => taken.has(p)), png.copy(3));
eq('no free copy within the bound', M.freeCopy(png, () => true, 5), null);

// ---- which files open in the editor ----
for (const f of ['a.png', 'b.JPG', 'c.jpeg', 'd.webp', 'e.bmp', 'f.gif', 'g.tif', 'h.TIFF', 'i.heic', 'j.avif']) ok(`opens ${f}`, M.canMarkup(f));
for (const f of ['a.svg', 'b.pdf', 'c.mp4', 'd.png.txt', '', null]) ok(`does not open ${f}`, !M.canMarkup(f));

console.log(`imageedit-model-test: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
