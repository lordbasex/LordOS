// widget_grid_test.mjs — the desktop widgets' grid (sdk/widgetgrid.js): cells per work area, blocks by size, a widget's
// cells reserved, the first free block from the top right and the nearest free one to a drop.
// node server/tests/widget_grid_test.mjs
import { gridOf, rectOf, occupancy, fits, firstFree, nearestFree, cellAt, SIZES, CELL, GAP, MARGIN } from '../../sdk/widgetgrid.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const area = { x: 1920, y: 52, w: 1856, h: 996 };
const g = gridOf(area);
eq('grid: columns and rows of a 1856×996 area', [g.cols, g.rows], [Math.floor((1856 - 2 * MARGIN + GAP) / (CELL + GAP)), Math.floor((996 - 2 * MARGIN + GAP) / (CELL + GAP))]);
eq('grid: anchored to the top right', g.x + g.cols * CELL + (g.cols - 1) * GAP, area.x + area.w - MARGIN);
eq('sizes: Apple-like blocks', SIZES, { s: [1, 1], m: [2, 1], l: [2, 2], xl: [4, 2] });
eq('rect: a large widget is two cells and a gap each way', rectOf(g, 0, 0, 'l'), { x: g.x, y: g.y, w: 2 * CELL + GAP, h: 2 * CELL + GAP });
const list = [{ id: 'a', size: 'l', col: g.cols - 2, row: 0 }];
const occ = occupancy(list);
eq('occupancy: a large widget holds four cells', occ.size, 4);
eq('fits: not over a widget', fits(g, occ, g.cols - 1, 1, 's'), false);
eq('fits: not past the edge', fits(g, occ, g.cols - 1, 3, 'm'), false);
eq('fits: a free block', fits(g, occ, 0, 0, 'xl'), true);
eq('firstFree: the first block from the top right, beside the one there', firstFree(g, list, 's'), { col: g.cols - 3, row: 0 });
eq('firstFree: the widget itself does not block its own move', firstFree(g, list, 'l', 'a'), { col: g.cols - 2, row: 0 });
eq('nearestFree: dropped over another widget, a nearest free block (two cells away)', nearestFree(g, list, 's', g.cols - 1, 0), { col: g.cols - 3, row: 0 });
eq('nearestFree: none when the grid is full', nearestFree({ ...g, cols: 2, rows: 2 }, [{ id: 'x', size: 'l', col: 0, row: 0 }], 's', 0, 0), null);
eq('cellAt: the nearest cell corner', cellAt(g, g.x + CELL + GAP + 20, g.y + 5), { col: 1, row: 0 });
eq('cellAt: clamped inside', cellAt(g, g.x - 900, g.y - 900), { col: 0, row: 0 });

console.log(bad ? `widget-grid-test: ${bad} FAILED` : `widget-grid-test: ${ok} ok`);
if (bad) process.exit(1);
