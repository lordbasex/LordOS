// Lord Classic theme glyphs, drawn with views (our own shapes: no icon font, no color emoji; see
// docs/DESIGN.md, "Icons"). Each one takes a color and, where it needs to cut a hole, the color
// of what is behind it (bg).
import { View } from '@lordos/sdk';

const box = (s, extra) => ({ width: s, height: s, alignItems: 'center', justifyContent: 'center', ...extra });

// Window buttons: minimize, maximize, restore (two squares) and close (an ×).
export function MinimizeGlyph({ color }) {
  return <View style={box(12)}><View style={{ width: 10, height: 1, backgroundColor: color }} /></View>;
}
export function MaximizeGlyph({ color }) {
  return <View style={box(12)}><View style={{ width: 9, height: 9, borderRadius: 1.5, border: `1px solid ${color}` }} /></View>;
}
export function RestoreGlyph({ color }) {
  return (
    <View style={box(12)}>
      <View style={{ position: 'absolute', left: 3.5, top: 1, width: 7.5, height: 7.5, borderTopWidth: 1, borderRightWidth: 1, borderColor: color, borderTopRightRadius: 1.5 }} />
      <View style={{ position: 'absolute', left: 1, top: 3.5, width: 7.5, height: 7.5, borderRadius: 1.5, border: `1px solid ${color}` }} />
    </View>
  );
}
export function CloseGlyph({ color, size = 12 }) {
  const bar = (deg) => <View style={{ position: 'absolute', left: size * 0.04, top: size / 2 - 0.5, width: size * 0.92, height: 1.1, backgroundColor: color, transform: `rotate(${deg}deg)` }} />;
  return <View style={box(size)}>{bar(45)}{bar(-45)}</View>;
}

// A magnifier.
export function SearchGlyph({ color, size = 16 }) {
  const r = size * 0.62;
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: 0, top: 0, width: r, height: r, borderRadius: r / 2, border: `${Math.max(1.4, size / 10)}px solid ${color}` }} />
      <View style={{ position: 'absolute', left: r * 0.82, top: r * 0.98, width: size * 0.42, height: Math.max(1.6, size / 9), borderRadius: 1, backgroundColor: color, transform: 'rotate(45deg)', transformOrigin: 'left top' }} />
    </View>
  );
}

// Task view: two cards, one in front of the other.
export function TaskViewGlyph({ color, bg, size = 18 }) {
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: size * 0.32, top: size * 0.12, width: size * 0.62, height: size * 0.5, borderRadius: 2, border: `1.4px solid ${color}` }} />
      <View style={{ position: 'absolute', left: size * 0.06, top: size * 0.38, width: size * 0.62, height: size * 0.5, borderRadius: 2, border: `1.4px solid ${color}`, backgroundColor: bg }} />
    </View>
  );
}

// Signal bars (network); off: the bars dim and a short slash.
export function NetworkGlyph({ color, size = 16, level = 4, off }) {
  const bars = [0.35, 0.55, 0.75, 1];
  return (
    <View style={{ width: size, height: size, flexDirection: 'row', alignItems: 'flex-end', gap: size * 0.08, direction: 'ltr' }}>
      {bars.map((k, i) => (
        <View key={i} style={{ width: size * 0.17, height: size * 0.9 * k, borderRadius: 1, backgroundColor: color, opacity: off || i >= level ? 0.3 : 1 }} />
      ))}
    </View>
  );
}

// A speaker with 0–2 sound waves; muted: a short cross. The cone is the left half of a diamond
// (a rotated square clipped at its middle), so it widens away from the body.
export function SpeakerGlyph({ color, size = 16, level = 2, muted }) {
  const d = size * 0.5;                     // the diamond's side
  const wave = (k) => (
    <View key={k} style={{ position: 'absolute', left: size * (0.3 + 0.14 * k), top: size * (0.5 - 0.15 * k - 0.1), width: size * (0.2 + 0.3 * k), height: size * (0.2 + 0.3 * k),
      borderRadius: size, borderRightWidth: 1.4, borderColor: color }} />
  );
  return (
    <View style={{ width: size, height: size, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: size * 0.04, top: size * 0.36, width: size * 0.18, height: size * 0.28, borderRadius: 1, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: size * 0.14, top: size * 0.14, width: size * 0.3, height: size * 0.72, overflow: 'hidden' }}>
        <View style={{ position: 'absolute', left: size * 0.3 - d / 2, top: size * 0.36 - d / 2, width: d, height: d, backgroundColor: color, transform: 'rotate(45deg)' }} />
      </View>
      {muted ? (
        <View style={{ position: 'absolute', left: size * 0.56, top: size * 0.29, width: size * 0.42, height: size * 0.42 }}>
          <CloseGlyph color={color} size={size * 0.42} />
        </View>
      ) : [1, 2].filter((k) => k <= level).map(wave)}
    </View>
  );
}

// A bell (notifications); with `dot`, a small accent dot for unread ones.
export function BellGlyph({ color, size = 16, dot }) {
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: size * 0.2, top: size * 0.1, width: size * 0.6, height: size * 0.62, borderTopLeftRadius: size * 0.3, borderTopRightRadius: size * 0.3,
        borderRadius: 1, border: `1.4px solid ${color}`, borderBottomWidth: 0 }} />
      <View style={{ position: 'absolute', left: size * 0.08, top: size * 0.7, width: size * 0.84, height: 1.4, borderRadius: 1, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: size * 0.4, top: size * 0.78, width: size * 0.2, height: size * 0.14, borderBottomLeftRadius: size, borderBottomRightRadius: size, backgroundColor: color }} />
      {dot ? <View style={{ position: 'absolute', right: -1, top: -1, width: size * 0.38, height: size * 0.38, borderRadius: size, backgroundColor: dot }} /> : null}
    </View>
  );
}

// The power symbol.
export function PowerGlyph({ color, bg, size = 14 }) {
  const k = size / 14;
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: 1 * k, top: 2 * k, width: 12 * k, height: 12 * k, borderRadius: 6 * k, border: `${1.6 * k}px solid ${color}` }} />
      <View style={{ position: 'absolute', left: 4 * k, top: 0, width: 6 * k, height: 5 * k, backgroundColor: bg }} />
      <View style={{ position: 'absolute', left: 6.2 * k, top: 0, width: 1.6 * k, height: 7 * k, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

// A gear: four crossed bars under a ring, with a hole of the background's color.
export function GearGlyph({ color, bg, size = 16 }) {
  const bar = (deg) => <View key={deg} style={{ position: 'absolute', left: size * 0.41, top: 0, width: size * 0.18, height: size, borderRadius: 1, backgroundColor: color, transform: `rotate(${deg}deg)` }} />;
  return (
    <View style={{ width: size, height: size }}>
      {[0, 45, 90, 135].map(bar)}
      <View style={{ position: 'absolute', left: size * 0.16, top: size * 0.16, width: size * 0.68, height: size * 0.68, borderRadius: size, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: size * 0.34, top: size * 0.34, width: size * 0.32, height: size * 0.32, borderRadius: size, backgroundColor: bg }} />
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

// A chevron pointing `dir` ('right' | 'left' | 'up' | 'down').
export function Chevron({ color, size = 8, dir = 'right' }) {
  const deg = { right: -45, down: 45, left: 135, up: -135 }[dir];
  return (
    <View style={box(size * 1.6)}>
      <View style={{ width: size, height: size, borderRightWidth: 1.5, borderBottomWidth: 1.5, borderColor: color, transform: `rotate(${deg}deg)` }} />
    </View>
  );
}

// A folder and a page, for search results.
export function FolderGlyph({ color, size = 20 }) {
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: size * 0.06, top: size * 0.16, width: size * 0.4, height: size * 0.2, borderTopLeftRadius: 2, borderTopRightRadius: 2, backgroundColor: color, opacity: 0.8 }} />
      <View style={{ position: 'absolute', left: size * 0.06, top: size * 0.26, width: size * 0.88, height: size * 0.6, borderRadius: 2, backgroundColor: color }} />
    </View>
  );
}
export function PageGlyph({ color, size = 20 }) {
  return (
    <View style={{ width: size, height: size, alignItems: 'center' }}>
      <View style={{ width: size * 0.66, height: size * 0.86, marginTop: size * 0.07, borderRadius: 2, border: `1.4px solid ${color}`, paddingTop: size * 0.18, paddingHorizontal: size * 0.12, gap: size * 0.1 }}>
        {[1, 0.8, 0.9].map((k, i) => <View key={i} style={{ height: 1.2, width: `${k * 100}%`, backgroundColor: color, opacity: 0.7 }} />)}
      </View>
    </View>
  );
}

// The Trash: a small bin, with a paper when it isn't empty.
export function TrashGlyph({ size: s, full, dark }) {
  const line = dark ? 'rgba(255,255,255,0.78)' : 'rgba(30,34,44,0.7)';
  return (
    <View style={{ width: s, height: s }}>
      {full && <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.02, width: s * 0.4, height: s * 0.24, borderRadius: 2,
        backgroundColor: dark ? '#d9dde6' : '#ffffff', border: `1px solid ${line}`, transform: 'rotate(-10deg)' }} />}
      <View style={{ position: 'absolute', left: s * 0.18, top: s * 0.18, width: s * 0.64, height: s * 0.07, backgroundColor: line, borderRadius: 1 }} />
      <View style={{ position: 'absolute', left: s * 0.24, top: s * 0.28, width: s * 0.52, height: s * 0.62, border: `1.5px solid ${line}`,
        borderBottomLeftRadius: s * 0.08, borderBottomRightRadius: s * 0.08 }}>
        {[0.3, 0.6].map((k) => <View key={k} style={{ position: 'absolute', top: s * 0.08, bottom: s * 0.08, left: s * 0.52 * k - 1, width: 1.5, backgroundColor: line }} />)}
      </View>
    </View>
  );
}

// The snap layouts, in miniature: each zone of `cells` ([[x, y, w, h] in 0..1]) is a tile.
export function LayoutGlyph({ cells, w = 60, h = 40, color, hoverIndex, accent, onPick, onHover }) {
  return (
    <View style={{ width: w, height: h, direction: 'ltr' }}>
      {cells.map(([x, y, cw, ch], i) => (
        <View key={i} onClick={() => onPick?.(i)} onMouseEnter={() => onHover?.(i)} onMouseLeave={() => onHover?.(-1)}
          style={{ position: 'absolute', left: x * w + 1.5, top: y * h + 1.5, width: cw * w - 3, height: ch * h - 3, borderRadius: 3,
            backgroundColor: hoverIndex === i ? accent : 'transparent', border: `1px solid ${hoverIndex === i ? accent : color}` }} />
      ))}
    </View>
  );
}
