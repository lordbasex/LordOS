// focus_model_test.mjs — keyboard focus between controls (sdk/focusmodel.js): the Tab order over a node tree, the
// next and the previous stop skipping hidden and disabled ones, a modal node keeping Tab inside, the focus-visible
// state, and what the keys do to a slider, a stepper, a row of choices, a menu and a scroll view (right to left too).
import {
  isHidden, canFocus, treeOrder, within, tabArea, tabStops, nextStop, tabKey, activates, NO_FOCUS, focusAfter,
  rangeKey, choiceKey, menuStep, scrollKey,
} from '../../sdk/focusmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const key = (k, mods = {}) => ({ key: k, ...mods });

// a tree shaped as the runtime's instances: props, style, host, first, last, prev and next
function node(name, props = {}, children = [], style = {}) {
  const n = { name, props, style, host: null, first: children[0] || null, last: children[children.length - 1] || null, prev: null, next: null };
  children.forEach((c, i) => { c.host = n; c.prev = children[i - 1] || null; c.next = children[i + 1] || null; });
  return n;
}
const names = (list) => list.map((n) => n && n.name);

const a = node('a', { focusable: true });
const off = node('off', { focusable: true, disabled: true });
const hiddenBtn = node('hiddenBtn', { focusable: true });
const hiddenBox = node('hiddenBox', {}, [hiddenBtn], { display: 'none' });
const b = node('b', { focusable: true });
const label = node('label');
const c = node('c', { focusable: true });
const row = node('row', {}, [b, label, c]);
const d = node('d', { focusable: true });
const root = node('root', {}, [a, off, hiddenBox, row, d]);

// what can be focused and what is out of sight
eq('canFocus: a stop', canFocus(a), true);
eq('canFocus: disabled', canFocus(off), false);
eq('canFocus: a plain node', canFocus(label), false);
eq('canFocus: deleted', canFocus({ props: { focusable: true }, deleted: true }), false);
eq('isHidden: display none', isHidden(hiddenBox), true);
eq('isHidden: hidden by React', isHidden({ hidden: true, style: {} }), true);
eq('isHidden: shown', isHidden(row), false);

// tree order: a node before its children, hidden subtrees left out
eq('treeOrder', names(treeOrder(root)), ['root', 'a', 'off', 'row', 'b', 'label', 'c', 'd']);
eq('treeOrder: a hidden root', treeOrder(hiddenBox), []);
eq('tabStops', names(tabStops(root)), ['a', 'b', 'c', 'd']);
eq('within: inside', within(c, root), true);
eq('within: itself', within(root, root), true);
eq('within: elsewhere', within(a, row), false);

// Tab and Shift+Tab, round the ends
eq('next: from nothing', nextStop(root, null, false)?.name, 'a');
eq('prev: from nothing', nextStop(root, null, true)?.name, 'd');
eq('next: a → b (skips the disabled and the hidden)', nextStop(root, a, false)?.name, 'b');
eq('next: b → c (skips the label)', nextStop(root, b, false)?.name, 'c');
eq('next: d wraps to a', nextStop(root, d, false)?.name, 'a');
eq('prev: a wraps to d', nextStop(root, a, true)?.name, 'd');
eq('prev: c → b', nextStop(root, c, true)?.name, 'b');
// from a node that is no stop (where the pointer pressed): the stop after it, or before it going back
eq('next: from the label', nextStop(root, label, false)?.name, 'c');
eq('prev: from the label', nextStop(root, label, true)?.name, 'b');
eq('next: from the row (its first child)', nextStop(root, row, false)?.name, 'b');
// from a node not in sight: the first or last stop
eq('next: from a hidden node', nextStop(root, hiddenBtn, false)?.name, 'a');
eq('next: from another tree', nextStop(root, node('x'), false)?.name, 'a');
eq('next: no stops', nextStop(node('empty', {}, [node('t')]), null, false), null);
const lone = node('lone', { focusable: true });
eq('next: the only stop stays', nextStop(node('r', {}, [lone]), lone, false)?.name, 'lone');
// a stop that became disabled is skipped next time round
off.props.disabled = false;
eq('next: an enabled one counts again', nextStop(root, a, false)?.name, 'off');
off.props.disabled = true;

// a modal node (a sheet in the window): Tab goes round it only, while it is shown
const s1 = node('s1', { focusable: true }), s2 = node('s2', { focusable: true });
const sheet = node('sheet', { modal: true }, [node('title'), s1, s2]);
const win = node('win', {}, [node('w1', { focusable: true }), sheet]);
eq('modal: the area', tabArea(win).name, 'sheet');
eq('modal: its stops only', names(tabStops(win)), ['s1', 's2']);
eq('modal: from outside starts inside', nextStop(win, win.first, false)?.name, 's1');
eq('modal: wraps inside', nextStop(win, s2, false)?.name, 's1');
sheet.style = { display: 'none' };
eq('modal: hidden, the whole window again', names(tabStops(win)), ['w1']);

// the keys that move and press
eq('tabKey: Tab', tabKey(key('Tab')), 1);
eq('tabKey: Shift+Tab', tabKey(key('Tab', { shift: true })), -1);
eq('tabKey: Ctrl+Tab belongs to apps', tabKey(key('Tab', { ctrl: true })), 0);
eq('tabKey: Alt+Tab belongs to the shell', tabKey(key('Tab', { alt: true })), 0);
eq('tabKey: another key', tabKey(key('a')), 0);
eq('activates: Space', activates(key(' ')), true);
eq('activates: Enter', activates(key('Enter')), true);
eq('activates: Ctrl+Enter is not', activates(key('Enter', { ctrl: true })), false);
eq('activates: an arrow is not', activates(key('ArrowDown')), false);

// focus-visible: only the keyboard shows the ring, a press hides it and Tab goes on from there
let st = NO_FOCUS;
st = focusAfter(st, { type: 'tab', to: b });
eq('visible: after Tab', [st.focus?.name, st.anchor?.name, st.visible], ['b', 'b', true]);
st = focusAfter(st, { type: 'pointer', target: label });
eq('visible: a press ends it', [st.focus, st.anchor?.name, st.visible], [null, 'label', false]);
eq('visible: the next Tab goes on from the press', nextStop(root, st.anchor, false)?.name, 'c');
st = focusAfter(st, { type: 'tab', to: null });
eq('visible: a Tab with nowhere to go changes nothing', [st.focus, st.anchor?.name], [null, 'label']);
st = focusAfter(focusAfter(st, { type: 'tab', to: c }), { type: 'gone', anchor: row });
eq('visible: the focused node went away', [st.focus, st.anchor?.name, st.visible], [null, 'row', false]);
eq('visible: an unknown event', focusAfter(st, { type: 'x' }) === st, true);

// a slider or a stepper
const r = { value: 50, min: 0, max: 100, step: 5, page: 10 };
eq('range: →', rangeKey(key('ArrowRight'), r), 55);
eq('range: ←', rangeKey(key('ArrowLeft'), r), 45);
eq('range: ↑', rangeKey(key('ArrowUp'), r), 55);
eq('range: ↓', rangeKey(key('ArrowDown'), r), 45);
eq('range: Page Up', rangeKey(key('PageUp'), r), 60);
eq('range: Page Down', rangeKey(key('PageDown'), r), 40);
eq('range: Home', rangeKey(key('Home'), r), 0);
eq('range: End', rangeKey(key('End'), r), 100);
eq('range: stays under max', rangeKey(key('PageUp'), { ...r, value: 95 }), 100);
eq('range: stays over min', rangeKey(key('ArrowLeft'), { ...r, value: 2 }), 0);
eq('range: rtl → goes toward the start', rangeKey(key('ArrowRight'), { ...r, rtl: true }), 45);
eq('range: rtl ← goes toward the end', rangeKey(key('ArrowLeft'), { ...r, rtl: true }), 55);
eq('range: rtl ↑ still adds', rangeKey(key('ArrowUp'), { ...r, rtl: true }), 55);
eq('range: page defaults to ten steps', rangeKey(key('PageUp'), { value: 0, step: 1 }), 10);
eq('range: no Home without a min', rangeKey(key('Home'), { value: 3, step: 1 }), null);
eq('range: not its key', rangeKey(key('a'), r), null);
eq('range: with Ctrl is not its', rangeKey(key('ArrowRight', { ctrl: true }), r), null);

// a row of choices
eq('choice: →', choiceKey(key('ArrowRight'), { index: 0, count: 3 }), 1);
eq('choice: → at the end stays', choiceKey(key('ArrowRight'), { index: 2, count: 3 }), 2);
eq('choice: ←', choiceKey(key('ArrowLeft'), { index: 2, count: 3 }), 1);
eq('choice: rtl → goes back', choiceKey(key('ArrowRight'), { index: 2, count: 3, rtl: true }), 1);
eq('choice: rtl ← goes on', choiceKey(key('ArrowLeft'), { index: 0, count: 3, rtl: true }), 1);
eq('choice: Home', choiceKey(key('Home'), { index: 2, count: 3 }), 0);
eq('choice: End', choiceKey(key('End'), { index: 0, count: 3 }), 2);
eq('choice: none chosen yet, →', choiceKey(key('ArrowRight'), { index: -1, count: 3 }), 0);
eq('choice: none chosen yet, ←', choiceKey(key('ArrowLeft'), { index: -1, count: 3 }), 0);
eq('choice: no choices', choiceKey(key('ArrowRight'), { index: -1, count: 0 }), null);
eq('choice: not its key', choiceKey(key(' '), { index: 0, count: 3 }), null);

// a menu: separators and disabled items are skipped, round the ends
const items = ['a', '-', 'b', 'off', 'c'];
const skip = (i) => items[i] === '-' || items[i] === 'off';
eq('menu: ↓ from nothing', menuStep(items.length, -1, 1, skip), 0);
eq('menu: ↑ from nothing', menuStep(items.length, -1, -1, skip), 4);
eq('menu: ↓ skips the separator', menuStep(items.length, 0, 1, skip), 2);
eq('menu: ↓ skips the disabled', menuStep(items.length, 2, 1, skip), 4);
eq('menu: ↓ wraps', menuStep(items.length, 4, 1, skip), 0);
eq('menu: ↑ wraps', menuStep(items.length, 0, -1, skip), 4);
eq('menu: nothing to highlight', menuStep(2, -1, 1, () => true), -1);
eq('menu: empty', menuStep(0, -1, 1), -1);

// a scroll view 100 px high over 1000 px
const sv = { offset: 200, view: 100, content: 1000 };
eq('scroll: ↓ a line', scrollKey(key('ArrowDown'), sv), 240);
eq('scroll: ↑ a line', scrollKey(key('ArrowUp'), sv), 160);
eq('scroll: Page Down', scrollKey(key('PageDown'), sv), 260);
eq('scroll: Page Up', scrollKey(key('PageUp'), sv), 140);
eq('scroll: Space', scrollKey(key(' '), sv), 260);
eq('scroll: Shift+Space', scrollKey(key(' ', { shift: true }), sv), 140);
eq('scroll: Home', scrollKey(key('Home'), sv), 0);
eq('scroll: End', scrollKey(key('End'), sv), 900);
eq('scroll: a big view pages most of itself', scrollKey(key('PageDown'), { offset: 0, view: 500, content: 2000 }), 460);
eq('scroll: stays at the end', scrollKey(key('PageDown'), { ...sv, offset: 880 }), 900);
eq('scroll: stays at the top', scrollKey(key('ArrowUp'), { ...sv, offset: 10 }), 0);
eq('scroll: content that fits takes no key', scrollKey(key('ArrowDown'), { offset: 0, view: 100, content: 80 }), null);
eq('scroll: → is not a vertical view\'s', scrollKey(key('ArrowRight'), sv), null);
eq('scroll: horizontal →', scrollKey(key('ArrowRight'), { ...sv, horizontal: true }), 240);
eq('scroll: horizontal ←', scrollKey(key('ArrowLeft'), { ...sv, horizontal: true }), 160);
eq('scroll: horizontal rtl ← goes on', scrollKey(key('ArrowLeft'), { ...sv, horizontal: true, rtl: true }), 240);
eq('scroll: horizontal ↓ is not its', scrollKey(key('ArrowDown'), { ...sv, horizontal: true }), null);
eq('scroll: with Ctrl is not its', scrollKey(key('End', { ctrl: true }), sv), null);

console.log(`focus-model: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
