// Lord Workspaces glyphs, drawn with views (our own shapes: no icon font, no colour emoji; docs/DESIGN.md, "Icons").
// Each takes a colour and a size; where one cuts a hole it also takes the colour behind it (bg).
import { View } from '@lordos/sdk';

export { MoonGlyph, SunGlyph, ContrastGlyph, LayersGlyph, BluetoothGlyph, SignalGlyph } from '@lordos/sdk';
export { MinimizeGlyph, MaximizeGlyph, RestoreGlyph, CloseGlyph, SearchGlyph, SpeakerGlyph, BellGlyph, PowerGlyph, Chevron, FolderGlyph, PageGlyph, GearGlyph } from '../classic/glyphs.jsx';

const abs = (s) => ({ position: 'absolute', ...s });

// Activities: four tiles, the first one filled.
export function ActivitiesGlyph({ color, size = 20 }) {
  const k = size / 20, c = 8 * k, g = 2.5 * k;
  return (
    <View style={{ width: size, height: size }}>
      <View style={abs({ left: 1 * k, top: 1 * k, width: c, height: c, borderRadius: 2.2 * k, backgroundColor: color })} />
      <View style={abs({ left: 1 * k + c + g, top: 1 * k, width: c, height: c, borderRadius: 2.2 * k, border: `${1.6 * k}px solid ${color}` })} />
      <View style={abs({ left: 1 * k, top: 1 * k + c + g, width: c, height: c, borderRadius: 2.2 * k, border: `${1.6 * k}px solid ${color}` })} />
      <View style={abs({ left: 1 * k + c + g, top: 1 * k + c + g, width: c, height: c, borderRadius: 2.2 * k, border: `${1.6 * k}px solid ${color}` })} />
    </View>
  );
}

// Apps: a 3 × 3 grid of dots.
export function AppsGlyph({ color, size = 20 }) {
  const k = size / 20, d = 4 * k;
  return (
    <View style={{ width: size, height: size, flexDirection: 'row', flexWrap: 'wrap', gap: 2.6 * k, padding: 1.6 * k, direction: 'ltr' }}>
      {Array.from({ length: 9 }, (_, i) => <View key={i} style={{ width: d, height: d, borderRadius: d / 2, backgroundColor: color }} />)}
    </View>
  );
}

// The Control Center: two sliders.
export function ControlGlyph({ color, bg, size = 20 }) {
  const k = size / 20;
  const line = (top, knob) => (
    <View key={top} style={abs({ left: 1.5 * k, top, width: 17 * k, height: 7 * k })}>
      <View style={abs({ left: 0, right: 0, top: 2.7 * k, height: 1.6 * k, borderRadius: 1, backgroundColor: color })} />
      <View style={abs({ left: knob, top: 0, width: 7 * k, height: 7 * k, borderRadius: 3.5 * k, backgroundColor: bg, border: `${1.6 * k}px solid ${color}` })} />
    </View>
  );
  return <View style={{ width: size, height: size, direction: 'ltr' }}>{line(2.5 * k, 2 * k)}{line(10.5 * k, 9 * k)}</View>;
}

// A plus sign.
export function PlusGlyph({ color, size = 12 }) {
  const w = Math.max(1.6, size / 7);
  return (
    <View style={{ width: size, height: size }}>
      <View style={abs({ left: 0, right: 0, top: (size - w) / 2, height: w, borderRadius: w, backgroundColor: color })} />
      <View style={abs({ top: 0, bottom: 0, left: (size - w) / 2, width: w, borderRadius: w, backgroundColor: color })} />
    </View>
  );
}

// A bookmark (Save layout).
export function SaveGlyph({ color, bg, size = 14 }) {
  const k = size / 14;
  return (
    <View style={{ width: size, height: size, alignItems: 'center' }}>
      <View style={{ width: 10 * k, height: 12 * k, marginTop: 1 * k, borderTopLeftRadius: 2 * k, borderTopRightRadius: 2 * k, border: `${1.5 * k}px solid ${color}`, borderBottomWidth: 0 }} />
      <View style={abs({ left: 4.6 * k, top: 8.6 * k, width: 4.8 * k, height: 4.8 * k, backgroundColor: bg, borderTopWidth: 1.5 * k, borderLeftWidth: 1.5 * k, borderColor: color, transform: 'rotate(45deg)' })} />
    </View>
  );
}

// A padlock (the display's lock): shackle and body; open, the shackle lifts and turns to the side.
export function LockGlyph({ color, size = 14, open = false }) {
  const k = size / 14;
  return (
    <View style={{ width: size, height: size, direction: 'ltr' }}>
      <View style={abs({ left: (open ? 5.6 : 3.6) * k, top: (open ? -0.6 : 0.6) * k, width: 6.8 * k, height: 7 * k, borderTopLeftRadius: 3.4 * k, borderTopRightRadius: 3.4 * k,
        border: `${1.5 * k}px solid ${color}`, borderBottomWidth: 0 })} />
      <View style={abs({ left: 2 * k, top: 6 * k, width: 10 * k, height: 7.6 * k, borderRadius: 1.8 * k, backgroundColor: color })} />
    </View>
  );
}

// An arrow into a stack (Move to another activity).
export function MoveGlyph({ color, size = 14 }) {
  const k = size / 14;
  return (
    <View style={{ width: size, height: size, direction: 'ltr' }}>
      <View style={abs({ left: 5 * k, top: 1 * k, width: 8 * k, height: 8 * k, borderRadius: 2 * k, border: `${1.4 * k}px solid ${color}` })} />
      <View style={abs({ left: 1 * k, top: 9 * k, width: 7 * k, height: 1.5 * k, borderRadius: 1, backgroundColor: color })} />
      <View style={abs({ left: 5 * k, top: 7.2 * k, width: 3.6 * k, height: 3.6 * k, borderTopWidth: 1.5 * k, borderRightWidth: 1.5 * k, borderColor: color, transform: 'rotate(45deg)' })} />
    </View>
  );
}

// A layout in miniature: its tiles in 0..1 of a w × h box (the layout picker and the header's button).
export function LayoutMini({ cells, w = 22, h = 15, color, fill, active }) {
  return (
    <View style={{ width: w, height: h, direction: 'ltr' }}>
      {cells.map(([x, y, cw, ch], i) => (
        <View key={i} style={abs({ left: x * w + 0.75, top: y * h + 0.75, width: cw * w - 1.5, height: ch * h - 1.5, borderRadius: 2,
          backgroundColor: active && i === 0 ? fill : 'transparent', border: `1.2px solid ${color}` })} />
      ))}
    </View>
  );
}
// the cells each layout shows in its miniature (the free one: two overlapping windows)
export const LAYOUT_CELLS = {
  free: [[0.04, 0.08, 0.6, 0.6], [0.36, 0.34, 0.6, 0.6]],
  single: [[0, 0, 1, 1]],
  halves: [[0, 0, 0.5, 1], [0.5, 0, 0.5, 1]],
  split: [[0, 0, 0.64, 1], [0.64, 0, 0.36, 0.5], [0.64, 0.5, 0.36, 0.5]],
  three: [[0, 0, 1 / 3, 1], [1 / 3, 0, 1 / 3, 1], [2 / 3, 0, 1 / 3, 1]],
  grid: [[0, 0, 0.5, 0.5], [0.5, 0, 0.5, 0.5], [0, 0.5, 0.5, 0.5], [0.5, 0.5, 0.5, 0.5]],
};

// ---- the activities' icons (model/activities.js ACTIVITY_ICONS) ----

function Spark({ color, size: s }) {
  const bar = (deg) => <View key={deg} style={abs({ left: s * 0.44, top: s * 0.06, width: s * 0.12, height: s * 0.88, borderRadius: s, backgroundColor: color, transform: `rotate(${deg}deg)` })} />;
  return <View style={{ width: s, height: s }}>{bar(0)}{bar(90)}{bar(45)}{bar(-45)}</View>;
}
function Code({ color, size: s }) {
  const w = Math.max(1.5, s / 9);
  const arm = (left, deg) => <View style={abs({ left, top: s * 0.3, width: s * 0.34, height: s * 0.34, borderLeftWidth: w, borderBottomWidth: w, borderColor: color, transform: `rotate(${deg}deg)` })} />;
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      {arm(s * 0.08, 45)}{arm(s * 0.58, -135)}
      <View style={abs({ left: s * 0.47, top: s * 0.18, width: w, height: s * 0.64, backgroundColor: color, transform: 'rotate(18deg)' })} />
    </View>
  );
}
function Design({ color, size: s, bg }) {
  return (
    <View style={{ width: s, height: s }}>
      <View style={abs({ left: s * 0.08, top: s * 0.08, width: s * 0.84, height: s * 0.84, borderRadius: s, backgroundColor: color })} />
      {[[0.3, 0.28], [0.55, 0.24], [0.26, 0.52]].map(([x, y]) => <View key={x} style={abs({ left: s * x, top: s * y, width: s * 0.16, height: s * 0.16, borderRadius: s, backgroundColor: bg })} />)}
      <View style={abs({ left: s * 0.56, top: s * 0.56, width: s * 0.22, height: s * 0.22, borderRadius: s, backgroundColor: bg })} />
    </View>
  );
}
function Server({ color, size: s, bg }) {
  const row = (top) => (
    <View key={top} style={abs({ left: s * 0.1, top: s * top, width: s * 0.8, height: s * 0.24, borderRadius: s * 0.06, backgroundColor: color })}>
      <View style={abs({ left: s * 0.08, top: s * 0.08, width: s * 0.08, height: s * 0.08, borderRadius: s, backgroundColor: bg })} />
    </View>
  );
  return <View style={{ width: s, height: s, direction: 'ltr' }}>{row(0.12)}{row(0.4)}{row(0.68)}</View>;
}
function Chat({ color, size: s }) {
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      <View style={abs({ left: s * 0.06, top: s * 0.1, width: s * 0.88, height: s * 0.62, borderRadius: s * 0.22, backgroundColor: color })} />
      <View style={abs({ left: s * 0.2, top: s * 0.58, width: s * 0.24, height: s * 0.24, backgroundColor: color, transform: 'rotate(45deg)' })} />
    </View>
  );
}
function Music({ color, size: s }) {
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      <View style={abs({ left: s * 0.1, top: s * 0.62, width: s * 0.3, height: s * 0.26, borderRadius: s, backgroundColor: color })} />
      <View style={abs({ left: s * 0.56, top: s * 0.52, width: s * 0.3, height: s * 0.26, borderRadius: s, backgroundColor: color })} />
      <View style={abs({ left: s * 0.32, top: s * 0.12, width: s * 0.1, height: s * 0.62, backgroundColor: color })} />
      <View style={abs({ left: s * 0.78, top: s * 0.06, width: s * 0.1, height: s * 0.6, backgroundColor: color })} />
      <View style={abs({ left: s * 0.32, top: s * 0.06, width: s * 0.56, height: s * 0.14, backgroundColor: color, transform: 'rotate(-8deg)' })} />
    </View>
  );
}
function Book({ color, size: s }) {
  const page = (left, deg) => <View style={abs({ left, top: s * 0.18, width: s * 0.4, height: s * 0.64, borderRadius: s * 0.06, border: `${Math.max(1.5, s / 10)}px solid ${color}`, transform: `rotate(${deg}deg)` })} />;
  return <View style={{ width: s, height: s, direction: 'ltr' }}>{page(s * 0.08, -4)}{page(s * 0.52, 4)}</View>;
}
function Home({ color, size: s }) {
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      <View style={abs({ left: s * 0.22, top: s * 0.12, width: s * 0.56, height: s * 0.56, backgroundColor: color, transform: 'rotate(45deg)' })} />
      <View style={abs({ left: s * 0.18, top: s * 0.44, width: s * 0.64, height: s * 0.46, borderRadius: s * 0.06, backgroundColor: color })} />
    </View>
  );
}
function Work({ color, size: s }) {
  const w = Math.max(1.5, s / 10);
  return (
    <View style={{ width: s, height: s }}>
      <View style={abs({ left: s * 0.34, top: s * 0.12, width: s * 0.32, height: s * 0.2, borderRadius: s * 0.06, border: `${w}px solid ${color}` })} />
      <View style={abs({ left: s * 0.06, top: s * 0.28, width: s * 0.88, height: s * 0.6, borderRadius: s * 0.1, backgroundColor: color })} />
    </View>
  );
}
function Globe({ color, size: s }) {
  const w = Math.max(1.5, s / 10);
  return (
    <View style={{ width: s, height: s }}>
      <View style={abs({ left: s * 0.08, top: s * 0.08, width: s * 0.84, height: s * 0.84, borderRadius: s, border: `${w}px solid ${color}` })} />
      <View style={abs({ left: s * 0.32, top: s * 0.08, width: s * 0.36, height: s * 0.84, borderRadius: s, border: `${w}px solid ${color}` })} />
      <View style={abs({ left: s * 0.1, top: s * 0.48, width: s * 0.8, height: w, backgroundColor: color })} />
    </View>
  );
}
const ICONS = { spark: Spark, code: Code, design: Design, server: Server, chat: Chat, music: Music, book: Book, home: Home, work: Work, globe: Globe };

export function ActivityIcon({ icon, color, bg = 'transparent', size = 14 }) {
  const G = ICONS[icon] || Spark;
  return <G color={color} bg={bg} size={size} />;
}
