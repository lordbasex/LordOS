// controlmodel.js — what the keys and the wheel do to the SDK's choice controls (sdk/controls.jsx: Dropdown, Tabs,
// RadioGroup, Tooltip, ProgressBar; sdk/components.jsx: Slider and Stepper), with no screen. Pure logic: tests in
// server/tests/control_model_test.mjs. The keys of sliders, steppers, Segmented and scroll views are in
// sdk/focusmodel.js.

import { menuStep as stepChoice } from './focusmodel.js';

const plain = (e) => !!e && !e.ctrl && !e.meta && !e.alt;

// Tabs (vertical false) and a radio group (vertical true): the index after key e, or null when the key is not one of
// theirs. ← → follow the row, swapped in right to left where the first tab is on the right; a radio group also takes
// ↑ ↓. They go round, as in a desktop's tab bar, and Home and End go to the first and the last.
export function groupKey(e, { index, count, rtl = false, vertical = false, skip = () => false }) {
  if (!plain(e) || !(count > 0)) return null;
  const next = rtl ? 'ArrowLeft' : 'ArrowRight', prev = rtl ? 'ArrowRight' : 'ArrowLeft';
  let i;
  switch (e.key) {
    case next: i = stepChoice(count, index, 1, skip); break;
    case prev: i = stepChoice(count, index, -1, skip); break;
    case 'ArrowDown': if (!vertical) return null; i = stepChoice(count, index, 1, skip); break;
    case 'ArrowUp': if (!vertical) return null; i = stepChoice(count, index, -1, skip); break;
    case 'Home': i = stepChoice(count, -1, 1, skip); break;
    case 'End': i = stepChoice(count, -1, -1, skip); break;
    default: return null;
  }
  return i < 0 ? null : i;
}

// A Dropdown: what key e does, or null when it is not one of its keys. Closed, Space, Enter, ↓ and ↑ open it with the
// current choice lit ({ open: true, lit }); open, ↓ ↑ move the light without going round, Home and End (Page Up and
// Page Down) go to the ends, Enter and Space choose the lit one ({ open: false, choose: lit }), and Escape closes it
// ({ open: false }). Tab closes it too but goes on to the next control (pass: true). Letters are type-ahead
// (sdk/menunav.js), handled by the control.
export function dropdownKey(e, { open, lit, current = -1, count, skip = () => false }) {
  if (!plain(e)) return null;
  const k = e.key;
  if (!open) {
    if (k === ' ' || k === 'Enter' || k === 'ArrowDown' || k === 'ArrowUp') {
      return { open: true, lit: current >= 0 && !skip(current) ? current : stepChoice(count, -1, 1, skip) };
    }
    return null;
  }
  const from = lit >= 0 ? lit : current;
  const clampStep = (d) => {
    for (let i = from + d; i >= 0 && i < count; i += d) if (!skip(i)) return i;
    return from >= 0 ? from : stepChoice(count, -1, d, skip);
  };
  switch (k) {
    case 'ArrowDown': return { open: true, lit: clampStep(1) };
    case 'ArrowUp': return { open: true, lit: clampStep(-1) };
    case 'Home': case 'PageUp': return { open: true, lit: stepChoice(count, -1, 1, skip) };
    case 'End': case 'PageDown': return { open: true, lit: stepChoice(count, -1, -1, skip) };
    case 'Enter': case ' ': return lit >= 0 && !skip(lit) ? { open: false, choose: lit } : { open: false };
    case 'Escape': return { open: false };
    case 'Tab': return { open: false, pass: true };
    default: return null;
  }
}

// The wheel over a Slider or a Stepper: the value after wheel event e, or null when it does not move it. A notch of the
// wheel up (notches > 0) adds a step, down takes one; a touchpad's swipe (precise) counts only while the control has
// the keyboard focus (so a page that scrolls past a slider keeps scrolling) and never while it glides on by inertia.
export function wheelValue(e, { value, min = -Infinity, max = Infinity, step = 1, focused = false }) {
  if (!e || !e.notches) return null;
  if (e.precise && (!focused || e.momentum)) return null;
  const v = Math.max(min, Math.min(max, value + e.notches * step));
  return Number.isFinite(v) ? v : null;
}

// Where a tooltip goes: below its anchor ({ x, y, w, h }), or above it when the window's area has no room below (or
// when the bubble below would cover the pointer, a point; the pointer is inside the anchor while it hovers, so this
// only matters for a tooltip shown by the keyboard focus). size: { w, h } of the bubble; gap between the two. Returns
// 'below' or 'above'.
export function tipSide(anchor, size, area, pointer = null, gap = 6) {
  const below = { x: anchor.x, y: anchor.y + anchor.h + gap, w: size.w, h: size.h };
  const above = { x: anchor.x, y: anchor.y - gap - size.h, w: size.w, h: size.h };
  const fits = (r) => !area || (r.y >= area.y && r.y + r.h <= area.y + area.h);
  const covers = (r) => !!pointer && pointer.x >= r.x && pointer.x < r.x + r.w && pointer.y >= r.y && pointer.y < r.y + r.h;
  const ok = (r) => fits(r) && !covers(r);
  if (ok(below)) return 'below';
  if (ok(above)) return 'above';
  return covers(below) && !covers(above) ? 'above' : fits(below) || !fits(above) ? 'below' : 'above';
}

// A ProgressBar: the filled part for value (0..1; null or undefined for indeterminate). Determinate: from the start
// edge to the value ({ from: 0, to }). Indeterminate: a band a third of the bar wide that slides across and comes back
// in at the start, at time ms (period: one pass); with reduced motion it stays still over the whole bar
// ({ still: true }).
export function progressSpan(value, ms = 0, { period = 1500, band = 0.3, reduceMotion = false } = {}) {
  if (value != null && Number.isFinite(value)) return { from: 0, to: Math.max(0, Math.min(1, value)) };
  if (reduceMotion) return { from: 0, to: 1, still: true };
  const f = ((ms % period) + period) % period / period;
  const head = f * (1 + band);
  return { from: Math.max(0, head - band), to: Math.min(1, head) };
}
