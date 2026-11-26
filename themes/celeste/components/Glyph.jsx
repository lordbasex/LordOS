// Celeste's line icons, drawn from views (no image files): any colour, any size, on a 20 × 20 grid with a 2 px
// stroke. Memoized: a bar that re-renders doesn't rebuild them. Wi-Fi, Bluetooth, the sun and the moon come from the
// SDK's quick-settings glyphs instead (sdk/quickglyphs.jsx).
import { View } from '@lordos/sdk';
import { memo } from 'react';

export const Glyph = memo(function Glyph({ name, size = 18, color = '#938f99' }) {
  const k = size / 20;
  const c = color;
  const L = [];
  let n = 0;
  const key = () => `g${n++}`;
  const ring = (x, y, d, w = 2, col = c, extra) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: d * k, height: d * k, borderRadius: (d * k) / 2, borderWidth: w * k, borderColor: col, ...extra }} />
  );
  const dot = (x, y, d, col = c) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: d * k, height: d * k, borderRadius: (d * k) / 2, backgroundColor: col }} />
  );
  const box = (x, y, w, h, r = 0, col = c, extra) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: w * k, height: h * k, borderRadius: r * k, backgroundColor: col, ...extra }} />
  );
  const outline = (x, y, w, h, r, wd = 2, col = c) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: w * k, height: h * k, borderRadius: r * k, borderWidth: wd * k, borderColor: col }} />
  );
  // a line from (ax,ay) to (bx,by)
  const seg = (ax, ay, bx, by, w = 2, col = c) => {
    const len = Math.hypot(bx - ax, by - ay);
    const cx = (ax + bx) / 2, cy = (ay + by) / 2;
    const ang = (Math.atan2(by - ay, bx - ax) * 180) / Math.PI;
    return (
      <View key={key()} style={{ position: 'absolute', left: (cx - len / 2) * k, top: (cy - w / 2) * k, width: len * k, height: w * k, borderRadius: (w / 2) * k, backgroundColor: col, transform: `rotate(${ang}deg)` }} />
    );
  };
  // a clipped window onto shapes drawn inside it (for half rings)
  const clip = (x, y, w, h, kids) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: w * k, height: h * k, overflow: 'hidden' }}>{kids}</View>
  );
  const inner = (x, y, d, w = 2) => (
    <View key={key()} style={{ position: 'absolute', left: x * k, top: y * k, width: d * k, height: d * k, borderRadius: (d * k) / 2, borderWidth: w * k, borderColor: c }} />
  );

  switch (name) {
    case 'power':
      L.push(ring(3, 3.5, 14, 2), seg(10, 1.5, 10, 10, 2.4));
      break;
    case 'lock':
      L.push(outline(6, 2.5, 8, 10, 4, 2), box(4, 9, 12, 9, 3));
      break;
    case 'wifi':
    case 'wifi-off':
      L.push(
        clip(0, 2, 20, 10, [inner(1, 0, 18, 2), inner(4.5, 3.5, 11, 2), inner(7.5, 6.5, 5, 2)]),
        dot(8.5, 13, 3)
      );
      if (name === 'wifi-off') L.push(seg(3, 3, 17, 17, 2));
      break;
    case 'bluetooth':
    case 'bluetooth-off':
      L.push(seg(10, 2, 10, 18), seg(10, 2, 15, 6.5), seg(15, 6.5, 5, 13.5), seg(5, 6.5, 15, 13.5), seg(15, 13.5, 10, 18));
      if (name === 'bluetooth-off') L.push(seg(3, 3, 17, 17, 2));
      break;
    case 'dnd':
      L.push(ring(2, 2, 16, 2), seg(6, 10, 14, 10, 2.2));
      break;
    case 'moon':
      L.push(clip(3, 2, 9, 16, [inner(0, 0, 16, 2)]), clip(5.5, 4, 8, 12, [inner(0, 0, 12, 2)]), seg(12, 2.5, 13.5, 4, 2), seg(12, 17.5, 13.5, 16, 2));
      break;
    case 'key':
      L.push(ring(2, 2, 9, 2), seg(9, 9, 17, 17, 2.2), seg(14.5, 14.5, 17.5, 11.5, 2), seg(12, 12, 14.5, 9.5, 2));
      break;
    case 'monitor':
      L.push(outline(2, 3, 16, 11, 2.5, 2), seg(10, 14.5, 10, 17.5, 2), seg(6, 17.5, 14, 17.5, 2));
      break;
    case 'gear': {
      L.push(ring(5, 5, 10, 2.4));
      for (let i = 0; i < 8; i += 1) {
        const a = (i * 45 * Math.PI) / 180;
        L.push(seg(10 + Math.cos(a) * 6.2, 10 + Math.sin(a) * 6.2, 10 + Math.cos(a) * 9, 10 + Math.sin(a) * 9, 3));
      }
      break;
    }
    case 'sun': {
      L.push(ring(5, 5, 10, 2));
      for (let i = 0; i < 8; i += 1) {
        const a = (i * 45 * Math.PI) / 180;
        L.push(seg(10 + Math.cos(a) * 7.2, 10 + Math.sin(a) * 7.2, 10 + Math.cos(a) * 9.4, 10 + Math.sin(a) * 9.4, 2));
      }
      break;
    }
    case 'volume':
    case 'volume-muted':
      L.push(box(2, 7.5, 4, 5, 1), seg(6, 8, 11, 4, 3), seg(6, 12, 11, 16, 3), box(8, 5, 3, 10, 1));
      if (name === 'volume') L.push(clip(12, 5, 7, 10, [inner(-3, 0, 10, 2)]), clip(12, 2, 8, 16, [inner(-7, 0, 16, 2)]));
      else L.push(seg(13, 7, 18, 13, 2), seg(18, 7, 13, 13, 2));
      break;
    case 'mic':
      L.push(box(7, 1.5, 6, 10, 3), clip(4, 7, 12, 6.5, [inner(0, -6, 12, 2)]), seg(10, 13.5, 10, 17.5, 2), seg(6.5, 17.5, 13.5, 17.5, 2));
      break;
    case 'contrast':
      L.push(ring(2, 2, 16, 2), clip(2, 2, 8, 16, [<View key={key()} style={{ width: 16 * k, height: 16 * k, borderRadius: 8 * k, backgroundColor: c }} />]));
      break;
    case 'grid':
      for (let i = 0; i < 4; i += 1) L.push(box(3 + (i % 2) * 7.5, 3 + Math.floor(i / 2) * 7.5, 6.5, 6.5, 2));
      break;
    case 'logout':
      L.push(outline(2.5, 3, 9, 14, 2, 2), seg(8, 10, 17, 10, 2), seg(13.5, 6.5, 17, 10, 2), seg(13.5, 13.5, 17, 10, 2));
      break;
    case 'restart':
      L.push(clip(2, 2, 16, 16, [inner(1, 1, 14, 2)]), box(10, 0, 8, 8, 0, 'rgba(0,0,0,0)'), seg(11, 3, 16, 3, 2), seg(16, 3, 16, 8, 2));
      break;
    case 'search':
      L.push(ring(2.5, 2.5, 11, 2), seg(12, 12, 17.5, 17.5, 2.6));
      break;
    case 'bell':
      L.push(clip(4, 2, 12, 12, [inner(0, 0, 12, 2)]), box(4, 8, 12, 6, 0), box(3, 13, 14, 2, 1), dot(8.5, 16, 3));
      break;
    case 'chip':
      L.push(outline(5, 5, 10, 10, 2, 2), seg(8, 2, 8, 5), seg(12, 2, 12, 5), seg(8, 15, 8, 18), seg(12, 15, 12, 18), seg(2, 8, 5, 8), seg(2, 12, 5, 12), seg(15, 8, 18, 8), seg(15, 12, 18, 12));
      break;
    case 'play':
      L.push(seg(6, 4, 6, 16, 3), seg(6, 4, 15, 10, 3), seg(6, 16, 15, 10, 3), box(6, 5, 5, 10, 0));
      break;
    case 'pause':
      L.push(box(5, 4, 3.5, 12, 1.5), box(11.5, 4, 3.5, 12, 1.5));
      break;
    case 'next':
      L.push(seg(5, 5, 13, 10, 3), seg(5, 15, 13, 10, 3), box(5, 6, 4, 8, 0), box(14, 4, 2.5, 12, 1));
      break;
    case 'prev':
      L.push(seg(15, 5, 7, 10, 3), seg(15, 15, 7, 10, 3), box(11, 6, 4, 8, 0), box(3.5, 4, 2.5, 12, 1));
      break;
    case 'user':
      L.push(ring(6, 2.5, 8, 2), clip(2, 11, 16, 7, [inner(1, 0, 14, 2)]));
      break;
    case 'calc':
      L.push(outline(3.5, 2.5, 13, 15, 3, 2), box(6, 5, 8, 3, 1), dot(6.5, 11, 2), dot(9, 11, 2), dot(11.5, 11, 2), dot(6.5, 14, 2), dot(9, 14, 2), dot(11.5, 14, 2));
      break;
    case 'image':
      L.push(outline(2.5, 3.5, 15, 13, 3, 2), dot(6, 7, 3), seg(4, 15, 9, 10, 2), seg(9, 10, 16, 15, 2));
      break;
    default:
      L.push(dot(6, 6, 8));
  }
  return <View style={{ width: size, height: size }}>{L}</View>;
});
