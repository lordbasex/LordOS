// timerwidgetmodel.js — how the Timer desktop widget (sdk/deskwidgets.jsx TimerBody) fits its card, plain functions
// with no imports (server/tests/timer_widget_test.mjs). Every size and every language: nothing wraps and nothing leaves
// the card. The widget gives it its card's pixels, the texts it will show and a measure (lord.measureText); the model
// picks the ring's size, the fonts and each pill's width, and gives every part's box inside the card's content box so
// the test can check them.
//
//   contentOf(w, h)          the card's content box (inside its padding)
//   fitRow(labels, room, measure, drop)   a row of pills inside `room`: the largest style that keeps every label whole;
//                          else the smallest one with the pills in `drop` (indices, first dropped first) left out until
//                          the rest fit; else the widest ones cut with an ellipsis. { style, keep, widths, width }
//   timerLayout(o)          { content, cols, timer: { ring, ringFont, row, boxes }, stopwatch: { figure, row, boxes } }
//   idleSeconds(tm)          what an idle timer shows and starts when its ring is tapped: the last duration, else 5 min
export const PAD = { top: 13, bottom: 14, x: 15 };
export const COL_GAP = 12;                // each side of the 1 px rule between the timer and the stopwatch
export const PRESETS = [1, 5, 25];        // minutes
export const PRESET_DROP = [0, 2];        // when the three don't fit: 1 min goes first, then 25 min (5 min stays)
export const DEFAULT_SECONDS = 300;
export const STYLES = [
  { font: 13, pad: 12, h: 30, gap: 6 },
  { font: 12, pad: 9, h: 26, gap: 4 },
  { font: 11, pad: 7, h: 24, gap: 4 },
  { font: 10.5, pad: 5, h: 22, gap: 4 },
];
const LINE = 1.3;                        // a line of text is its font size × LINE high
const RING_GAP = 8, SW_GAP = 6, SUB_FONT = 11.5;

export const contentOf = (w, h) => ({ x: PAD.x, y: PAD.top, w: Math.max(0, w - 2 * PAD.x), h: Math.max(0, h - PAD.top - PAD.bottom) });
export const idleSeconds = (tm) => (tm && tm.duration > 0 ? Math.round(tm.duration / 1000) : DEFAULT_SECONDS);

const widthsOf = (labels, s, measure) => labels.map((l) => Math.ceil(measure(String(l), s.font, 600)) + 2 * s.pad);
const sum = (a) => a.reduce((x, y) => x + y, 0);

export function fitRow(labels, room, measure, drop = []) {
  for (const s of STYLES) {
    const widths = widthsOf(labels, s, measure);
    const width = sum(widths) + s.gap * (labels.length - 1);
    if (width <= room) return { style: s, keep: labels.map((_, i) => i), widths, width };
  }
  const s = STYLES[STYLES.length - 1];
  let keep = labels.map((_, i) => i);
  for (const d of drop) {
    const ws = widthsOf(keep.map((i) => labels[i]), s, measure);
    if (sum(ws) + s.gap * (keep.length - 1) <= room) break;
    if (keep.length > 1) keep = keep.filter((i) => i !== d);
  }
  let widths = widthsOf(keep.map((i) => labels[i]), s, measure);
  const avail = room - s.gap * (keep.length - 1);
  if (sum(widths) > avail) {
    // the widest ones share what is left, each cut to the same cap (never under its padding and an ellipsis)
    const sorted = [...widths].sort((a, b) => a - b);
    let cap = 0, left = avail;
    for (let i = 0; i < sorted.length; i++) {
      const even = left / (sorted.length - i);
      if (sorted[i] <= even) { left -= sorted[i]; continue; }
      cap = Math.floor(even);
      break;
    }
    widths = widths.map((w) => Math.max(2 * s.pad + 8, Math.min(w, cap || w)));
  }
  return { style: s, keep, widths, width: sum(widths) + s.gap * (keep.length - 1) };
}

// the biggest font (≤ max) at which `text` fits `room` wide
function fontFor(text, room, max, min, measure, weight = 600) {
  for (let f = max; f > min; f -= 1) if (measure(text, f, weight) <= room) return f;
  return min;
}

// a row of pills centred in a column: their boxes
function rowBoxes(row, x0, colW, y) {
  let x = x0 + Math.max(0, (colW - row.width) / 2);
  return row.widths.map((w) => { const b = { x, y, w, h: row.style.h }; x += w + row.style.gap; return b; });
}

// o: { size: 's'|'m', w, h, measure, timer: { text, labels, drop }, stopwatch: { title, text, labels } }
export function timerLayout(o) {
  const content = contentOf(o.w, o.h);
  const two = o.size !== 's';
  const colW = two ? Math.floor((content.w - 2 * COL_GAP - 1) / 2) : content.w;
  const cols = two ? [{ x: content.x, w: colW }, { x: content.x + colW + 2 * COL_GAP + 1, w: colW }] : [{ x: content.x, w: colW }];
  const boxes = [];
  // the timer: the ring and one row of pills under it, centred in the column
  const trow = fitRow(o.timer.labels, colW, o.measure, o.timer.drop || []);
  const ring = Math.max(48, Math.min(two ? 100 : 96, colW, content.h - trow.style.h - RING_GAP));
  const ringFont = fontFor(o.timer.text, ring - 26, 22, 11, o.measure);
  const th = ring + RING_GAP + trow.style.h;
  const ty = content.y + Math.max(0, (content.h - th) / 2);
  const ringBox = { x: cols[0].x + (colW - ring) / 2, y: ty, w: ring, h: ring };
  const timerBoxes = [ringBox, ...rowBoxes(trow, cols[0].x, colW, ty + ring + RING_GAP)];
  boxes.push(...timerBoxes);
  const out = { content, cols, colGap: COL_GAP, timer: { ring, ringFont, gap: RING_GAP, row: trow, boxes: timerBoxes }, boxes };
  if (!two) return out;
  // the stopwatch: its name, the figure and one row of pills, centred in the other column
  const srow = fitRow(o.stopwatch.labels, colW, o.measure);
  const figure = fontFor(o.stopwatch.text, colW, 26, 12, o.measure);
  const subH = Math.ceil(SUB_FONT * LINE), figH = Math.ceil(figure * LINE);
  const sh = subH + SW_GAP + figH + SW_GAP + srow.style.h;
  const sy = content.y + Math.max(0, (content.h - sh) / 2), x1 = cols[1].x;
  const swBoxes = [
    { x: x1, y: sy, w: Math.min(colW, Math.ceil(o.measure(String(o.stopwatch.title), SUB_FONT, 400))), h: subH },
    { x: x1 + Math.max(0, (colW - o.measure(o.stopwatch.text, figure, 600)) / 2), y: sy + subH + SW_GAP, w: Math.min(colW, Math.ceil(o.measure(o.stopwatch.text, figure, 600))), h: figH },
    ...rowBoxes(srow, x1, colW, sy + subH + SW_GAP + figH + SW_GAP),
  ];
  boxes.push(...swBoxes);
  out.stopwatch = { figure, row: srow, boxes: swBoxes, gap: SW_GAP };
  return out;
}
