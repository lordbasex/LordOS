// quickglyphs.jsx — the glyphs of the quick settings, drawn with views (our own shapes: no icon font, no color emoji;
// docs/DESIGN.md, "Icons"): Lord Classic's Quick Settings and (issue #114) Lord Flow's Control Center draw the same
// ones. Each takes a color and, where it cuts a hole, the color of what is behind it (bg).
import { View } from '@lordos/runtime';

// Signal bars (a network, Wi-Fi's strength): level 0–4 lit; off: every bar dims.
export function SignalGlyph({ color, size = 16, level = 4, off }) {
  const bars = [0.35, 0.55, 0.75, 1];
  return (
    <View style={{ width: size, height: size, flexDirection: 'row', alignItems: 'flex-end', gap: size * 0.08, direction: 'ltr' }}>
      {bars.map((k, i) => (
        <View key={i} style={{ width: size * 0.17, height: size * 0.9 * k, borderRadius: 1, backgroundColor: color, opacity: off || i >= level ? 0.3 : 1 }} />
      ))}
    </View>
  );
}

// A crescent (Do Not Disturb): a disc with another one of the background's color over it.
export function MoonGlyph({ color, bg, size = 16 }) {
  return (
    <View style={{ width: size, height: size, overflow: 'hidden', direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: size * 0.1, top: size * 0.1, width: size * 0.8, height: size * 0.8, borderRadius: size, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: size * 0.36, top: size * 0.02, width: size * 0.66, height: size * 0.66, borderRadius: size, backgroundColor: bg }} />
    </View>
  );
}

// A sun (Night Light): a disc with eight short rays.
export function SunGlyph({ color, size = 16 }) {
  const ray = (deg) => (
    <View key={deg} style={{ position: 'absolute', left: size / 2 - 0.75, top: 0, width: 1.5, height: size, transform: `rotate(${deg}deg)` }}>
      <View style={{ height: size * 0.18, borderRadius: 1, backgroundColor: color }} />
      <View style={{ flex: 1 }} />
      <View style={{ height: size * 0.18, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
  return (
    <View style={{ width: size, height: size }}>
      {[0, 45, 90, 135].map(ray)}
      <View style={{ position: 'absolute', left: size * 0.3, top: size * 0.3, width: size * 0.4, height: size * 0.4, borderRadius: size, backgroundColor: color }} />
    </View>
  );
}

// Half a disc (light / dark).
export function ContrastGlyph({ color, size = 16 }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size, border: `1.5px solid ${color}`, overflow: 'hidden', direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: size / 2 - 1.5, top: 0, bottom: 0, right: 0, backgroundColor: color }} />
    </View>
  );
}

// Two overlapping panes (transparency).
export function LayersGlyph({ color, size = 16 }) {
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: size * 0.05, top: size * 0.05, width: size * 0.6, height: size * 0.6, borderRadius: 2, border: `1.4px solid ${color}` }} />
      <View style={{ position: 'absolute', left: size * 0.35, top: size * 0.35, width: size * 0.6, height: size * 0.6, borderRadius: 2, backgroundColor: color, opacity: 0.45 }} />
    </View>
  );
}

// The Bluetooth rune, drawn as a stem with two folds.
export function BluetoothGlyph({ color, size = 16 }) {
  const s = size, line = 1.5;
  const seg = (x, y, len, deg) => <View style={{ position: 'absolute', left: x, top: y, width: len, height: line, borderRadius: 1, backgroundColor: color, transform: `rotate(${deg}deg)`, transformOrigin: 'left center' }} />;
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.06, width: line, height: s * 0.88, backgroundColor: color, borderRadius: 1 }} />
      {seg(s * 0.5, s * 0.06, s * 0.31, 45)}
      {seg(s * 0.72, s * 0.28, s * 0.62, 135)}
      {seg(s * 0.5, s * 0.94, s * 0.31, -45)}
      {seg(s * 0.72, s * 0.72, s * 0.62, -135)}
    </View>
  );
}

