// editor_scrollbar_test.mjs — the Text Editor's scroll bar (apps/editor/scrollbar.js, GitHub #84): the thumb's size
// and place, and dragging it back to a scroll position (the end of a long log in one drag).
import { BAR_COL, textWidth, thumbOf, topOfThumb, widest, maxLeft, clampLeft, RIGHT_ROOM } from '../../apps/editor/scrollbar.js';

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

// GitHub #109: the text stops before the bar's column, so the caret is scrolled into view before it reaches the bar
eq('bar shown: its column is not text', textWidth(600, 40, 10, true), 600 - 40 - 20 - BAR_COL);
eq('no bar: the full width', textWidth(600, 40, 10, false), 600 - 40 - 20);
eq('a view narrower than the gutter: no width, not negative', textWidth(30, 40, 10, true), 0);

// GitHub #264: the horizontal bar. The widest line's width with room after it; no bar while every line fits
const w8 = (line) => line.length * 8;
eq('the widest line, with room after it', widest(['ab', 'a'.repeat(100), ''], w8), 800 + RIGHT_ROOM);
eq('an empty file is no width', widest(['', ''], w8), 0);
eq('lines that fit: no horizontal bar', thumbOf(600, widest(['short'], w8), 0), null);
eq('a 2000-character line in a 500 px column: a bar', thumbOf(500, widest(['x'.repeat(2000)], w8), 0).size, Math.max(24, 500 * 500 / (16000 + RIGHT_ROOM)));
eq('scrolled to the end of that line: the thumb at the right end', thumbOf(500, 16040, maxLeft(16040, 500)).pos, 500 - thumbOf(500, 16040, 0).size);
eq('sideways scroll stops at the end of the widest line', clampLeft(99999, 16040, 500), 16040 - 500);
eq('…and at the start', clampLeft(-30, 16040, 500), 0);
eq('nothing to scroll when it fits', clampLeft(100, 300, 500), 0);
eq('dragged all the way right: the end', topOfThumb(500, 16040, 9999), 16040 - 500);

console.log(`editor_scrollbar_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
