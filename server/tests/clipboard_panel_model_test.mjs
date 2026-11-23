// clipboard_panel_model_test.mjs — the clipboard history panels' list (sdk/clipboardpanelmodel.js; GitHub #203, #204):
// the arrow keys move the selection inside the list, the list scrolls only to show the row the keys chose, and a row
// under a pointer at rest never takes the selection back.
//
//   node tests/clipboard_panel_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { stepSel, scrollFor, hoverGate, rowWindow } = await import(path.join(here, '../../sdk/clipboardpanelmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- the keys ----
eq('down', stepSel(0, 40, 1), 1);
eq('up', stepSel(5, 40, -1), 4);
eq('not above the first', stepSel(0, 40, -1), 0);
eq('not below the last', stepSel(39, 40, 1), 39);
eq('every row is reachable (past the old 16-row window)', Array.from({ length: 39 }).reduce((s) => stepSel(s, 40, 1), 0), 39);
eq('an empty list', stepSel(3, 0, 1), 0);
eq('a selection past a list that got shorter', stepSel(30, 10, -1), 9);

// ---- scrolling to the selection, and only then ----
const ROW = 47, VIEW = 300;          // rows 47 px apart, 300 px of list
eq('in view: no scroll', scrollFor(2, ROW, 0, VIEW, 6), null);
eq('below: just enough to show it whole', scrollFor(6, ROW, 0, VIEW, 6), 6 + 7 * ROW + 6 - VIEW);
eq('above: its top', scrollFor(3, ROW, 400, VIEW, 6), 6 + 3 * ROW - 6);
eq('the first row: the very top', scrollFor(0, ROW, 200, VIEW, 6), 0);
eq('a row already whole at the bottom: no scroll', scrollFor(5, ROW, 6 + 6 * ROW + 6 - VIEW, VIEW, 6), null);
eq('no list height yet: no scroll', scrollFor(5, ROW, 0, 0), null);

// ---- only the rows in sight are made ----
eq('the top of 500 entries', rowWindow(0, 300, ROW, 500, 6), { from: 0, to: 13, before: 0, after: (500 - 13) * ROW });
eq('scrolled down: the rows around the view', rowWindow(6 + 100 * ROW, 300, ROW, 500, 6), { from: 94, to: 113, before: 94 * ROW, after: (500 - 113) * ROW });
eq('the end', rowWindow(6 + 495 * ROW, 300, ROW, 500, 6).to, 500);
eq('a short list: all of it', rowWindow(0, 300, ROW, 4, 6), { from: 0, to: 4, before: 0, after: 0 });
eq('nothing', rowWindow(0, 300, ROW, 0, 6), { from: 0, to: 0, before: 0, after: 0 });
{
  const w = rowWindow(6 + 250 * ROW, 300, ROW, 500, 6);
  eq('the spaces and the rows add up to the whole list', w.before + (w.to - w.from) * ROW + w.after, 500 * ROW);
  eq('the selection scrolled to is among the rows made', [scrollFor(250, ROW, 0, 300, 6)].map((y) => { const v = rowWindow(y, 300, ROW, 500, 6); return 250 >= v.from && 250 < v.to; })[0], true);
}

// ---- the pointer ----
const g = hoverGate();
eq('before any move a hover selects', g.allow(), true);
g.move(100, 200);
eq('after a move it selects', g.allow(), true);
g.key();
eq('after an arrow key a row sliding under the still pointer does not', g.allow(), false);
g.move(100, 200);
eq('an event at the same place is not a move', g.allow(), false);
g.move(101, 200);
eq('the pointer moving gives the hover back', g.allow(), true);
g.key();
g.key();
g.move(101, 203);
eq('again after several keys', g.allow(), true);

console.log(bad ? `clipboard-panel-model: ${bad} of ${n} failed` : `clipboard-panel-model: ${n} checks passed`);
process.exit(bad ? 1 : 0);
