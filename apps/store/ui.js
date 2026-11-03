// ui.js — the Store's look (0.8.5, modelled on an app center): cards in a fluid grid with a "Get"
// pill, the featured banner, the facts row of an app page and the sidebar glyphs. Data comes from the
// pages (debian.js, ai.js, index.jsx); these pieces only draw.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, Image, useTheme, useStore, settings, typeScale, spacing } from '@lordos/sdk';

export const CARD_BASIS = 250;
const CARD_HEIGHT = 148;     // two lines of summary and the foot, so the action keeps the card's padding

// Sidebar glyphs: monochrome symbols drawn in the accent color (docs/DESIGN.md, "Icons").
export const GLYPH = {
  discover: '✦', suggested: '◫', installed: '▣', updates: '↻', lordos: '◆', ai: '✧', commercial: '★', remote: '⇄', suites: '❖',
  AudioVideo: '♪', Development: '⌘', Education: 'ⓘ', Game: '♟', Graphics: '◐', Network: '⇄', Office: '▤',
  Science: '⚛', System: '⚙', Utility: '⚒',
};

// #rrggbb + alpha → rgba() (the theme's accent tinted for the pill and the hover)
export function tint(hex, a) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(hex || ''));
  if (!m) return `rgba(10,132,255,${a})`;
  const n = parseInt(m[1], 16);
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`;
}

// The action of a card or an app page: Get / Open / View, a tinted pill (white on the banner).
export function Pill({ label, onClick, big, onDark, disabled }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const hgt = big ? 30 : 24;
  return h(View, {
    onClick: (e) => { e?.stopPropagation?.(); if (!disabled) onClick?.(); },
    onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { height: hgt, paddingHorizontal: big ? 20 : 14, borderRadius: hgt / 2, alignItems: 'center', justifyContent: 'center', flexShrink: 0,
      opacity: disabled ? 0.45 : 1,
      backgroundColor: onDark ? (hover ? '#ffffff' : 'rgba(255,255,255,0.92)') : tint(th.accent, hover && !disabled ? 0.2 : 0.12) },
  }, h(Text, { style: { fontSize: big ? 13.5 : 12.5, fontWeight: 650, color: onDark ? '#1d1d3a' : th.accent, whiteSpace: 'nowrap' } }, label));
}

// A card: icon, name, kind, two lines of summary, and a foot with a note (the size) and the action.
export function Card({ icon, name, kind, summary, note, action, onClick, dim }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const bg = th.mode === 'dark' ? 'rgba(255,255,255,0.045)' : 'rgba(0,0,0,0.025)';
  return h(View, {
    onClick, onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { flexGrow: 1, flexBasis: CARD_BASIS, minWidth: 220, minHeight: CARD_HEIGHT, flexShrink: 0, padding: spacing.m, gap: spacing.m, borderRadius: 12, opacity: dim ? 0.55 : 1,
      backgroundColor: hover ? th.hover : bg, border: `1px solid ${th.separator}` },
  },
  h(View, { style: { flexDirection: 'row', gap: 14, alignItems: 'flex-start' } },
    h(View, { style: { width: 56, height: 56, alignItems: 'center', justifyContent: 'center', flexShrink: 0 } }, icon),
    h(View, { style: { flex: 1, minWidth: 0, gap: 1 } },
      h(Text, { style: { fontSize: 14, fontWeight: 650, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' } }, name),
      kind ? h(Text, { style: { fontSize: 11.5, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, kind) : null,
      summary ? h(Text, { style: { height: 34, fontSize: 12, lineHeight: 1.42, color: th.textDim, marginTop: 5, numberOfLines: 2, overflow: 'hidden', textAlign: 'start' } }, summary) : null)),
  h(View, { style: { flex: 1 } }),
  h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 24, flexShrink: 0 } },
    h(Text, { style: { flex: 1, minWidth: 0, fontSize: 11.5, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, note || ''),
    action || null));
}

// The grid: cards wrap and grow to fill each row; empty fillers keep the last row's cards the same
// width as the others (a CSS auto-fill grid, which the engine doesn't have).
export function CardGrid({ children }) {
  const items = (Array.isArray(children) ? children.flat() : [children]).filter(Boolean);
  const fill = [0, 1, 2, 3].map((i) => h(View, { key: `fill${i}`, style: { flexGrow: 1, flexBasis: CARD_BASIS, minWidth: 220, height: 0 } }));
  return h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', gap: 12 } }, ...items, ...fill);
}

// A section heading over a grid, with an optional "See all" at the end.
export function Shelf({ title, more, onMore, children }) {
  const th = useTheme();
  return h(View, { style: { gap: 12 } },
    h(View, { style: { flexDirection: 'row', alignItems: 'baseline', gap: spacing.s } },
      h(Text, { style: { flex: 1, fontSize: 17, fontWeight: 700, color: th.text, textAlign: 'start' } }, title),
      onMore ? h(Text, { onClick: onMore, style: { fontSize: typeScale.callout, fontWeight: 600, color: th.accent } }, more) : null),
    children);
}

// A color from a name (the same hue the letter icons use), for the banner's glow.
export function nameColor(name, l = 0.55) {
  let hsh = 0;
  for (const ch of String(name)) hsh = (hsh * 31 + ch.codePointAt(0)) >>> 0;
  const hue = hsh % 360, a = 0.6 * Math.min(l, 1 - l);
  const f = (n) => { const k = (n + hue / 30) % 12; return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
  return '#' + [0, 8, 4].map((n) => Math.round(f(n) * 255).toString(16).padStart(2, '0')).join('');
}

// The featured banner: a dark gradient with two soft lights in the app's color behind (its glow;
// the engine has no blur filter, so a wide shadow does it), then the icon, a kicker, the name, the
// summary and the action.
// With `image` (the banner published in LordOS's repository) the picture fills it, under a shade
// that keeps the text readable; `colors` replaces the default gradient.
export function Featured({ icon, glow, image, colors, kicker, title, text, action, onClick }) {
  const light = (size, right, top, alpha) => h(View, { style: { position: 'absolute', right, top, width: size, height: size, borderRadius: size / 2, pointerEvents: 'none',
    backgroundColor: glow, opacity: alpha, boxShadow: `0 0 ${size * 0.6}px ${glow}` } });
  return h(View, {
    onClick,
    style: { minHeight: 168, paddingVertical: 26, paddingHorizontal: 28, borderRadius: 16, overflow: 'hidden', flexDirection: 'row', alignItems: 'center', gap: 22,
      background: `linear-gradient(120deg, ${(colors && colors[0]) || '#1c1638'}, ${(colors && colors[1]) || '#3a2f8a'})`, border: '1px solid rgba(255,255,255,0.12)' },
  },
  image ? h(Image, { src: image, style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, width: '100%', height: '100%', objectFit: 'cover', pointerEvents: 'none' } }) : null,
  image ? h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none',
    background: 'linear-gradient(90deg, rgba(10,8,30,0.82), rgba(10,8,30,0.15))' } }) : null,
  glow && !image ? light(260, 40, -60, 0.22) : null,
  glow && !image ? light(180, 260, 60, 0.12) : null,
  icon ? h(View, { style: { width: 96, height: 96, alignItems: 'center', justifyContent: 'center', flexShrink: 0,
    boxShadow: '0 8px 18px rgba(0,0,0,0.35)', borderRadius: 22 } }, icon) : null,
  h(View, { style: { flex: 1, minWidth: 0, gap: 4 } },
    h(Text, { style: { fontSize: 11.5, fontWeight: 600, color: 'rgba(255,255,255,0.75)', textAlign: 'start' } }, kicker),
    h(Text, { style: { fontSize: 26, fontWeight: 750, color: '#fff', textAlign: 'start' } }, title),
    h(Text, { style: { fontSize: 13.5, lineHeight: 1.5, color: 'rgba(255,255,255,0.86)', maxWidth: 520, numberOfLines: 3, overflow: 'hidden', textAlign: 'start' } }, text)),
  action || null);
}

// The facts row of an app page: up to four label/value columns between hairlines.
export function Facts({ items }) {
  const th = useTheme();
  const list = items.filter((x) => x && x[1]);
  return h(View, { style: { flexDirection: 'row', borderTop: `1px solid ${th.separator}`, borderBottom: `1px solid ${th.separator}` } },
  ...list.map(([label, value], i) => h(View, { key: label, style: { flex: 1, minWidth: 0, paddingVertical: 12, paddingHorizontal: 10, gap: 2, alignItems: 'center',
    ...(i ? { borderStart: `1px solid ${th.separator}` } : {}) } },
  h(Text, { style: { fontSize: 11, color: th.textDim, whiteSpace: 'nowrap' } }, label),
  h(Text, { style: { fontSize: 13, fontWeight: 650, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', maxWidth: '100%' } }, value))));
}

// A placeholder while something isn't there yet (the banner before the first package list): grey
// shapes in its layout that pulse slowly, still with "Reduce motion".
function usePulse() {
  const still = useStore(settings, (st) => !!st.reduceMotion);
  const [on, setOn] = useState(false);
  useEffect(() => {
    if (still) return undefined;
    const id = setInterval(() => setOn((v) => !v), 900);
    return () => clearInterval(id);
  }, [still]);
  return still ? 0.75 : on ? 0.5 : 1;
}

export function Skeleton({ width, height, radius = 6, style }) {
  const th = useTheme();
  const op = usePulse();
  return h(View, { style: [{ width, height, borderRadius: radius, opacity: op, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.07)' }, style] });
}

// The banner's skeleton: the same size and layout, no content.
export function FeaturedSkeleton() {
  const th = useTheme();
  return h(View, { style: { minHeight: 168, paddingVertical: 26, paddingHorizontal: 28, borderRadius: 16, flexDirection: 'row', alignItems: 'center', gap: 22,
    border: `1px solid ${th.separator}` } },
  h(Skeleton, { width: 96, height: 96, radius: 22 }),
  h(View, { style: { flex: 1, gap: 10 } },
    h(Skeleton, { width: 180, height: 11 }),
    h(Skeleton, { width: 280, height: 24 }),
    h(Skeleton, { width: '80%', height: 12 }),
    h(Skeleton, { width: '60%', height: 12 })),
  h(Skeleton, { width: 96, height: 30, radius: 15 }));
}
