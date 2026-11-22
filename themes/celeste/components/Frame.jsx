// The screen frame: a border around a display (as wide as the bar on its side) with rounded inner corners. The
// engine has no masks, so each concave corner (a "fillet") is a ring clipped to an r × r square: the ring's inner edge
// is the corner's curve and its body fills the square's outer corner. Two nodes per corner.
import { View } from '@lordos/sdk';
import { memo } from 'react';
import { BAR_W, FRAME, R } from '../model.js';

// An r × r square at (x, y) whose corner `corner` ('tl' | 'tr' | 'bl' | 'br') is solid, scooped by a circle of radius
// r centred on the opposite corner.
export const Fillet = memo(function Fillet({ x, y, r = R, corner, color }) {
  const k = Math.ceil(r * 0.42) + 1;            // the ring reaches past the square's corner (r·√2 < r + k)
  const o = r + k;
  const cx = corner[1] === 'l' ? r : 0;         // the circle's centre, inside the square
  const cy = corner[0] === 't' ? r : 0;
  return (
    <View style={{ position: 'absolute', left: x, top: y, width: r, height: r, overflow: 'hidden', pointerEvents: 'none' }}>
      <View style={{ position: 'absolute', left: cx - o, top: cy - o, width: o * 2, height: o * 2, borderRadius: o, borderWidth: k, borderColor: color }} />
    </View>
  );
});

// area: the display; rtl: the bar (the thick side) is on the right.
export const Frame = memo(function Frame({ color, area, rtl }) {
  const { x, y, w, h } = area;
  const start = BAR_W, end = FRAME;
  const left = rtl ? end : start, right = rtl ? start : end;
  const ix = x + left, iy = y + FRAME, iw = w - left - right, ih = h - FRAME * 2;
  const strip = (l, tp, ww, hh) => ({ position: 'absolute', left: l, top: tp, width: ww, height: hh, backgroundColor: color });
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, width: 0, height: 0, pointerEvents: 'box-none' }}>
      <View style={strip(x, y, w, FRAME)} />
      <View style={strip(x, y + h - FRAME, w, FRAME)} />
      <View style={strip(x, iy, left, ih)} />
      <View style={strip(x + w - right, iy, right, ih)} />
      <Fillet x={ix} y={iy} corner="tl" color={color} />
      <Fillet x={ix + iw - R} y={iy} corner="tr" color={color} />
      <Fillet x={ix} y={iy + ih - R} corner="bl" color={color} />
      <Fillet x={ix + iw - R} y={iy + ih - R} corner="br" color={color} />
    </View>
  );
});
