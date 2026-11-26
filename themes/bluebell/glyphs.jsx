// glyphs.jsx — Bluebell's own icons, drawn with nodes in the style of the desktops of 2001 (glossy, rounded,
// friendly colours). Original drawings: nothing here is copied or traced from another system's icons. They are the
// theme's style pack (index.jsx registerStylePack): file-type icons, toolbar icons and a few app icons, by our names.
import { View, Text } from '@lordos/sdk';

// a chevron drawn with two borders (no font glyph): dir 'left' | 'right' | 'up' | 'down'
function Chevron({ size, color, dir, stroke }) {
  const deg = { left: -45, up: 45, right: 135, down: 225 }[dir];
  return (
    <View style={{ width: size, height: size, borderTopWidth: stroke, borderLeftWidth: stroke, borderColor: color, transform: `rotate(${deg}deg)` }} />
  );
}

// A round green button with a white arrow (Back and Forward), the "go" buttons of the era.
export function RoundArrow({ size = 22, dir = 'left', color = '#3a9a2e' }) {
  const s = size, h = Math.max(2, Math.round(s * 0.13)), c = s * 0.3, left = dir === 'left';
  return (
    <View style={{ width: s, height: s, borderRadius: s / 2, direction: 'ltr', overflow: 'hidden',
      background: `linear-gradient(180deg, #8fdc7a, ${color})`, border: '1px solid rgba(0,60,0,0.45)', boxShadow: '0 1px 2px rgba(0,0,0,0.3)' }}>
      <View style={{ position: 'absolute', left: s * 0.2, right: s * 0.2, top: s * 0.06, height: s * 0.3, borderRadius: s * 0.15, backgroundColor: 'rgba(255,255,255,0.32)' }} />
      <View style={{ position: 'absolute', left: s * 0.25, width: s * 0.48, top: (s - h) / 2 - 1, height: h, borderRadius: 1, backgroundColor: '#ffffff' }} />
      <View style={{ position: 'absolute', left: left ? s * 0.26 : s - s * 0.26 - c - 2, top: (s - c) / 2 - 1 }}>
        <Chevron size={c} color="#ffffff" dir={dir} stroke={h} />
      </View>
    </View>
  );
}

// The manila folder: a tab and a front, warm yellow.
export function Folder({ size = 32, open, badge }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, justifyContent: 'center' }}>
      <View style={{ width: s, height: s * 0.78 }}>
        <View style={{ position: 'absolute', left: s * 0.04, top: 0, width: s * 0.4, height: s * 0.22, borderTopLeftRadius: s * 0.06, borderTopRightRadius: s * 0.1,
          background: 'linear-gradient(180deg, #f7d77e, #d9a93a)', border: '1px solid #b98a22' }} />
        <View style={{ position: 'absolute', left: s * 0.04, right: s * 0.04, top: s * 0.12, bottom: 0, borderRadius: s * 0.05,
          background: 'linear-gradient(180deg, #fbe7a6, #e9bb4c)', border: '1px solid #b98a22' }} />
        {open ? <View style={{ position: 'absolute', left: s * 0.1, right: s * 0.02, top: s * 0.3, bottom: 0, borderRadius: s * 0.05,
          background: 'linear-gradient(180deg, #fff2c6, #f1c95c)', border: '1px solid #c39528' }} /> : null}
        <View style={{ position: 'absolute', left: s * 0.08, right: s * 0.08, top: s * 0.16, height: Math.max(1, s * 0.04), backgroundColor: 'rgba(255,255,255,0.7)' }} />
        {badge}
      </View>
    </View>
  );
}

// A sheet of paper with a folded corner; `band` marks its kind.
export function Page({ size = 32, band, label }) {
  const s = size, fold = s * 0.26;
  return (
    <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: s * 0.72, height: s * 0.92, backgroundColor: '#ffffff', border: '1px solid #8c9db8', borderTopRightRadius: fold * 0.6, overflow: 'hidden' }}>
        <View style={{ position: 'absolute', right: -1, top: -1, width: fold, height: fold, backgroundColor: '#dfe7f5', borderBottomLeftRadius: 2, border: '1px solid #8c9db8' }} />
        {s >= 28 ? [0.34, 0.46, 0.58].map((y) => <View key={y} style={{ position: 'absolute', left: s * 0.1, right: s * 0.1, top: s * y, height: 1, backgroundColor: '#c9d3e4' }} />) : null}
        {band ? (
          <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: s * 0.26, backgroundColor: band, alignItems: 'center', justifyContent: 'center' }}>
            {label && s >= 28 ? <Text style={{ fontSize: Math.max(7, s * 0.15), fontWeight: 700, color: '#ffffff' }}>{label}</Text> : null}
          </View>
        ) : null}
      </View>
    </View>
  );
}

const KIND = { text: ['#6b7b93', 'TXT'], code: ['#5a55c8', '</>'], pdf: ['#c8302a', 'PDF'], archive: ['#a37b3f', 'ZIP'], audio: ['#d2367a', '♪'],
  video: ['#7c45b8', '▶'], image: ['#2a95b0', 'IMG'], doc: ['#2f6bd0', 'DOC'], deb: ['#b0173f', 'DEB'], lpk: ['#6a4fd8', 'LPK'], other: ['#7f8a9a', ''] };
// the pack's file-type icons: one component per kind of sdk/filekinds.js
export const fileIcons = Object.fromEntries(Object.entries(KIND).map(([k, [band, label]]) => [k, ({ size }) => <Page size={size} band={band} label={label} />]));
fileIcons.folder = ({ size }) => <Folder size={size} />;

// Up: a folder with a green arrow pointing up.
function UpIcon({ size = 22 }) {
  return (
    <View style={{ width: size, height: size }}>
      <Folder size={size} />
      <View style={{ position: 'absolute', right: 0, top: 0, width: size * 0.5, height: size * 0.5, borderRadius: size * 0.25, alignItems: 'center', justifyContent: 'center',
        background: 'linear-gradient(180deg, #8fdc7a, #2f8a25)', border: '1px solid rgba(0,60,0,0.45)' }}>
        <View style={{ marginTop: size * 0.08 }}><Chevron size={size * 0.16} color="#ffffff" dir="up" stroke={2} /></View>
      </View>
    </View>
  );
}
function SearchIcon({ size = 22 }) {
  const s = size;
  return (
    <View style={{ width: s, height: s }}>
      <View style={{ position: 'absolute', left: s * 0.08, top: s * 0.08, width: s * 0.58, height: s * 0.58, borderRadius: s * 0.3, border: `${Math.max(2, s * 0.1)}px solid #3d6fb8`,
        backgroundColor: 'rgba(190,225,255,0.6)' }} />
      <View style={{ position: 'absolute', left: s * 0.58, top: s * 0.6, width: s * 0.36, height: Math.max(3, s * 0.16), borderRadius: 2, backgroundColor: '#8a5a24', transform: 'rotate(45deg)' }} />
    </View>
  );
}
function ViewsIcon({ size = 22 }) {
  const c = size * 0.36;
  return (
    <View style={{ width: size, height: size, flexDirection: 'row', flexWrap: 'wrap', gap: size * 0.1, padding: size * 0.06, direction: 'ltr' }}>
      {[0, 1, 2, 3].map((i) => <View key={i} style={{ width: c, height: c, borderRadius: 2, background: 'linear-gradient(180deg, #a9cdf7, #3a74c9)', border: '1px solid #2b5b9e' }} />)}
    </View>
  );
}
function NewFolderIcon({ size = 22 }) {
  return (
    <View style={{ width: size, height: size }}>
      <Folder size={size} />
      <Text style={{ position: 'absolute', right: 0, top: -2, fontSize: size * 0.5, fontWeight: 700, color: '#2f8a25' }}>+</Text>
    </View>
  );
}
function TerminalIcon({ size = 22 }) {
  return (
    <View style={{ width: size, height: size * 0.8, marginTop: size * 0.1, borderRadius: 3, backgroundColor: '#111', border: '1px solid #6b7b93', paddingStart: 3, justifyContent: 'center' }}>
      <Text style={{ fontSize: size * 0.4, fontWeight: 700, color: '#b6f0a0', fontFamily: 'monospace' }}>{'>_'}</Text>
    </View>
  );
}
export const toolbarIcons = {
  back: ({ size }) => <RoundArrow size={Math.max(20, size)} dir="left" />,
  forward: ({ size }) => <RoundArrow size={Math.max(20, size)} dir="right" />,
  up: ({ size }) => <UpIcon size={Math.max(20, size)} />,
  search: ({ size }) => <SearchIcon size={Math.max(20, size)} />,
  views: ({ size }) => <ViewsIcon size={Math.max(18, size)} />,
  newFolder: ({ size }) => <NewFolderIcon size={Math.max(20, size)} />,
  terminal: ({ size }) => <TerminalIcon size={Math.max(18, size)} />,
};

// Files' app icon in this style: the folder with a small magnifier.
function FilesAppIcon({ size = 32 }) {
  return (
    <View style={{ width: size, height: size }}>
      <Folder size={size} open />
      <View style={{ position: 'absolute', right: 0, bottom: 0 }}><SearchIcon size={size * 0.5} /></View>
    </View>
  );
}
export const appIcons = { files: ({ size }) => <FilesAppIcon size={size} /> };

// The window buttons: a rounded square, glossy blue (close in warm red), with a white glyph.
export function FrameGlyph({ kind, size = 21 }) {
  const s = size, w = Math.max(2, Math.round(s * 0.12));
  if (kind === 'close') {
    return (
      <View style={{ width: s * 0.5, height: s * 0.5, alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ position: 'absolute', width: s * 0.62, height: w, backgroundColor: '#fff', transform: 'rotate(45deg)' }} />
        <View style={{ position: 'absolute', width: s * 0.62, height: w, backgroundColor: '#fff', transform: 'rotate(-45deg)' }} />
      </View>
    );
  }
  if (kind === 'minimize') return <View style={{ width: s * 0.42, height: w + 1, marginTop: s * 0.3, backgroundColor: '#fff' }} />;
  if (kind === 'restore') {
    return (
      <View style={{ width: s * 0.5, height: s * 0.5 }}>
        <View style={{ position: 'absolute', right: 0, top: 0, width: s * 0.34, height: s * 0.3, borderTopWidth: w, borderRightWidth: 1, borderLeftWidth: 1, borderBottomWidth: 1, borderColor: '#fff' }} />
        <View style={{ position: 'absolute', left: 0, bottom: 0, width: s * 0.34, height: s * 0.3, borderTopWidth: w, borderRightWidth: 1, borderLeftWidth: 1, borderBottomWidth: 1, borderColor: '#fff' }} />
      </View>
    );
  }
  return <View style={{ width: s * 0.5, height: s * 0.46, borderTopWidth: w + 1, borderLeftWidth: 1, borderRightWidth: 1, borderBottomWidth: 1, borderColor: '#fff' }} />;
}

// The start button's mark: a bluebell flower, a blue bell on a green stem with a leaf (the theme's own name, drawn by us;
// no other system's logo).
export function StartMark({ size = 18 }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: s * 0.46, top: 0, width: Math.max(2, s * 0.1), height: s * 0.5, borderRadius: 1, backgroundColor: '#2f7a22', transform: 'rotate(18deg)' }} />
      <View style={{ position: 'absolute', left: s * 0.08, top: s * 0.26, width: s * 0.36, height: s * 0.2, borderTopLeftRadius: s * 0.2, borderBottomRightRadius: s * 0.2,
        backgroundColor: '#5cc04a', transform: 'rotate(-20deg)' }} />
      <View style={{ position: 'absolute', left: s * 0.28, top: s * 0.38, width: s * 0.56, height: s * 0.5, borderTopLeftRadius: s * 0.22, borderTopRightRadius: s * 0.22,
        borderBottomLeftRadius: s * 0.08, borderBottomRightRadius: s * 0.08, background: 'linear-gradient(180deg, #8ec5ff, #2f63d8)', border: '1px solid #1d3f9a' }} />
      {[0.26, 0.5, 0.74].map((x) => (
        <View key={x} style={{ position: 'absolute', left: s * (0.28 + 0.56 * x) - s * 0.07, top: s * 0.8, width: s * 0.14, height: s * 0.14, borderRadius: s * 0.07, backgroundColor: '#2f63d8' }} />
      ))}
    </View>
  );
}
