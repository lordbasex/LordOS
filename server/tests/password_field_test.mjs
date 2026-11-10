// password_field_test.mjs — the eye and the Caps Lock hint of a password field (sdk/passwordfieldmodel.js).
import { revealAfter, eyeShown, maskedText, eyeLabel, capsAfter, capsHint, capsFits } from '../../sdk/passwordfieldmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

// shown and hidden
eq('a click on the eye shows', revealAfter(false, 'toggle'), true);
eq('a second click hides', revealAfter(true, 'toggle'), false);
eq('losing focus hides', revealAfter(true, 'blur'), false);
eq('losing focus keeps it hidden', revealAfter(false, 'blur'), false);
eq('Enter hides', revealAfter(true, 'submit'), false);
eq('no longer a password field: hidden for when it is one again', revealAfter(true, 'unsecure'), false);
eq('anything else leaves it', revealAfter(true, 'key'), true);
eq('anything else leaves it hidden', revealAfter(false, undefined), false);
// a window minimized moves the focus away (components.jsx clearTextFocus): a blur, so it is hidden again
let s = revealAfter(false, 'toggle');
s = revealAfter(s, 'blur');
eq('shown, the window hidden, back: hidden', s, false);

// the eye
eq('a password field has the eye', eyeShown({ secure: true }), true);
eq('one with its own Show does not', eyeShown({ secure: true, eye: false }), false);
eq('a plain field has none', eyeShown({ secure: false }), false);
eq('no props: none', eyeShown(), false);
eq('the label of a hidden one', eyeLabel(false), 'shell.password.show');
eq('the label of a shown one', eyeLabel(true), 'shell.password.hide');

// what is drawn: one dot per code point
eq('hidden: dots', maskedText('abc', false), '•••');
eq('hidden: one dot per code point', maskedText('a😀ñ', false), '•••');
eq('shown: the text', maskedText('abc', true), 'abc');
eq('empty', maskedText('', false), '');
eq('nothing', maskedText(undefined, true), '');

// Caps Lock
eq('a key that says it is on', capsAfter(false, { caps: true }), true);
eq('a key that says it is off', capsAfter(true, { caps: false }), false);
eq('an event that does not say keeps it', capsAfter(true, { key: 'a' }), true);
eq('no event keeps it', capsAfter(false, null), false);
eq('focused password field with Caps Lock: the hint', capsHint({ secure: true, focused: true, caps: true }), true);
eq('not focused: no hint', capsHint({ secure: true, focused: false, caps: true }), false);
eq('Caps Lock off: no hint', capsHint({ secure: true, focused: true, caps: false }), false);
eq('a plain field: no hint', capsHint({ secure: false, focused: true, caps: true }), false);
eq('no props: no hint', capsHint(), false);
eq('the words fit', capsFits(200, 40, 90), true);
eq('the words do not fit beside a long password', capsFits(200, 120, 90), false);
eq('no layout yet: only the glyph', capsFits(0, 0, 90), false);

console.log(`password_field_test: ${ok}/${ok + bad} checks`);
if (bad) process.exit(1);
