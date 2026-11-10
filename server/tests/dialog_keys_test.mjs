// dialog_keys_test.mjs — which confirmation dialog takes the keys, and what a key does in it (sdk/dialogkeys.js).
import { defaultButton, dialogKey, keyOwner, blocksWindow, createDialogStack } from '../../sdk/dialogkeys.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const key = (k, mods = {}) => ({ key: k, ...mods });

// the default: Cancel for a destructive dialog (as in libadwaita), the action otherwise
eq('default: destructive', defaultButton(true), 'cancel');
eq('default: plain', defaultButton(false), 'ok');

// Enter does the default, never the destructive button
eq('enter: destructive cancels', dialogKey(key('Enter'), { destructive: true, focus: 'cancel' }), { press: 'cancel' });
eq('enter: destructive cancels with the focus on the action', dialogKey(key('Enter'), { destructive: true, focus: 'ok' }), { press: 'cancel' });
eq('enter: plain confirms', dialogKey(key('Enter'), { destructive: false, focus: 'ok' }), { press: 'ok' });
eq('enter: with Ctrl is not ours', dialogKey(key('Enter', { ctrl: true }), { destructive: false, focus: 'ok' }), null);
// Escape cancels, with or without modifiers
eq('escape: cancels', dialogKey(key('Escape'), { destructive: false, focus: 'ok' }), { press: 'cancel' });
eq('escape: with Shift', dialogKey(key('Escape', { shift: true }), { destructive: true, focus: 'ok' }), { press: 'cancel' });
// the destructive action: Tab to it, then Space
eq('tab: to the action', dialogKey(key('Tab'), { destructive: true, focus: 'cancel' }), { focus: 'ok' });
eq('shift+tab: back', dialogKey(key('Tab', { shift: true }), { destructive: true, focus: 'ok' }), { focus: 'cancel' });
eq('tab: with Alt is not ours', dialogKey(key('Tab', { alt: true }), { destructive: true, focus: 'cancel' }), null);
eq('space: the focused action', dialogKey(key(' '), { destructive: true, focus: 'ok' }), { press: 'ok' });
eq('space: the focused cancel', dialogKey(key(' '), { destructive: true, focus: 'cancel' }), { press: 'cancel' });
eq('space: no focus does the default', dialogKey(key(' '), { destructive: true }), { press: 'cancel' });
eq('other keys: not ours', dialogKey(key('a'), { destructive: false, focus: 'ok' }), null);
eq('arrow: not ours', dialogKey(key('ArrowLeft'), { destructive: false, focus: 'ok' }), null);

// who has the keys: the newest dialog in front
eq('owner: none open', keyOwner([]), null);
eq('owner: a window behind takes none', keyOwner([{ token: 'a', win: 1, focused: false }]), null);
eq('owner: the focused window', keyOwner([{ token: 'a', win: 1, focused: true }]), 'a');
eq('owner: the desktop\'s own is always in front', keyOwner([{ token: 's', win: null, focused: false }]), 's');
eq('owner: the newest of two in front', keyOwner([{ token: 'a', win: 1, focused: true }, { token: 'b', win: 1, focused: true }]), 'b');
eq('owner: a newer one behind does not take it', keyOwner([{ token: 'a', win: 1, focused: true }, { token: 'b', win: 2, focused: false }]), 'a');
eq('owner: the desktop\'s over a window', keyOwner([{ token: 'a', win: 1, focused: true }, { token: 's', win: null, focused: false }]), 's');

// the app's own keys (useKeyboard) wait while a dialog of its window or of the desktop is open
eq('blocks: its window', blocksWindow([{ token: 'a', win: 1, focused: true }], 1), true);
eq('blocks: another window', blocksWindow([{ token: 'a', win: 1, focused: true }], 2), false);
eq('blocks: its window behind', blocksWindow([{ token: 'a', win: 1, focused: false }], 1), false);
eq('blocks: the desktop\'s', blocksWindow([{ token: 's', win: null, focused: false }], 2), true);
eq('blocks: nothing open', blocksWindow([], 1), false);

// the stack: open, focus, close
const st = createDialogStack();
let changes = 0;
st.subscribe(() => changes++);
st.open('a', 1, true);
eq('stack: one open', st.owner(), 'a');
st.open('b', 2, false);
eq('stack: the newer is behind', st.owner(), 'a');
st.setFocused('a', false);
st.setFocused('b', true);
eq('stack: window 2 comes to the front', st.owner(), 'b');
eq('stack: window 1 is not blocked now', st.blocks(1), false);
eq('stack: window 2 is', st.blocks(2), true);
st.setFocused('b', true);
eq('stack: the same focus does not notify', changes, 4);
st.close('b');
eq('stack: closed', st.owner(), null);
st.setFocused('a', true);
eq('stack: back to the first', st.owner(), 'a');
st.close('a');
st.close('a');
eq('stack: empty', st.get(), []);
// a key a dialog acted on does not reach the app, even after the dialog closed
const ev = key('Escape');
st.take(ev);
eq('stack: the taken key', st.blocks(1, ev), true);
eq('stack: another key', st.blocks(1, key('Escape')), false);
eq('stack: taken', st.taken(ev), true);

console.log(`dialog-keys: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
