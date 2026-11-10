// primary_model_test.mjs — the primary selection of the SDK's text fields (sdk/primarymodel.js): what is published when
// a selection is made, and what a middle click pastes where.
import { primaryStep, pasteAt } from '../../sdk/primarymodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

// publishing
const s = { value: 'hello world', pos: 11, anchor: 6 };
let st = primaryStep(null, s);
eq('a selection is published', st.text, 'world');
eq('the same selection is not published twice', primaryStep(st.last, s).text, '');
eq('the same selection made from the other end is not either', primaryStep(st.last, { ...s, pos: 6, anchor: 11 }).text, '');
eq('another selection is', primaryStep(st.last, { ...s, anchor: 0, pos: 5 }).text, 'hello');
eq('no selection publishes nothing', primaryStep(st.last, { ...s, anchor: null }), { last: null, text: '' });
eq('an empty selection publishes nothing', primaryStep(null, { ...s, anchor: 11 }).text, '');
st = primaryStep(primaryStep(st.last, { ...s, anchor: null }).last, s);
eq('once let go, the same selection is published again', st.text, 'world');
eq('never from a password field', primaryStep(null, s, true), { last: null, text: '' });
eq('code points, not UTF-16 units', primaryStep(null, { value: 'a😀b', pos: 2, anchor: 1 }).text, '😀');
eq('a multi-line selection keeps its breaks', primaryStep(null, { value: 'one\ntwo', pos: 7, anchor: 0 }).text, 'one\ntwo');

// pasting with a middle click
eq('pastes at the click point', pasteAt({ value: 'hello world', pos: 0, anchor: null }, 5, ','), { value: 'hello, world', pos: 6, anchor: null });
eq('not over the selection, which is let go', pasteAt({ value: 'hello world', pos: 11, anchor: 6 }, 0, '> '),
  { value: '> hello world', pos: 2, anchor: null });
eq('past the end goes at the end', pasteAt({ value: 'ab', pos: 0, anchor: null }, 99, 'c'), { value: 'abc', pos: 3, anchor: null });
eq('a one-line field gets spaces for line breaks', pasteAt({ value: '', pos: 0, anchor: null }, 0, 'a\nb\r\nc', true).value, 'a b c');
eq('a multi-line field keeps them (CRLF as LF)', pasteAt({ value: '', pos: 0, anchor: null }, 0, 'a\r\nb\rc').value, 'a\nb\nc');
eq('nothing to paste changes nothing', pasteAt({ value: 'x', pos: 1, anchor: 0 }, 0, ''), { value: 'x', pos: 1, anchor: 0 });
eq('positions count code points', pasteAt({ value: '😀😀', pos: 0, anchor: null }, 1, 'x'), { value: '😀x😀', pos: 2, anchor: null });

console.log(`primary-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
