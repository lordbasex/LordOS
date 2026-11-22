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

// The tallest a Popover may be on the side it opens to: from the anchor's edge (with the 4 px gap) to the area's edge,
// less an 8 px margin; never under 80 so a cramped spot still shows a few rows (the content scrolls).
export function popoverRoom(anchor, area, up) {
  const room = up ? anchor.y - area.y - 12 : area.y + area.h - (anchor.y + anchor.h) - 12;
  return Math.max(80, Math.floor(room));
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

// ---- the disclosure triangle (issue #143) ----
// One drawn chevron for every app's folders and groups (Mail, Files, Monitor, Terminal), so it no longer depends on a
// font's small ▸ ▾. Open: it points down; closed: toward the text, right in left to right and left in right to left.
export const DISCLOSURE_SIZE = 12;      // the chevron's long side, in px
export const DISCLOSURE_BOX = 16;       // the hit box's width
export const disclosureDir = (open, rtl = false) => (open ? 'down' : rtl ? 'left' : 'right');

// The square whose two borders (right and bottom) draw the chevron in a width × height box: its side, stroke,
// rotation and top-left corner. Rotated, the strokes sit off the square's centre by side / (2√2) toward the tip, so
// the square moves back by that much and the visible chevron is the one centred in the box.
export function chevronGeometry(dir, size = DISCLOSURE_SIZE, width = DISCLOSURE_BOX, height = DISCLOSURE_BOX) {
  const side = Math.max(4, Math.round(size / Math.SQRT2));
  const stroke = size >= 11 ? 1.75 : 1.5;
  const off = side / (2 * Math.SQRT2);
  const deg = { right: -45, down: 45, left: 135, up: -135 }[dir] ?? -45;
  const dx = dir === 'right' ? -off : dir === 'left' ? off : 0;
  const dy = dir === 'down' ? -off : dir === 'up' ? off : 0;
  const r = (v) => Math.round(v * 100) / 100;
  return { side, stroke, deg, left: r(width / 2 + dx - side / 2), top: r(height / 2 + dy - side / 2) };
}
