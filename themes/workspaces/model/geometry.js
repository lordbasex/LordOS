// geometry.js — where Workspaces puts its pieces on each display, as plain functions of the display (no runtime or
// React, so server/tests/workspaces_model_test.mjs checks them without a screen).
//
//   spineMode(pref, width)          'expanded' | 'collapsed' for the Context Spine on a display that wide
//   shownDisplays(list, mirror)     the displays that get their own group frame (mirrored ones share the main one's)
//   displayPlan(d, opts)            the rectangles of one display: { spine, top, frame, header, work, mode }
//
// One display, left to right (right to left mirrors the sides; physical sides stay physical in the work area):
//
//   ┌─ MARGIN ────────────────────────────────────────────────┐
//   │ ┌─────┐  ┌ activity strip ──────────┐   ┌ status ─────┐ │  top: TOP_H, only on the main display
//   │ │spine│  ┌ group frame ─────────────────────────────────┐ │
//   │ │     │  │ header (activity, layout, actions) HEADER_H  │ │
//   │ │     │  │ ┌ work area: the tiles ───────────────────┐  │ │  work = what configureWindows gets
//   │ └─────┘  │ └──────────────────────────────────────────┘  │ │
//   │          └───────────────────────────────────────────────┘ │
//   └──────────────────────────────────────────────────────────┘
//
// The spine blurs what is behind it (backdropFilter reads a margin of twice its radius around its painted box, its
// shadow included; docs/LAYERS.md). SPINE_GAP plus FRAME_PAD keeps every window and the top band past that margin, so
// an app redrawing next to the spine, or the strip's glide, never makes the whole spine repaint (measured: a strip
// inside the margin turned each glide frame into a repaint of the spine's whole height).
export const MARGIN = 12;
export const SPINE_GAP = 14;
export const TOP_H = 44;
export const TOP_GAP = 10;
export const FRAME_PAD = 20;
export const HEADER_H = 40;
export const SPINE_BLUR = 8;
// how far the spine's shadow reaches past its edge (tokens.js spineShadow): the engine repaints a blurred node's painted
// box, shadow included, plus twice its blur radius around it
export const SPINE_SHADOW_REACH = 0;
// how far a window's shadow reaches past its left edge (tokens.js shadow: 16 px of blur)
export const WINDOW_SHADOW = 16;
export const SPINE_W = { expanded: 80, collapsed: 56 };
// below this width (logical px) the spine shows icons only when its setting is Automatic
export const SMALL_W = 1440;
export const SPINE_PREFS = ['auto', 'expanded', 'collapsed'];

export function spineMode(pref, width) {
  if (pref === 'expanded' || pref === 'collapsed') return pref;
  return width < SMALL_W ? 'collapsed' : 'expanded';
}

// Mirrored displays show the same picture: only the main one is laid out (the others are the same pixels).
export function shownDisplays(list, mirror) {
  const all = Array.isArray(list) ? list.filter(Boolean) : [];
  if (!mirror) return all;
  const main = all.find((d) => d.main) || all[0];
  return main ? [main] : [];
}

const rect = (x, y, w, h) => ({ x: Math.round(x), y: Math.round(y), w: Math.max(0, Math.round(w)), h: Math.max(0, Math.round(h)) });

// d: { x, y, w, h, main }. opts: { spine: pref, every: spine on every display, rtl, primary: has the top band (the
// main display), collapsed: an explicit collapse from the spine's own button (wins over the width) }.
export function displayPlan(d, { spine = 'auto', every = false, rtl = false, primary = !!d.main } = {}) {
  const withSpine = primary || !!every;
  const mode = spineMode(spine, d.w);
  const sw = withSpine ? SPINE_W[mode] : 0;
  const side = withSpine ? sw + SPINE_GAP : 0;
  const spineRect = withSpine ? rect(rtl ? d.x + d.w - MARGIN - sw : d.x + MARGIN, d.y + MARGIN, sw, d.h - 2 * MARGIN) : null;
  const left = d.x + MARGIN + (rtl ? 0 : side);
  const right = d.x + d.w - MARGIN - (rtl ? side : 0);
  // the top band lines up with the work area, so what moves in it (the strip's glide) stays out of the margin the
  // spine's blur reads: a change there would repaint the whole spine (docs/LAYERS.md)
  const top = primary ? rect(left + FRAME_PAD, d.y + MARGIN, right - left - 2 * FRAME_PAD, TOP_H) : null;
  const fy = d.y + MARGIN + (primary ? TOP_H + TOP_GAP : 0);
  const frame = rect(left, fy, right - left, d.y + d.h - MARGIN - fy);
  const header = rect(frame.x + FRAME_PAD, frame.y, frame.w - 2 * FRAME_PAD, HEADER_H);
  const work = rect(frame.x + FRAME_PAD, frame.y + HEADER_H, frame.w - 2 * FRAME_PAD, frame.h - HEADER_H - FRAME_PAD);
  return { spine: spineRect, top, frame, header, work, mode: withSpine ? mode : null };
}

// Is (x, y) on one of the theme's own bars (the spine or the top band)? A click there doesn't close a panel: its button
// toggles it.
export function onChrome(plans, x, y) {
  const inside = (r) => !!r && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
  return plans.some((p) => inside(p.spine) || inside(p.top));
}
