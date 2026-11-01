// glyphs.jsx — Files icons: folder, document by kind (with the extension) and image
// thumbnails. Drawn with nodes (no images), except the thumbnails.
import { View, Text, Image, useThumb } from '@lordos/sdk';
import { kindOf, extOf, isImage, KIND_COLOR } from './kinds.js';

export function FolderGlyph({ size: s, special }) {
  return (
    <View style={{ width: s, height: s, justifyContent: 'center' }}>
      <View style={{ width: s, height: s * 0.8 }}>
        <View style={{ position: 'absolute', left: 0, top: 0, width: s * 0.42, height: s * 0.24, borderRadius: s * 0.07, backgroundColor: '#2f8cf0' }} />
        <View style={{ position: 'absolute', left: 0, top: s * 0.11, width: s, height: s * 0.69, borderRadius: s * 0.09,
          background: 'linear-gradient(180deg, #8fd0ff, #3c9cff)' }} />
        {special ? <Text style={{ position: 'absolute', left: 0, right: 0, top: s * 0.28, textAlign: 'center', fontSize: s * 0.3,
          fontWeight: 700, color: 'rgba(16,80,160,0.55)' }}>{special}</Text> : null}
      </View>
    </View>
  );
}

export function DocGlyph({ size: s, kind, ext, dark }) {
  const band = KIND_COLOR[kind] || KIND_COLOR.other;
  return (
    <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: s * 0.74, height: s * 0.94, borderRadius: s * 0.07, backgroundColor: dark ? '#ececf1' : '#ffffff',
        border: '1px solid rgba(0,0,0,0.18)', overflow: 'hidden', alignItems: 'center', justifyContent: 'flex-end' }}>
        {s >= 32 && ['text', 'code', 'doc'].includes(kind) && [0.22, 0.34, 0.46].map((y) => (
          <View key={y} style={{ position: 'absolute', left: s * 0.12, right: s * 0.12, top: s * y, height: Math.max(1, s * 0.025), backgroundColor: '#c4c4cc' }} />
        ))}
        <View style={{ width: '100%', height: s * 0.3, backgroundColor: band, alignItems: 'center', justifyContent: 'center' }}>
          {s >= 28 && ext ? <Text style={{ fontSize: Math.max(7, s * 0.17), fontWeight: 700, color: '#fff' }}>{ext.slice(0, 4).toUpperCase()}</Text> : null}
        </View>
      </View>
    </View>
  );
}

// Icon of an entry: folder, thumbnail (images) or document.
// A computer on the network: a screen on its stand; a way into it, a small one.
function ComputerGlyph({ size, way }) {
  const w = size * (way ? 0.7 : 0.86), h = w * 0.62;
  return (
    <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: w, height: h, borderRadius: size * 0.08, backgroundColor: '#7c8db5', border: `${Math.max(1, size * 0.05)}px solid #4b5a7d` }} />
      <View style={{ width: w * 0.16, height: size * 0.08, backgroundColor: '#4b5a7d' }} />
      <View style={{ width: w * 0.46, height: Math.max(1, size * 0.05), borderRadius: 1, backgroundColor: '#4b5a7d' }} />
    </View>
  );
}

export function EntryIcon({ e, size, dark, thumb = true }) {
  if (e.netHost || e.netWay) return <ComputerGlyph size={size} way={!!e.netWay} />;
  if (e.Icon) return <e.Icon size={size} />;
  const kind = kindOf(e);
  if (kind === 'folder' && e.locked) {
    // a folder the user can't open (This computer): the system's permissions say no
    return (
      <View style={{ width: size, height: size }}>
        <FolderGlyph size={size} special={e.special} />
        <Text style={{ position: 'absolute', right: 0, bottom: 0, fontSize: Math.max(9, Math.round(size * 0.32)) }}>🔒</Text>
      </View>
    );
  }
  if (kind === 'folder') return <FolderGlyph size={size} special={e.special} />;
  if (thumb && isImage(e) && size >= 32 && e.size < 24 * 1024 * 1024) {
    return (
      <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>
        <Image src={e.path} style={{ width: size, height: size, objectFit: 'contain' }} />
      </View>
    );
  }
  // media (M9): video thumbnails, covers and formats stb cannot read (WebP, HEIC, AVIF), through lord-media
  if (thumb && size >= 32 && (kind === 'video' || kind === 'audio' || (kind === 'image' && !isImage(e) && !/\.svg$/i.test(e.name)))) {
    return <MediaThumb e={e} size={size} kind={kind} dark={dark} />;
  }
  return <DocGlyph size={size} kind={kind} ext={extOf(e.name)} dark={dark} />;
}

function MediaThumb({ e, size, kind, dark }) {
  const src = useThumb(e.path, size > 128 ? 512 : 256);
  if (!src) return <DocGlyph size={size} kind={kind} ext={extOf(e.name)} dark={dark} />;
  return (
    <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>
      <Image src={src} style={{ width: size, height: size, objectFit: 'contain' }} />
      {kind === 'video' ? (
        <View style={{ position: 'absolute', right: size * 0.06, bottom: size * 0.12, paddingHorizontal: 3, borderRadius: 3, backgroundColor: 'rgba(0,0,0,0.6)' }}>
          <Text style={{ fontSize: Math.max(7, size * 0.11), color: '#fff' }}>▶</Text>
        </View>
      ) : null}
    </View>
  );
}

export const TAG_COLORS = { red: '#ff453a', orange: '#ff9f0a', yellow: '#ffd60a', green: '#30d158', blue: '#0a84ff', purple: '#bf5af2', gray: '#8e8e93' };

export function TagDots({ tags, size = 8 }) {
  if (!tags || !tags.length) return null;
  return (
    <View style={{ flexDirection: 'row', gap: 2 }}>
      {tags.map((c) => <View key={c} style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: TAG_COLORS[c] || '#888' }} />)}
    </View>
  );
}

// Small monochrome source-list glyphs drawn with nodes (🗑 and ⏏ would render as color emoji).
export function TrashGlyph({ color, size = 14 }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, alignItems: 'center' }}>
      <View style={{ width: s * 0.36, height: 2, borderRadius: 1, backgroundColor: color }} />
      <View style={{ width: s * 0.9, height: 2, marginTop: 1, borderRadius: 1, backgroundColor: color }} />
      <View style={{ width: s * 0.66, height: s * 0.66, marginTop: 1, borderRadius: 2, border: `2px solid ${color}` }} />
    </View>
  );
}

export function DriveGlyph({ color, size = 14 }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, justifyContent: 'center' }}>
      <View style={{ width: s, height: Math.round(s * 0.62), borderRadius: 3, border: `2px solid ${color}`, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', paddingHorizontal: 2, direction: 'ltr' }}>
        <View style={{ width: 3, height: 3, borderRadius: 1.5, backgroundColor: color }} />
      </View>
    </View>
  );
}
