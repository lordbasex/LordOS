// passwordfieldmodel.js — the eye and the Caps Lock hint of a password field (a TextInput with `secure`, components.jsx),
// no screen. The eye shows the text for a moment; the field stays a password field all along: no undo history, no input
// method, nothing copied or cut, and the text is hidden again as soon as the field loses the keyboard.
//   revealAfter(shown, what)        → is the text shown after `what`: 'toggle' (a click on the eye) flips it; 'blur' (the
//                                     field lost focus, also because its window was minimized or left the desktop in view),
//                                     'unsecure' (the field stopped being a password field) and 'submit' hide it
//   eyeShown({ secure, eye })       → does the field draw the eye (eye: false for a field whose app has its own Show)
//   maskedText(value, shown)        → what the field draws: the value, or one • per code point
//   eyeLabel(shown)                 → the i18n key of what a click on the eye does ('shell.password.show' or '…hide')
//   capsAfter(caps, e)              → Caps Lock after a key event: e.caps when the event says (runtime/keymap.js), else as it was
//   capsHint({ secure, focused, caps }) → is "Caps Lock is on" shown: a focused password field with Caps Lock on
//   capsFits(room, textW, labelW)   → does the hint fit in words beside the text (room: the field's width for both), or only as ⇪

export function revealAfter(shown, what) {
  if (what === 'toggle') return !shown;
  if (what === 'blur' || what === 'unsecure' || what === 'submit') return false;
  return !!shown;
}

export const eyeShown = ({ secure, eye } = {}) => !!secure && eye !== false;

export const maskedText = (value, shown) => (shown ? value || '' : '•'.repeat([...(value || '')].length));

export const eyeLabel = (shown) => (shown ? 'shell.password.hide' : 'shell.password.show');

export const capsAfter = (caps, e) => (e && typeof e.caps === 'boolean' ? e.caps : !!caps);

export const capsHint = ({ secure, focused, caps } = {}) => !!(secure && focused && caps);

// the words need their own width and a gap after the text typed so far; a narrow field keeps just the ⇪
export const capsFits = (room, textW, labelW) => room > 0 && textW + labelW + 12 <= room;
