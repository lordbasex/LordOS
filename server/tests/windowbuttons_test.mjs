// The side of the title bar buttons (sdk/windowbuttons.js, issue #58). node server/tests/windowbuttons_test.mjs
import { WINDOW_BUTTON_SIDES, windowButtonsPref, buttonsAtStart, buttonOrder, gtkButtonLayout } from '../../sdk/windowbuttons.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

eq('choices', WINDOW_BUTTON_SIDES, ['auto', 'left', 'right']);
eq('nothing saved', windowButtonsPref({}), 'auto');
eq('no settings', windowButtonsPref(null), 'auto');
eq('left', windowButtonsPref({ windowButtons: 'left' }), 'left');
eq('right', windowButtonsPref({ windowButtons: 'right' }), 'right');
eq('anything else', windowButtonsPref({ windowButtons: 'top' }), 'auto');
eq('not a string', windowButtonsPref({ windowButtons: 1 }), 'auto');

// auto: each theme's own place, mirrored right to left by the engine as before
eq('Lord Flow auto', buttonsAtStart('auto', true), true);
eq('Lord Classic auto', buttonsAtStart('auto', false), false);
eq('Lord Flow auto, right to left', buttonsAtStart('auto', true, true), true);
eq('Lord Classic auto, right to left', buttonsAtStart('auto', false, true), false);
// left and right are the screen's sides: right to left, the row is mirrored, so the place in it is the other one
for (const theme of [true, false]) {
  eq(`left ${theme}`, buttonsAtStart('left', theme), true);
  eq(`right ${theme}`, buttonsAtStart('right', theme), false);
  eq(`left ${theme} rtl`, buttonsAtStart('left', theme, true), false);
  eq(`right ${theme} rtl`, buttonsAtStart('right', theme, true), true);
}

eq('at the start', buttonOrder(true), ['close', 'minimize', 'maximize']);
eq('at the end', buttonOrder(false), ['minimize', 'maximize', 'close']);
eq('close nearest the corner at the start', buttonOrder(true)[0], 'close');
eq('close nearest the corner at the end', buttonOrder(false).at(-1), 'close');

eq('GTK left', gtkButtonLayout('left', ':minimize,maximize,close'), 'close,minimize,maximize:');
eq('GTK right', gtkButtonLayout('right', 'close,minimize,maximize:'), ':minimize,maximize,close');
eq('GTK auto: the theme\'s', gtkButtonLayout('auto', ':minimize,maximize,close'), ':minimize,maximize,close');
eq('GTK auto without one', gtkButtonLayout('auto'), 'close,minimize,maximize:');

console.log(`${ok} window button checks passed`);
