// textfocus.js — does a text field of this process have the focus, and where is its caret? Only that:
// never the text. TextInput (components.jsx) and custom fields through the input method (ime.js)
// report here; the on-screen keyboard listens (osk.js). In an isolated app's process there is no
// keyboard: the hint goes to the session through the broker, which checks nothing else can come of it.
import { onCommit, onScreenChange } from '@lordos/runtime';

const sources = { input: null, ime: null };    // a function → the caret, or null without focus
const subs = new Set();

export const textFieldActive = () => !!(sources.input || sources.ime);
export const textFieldCaret = () => {
  try { return sources.input?.() || sources.ime?.() || null; } catch (e) { return null; }
};
export function onTextField(fn) { subs.add(fn); return () => subs.delete(fn); }

const inApp = () => typeof lord !== 'undefined' && typeof lord.appId === 'string' && !!lord.broker?.textFocus;
let sent = '';
let timer = 0;
function tellSession(again = true) {
  if (timer) return;
  // the caret is drawn after the render that gives the focus
  timer = setTimeout(() => {
    timer = 0;
    const c = textFieldCaret();
    const on = textFieldActive(), y = c ? Math.round(c.y) : -1, h = c ? Math.round(c.h) : 0;
    const now = `${on}|${y}|${h}`;
    if (now !== sent) { sent = now; lord.broker.textFocus(on, y, h); }
    // a field focused while its window opens has no place yet: once more, after the layout settles
    if (again) setTimeout(() => tellSession(false), 250);
  }, 30);
}

// The field can move and keep the focus: a row appears above it, or the window shrinks for the keyboard
// and a centered form centers again. The session can't see into this process, so it is told. The caret
// is read only while a field has the focus, and a position that didn't change sends nothing.
const fieldMayHaveMoved = () => { if (inApp() && textFieldActive()) tellSession(); };
onCommit(fieldMayHaveMoved);
onScreenChange(fieldMayHaveMoved);

export function noteTextField(kind, caret) {
  sources[kind] = caret || null;
  for (const fn of subs) fn();
  if (inApp()) tellSession();
}
