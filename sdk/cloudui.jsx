// cloudui.jsx — Cloud's small pieces shared by Settings → Cloud, Files and the menu bar item (docs/CLOUD.md):
// the status glyph of a file, an account's usage ring and badge, and the space bar. Drawn as pixels
// (cloudglyphs.js), in the mocks' colors, the secondary ones from the theme.
import { createElement as h } from 'react';
import { View, Text, Image } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { glyphPixels, ringPixels } from './cloudglyphs.js';
import { statusGlyph, STATUS_COLOR } from './cloudmodel.js';
import { t } from './i18n.js';

const track = (th) => (th.mode === 'dark' ? '#3a3a42' : '#d6d6dc');

// A glyph of cloudglyphs.js at `size` px (kind: cloud | down | here | kept | up | error | menu)
export function CloudGlyph({ kind, size = 18, color, pct = null, arrow = null, ink }) {
  const th = useTheme();
  const c = color || (STATUS_COLOR[kind] === 'dim' || !STATUS_COLOR[kind] ? (th.mode === 'dark' ? '#a4a4ad' : '#7c7c85') : STATUS_COLOR[kind]);
  const px = glyphPixels(kind, size, c, { pct, track: track(th), ink: ink || (kind === 'error' ? th.windowBg : '#ffffff'), arrow });
  return px ? h(Image, { pixels: px, style: { width: size, height: size } }) : null;
}

// One file's status (cloud.status() entry), with its label for screen readers and tooltips.
export function StatusGlyph({ s, size = 18 }) {
  const g = statusGlyph(s);
  if (!g) return null;
  return h(View, { title: t(g.key), style: { width: size, height: size } }, h(CloudGlyph, { kind: g.kind, size, pct: g.pct }));
}

// The badge on an icon in the icon and gallery views: the glyph on a disc of the window's color.
export function StatusBadge({ s, size = 22 }) {
  const th = useTheme();
  const g = statusGlyph(s);
  if (!g) return null;
  return h(View, { title: t(g.key), style: { width: size, height: size, borderRadius: size / 2, backgroundColor: th.windowBg, alignItems: 'center', justifyContent: 'center' } },
    h(CloudGlyph, { kind: g.kind, size: Math.round(size * 0.82), pct: g.pct }));
}

// A ring of parts ([{ frac, color }]) over the track; thickness in units of a 24-unit box (12 fills it).
export function UsageRing({ parts, size = 14, thickness = 12 }) {
  const th = useTheme();
  return h(Image, { pixels: ringPixels(size, parts, { thickness, track: th.mode === 'dark' ? '#48484f' : '#d1d1d6' }), style: { width: size, height: size } });
}

// The account's square badge: a cloud on the account's color (or "!" when it needs the person).
export function AccountBadge({ color, size = 32, alert = false }) {
  return h(View, { style: { width: size, height: size, borderRadius: Math.round(size / 4), backgroundColor: color, alignItems: 'center', justifyContent: 'center', flexShrink: 0 } },
    alert ? h(Text, { style: { fontSize: Math.round(size * 0.5), fontWeight: 700, color: '#fff' } }, '!')
      : h(CloudGlyph, { kind: 'cloud', size: Math.round(size * 0.56), color: '#ffffff' }));
}

// A horizontal bar: [{ frac, color }] over the track (direction left to right in every language, as a gauge)
export function SpaceBar({ parts, height = 6, gap = 0 }) {
  const th = useTheme();
  return h(View, { style: { height, borderRadius: height / 2, overflow: 'hidden', flexDirection: 'row', gap, backgroundColor: th.mode === 'dark' ? '#3a3a42' : '#e2e2e7', direction: 'ltr' } },
    ...(parts || []).filter((p) => p.frac > 0).map((p, i) => h(View, { key: i, style: { width: `${Math.max(0.4, Math.min(100, p.frac * 100))}%`, height, backgroundColor: p.color } })));
}
