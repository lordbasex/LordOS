// logo.js — the Axia DE logo (tools/gen-brand.py).
//
//   <AxiaLogo size={64} />            the app icon: the mark on the violet rounded square (About,
//                                     Settings → General, Monitor, the theme menu in other themes)
//   <AxiaGlyph size={16} color={c} /> the flat one-colour mark, for a menu bar; it follows the
//                                     light or dark ink of `color` (the UI can't tint a PNG)
//   <AxiaWordmark size={28} />        "Axia" as live text in the brand face (fontFamily 'brand':
//                                     Plus Jakarta Sans ExtraBold), with the mark in front unless
//                                     mark={false}. Navy on light, white on dark (or `color`).
//                                     Always left to right, also in RTL.
//
// The images are renders of assets/brand/*.svg in assets/icons/. The icon picks the smallest render
// at least as big as it is drawn (twice as big above 32); 16 and 32 come from the pixel-grid variant.
import { createElement as h } from 'react';
import { View, Text, Image } from '@lordos/runtime';
import { useTheme } from './theme.js';

const ICON_SIZES = [16, 32, 64, 128, 256, 512, 1024];

export function AxiaLogo({ size: s = 64 }) {
  const px = ICON_SIZES.find((z) => z >= (s > 32 ? s * 2 : s)) || 1024;   // big ones stay sharp on 2x displays
  return h(View, {
    style: {
      width: s, height: s, borderRadius: s * 0.22,
      boxShadow: s > 24 ? `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.3)` : 'none',
    },
  }, h(Image, { src: `asset:icons/axia-icon-${px}.png`, style: { width: s, height: s } }));
}

// light ink (a dark bar) → the light glyph; anything else → the dark one
function isLight(color) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(color || '').trim());
  if (!m) return /white|rgba?\(\s*2[0-9]{2}/i.test(String(color));
  const n = parseInt(m[1], 16);
  return (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) > 140;
}

export function AxiaGlyph({ size: s = 16, color = '#1d1d1f' }) {
  const tone = isLight(color) ? 'light' : 'dark';
  return h(Image, { src: `asset:icons/axia-glyph-${tone}-${s <= 16 ? 16 : 32}.png`, style: { width: s, height: s } });
}

// size = the font size. The mark is 1.2 times the font size and sits 0.25 em before the text.
export function AxiaWordmark({ size: s = 28, color, mark = true }) {
  const t = useTheme();
  const ink = color || (t.mode === 'light' ? '#12122A' : '#ffffff');
  const box = Math.round(s * 1.2);
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', direction: 'ltr' } },
    mark ? h(Image, { src: `asset:icons/axia-mark${box > 256 ? '-512' : ''}.png`,
      style: { width: box, height: box, marginRight: s * 0.25 } }) : null,
    h(Text, { style: { fontFamily: 'brand', fontWeight: 800, fontSize: s, color: ink, whiteSpace: 'nowrap' } }, 'Axia'));
}
