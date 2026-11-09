// editor_scrollbar_test.mjs — the Text Editor's scroll bar (apps/editor/scrollbar.js, GitHub #84): the thumb's size
// and place, and dragging it back to a scroll position (the end of a long log in one drag).
import { thumbOf, topOfThumb } from '../../apps/editor/scrollbar.js';

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${what}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } };

eq('short file: no bar', thumbOf(400, 300, 0), null);
eq('half the file visible: half the track', thumbOf(400, 800, 0), { size: 200, pos: 0 });
eq('scrolled to the end: thumb at the bottom', thumbOf(400, 800, 400), { size: 200, pos: 200 });
const log = 200000 * 20;                                   // a 200 000-line log
eq('a long log: the thumb keeps a usable size', thumbOf(400, log, 0).size, 24);
eq('dragged to the bottom: the end of the file', topOfThumb(400, log, 400 - 24), log - 400);
eq('dragged past the bottom: still the end', topOfThumb(400, log, 9999), log - 400);
eq('dragged above the top: the start', topOfThumb(400, log, -50), 0);
eq('round trip', Math.round(topOfThumb(400, 800, thumbOf(400, 800, 123).pos)), 123);

console.log(`editor_scrollbar_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
