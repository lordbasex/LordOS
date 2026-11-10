// focusmodel.js — keyboard focus between the controls of a window (sdk/focus.js): the Tab order over the node tree,
// the next and the previous stop, the focus-visible state, and what a key does to a slider, a stepper, a list of options
// or a scroll view. Pure logic, with no lord.* calls: tests in server/tests/focus_model_test.mjs.
//
// A node is the runtime's instance (runtime/index.js): { props, style, host, first, last, prev, next, hidden, deleted }.
// host is the node it hangs from, first and last its children and prev and next its siblings, in tree order; hidden is
// set while React hides it (Suspense), deleted once it is gone.

const plain = (e) => !e.ctrl && !e.meta && !e.alt;

// Out of sight: display none, or hidden by React.
export const isHidden = (n) => !!n && (!!n.hidden || n.style?.display === 'none');

// A Tab stop: a node with the focusable prop that is not disabled and still mounted.
export const canFocus = (n) => !!n?.props?.focusable && !n.props.disabled && !n.deleted;

// The nodes under root in tree order (depth first, a node before its children), leaving out hidden subtrees.
export function treeOrder(root) {
  const out = [];
  if (!root || isHidden(root)) return out;
  const stack = [root];
  while (stack.length) {
    const n = stack.pop();
    out.push(n);
    for (let k = n.last; k; k = k.prev) if (!isHidden(k)) stack.push(k);
  }
  return out;
}

// Is n inside root (or root itself)? It follows host, the node each one hangs from.
export function within(n, root) {
  for (let x = n; x; x = x.host) if (x === root) return true;
  return false;
}

// Where Tab goes: inside the last open node with the modal prop (a sheet drawn in the window), else the whole root.
export function tabArea(root) {
  const list = treeOrder(root);
  for (let i = list.length - 1; i > 0; i--) if (list[i].props?.modal) return list[i];
  return root;
}

// The Tab stops under root, in tree order.
export const tabStops = (root) => treeOrder(tabArea(root)).filter(canFocus);

// Where Tab (Shift+Tab: back) goes from `from`: the next stop after it in tree order, round to the first one (the last
// one going back). from may be a node that is not a stop (where the pointer last pressed): the stop after it then.
// A from that is not in sight under root (none yet, hidden, in another window) starts at the first or the last stop.
// null when root has no stop at all.
export function nextStop(root, from, back) {
  const area = tabArea(root);
  const list = treeOrder(area);
  const stops = list.filter(canFocus);
  if (!stops.length) return null;
  const at = from ? list.indexOf(from) : -1;
  if (at < 0) return back ? stops[stops.length - 1] : stops[0];
  const d = back ? -1 : 1;
  for (let k = 1; k <= list.length; k++) {
    const n = list[(at + d * k + list.length * 2) % list.length];
    if (canFocus(n)) return n;
  }
  return null;
}

// Tab: 1, Shift+Tab: -1, anything else (Ctrl+Tab and ⌥Tab belong to apps and the shell): 0.
export const tabKey = (e) => (e.key === 'Tab' && plain(e) ? (e.shift ? -1 : 1) : 0);

// Space and Enter press a focused button or toggle.
export const activates = (e) => plain(e) && (e.key === ' ' || e.key === 'Enter');

// ---- the focus-visible state ----
// One area (a window, a shell sheet) has { focus, anchor, visible }: the focused node, the node Tab starts from (the
// focus, or where the pointer pressed last) and whether the ring is drawn. As :focus-visible in a browser, only the
// keyboard moves the focus and shows the ring; a press of the pointer ends the focus and hides it, and the next Tab
// goes on from where it pressed. When the focused node goes away the next Tab starts from the nearest node still there.
export const NO_FOCUS = Object.freeze({ focus: null, anchor: null, visible: false });

export function focusAfter(s, ev) {
  switch (ev.type) {
    case 'tab': return ev.to ? { focus: ev.to, anchor: ev.to, visible: true } : s;
    case 'pointer': return { focus: null, anchor: ev.target || null, visible: false };
    case 'gone': return { focus: null, anchor: ev.anchor || null, visible: false };
    default: return s;
  }
}

// ---- what keys do to the controls ----

// A slider or a stepper: the value after key e, or null when the key is not one of its own. ← → (swapped in right to
// left, where the start is on the right) and ↓ ↑ move a step, Page Down and Page Up a page, Home and End go to the ends
// (when they are finite). The value stays between min and max.
export function rangeKey(e, { value, min = -Infinity, max = Infinity, step = 1, page = step * 10, rtl = false }) {
  if (!plain(e)) return null;
  const fwd = rtl ? 'ArrowLeft' : 'ArrowRight', bwd = rtl ? 'ArrowRight' : 'ArrowLeft';
  let v;
  switch (e.key) {
    case fwd: case 'ArrowUp': v = value + step; break;
    case bwd: case 'ArrowDown': v = value - step; break;
    case 'PageUp': v = value + page; break;
    case 'PageDown': v = value - page; break;
    case 'Home': if (!Number.isFinite(min)) return null; v = min; break;
    case 'End': if (!Number.isFinite(max)) return null; v = max; break;
    default: return null;
  }
  return Math.max(min, Math.min(max, v));
}

// A row of choices (Segmented): the index after key e among n of them, or null. ← → follow the row (swapped in right
// to left), Home and End go to the first and the last.
export function choiceKey(e, { index, count, rtl = false }) {
  if (!plain(e) || !(count > 0)) return null;
  const next = rtl ? 'ArrowLeft' : 'ArrowRight', prev = rtl ? 'ArrowRight' : 'ArrowLeft';
  const i = index < 0 ? -1 : index;
  switch (e.key) {
    case next: case 'ArrowDown': return Math.min(count - 1, i + 1);
    case prev: case 'ArrowUp': return Math.max(0, i < 0 ? 0 : i - 1);
    case 'Home': return 0;
    case 'End': return count - 1;
    default: return null;
  }
}

// An open menu or list: the highlighted item after ↓ (d 1) or ↑ (d -1) from index, skipping the ones skip(i) says
// (separators, disabled items) and going round. -1 when no item can be highlighted.
export function menuStep(count, index, d, skip = () => false) {
  for (let k = 1; k <= count; k++) {
    const i = index < 0 ? (d > 0 ? k - 1 : count - k) : (((index + d * k) % count) + count) % count;
    if (!skip(i)) return i;
  }
  return -1;
}

// A scroll view: the offset after key e, or null. view and content are its length and its content's, along the axis
// it scrolls. ↓ ↑ (→ ← for a horizontal one, swapped in right to left) move a line, Page Down and Page Up (Space and
// Shift+Space) most of a view, Home and End go to the ends.
export function scrollKey(e, { offset, view, content, line = 40, horizontal = false, rtl = false }) {
  if (!plain(e)) return null;
  const max = Math.max(0, content - view);
  if (!(max > 0)) return null;
  const pg = Math.max(line, view - line);
  const fwd = horizontal ? (rtl ? 'ArrowLeft' : 'ArrowRight') : 'ArrowDown';
  const bwd = horizontal ? (rtl ? 'ArrowRight' : 'ArrowLeft') : 'ArrowUp';
  let v;
  switch (e.key) {
    case fwd: v = offset + line; break;
    case bwd: v = offset - line; break;
    case 'PageDown': v = offset + pg; break;
    case 'PageUp': v = offset - pg; break;
    case ' ': v = offset + (e.shift ? -pg : pg); break;
    case 'Home': v = 0; break;
    case 'End': v = max; break;
    default: return null;
  }
  return Math.max(0, Math.min(max, v));
}
