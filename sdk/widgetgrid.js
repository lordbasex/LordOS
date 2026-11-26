// widgetgrid.js — the desktop widgets' grid (0.8.8.10, Workspaces), plain functions with no imports
// (server/tests/widget_grid_test.mjs). Each display's work area holds a grid of square cells, anchored to its top right
// like TangoOS's; a widget takes a block of cells by its size and keeps them: no other widget can be placed or dropped
// over them.
//
//   SIZES                    s 1×1, m 2×1, l 2×2, w 4×1 (wide), xl 4×2 (cells: columns × rows)
//   gridOf(area)             { cols, rows, x, y, cell, gap } for a work area
//   rectOf(g, col, row, size)   the pixels of a block
//   occupancy(list, except)  the cells the widgets in `list` hold (a Set of "col,row")
//   fits(g, occ, col, row, size)   whether a block is inside the grid and free
//   firstFree(g, list, size) the first free block, from the top right (rows down, columns leftwards)
//   nearestFree(g, list, size, col, row, except)   the free block nearest to (col, row)
//   cellAt(g, x, y)          the cell whose top left is nearest to a point (where a dragged widget's corner is)
//   PINS / pinnedRect(area, size, pin)   a widget pinned to a place of its display instead of the grid: 'bottom-center'
//                          (centred, along the bottom of the work area). It holds no cells: it may sit over others
export const CELL = 170;
export const GAP = 16;
export const MARGIN = 24;
export const SIZES = { s: [1, 1], m: [2, 1], l: [2, 2], w: [4, 1], xl: [4, 2] };
export const SIZE_ORDER = ['s', 'm', 'l', 'w', 'xl'];
export const spanOf = (size) => SIZES[size] || SIZES.s;

export function gridOf(area, cell = CELL, gap = GAP, margin = MARGIN) {
  const cols = Math.max(1, Math.floor((area.w - 2 * margin + gap) / (cell + gap)));
  const rows = Math.max(1, Math.floor((area.h - 2 * margin + gap) / (cell + gap)));
  const w = cols * cell + (cols - 1) * gap;
  return { cols, rows, x: Math.round(area.x + area.w - margin - w), y: Math.round(area.y + margin), cell, gap };
}

export function rectOf(g, col, row, size) {
  const [c, r] = spanOf(size);
  return { x: g.x + col * (g.cell + g.gap), y: g.y + row * (g.cell + g.gap), w: c * g.cell + (c - 1) * g.gap, h: r * g.cell + (r - 1) * g.gap };
}

export function occupancy(list, except) {
  const occ = new Set();
  for (const w of list || []) {
    if (!w || w.id === except || !Number.isInteger(w.col) || !Number.isInteger(w.row)) continue;
    const [c, r] = spanOf(w.size);
    for (let i = 0; i < c; i++) for (let j = 0; j < r; j++) occ.add(`${w.col + i},${w.row + j}`);
  }
  return occ;
}

export function fits(g, occ, col, row, size) {
  const [c, r] = spanOf(size);
  if (!Number.isInteger(col) || !Number.isInteger(row) || col < 0 || row < 0 || col + c > g.cols || row + r > g.rows) return false;
  for (let i = 0; i < c; i++) for (let j = 0; j < r; j++) if (occ.has(`${col + i},${row + j}`)) return false;
  return true;
}

export function firstFree(g, list, size, except) {
  const occ = occupancy(list, except);
  const [c] = spanOf(size);
  for (let row = 0; row < g.rows; row++) for (let col = g.cols - c; col >= 0; col--) if (fits(g, occ, col, row, size)) return { col, row };
  return null;
}

export function nearestFree(g, list, size, col, row, except) {
  const occ = occupancy(list, except);
  let best = null, bd = Infinity;
  for (let r = 0; r < g.rows; r++) for (let c = 0; c < g.cols; c++) {
    if (!fits(g, occ, c, r, size)) continue;
    const d = (c - col) ** 2 + (r - row) ** 2;
    if (d < bd) { bd = d; best = { col: c, row: r }; }
  }
  return best;
}

export function cellAt(g, x, y) {
  const step = g.cell + g.gap;
  return { col: Math.max(0, Math.min(g.cols - 1, Math.round((x - g.x) / step))), row: Math.max(0, Math.min(g.rows - 1, Math.round((y - g.y) / step))) };
}

export const PINS = ['bottom-center'];
export function pinnedRect(area, size, pin, margin = MARGIN) {
  if (!PINS.includes(pin)) return null;
  const [c, r] = spanOf(size);
  const w = c * CELL + (c - 1) * GAP, h = r * CELL + (r - 1) * GAP;
  return { x: Math.round(area.x + (area.w - w) / 2), y: Math.round(area.y + area.h - margin - h), w, h };
}
