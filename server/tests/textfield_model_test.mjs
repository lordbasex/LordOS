// The caret of a one-line TextInput without a screen (sdk/textfieldmodel.js, issue #59). node server/tests/textfield_model_test.mjs
import { fieldKey, insertAt, caretAt, clampPos } from '../../sdk/textfieldmodel.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const k = (key, more = {}) => ({ key, ...more });

// a typo in the middle of an address is fixed where it is
let s = { value: 'smb://nsa/Public', pos: 16 };
for (let i = 0; i < 8; i++) s = fieldKey(s.value, s.pos, k('ArrowLeft'));
eq('moved left', s.pos, 8);
s = fieldKey(s.value, s.pos, k('Backspace'));
s = fieldKey(s.value, s.pos, k('Backspace'));
eq('backspace at the caret', s, { value: 'smb://a/Public', pos: 6 });
s = insertAt(s.value, s.pos, 'ns');
eq('typed at the caret', s, { value: 'smb://nsa/Public', pos: 8 });
s = fieldKey(s.value, 6, k('Delete'));
eq('delete after the caret', s, { value: 'smb://sa/Public', pos: 6 });

eq('home', fieldKey('abc', 2, k('Home')), { value: 'abc', pos: 0 });
eq('end', fieldKey('abc', 0, k('End')), { value: 'abc', pos: 3 });
eq('right stops at the end', fieldKey('abc', 3, k('ArrowRight')).pos, 3);
eq('left stops at the start', fieldKey('abc', 0, k('ArrowLeft')).pos, 0);
eq('backspace at the start', fieldKey('abc', 0, k('Backspace')), { value: 'abc', pos: 0 });
eq('delete at the end', fieldKey('abc', 3, k('Delete')), { value: 'abc', pos: 3 });
eq('rtl: left goes on', fieldKey('שלום', 1, k('ArrowLeft'), true).pos, 2);
eq('rtl: right goes back', fieldKey('שלום', 1, k('ArrowRight'), true).pos, 0);
eq('ctrl+arrow is the window\'s', fieldKey('abc', 1, k('ArrowLeft', { ctrl: true })), null);
eq('other keys', [fieldKey('abc', 1, k('Enter')), fieldKey('abc', 1, k('a', { char: 'a' }))], [null, null]);
// code points, not UTF-16 units: an emoji is one step and one deletion
eq('emoji step', fieldKey('a😀b', 3, k('ArrowLeft')).pos, 2);
eq('emoji deleted whole', fieldKey('a😀b', 2, k('Backspace')), { value: 'ab', pos: 1 });
eq('paste in the middle', insertAt('ab', 1, '😀x'), { value: 'a😀xb', pos: 3 });
eq('stale caret clamped', [clampPos('ab', 9), clampPos('ab', -2), clampPos('ab', null), clampPos('', 3)], [2, 0, 2, 0]);
eq('stale caret, key', fieldKey('ab', 9, k('Backspace')), { value: 'a', pos: 1 });

// a click: the nearest gap between two characters, each 10 wide here
const w = (t) => [...t].length * 10;
eq('click positions', [caretAt('abcd', 0, w), caretAt('abcd', 4, w), caretAt('abcd', 6, w), caretAt('abcd', 21, w), caretAt('abcd', 500, w), caretAt('', 30, w), caretAt('ab', -5, w)], [0, 0, 1, 2, 4, 0, 0]);

console.log(`textfield-model-test: ${ok} ok`);

// ---- selection (0.8.8.8): the person selects and types over it, as in any text field
import('../../sdk/textfieldmodel.js').then(({ editKey, replaceSel, selRange, selectedText, wordAt }) => {
  const S = (value, pos, anchor = null) => ({ value, pos, anchor });
  const K = (key, more = {}) => ({ key, ...more });
  eq('ctrl+a selects all', editKey(S('ssh pi', 3), K('a', { ctrl: true, base: 'a' })), S('ssh pi', 6, 0));
  eq('cmd+a selects all', selectedText(editKey(S('ssh pi', 0), K('a', { meta: true, base: 'a' }))), 'ssh pi');
  eq('typing over a selection replaces it', replaceSel(S('ssh old-host', 12, 4), 'pi'), S('ssh pi', 6));
  eq('Backspace on a selection deletes it', editKey(S('abcdef', 4, 1), K('Backspace')), S('aef', 1));
  eq('Delete on a selection deletes it', editKey(S('abcdef', 1, 4), K('Delete')), S('aef', 1));
  eq('shift+← extends', editKey(editKey(S('abcd', 4), K('ArrowLeft', { shift: true })), K('ArrowLeft', { shift: true })), S('abcd', 2, 4));
  eq('shift+→ after shift+← shrinks', editKey(S('abcd', 2, 4), K('ArrowRight', { shift: true })), S('abcd', 3, 4));
  eq('← collapses to the start', editKey(S('abcd', 3, 1), K('ArrowLeft')), S('abcd', 1));
  eq('→ collapses to the end', editKey(S('abcd', 1, 3), K('ArrowRight')), S('abcd', 3));
  eq('shift+Home selects to the start', editKey(S('abcd', 3), K('Home', { shift: true })), S('abcd', 0, 3));
  eq('shift+End selects to the end', selRange(editKey(S('abcd', 1), K('End', { shift: true }))), [1, 4]);
  eq('ctrl+← jumps a word', editKey(S('ssh -o Server', 13), K('ArrowLeft', { ctrl: true })).pos, 7);
  eq('ctrl+→ jumps a word', editKey(S('ssh -o Server', 0), K('ArrowRight', { ctrl: true })).pos, 3);
  eq('ctrl+shift+← selects a word', selectedText(editKey(S('ssh pi', 6), K('ArrowLeft', { ctrl: true, shift: true }))), 'pi');
  eq('ctrl+Backspace deletes a word', editKey(S('ssh old-host', 12), K('Backspace', { ctrl: true })), S('ssh old-', 8));
  eq('⌘+← is the window\'s', editKey(S('ab', 1), K('ArrowLeft', { meta: true })), null);
  eq('a plain letter is not a key here', editKey(S('ab', 1), K('x', { char: 'x' })), null);
  eq('double click on a word', wordAt('-o ServerAliveInterval=60', 8), [3, 22]);
  eq('double click just after a word', wordAt('ssh pi', 3), [0, 3]);
  eq('double click on punctuation', wordAt('a==b', 1), [1, 3]);
  eq('double click on an accented word', wordAt('año próximo', 6), [4, 11]);
  eq('no selection without an anchor', selRange(S('ab', 1)), null);
  eq('an anchor at the caret is no selection', selRange(S('ab', 1, 1)), null);
  console.log(`textfield-model selection: ${ok} checks`);
});
