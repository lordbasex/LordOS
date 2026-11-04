// art.jsx — the LordOS Imager's drawings, made of views (no product photos or renders, no other
// project's artwork): the board, a PC, a USB stick, an SD card, the coolers and the two desktop
// styles. Shared by the native and desktop apps.
import { View, Text, useTheme } from '@lordos/sdk';

const abs = (x, y, w, h, extra) => ({ position: 'absolute', left: x, top: y, width: w, height: h, ...extra });

// A single-board computer seen from above: green board, the SoC, RAM, the USB/Ethernet blocks
export function BoardArt({ size = 120, dim, fan }) {
  const s = size / 120;
  const g = (x, y, w, h, c, r = 2) => <View style={abs(x * s, y * s, w * s, h * s, { backgroundColor: c, borderRadius: r * s })} />;
  return (
    <View style={{ width: size, height: size * 0.7, opacity: dim ? 0.45 : 1 }}>
      <View style={abs(0, 4 * s, 116 * s, 76 * s, { borderRadius: 6 * s, background: 'linear-gradient(160deg, #1f8f55, #136b3f)', boxShadow: '0 4px 10px rgba(0,0,0,0.3)' })} />
      {[[4, 8], [108, 8], [4, 72], [108, 72]].map(([x, y], i) => <View key={i} style={abs(x * s, y * s, 4 * s, 4 * s, { borderRadius: 2 * s, backgroundColor: '#d9d9d9' })} />)}
      {g(10, 12, 70, 5, '#e2c35a', 1)}
      {g(34, 30, 24, 24, '#2b2b30', 3)}
      {g(37, 33, 18, 18, '#45454d', 2)}
      {g(62, 30, 12, 22, '#2b2b30', 2)}
      {g(92, 14, 22, 18, '#c8c8cc', 2)}
      {g(92, 36, 22, 18, '#c8c8cc', 2)}
      {g(92, 58, 22, 18, '#b0b0b5', 2)}
      {g(14, 70, 10, 8, '#b0b0b5', 1)}
      {g(30, 70, 10, 8, '#b0b0b5', 1)}
      {g(10, 40, 14, 10, '#f5f5f7', 1)}
      {fan ? <FanArt size={34 * s} style={abs(29 * s, 25 * s, 34 * s, 34 * s)} /> : null}
    </View>
  );
}

// A keyboard computer seen from above (the Raspberry Pi 500): a white body with rows of keys
export function KeyboardArt({ size = 120, dim }) {
  const s = size / 120;
  const rows = [[13, 0], [13, 0], [12, 1], [12, 1]];
  return (
    <View style={{ width: size, height: size * 0.7, opacity: dim ? 0.45 : 1, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 116 * s, height: 50 * s, borderRadius: 6 * s, backgroundColor: '#f4f4f6', border: `${Math.max(1, s)}px solid #c9cbd3`,
        boxShadow: '0 4px 10px rgba(0,0,0,0.3)', padding: 4 * s, gap: 2 * s }}>
        {rows.map(([n, off], r) => (
          <View key={r} style={{ flexDirection: 'row', gap: 2 * s, marginLeft: off * 3 * s }}>
            {Array.from({ length: n }, (_, i) => <View key={i} style={{ width: 6.4 * s, height: 7 * s, borderRadius: 1.5 * s, backgroundColor: '#ffffff', border: `${Math.max(0.5, 0.6 * s)}px solid #d4d6de` }} />)}
          </View>
        ))}
        <View style={{ flexDirection: 'row', gap: 2 * s, justifyContent: 'center' }}>
          <View style={{ width: 52 * s, height: 7 * s, borderRadius: 1.5 * s, backgroundColor: '#ffffff', border: `${Math.max(0.5, 0.6 * s)}px solid #d4d6de` }} />
        </View>
      </View>
    </View>
  );
}

// A round fan (the active cooler, a case fan)
export function FanArt({ size = 40, style }) {
  const blades = [0, 60, 120, 180, 240, 300];
  return (
    <View style={[{ width: size, height: size, borderRadius: size / 2, backgroundColor: '#2c2c33', border: `${Math.max(1, size / 16)}px solid #5a5a63` }, style]}>
      {blades.map((a) => (
        <View key={a} style={{ position: 'absolute', left: 0, top: 0, width: size, height: size, transform: `rotate(${a}deg)` }}>
          <View style={{ position: 'absolute', left: size / 2 - size * 0.13, top: size * 0.06, width: size * 0.26, height: size * 0.4,
            borderRadius: size * 0.13, borderTopLeftRadius: size * 0.2, backgroundColor: '#9a9aa4', opacity: 0.92 }} />
        </View>
      ))}
      <View style={{ position: 'absolute', left: size * 0.36, top: size * 0.36, width: size * 0.28, height: size * 0.28, borderRadius: size * 0.14, backgroundColor: '#B46CFF' }} />
    </View>
  );
}

// Heatsink fins
export function FinsArt({ width = 60, height = 34, color = '#9a9aa3' }) {
  const n = 7;
  return (
    <View style={{ width, height, flexDirection: 'row', gap: width / (n * 2.2), alignItems: 'flex-end' }}>
      {Array.from({ length: n }, (_, i) => <View key={i} style={{ flex: 1, height, borderRadius: 2, backgroundColor: color }} />)}
    </View>
  );
}

export function CoolerArt({ kind, size = 96 }) {
  const t = useTheme();
  const s = size / 96;
  if (kind === 'case-fan') {
    return (
      <View style={{ width: size, height: size * 0.72, borderRadius: 10 * s, background: 'linear-gradient(170deg, #b9bcc4, #7e828c)', alignItems: 'center', justifyContent: 'center', boxShadow: '0 4px 10px rgba(0,0,0,0.25)' }}>
        <View style={{ position: 'absolute', top: 8 * s, left: 10 * s, right: 10 * s, height: 3 * s, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.45)' }} />
        <FanArt size={40 * s} />
      </View>
    );
  }
  if (kind === 'active-cooler') {
    return (
      <View style={{ width: size, height: size * 0.72, alignItems: 'center', justifyContent: 'center' }}>
        <FinsArt width={72 * s} height={44 * s} color="#a3a6ae" />
        <FanArt size={38 * s} style={{ position: 'absolute', top: 15 * s, left: (size - 38 * s) / 2 }} />
      </View>
    );
  }
  if (kind === 'passive') {
    return (
      <View style={{ width: size, height: size * 0.72, borderRadius: 10 * s, background: 'linear-gradient(170deg, #4b4e57, #2c2e34)', alignItems: 'center', justifyContent: 'center' }}>
        <FinsArt width={64 * s} height={36 * s} color="#6b6f79" />
      </View>
    );
  }
  return (
    <View style={{ width: size, height: size * 0.72, alignItems: 'center', justifyContent: 'center' }}>
      <BoardArt size={size * 0.9} />
      <View style={{ position: 'absolute', right: 2 * s, top: 0, width: 22 * s, height: 22 * s, borderRadius: 11 * s, backgroundColor: t.mode === 'dark' ? '#3a2a20' : '#ffe6d5', alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: 12 * s, fontWeight: 700, color: '#ff9f0a' }}>!</Text>
      </View>
    </View>
  );
}

// A desktop PC with a USB stick
export function PcArt({ size = 120, dim }) {
  const s = size / 120;
  return (
    <View style={{ width: size, height: size * 0.7, opacity: dim ? 0.45 : 1 }}>
      <View style={abs(6 * s, 6 * s, 74 * s, 50 * s, { borderRadius: 6 * s, backgroundColor: '#2a2a31', border: `${3 * s}px solid #4a4a54` })}>
        <View style={{ flex: 1, margin: 3 * s, borderRadius: 3 * s, background: 'linear-gradient(135deg, #6E4BFF, #B46CFF)' }} />
      </View>
      <View style={abs(36 * s, 56 * s, 14 * s, 10 * s, { backgroundColor: '#4a4a54' })} />
      <View style={abs(24 * s, 64 * s, 38 * s, 4 * s, { borderRadius: 2 * s, backgroundColor: '#4a4a54' })} />
      <View style={abs(86 * s, 10 * s, 28 * s, 60 * s, { borderRadius: 5 * s, backgroundColor: '#3a3a42', border: `${2 * s}px solid #55555f` })} />
      <View style={abs(94 * s, 18 * s, 12 * s, 4 * s, { borderRadius: 2 * s, backgroundColor: '#77777f' })} />
      <View style={abs(98 * s, 30 * s, 4 * s, 4 * s, { borderRadius: 2 * s, backgroundColor: '#30d158' })} />
      <StickArt size={26 * s} style={abs(64 * s, 40 * s, 26 * s, 26 * s)} />
    </View>
  );
}

export function StickArt({ size = 40, style }) {
  const s = size / 40;
  return (
    <View style={[{ width: size, height: size }, style]}>
      <View style={abs(14 * s, 0, 12 * s, 9 * s, { borderRadius: 1.5 * s, backgroundColor: '#c8c8cc', border: `${1 * s}px solid #9a9aa0` })} />
      <View style={abs(10 * s, 8 * s, 20 * s, 30 * s, { borderRadius: 4 * s, background: 'linear-gradient(180deg, #7C5CFF, #4F46E5)' })} />
      <View style={abs(16 * s, 26 * s, 8 * s, 3 * s, { borderRadius: 1.5 * s, backgroundColor: 'rgba(255,255,255,0.7)' })} />
    </View>
  );
}

export function CardArt({ size = 40, style }) {
  const s = size / 40;
  return (
    <View style={[{ width: size, height: size }, style]}>
      <View style={abs(9 * s, 2 * s, 24 * s, 34 * s, { borderRadius: 3 * s, borderTopRightRadius: 9 * s, background: 'linear-gradient(180deg, #3b3b44, #24242a)', border: `${1 * s}px solid #5a5a64` })} />
      {[0, 1, 2, 3, 4].map((i) => <View key={i} style={abs((12 + i * 4) * s, 5 * s, 2.4 * s, 6 * s, { borderRadius: 1, backgroundColor: '#e2c35a' })} />)}
      <View style={abs(13 * s, 18 * s, 16 * s, 12 * s, { borderRadius: 2 * s, background: 'linear-gradient(135deg, #6E4BFF, #B46CFF)' })} />
    </View>
  );
}

// A drive in the Storage list: an SD card for cards, a stick for the rest
export const DriveArt = ({ transport, size = 36 }) => (transport === 'mmc' || transport === 'sd' ? <CardArt size={size} /> : <StickArt size={size} />);

// The two desktop styles, as small thumbnails
export function ThemeArt({ theme, mode, accent = '#0a84ff', width = 150 }) {
  const dark = mode === 'dark';
  const h = width * 0.62, s = width / 150;
  const bg = theme === 'classic' ? (dark ? 'linear-gradient(135deg, #1b2440, #2b1f4a)' : 'linear-gradient(135deg, #cfd9ff, #e9ddff)')
    : (dark ? 'linear-gradient(135deg, #2a1f5c, #12122A)' : 'linear-gradient(135deg, #c9b8ff, #f1ecff)');
  const win = dark ? '#26262c' : '#ffffff';
  const bar = dark ? 'rgba(20,20,26,0.75)' : 'rgba(255,255,255,0.75)';
  return (
    <View style={{ width, height: h, borderRadius: 7 * s, background: bg, overflow: 'hidden' }}>
      {theme === 'classic' ? (
        <>
          <View style={abs(24 * s, 16 * s, 96 * s, 54 * s, { borderRadius: 5 * s, backgroundColor: win, boxShadow: '0 3px 8px rgba(0,0,0,0.3)' })}>
            <View style={{ height: 9 * s, flexDirection: 'row', justifyContent: 'flex-end', gap: 3 * s, paddingRight: 4 * s, alignItems: 'center' }}>
              {[0, 1, 2].map((i) => <View key={i} style={{ width: 5 * s, height: 2 * s, backgroundColor: dark ? '#aaa' : '#666' }} />)}
            </View>
          </View>
          <View style={abs(0, h - 12 * s, width, 12 * s, { backgroundColor: bar, flexDirection: 'row', justifyContent: 'center', alignItems: 'center', gap: 4 * s })}>
            {[accent, '#888', '#888', '#888'].map((c, i) => <View key={i} style={{ width: 7 * s, height: 7 * s, borderRadius: 2 * s, backgroundColor: c }} />)}
          </View>
        </>
      ) : (
        <>
          <View style={abs(0, 0, width, 8 * s, { backgroundColor: bar })} />
          <View style={abs(22 * s, 16 * s, 100 * s, 54 * s, { borderRadius: 6 * s, backgroundColor: win, boxShadow: '0 3px 8px rgba(0,0,0,0.3)' })}>
            <View style={{ height: 9 * s, flexDirection: 'row', gap: 2.5 * s, paddingLeft: 5 * s, alignItems: 'center' }}>
              {['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: 4 * s, height: 4 * s, borderRadius: 2 * s, backgroundColor: c }} />)}
            </View>
          </View>
          <View style={abs(40 * s, h - 13 * s, 70 * s, 10 * s, { borderRadius: 4 * s, backgroundColor: bar, flexDirection: 'row', justifyContent: 'center', alignItems: 'center', gap: 4 * s })}>
            {[accent, '#888', '#888', '#888', '#888'].map((c, i) => <View key={i} style={{ width: 6 * s, height: 6 * s, borderRadius: 1.5 * s, backgroundColor: c }} />)}
          </View>
        </>
      )}
    </View>
  );
}

// The app icon: the crown's violet, an SD card with a down arrow
export function ImagerIcon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #B46CFF, #4F46E5)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: s * 0.46, height: s * 0.58, borderRadius: s * 0.05, borderTopRightRadius: s * 0.16, backgroundColor: '#fff', marginTop: s * 0.04 }}>
        {[0, 1, 2, 3].map((i) => <View key={i} style={{ position: 'absolute', top: s * 0.05, left: s * (0.06 + i * 0.075), width: s * 0.04, height: s * 0.1, borderRadius: s * 0.02, backgroundColor: '#FFC300' }} />)}
        <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.19, width: s * 0.06, height: s * 0.22, borderRadius: s * 0.03, backgroundColor: '#5B3FE0' }} />
        <View style={{ position: 'absolute', left: s * 0.135, top: s * 0.305, width: s * 0.06, height: s * 0.14, borderRadius: s * 0.03, backgroundColor: '#5B3FE0', transform: 'rotate(-45deg)' }} />
        <View style={{ position: 'absolute', left: s * 0.265, top: s * 0.305, width: s * 0.06, height: s * 0.14, borderRadius: s * 0.03, backgroundColor: '#5B3FE0', transform: 'rotate(45deg)' }} />
      </View>
    </View>
  );
}
