// Moving and resizing a window from anywhere in it with a modifier (sdk/moddragmodel.js, GitHub #219), and the title
// bar's size and the Window menu's tiling switch (sdk/windowbuttons.js, #151). node server/tests/moddrag_model_test.mjs
import { DRAG_MODIFIERS, dragModifierPref, nearestCorner, modGrabAction, dragOutAt, DRAG_OUT } from '../../sdk/moddragmodel.js';
import { WINDOW_BUTTON_SIZES, windowButtonSizePref, buttonSizeScale, scaledBar, windowMenuTiling } from '../../sdk/windowbuttons.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

// the modifier: off (nothing changes) unless the person chose ❖ or Alt
eq('choices, off first', DRAG_MODIFIERS, ['off', 'super', 'alt']);
eq('nothing saved: off', dragModifierPref({}), 'off');
eq('no settings: off', dragModifierPref(null), 'off');
eq('❖', dragModifierPref({ windowDragModifier: 'super' }), 'super');
eq('alt', dragModifierPref({ windowDragModifier: 'alt' }), 'alt');
eq('off', dragModifierPref({ windowDragModifier: 'off' }), 'off');
eq('anything else: off', dragModifierPref({ windowDragModifier: 'ctrl' }), 'off');
eq('not a string: off', dragModifierPref({ windowDragModifier: 2 }), 'off');

// the corner nearest the pointer, as xdg edges (1 top, 2 bottom, 4 left, 8 right)
const f = { x: 100, y: 50, w: 400, h: 300 };
eq('top left', nearestCorner(f, 110, 60), 5);
eq('top right', nearestCorner(f, 490, 60), 9);
eq('bottom left', nearestCorner(f, 110, 340), 6);
eq('bottom right', nearestCorner(f, 490, 340), 10);
eq('just past the middle: bottom right', nearestCorner(f, 300, 200), 10);
eq('just before the middle: top left', nearestCorner(f, 299, 199), 5);

// what a press with the modifier does
const w = { id: 'files', x: 100, y: 50, w: 400, h: 300 };
eq('left: move', modGrabAction(w, 0), 'move');
eq('right: resize', modGrabAction(w, 2), 'resize');
eq('middle: resize', modGrabAction(w, 1), 'resize');
eq('back button: the app\'s', modGrabAction(w, 3), null);
eq('forward button: the app\'s', modGrabAction(w, 4), null);
eq('no window: the app\'s', modGrabAction(null, 0), null);
eq('full screen: the app\'s (a game, a video)', modGrabAction({ ...w, fullscreen: true }, 0), null);
eq('full screen, right: the app\'s', modGrabAction({ ...w, fullscreen: true }, 2), null);
eq('minimized: nothing to take', modGrabAction({ ...w, minimized: true }, 0), null);
eq('picture in picture: its own drag', modGrabAction({ ...w, pip: { x: 0, y: 0, w: 10, h: 10 } }, 0), null);
eq('the Overview: its own clicks', modGrabAction(w, 0, { overview: true }), null);
eq('a shell panel: never', modGrabAction(w, 0, { shell: true }), null);
eq('maximized, left: comes out of its place', modGrabAction({ ...w, maximized: true }, 0), 'unmaximize');
eq('maximized, right: taken, nothing happens', modGrabAction({ ...w, maximized: true }, 2), 'hold');
eq('locked (Workspaces\' padlock): taken, nothing happens', modGrabAction(w, 0, { locked: true }), 'hold');
eq('locked, right too', modGrabAction(w, 2, { locked: true }), 'hold');
eq('snapped: moves (the end restores its size, as from the title bar)', modGrabAction({ ...w, snapped: 'left' }, 0), 'move');

// a maximized window dragged out: its own size, under the pointer at the same fraction of its width
const g = { x: 0, y: 30, w: 1920, h: 1050 };
eq('dragged out from the middle', dragOutAt(g, 960, 300, 1000, 320, 800, 40), { x: 600, y: 300 });
eq('dragged out from the left edge', dragOutAt(g, 0, 40, 10, 60, 800, 40), { x: 10, y: 50 });
eq('dragged out from the right edge', dragOutAt(g, 1920, 40, 1900, 60, 800, 40), { x: 1100, y: 50 });
eq('no width: from the middle', dragOutAt({ x: 0, y: 0, w: 0, h: 0 }, 0, 0, 500, 500, 400, 40), { x: 300, y: 500 });
eq('a jitter is no drag', DRAG_OUT >= 4, true);

// the title bar's size
eq('sizes', WINDOW_BUTTON_SIZES, ['normal', 'large', 'xlarge']);
eq('nothing saved: normal', windowButtonSizePref({}), 'normal');
eq('large', windowButtonSizePref({ windowButtonSize: 'large' }), 'large');
eq('extra large', windowButtonSizePref({ windowButtonSize: 'xlarge' }), 'xlarge');
eq('anything else: normal', windowButtonSizePref({ windowButtonSize: 'huge' }), 'normal');
eq('scales', WINDOW_BUTTON_SIZES.map(buttonSizeScale), [1, 1.25, 1.5]);
eq('unknown scale: 1', buttonSizeScale('x'), 1);
eq('normal keeps the theme\'s own lengths, halves too', [scaledBar(40, 'normal'), scaledBar(1.5, 'normal'), scaledBar(13, 'normal')], [40, 1.5, 13]);
eq('Flow\'s bar and lights, large', [scaledBar(40, 'large'), scaledBar(13, 'large')], [50, 16]);
eq('Flow\'s bar and lights, extra large', [scaledBar(40, 'xlarge'), scaledBar(13, 'xlarge')], [60, 20]);
eq('Classic\'s bar and buttons, extra large', [scaledBar(34, 'xlarge'), scaledBar(44, 'xlarge')], [51, 66]);
eq('larger lengths are whole pixels', [1.5, 13, 34, 36, 38, 44, 28].every((n) => Number.isInteger(scaledBar(n, 'large')) && Number.isInteger(scaledBar(n, 'xlarge'))), true);
eq('larger is never smaller', [13, 28, 34, 36, 38, 40, 44].every((n) => scaledBar(n, 'normal') < scaledBar(n, 'large') && scaledBar(n, 'large') < scaledBar(n, 'xlarge')), true);

// the Window menu's tiling layouts
eq('tiling: on by default', windowMenuTiling({}), true);
eq('tiling: no settings', windowMenuTiling(null), true);
eq('tiling: off', windowMenuTiling({ windowMenuTiling: false }), false);
eq('tiling: only false turns it off', windowMenuTiling({ windowMenuTiling: 0 }), true);

console.log(`${ok} modifier drag and title bar checks passed`);
