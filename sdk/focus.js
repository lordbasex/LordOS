// focus.js — keyboard focus between the controls of a window (docs/DESIGN.md, "Keyboard focus"). Tab and Shift+Tab go
// round the focusable nodes of the window in front, in tree order and skipping hidden and disabled ones
// (sdk/focusmodel.js); while a sheet or prompt of the shell is open they go round its own instead. The focused node
// gets a ring in the theme's focusRing color (the accent when it has none), drawn only after the keyboard moved the
// focus: a press of the pointer ends it and hides the ring, and the next Tab goes on from where it pressed.
//
// The focused node sees each key first, through its onKeyDown prop (true: the key was used); Space and Enter then press
// it (its onClick, as a click would); the nodes it is inside get onKeyDown after it (a ScrollView's Page Down). A key
// used there reaches no listener (useKeyboard), so Space on a button in Files presses the button and opens no preview.
// Tab itself moves after the listeners, unless one called e.preventDefault(): the Terminal, the editor and a machine's
// screen keep it. A text field keeps the keys while it has them (sdk/components.jsx) and lets Tab go to the next
// control; a confirmation dialog in front (sdk/dialogkeys.js) keeps Escape, Enter and Tab for its own buttons.
//
// Host props on a <View>: focusable (a Tab stop), disabled (skipped), modal (Tab stays inside it while it is shown),
// onKeyDown(e) → true when used, onFocus() and onBlur() (the keyboard focus came and went), focusRing={false} (the node
// draws its own, as a TextInput does), onRevealFocus(rect) (a ScrollView brings a focused node inside it into sight).
import { useEffect, useRef } from 'react';
import { setKeyFocus, addListener, setFocusRing, keyboardLayer, onCommit, onShellKeyboard, modifiers } from '@lordos/runtime';
import { NO_FOCUS, focusAfter, nextStop, tabKey, activates, within, canFocus, isHidden } from './focusmodel.js';
import { dialogStack } from './dialogkeys.js';
import { windows, wm } from './windows.js';
import { shellTokens } from './theme.js';

const areas = new Map();      // token of an AppHost → { id, root, focused, ring }: the window's root node and ring color
const states = new Map();     // area root (a window's root node or a layer's container) → { focus, anchor, visible }
const stateOf = (root) => states.get(root) || NO_FOCUS;

// The text fields of components.jsx: owner() is null while no field has the keys, the box of the TextInput or TextArea
// that has them, or false for a field of an app's own (Notes, Mail), which keeps Tab; blur() takes the keys from it.
let text = { owner: () => null, blur() {} };
export function setTextFocusBridge(b) { text = b; }

const ringOf = (t) => t?.focusRing || t?.accent || '#0a84ff';

// The area with the keys: the shell's layer in front (search, a sheet, a prompt), else the focused window.
function area() {
  const layer = keyboardLayer();
  if (layer) return { root: layer, ring: ringOf(shellTokens.get()) };
  const id = wm.focused();
  let any = null;
  for (const a of areas.values()) {
    if (!a.root || a.root.deleted || !a.focused) continue;
    if (a.id === id) return a;
    any = any || a;
  }
  return any;
}

const hiddenUp = (n) => { for (let x = n; x; x = x.host) if (isHidden(x)) return true; return false; };

// ---- the ring ----
let ringed = null, ringColor = null;
function paint() {
  const a = area();
  const s = a ? stateOf(a.root) : NO_FOCUS;
  const n = s.visible && s.focus && dialogStack.owner() === null ? s.focus : null;
  const want = n && n.type === 'view' && n.props.focusRing !== false && !n.deleted ? n : null;
  const color = want ? a.ring : null;
  if (want === ringed && color === ringColor) return;
  if (ringed && !ringed.deleted) setFocusRing(ringed, null);
  ringed = want; ringColor = color;
  if (want) setFocusRing(want, color);
}

function setState(root, next) {
  const old = stateOf(root);
  states.set(root, next);
  if (old.focus !== next.focus) {
    try { old.focus?.props?.onBlur?.(); } catch (e) { console.error('focus:', e); }
    try { next.focus?.props?.onFocus?.(); } catch (e) { console.error('focus:', e); }
  }
  paint();
}

// A ScrollView the node is in scrolls it into sight (the nearest one).
function reveal(n, root) {
  let r;
  try { const [x, y, w, h] = lord.getLayout(n.id); r = { x, y, w, h }; } catch (e) { return; }
  for (let p = n.host; p && p !== root; p = p.host) {
    if (typeof p.props?.onRevealFocus === 'function') { p.props.onRevealFocus(r); return; }
  }
}

// Space or Enter on a focused button: its onClick, and its ancestors' as a click bubbles, at the middle of its box.
function press(n) {
  let box = [0, 0, 0, 0];
  try { box = lord.getLayout(n.id); } catch (e) { /* not laid out yet */ }
  const m = modifiers();
  const ev = {
    type: 'click', x: box[0] + box[2] / 2, y: box[1] + box[3] / 2, button: 0, detail: 1, keyboard: true, target: n,
    mods: m, shift: !!(m & 1), alt: !!(m & 2), ctrl: !!(m & 4), meta: !!(m & 8), stopped: false,
    stopPropagation() { this.stopped = true; },
    local(inst) { const [lx, ly] = lord.getLayout(inst.id); return { x: ev.x - lx, y: ev.y - ly }; },
  };
  for (let p = n; p; p = p.parent) {
    const h = p.props.onClick;
    if (typeof h === 'function') { ev.currentTarget = p; h(ev); if (ev.stopped) break; }
  }
}

// Before the listeners: the focused node's keys. true takes the key.
function before(e) {
  if (e.forwarded || dialogStack.owner() !== null || text.owner() !== null) return false;
  const a = area();
  const n = a && stateOf(a.root).focus;
  if (!n || !canFocus(n) || !within(n, a.root) || hiddenUp(n)) return false;
  e.target = n;
  if (n.props.onKeyDown?.(e)) return true;
  if (activates(e) && typeof n.props.onClick === 'function') { if (!e.repeat) press(n); return true; }
  for (let p = n.host; p && p !== a.root; p = p.host) if (p.props?.onKeyDown?.(e)) return true;
  return false;
}

// After the listeners: Tab and Shift+Tab, unless a listener prevented it.
function after(e) {
  const d = tabKey(e);
  if (!d || e.forwarded || dialogStack.owner() !== null) return;
  const field = text.owner();
  if (field === false) return;
  const a = area();
  if (!a) return;
  const s = stateOf(a.root);
  const to = nextStop(a.root, field || s.focus || s.anchor, d < 0);
  if (!to) return;
  if (field && to !== field) text.blur();
  setState(a.root, focusAfter(s, { type: 'tab', to }));
  reveal(to, a.root);
}

setKeyFocus({ before, after });

// A press of the pointer ends the keyboard focus everywhere; Tab then goes on from where it pressed.
addListener('pointerdown', (e) => {
  for (const [root, s] of [...states]) {
    const anchor = e.target && within(e.target, root) ? e.target : s.focus || s.anchor;
    if (s.focus || s.visible || s.anchor !== anchor) setState(root, focusAfter(s, { type: 'pointer', target: anchor }));
  }
  const a = area();
  if (a && e.target && within(e.target, a.root) && !states.has(a.root)) states.set(a.root, focusAfter(NO_FOCUS, { type: 'pointer', target: e.target }));
});

// After each commit: a focused node that went away, got hidden or disabled loses the focus (the next Tab starts from
// the nearest node still there), and areas whose window closed are forgotten.
onCommit(() => {
  for (const [root, s] of [...states]) {
    if (root.deleted) { states.delete(root); continue; }
    const n = s.focus;
    if (!n || (canFocus(n) && !hiddenUp(n) && within(n, root))) continue;
    // parent stays set on a node React took out (host does not)
    let anchor = n;
    while (anchor && (anchor.deleted || !within(anchor, root))) anchor = anchor.parent;
    setState(root, focusAfter(s, { type: 'gone', anchor }));
  }
  if (ringed && ringed.deleted) { ringed = null; ringColor = null; }
  paint();
});
windows.subscribe(paint);
dialogStack.subscribe(paint);
onShellKeyboard(paint);

// AppHost's root node: the area Tab goes round while its window is focused. ring: the theme's focus ring color.
export function useFocusArea(id, focused, ring) {
  const ref = useRef(null);
  const token = useRef({}).current;
  useEffect(() => { areas.set(token, { id, root: ref.current, focused: !!focused, ring }); paint(); }, [id, focused, ring]);
  useEffect(() => () => { const a = areas.get(token); areas.delete(token); if (a?.root) states.delete(a.root); paint(); }, []);
  return ref;
}
export { ringOf as focusRingColor };
