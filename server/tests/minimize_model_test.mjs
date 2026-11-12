// minimize_model_test.mjs — which windows start Lord Flow's minimize effect (themes/default/minimize.js, GitHub #112).
import { minimizeStarts } from '../../themes/default/minimize.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const ids = (l) => l.map((x) => x.id);

let prev = new Map();
eq('first render: nothing starts', ids(minimizeStarts([{ id: 1 }, { id: 2, minimized: true }], prev, { now: 5 })), []);
eq('a window just minimized starts', minimizeStarts([{ id: 1, minimized: true }, { id: 2, minimized: true }], prev, { now: 7 }), [{ id: 1, t0: 7 }]);
eq('still minimized: no new start', ids(minimizeStarts([{ id: 1, minimized: true }], prev)), []);
eq('restored and minimized again starts again', (minimizeStarts([{ id: 1 }], prev), ids(minimizeStarts([{ id: 1, minimized: true }], prev))), [1]);

prev = new Map([[1, false], [2, false]]);
eq('fullscreen and picture in picture: none', ids(minimizeStarts([{ id: 1, minimized: true, fullscreen: true }, { id: 2, minimized: true, pip: true }], prev)), []);

// Reduce motion: none starts, and what was seen is still recorded
prev = new Map([[1, false], [2, false]]);
eq('reduce motion: none starts', ids(minimizeStarts([{ id: 1, minimized: true }, { id: 2, minimized: true }], prev, { reduce: true })), []);
eq('reduce motion: the state is kept', [...prev], [[1, true], [2, true]]);
eq('reduce motion off again: a later minimize animates', (minimizeStarts([{ id: 1 }], prev), ids(minimizeStarts([{ id: 1, minimized: true }], prev))), [1]);

console.log(`minimize-model-test: ${ok} ok, ${bad} failures`);
process.exit(bad ? 1 : 0);
