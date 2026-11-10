// list_model_test.mjs — (0.8.8.8) the selection of the SDK's List and Table (sdk/listmodel.js): a press, Shift ranges
// and Ctrl toggles, the arrows, Home, End and the page keys (with Shift to extend), Ctrl+A, Ctrl and an arrow moving
// only the lead, type-to-find, and rows that are no longer shown.
import { NO_SELECTION, rangeOf, keepShown, clickSelect, moveIndex, keySelect, findSelect, findChar } from '../../sdk/listmodel.js';
import { typeAhead } from '../../sdk/menunav.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const key = (k, mods = {}) => ({ key: k, ...mods });
const keys = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
const sel = (selected, anchor, lead) => ({ selected, anchor, lead });

// ---- ranges and rows that went away ----
eq('range forward', rangeOf(keys, 'b', 'd'), ['b', 'c', 'd']);
eq('range backward', rangeOf(keys, 'e', 'c'), ['c', 'd', 'e']);
eq('range from a row not shown', rangeOf(keys, 'z', 'c'), ['c']);
eq('range to a row not shown', rangeOf(keys, 'a', 'z'), []);
eq('kept as it is', keepShown(sel(['b'], 'b', 'b'), keys), sel(['b'], 'b', 'b'));
eq('a row filtered out drops', keepShown(sel(['b', 'x'], 'x', 'b'), keys), sel(['b'], null, 'b'));
const same = sel(['a'], 'a', 'a');
eq('the same object when nothing went', keepShown(same, keys) === same, true);

// ---- the pointer ----
eq('a press selects one row', clickSelect(NO_SELECTION, keys, 'c'), sel(['c'], 'c', 'c'));
eq('single: Shift is a plain press', clickSelect(sel(['b'], 'b', 'b'), keys, 'e', { shift: true }), sel(['e'], 'e', 'e'));
eq('Shift extends from the anchor', clickSelect(sel(['b'], 'b', 'b'), keys, 'e', { shift: true, multiple: true }), sel(['b', 'c', 'd', 'e'], 'b', 'e'));
eq('Shift again keeps the anchor', clickSelect(sel(['b', 'c', 'd', 'e'], 'b', 'e'), keys, 'a', { shift: true, multiple: true }), sel(['a', 'b'], 'b', 'a'));
eq('Ctrl adds a row', clickSelect(sel(['b'], 'b', 'b'), keys, 'e', { toggle: true, multiple: true }), sel(['b', 'e'], 'e', 'e'));
eq('Ctrl takes it out', clickSelect(sel(['b', 'e'], 'e', 'e'), keys, 'b', { toggle: true, multiple: true }), sel(['e'], 'b', 'b'));
eq('the selection stays in the shown order', clickSelect(sel(['f'], 'f', 'f'), keys, 'a', { toggle: true, multiple: true }).selected, ['a', 'f']);
eq('Shift from a Ctrl press', clickSelect(sel(['b', 'e'], 'e', 'e'), keys, 'g', { shift: true, multiple: true }), sel(['e', 'f', 'g'], 'e', 'g'));
eq('a row not shown changes nothing', clickSelect(same, keys, 'zz'), same);

// ---- moves ----
eq('↓ from none: the first', moveIndex('ArrowDown', -1, 8), 0);
eq('↑ from none: the last', moveIndex('ArrowUp', -1, 8), 7);
eq('↓ stops at the end', moveIndex('ArrowDown', 7, 8), 7);
eq('↑ stops at the start', moveIndex('ArrowUp', 0, 8), 0);
eq('Page Down a page', moveIndex('PageDown', 1, 8, 3), 4);
eq('Page Down stops at the end', moveIndex('PageDown', 6, 8, 3), 7);
eq('Page Up stops at the start', moveIndex('PageUp', 2, 8, 3), 0);
eq('Page Down from none', moveIndex('PageDown', -1, 8, 3), 2);
eq('Home', moveIndex('Home', 5, 8), 0);
eq('End', moveIndex('End', 1, 8), 7);
eq('another key', moveIndex('x', 1, 8), null);
eq('no rows', moveIndex('ArrowDown', -1, 0), null);

// ---- the keys ----
eq('↓ selects the next row', keySelect(key('ArrowDown'), sel(['b'], 'b', 'b'), keys), sel(['c'], 'c', 'c'));
eq('↓ with nothing selected: the first', keySelect(key('ArrowDown'), NO_SELECTION, keys), sel(['a'], 'a', 'a'));
eq('End selects the last', keySelect(key('End'), sel(['b'], 'b', 'b'), keys), sel(['h'], 'h', 'h'));
eq('Page Down by the page given', keySelect(key('PageDown'), sel(['a'], 'a', 'a'), keys, { page: 4 }), sel(['e'], 'e', 'e'));
eq('Shift+↓ extends', keySelect(key('ArrowDown', { shift: true }), sel(['b'], 'b', 'b'), keys, { multiple: true }), sel(['b', 'c'], 'b', 'c'));
const two = keySelect(key('ArrowDown', { shift: true }), sel(['b', 'c'], 'b', 'c'), keys, { multiple: true });
eq('Shift+↓ again', two, sel(['b', 'c', 'd'], 'b', 'd'));
eq('Shift+↑ shrinks it back', keySelect(key('ArrowUp', { shift: true }), two, keys, { multiple: true }), sel(['b', 'c'], 'b', 'c'));
eq('Shift+Home to the top', keySelect(key('Home', { shift: true }), sel(['c'], 'c', 'c'), keys, { multiple: true }), sel(['a', 'b', 'c'], 'c', 'a'));
eq('Shift+End to the bottom', keySelect(key('End', { shift: true }), sel(['f'], 'f', 'f'), keys, { multiple: true }), sel(['f', 'g', 'h'], 'f', 'h'));
eq('Shift+Page Down', keySelect(key('PageDown', { shift: true }), sel(['a'], 'a', 'a'), keys, { multiple: true, page: 2 }), sel(['a', 'b', 'c'], 'a', 'c'));
eq('single: Shift+↓ is not its key', keySelect(key('ArrowDown', { shift: true }), sel(['b'], 'b', 'b'), keys), null);
eq('Ctrl+A selects all', keySelect(key('a', { ctrl: true }), sel(['c'], 'c', 'c'), keys, { multiple: true }), sel(keys, 'a', 'c'));
eq('⌘A too', keySelect(key('a', { meta: true }), NO_SELECTION, keys, { multiple: true }).selected, keys);
eq('Ctrl+A with a layout that sends another key: its base', keySelect({ key: 'q', base: 'a', ctrl: true }, NO_SELECTION, keys, { multiple: true }).selected, keys);
eq('single: Ctrl+A is the app\'s', keySelect(key('a', { ctrl: true }), sel(['c'], 'c', 'c'), keys), null);
const moved = keySelect(key('ArrowDown', { ctrl: true }), sel(['b'], 'b', 'b'), keys, { multiple: true });
eq('Ctrl+↓ moves only the lead', moved, sel(['b'], 'b', 'c'));
eq('Ctrl+Space toggles the lead', keySelect(key(' ', { ctrl: true }), moved, keys, { multiple: true }), sel(['b', 'c'], 'c', 'c'));
eq('Space selects the lead alone', keySelect(key(' '), moved, keys, { multiple: true }), sel(['c'], 'c', 'c'));
eq('Space on the only selected row: the same', keySelect(key(' '), same, keys), same);
eq('Space with no lead is not its key', keySelect(key(' '), NO_SELECTION, keys), null);
eq('⌥ and an arrow are not its keys', keySelect(key('ArrowDown', { alt: true }), same, keys), null);
eq('Enter is the list\'s, not the selection\'s', keySelect(key('Enter'), same, keys), null);
eq('no rows', keySelect(key('ArrowDown'), NO_SELECTION, []), null);

// ---- type-to-find ----
const names = ['Apple', 'Banana', 'blueberry', 'Cherry', 'cranberry', 'Date', 'Elder', 'Fig'];
let t = typeAhead(null, 'b', 0);
eq('a letter: the first row with it', findSelect(NO_SELECTION, keys, names, t.buf), sel(['b'], 'b', 'b'));
eq('the same letter again: the next one', findSelect(sel(['b'], 'b', 'b'), keys, names, typeAhead(t, 'b', 100).buf), sel(['c'], 'c', 'c'));
t = typeAhead(typeAhead(null, 'c', 0), 'r', 100);
eq('more letters narrow it', findSelect(sel(['a'], 'a', 'a'), keys, names, t.buf), sel(['e'], 'e', 'e'));
eq('and keep the row while it matches', findSelect(sel(['e'], 'e', 'e'), keys, names, 'cra'), sel(['e'], 'e', 'e'));
eq('it goes round', findSelect(sel(['h'], 'h', 'h'), keys, names, 'a'), sel(['a'], 'a', 'a'));
eq('case does not matter', findSelect(NO_SELECTION, keys, names, 'fig'), sel(['h'], 'h', 'h'));
eq('nothing starts with it', findSelect(same, keys, names, 'zz'), null);
eq('a pause starts over', typeAhead(typeAhead(null, 'c', 0), 'b', 5000).buf, 'b');
eq('a letter is a find key', findChar({ key: 'x', char: 'x' }), 'x');
eq('Shift is fine', findChar({ key: 'X', char: 'X', shift: true }), 'X');
eq('Ctrl is not', findChar({ key: 'x', char: 'x', ctrl: true }), '');
eq('Space only while searching', [findChar({ key: ' ', char: ' ' }), findChar({ key: ' ', char: ' ' }, true)], ['', ' ']);
eq('an arrow is not', findChar({ key: 'ArrowDown', char: '' }), '');

console.log(`list-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
