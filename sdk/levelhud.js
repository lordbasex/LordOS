// levelhud.js — the brief level indicator of the volume, screen brightness and keyboard-light keys
// (docs/SHORTCUTS.md). sdk/hwkeys.js calls showLevel after each press; a theme mounts <LevelHud />
// above its windows, as it does <DesktopHud />. The login and lock screens are another session
// (lord-sessiond) and never mount it.
//
//   showLevel(kind, fraction, muted)   kind: 'volume' | 'brightness' | 'keylight', fraction 0..1
//
// A rounded translucent panel near the bottom of the main display: a drawn icon and a bar of
// segments. It stays LEVEL_HUD_MS after the last press, so a held key keeps it up, then fades out by
// setting its node's opacity (no React render per frame); with Reduce motion it just goes.
import { createElement as h } from 'react';
import { View, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { LEVEL_HUD_MS, LEVEL_SEGMENTS, litSegments, speakerWaves, hudOpacity } from './hwkeymodel.js';

const hud = createStore(null);          // { kind, fraction, muted } while shown
let timer = 0, at = 0, panel = null;

const setOpacity = (o) => { if (panel) try { lord.setStyle(panel, 'opacity', o); } catch (e) { /* closed meanwhile */ } };

function tick() {
  const o = hudOpacity(Date.now() - at, !!settings.get().reduceMotion);
  if (o <= 0) { timer = 0; hud.set(null); return; }
  setOpacity(o);
  timer = setTimeout(tick, 16);
}

export function showLevel(kind, fraction, muted = false) {
  hud.set({ kind, fraction: Math.min(1, Math.max(0, +fraction || 0)), muted: !!muted });
  at = Date.now();
  setOpacity(1);
  clearTimeout(timer);
  timer = setTimeout(tick, LEVEL_HUD_MS);
}

// ---- the icons, drawn with views (docs/DESIGN.md, "Icons") ----

const abs = (style) => ({ position: 'absolute', ...style });

// A speaker: a small body, a cone widening to the right, and 0–3 arcs; muted, a cross instead.
function Speaker({ size: s, color, waves, muted }) {
  const d = s * 0.46;
  const arc = (k) => h(View, { key: k, style: abs({ left: s * (0.28 + 0.1 * k), top: s * (0.5 - 0.11 * k - 0.06), width: s * (0.12 + 0.22 * k),
    height: s * (0.12 + 0.22 * k), borderRadius: s, borderRightWidth: Math.max(1.5, s * 0.07), borderColor: color }) });
  const bar = (deg) => h(View, { key: deg, style: abs({ left: s * 0.74, top: s * 0.34, width: Math.max(1.5, s * 0.07), height: s * 0.32,
    borderRadius: 1, backgroundColor: color, transform: `rotate(${deg}deg)` }) });
  return h(View, { style: { width: s, height: s, direction: 'ltr' } },
    h(View, { style: abs({ left: s * 0.04, top: s * 0.37, width: s * 0.16, height: s * 0.26, borderRadius: s * 0.04, backgroundColor: color }) }),
    h(View, { style: abs({ left: s * 0.14, top: s * 0.16, width: s * 0.27, height: s * 0.68, overflow: 'hidden' }) },
      h(View, { style: abs({ left: s * 0.27 - d / 2, top: s * 0.34 - d / 2, width: d, height: d, borderRadius: s * 0.05, backgroundColor: color, transform: 'rotate(45deg)' }) })),
    ...(muted ? [bar(45), bar(-45)] : [1, 2, 3].filter((k) => k <= waves).map(arc)));
}

// Screen brightness: a disc in a ring of eight dots.
function Sun({ size: s, color }) {
  const dots = [0, 45, 90, 135, 180, 225, 270, 315].map((deg) => {
    const r = s * 0.42, a = (deg * Math.PI) / 180, k = s * 0.13;
    return h(View, { key: deg, style: abs({ left: s / 2 + r * Math.cos(a) - k / 2, top: s / 2 + r * Math.sin(a) - k / 2, width: k, height: k, borderRadius: k, backgroundColor: color }) });
  });
  return h(View, { style: { width: s, height: s } },
    ...dots,
    h(View, { style: abs({ left: s * 0.29, top: s * 0.29, width: s * 0.42, height: s * 0.42, borderRadius: s, backgroundColor: color }) }));
}

// Keyboard light: a keyboard outline with two rows of keys and a space bar, three dots of light above.
function KeyLight({ size: s, color }) {
  const line = Math.max(1.5, s * 0.06);
  const key = (row, i) => h(View, { key: `${row}${i}`, style: abs({ left: s * (0.2 + 0.16 * i), top: s * (0.55 + 0.11 * row), width: s * 0.1, height: s * 0.06, borderRadius: 1, backgroundColor: color }) });
  const glow = (i) => h(View, { key: `g${i}`, style: abs({ left: s * (0.235 + 0.21 * i), top: s * (i === 1 ? 0.06 : 0.16), width: s * 0.11, height: s * 0.11, borderRadius: s, backgroundColor: color }) });
  return h(View, { style: { width: s, height: s, direction: 'ltr' } },
    [0, 1, 2].map(glow),
    h(View, { style: abs({ left: s * 0.06, top: s * 0.42, width: s * 0.88, height: s * 0.48, borderRadius: s * 0.1, border: `${line}px solid ${color}` }) }),
    [0, 1, 2, 3].map((i) => key(0, i)), key(1, 0), key(1, 3),
    h(View, { style: abs({ left: s * 0.36, top: s * 0.66, width: s * 0.28, height: s * 0.06, borderRadius: 1, backgroundColor: color }) }));
}

// ---- the panel ----

const ICON = 26, SEG_W = 8, SEG_GAP = 2, SEG_H = 6;

export function LevelHud() {
  const st = useStore(hud);
  const th = useTheme();
  if (!st) { panel = null; return null; }
  const dark = th.mode === 'dark';
  const d = mainDisplay();
  const color = st.muted ? th.textDim : th.text;
  const lit = litSegments(st.fraction);              // muted: the level it comes back to, dimmed
  const icon = st.kind === 'volume' ? h(Speaker, { size: ICON, color, waves: speakerWaves(st.fraction, st.muted), muted: st.muted })
    : st.kind === 'keylight' ? h(KeyLight, { size: ICON, color }) : h(Sun, { size: ICON, color });
  const off = dark ? 'rgba(255,255,255,0.16)' : 'rgba(0,0,0,0.13)';
  const bottom = Math.max(96, Math.round(d.h * 0.13));
  return h(Layer, { name: 'tooltip' },
    h(View, { style: { position: 'absolute', left: d.x, top: d.y + d.h - bottom - 52, width: d.w, alignItems: 'center', pointerEvents: 'none' } },
      h(View, { ref: (n) => { panel = n && n.id ? n.id : null; },
        style: { flexDirection: 'row', alignItems: 'center', gap: 14, height: 52, paddingHorizontal: 18, borderRadius: 26, opacity: 1,
          backgroundColor: dark ? 'rgba(26,26,30,0.78)' : 'rgba(250,250,252,0.82)', border: `1px solid ${th.separator}`, boxShadow: '0 12px 30px rgba(0,0,0,0.28)' } },
        icon,
        h(View, { style: { flexDirection: 'row', gap: SEG_GAP } }, Array.from({ length: LEVEL_SEGMENTS }, (_, i) => h(View, { key: i,
          style: { width: SEG_W, height: SEG_H, borderRadius: 2, backgroundColor: i < lit ? (st.muted ? th.textDim : th.accent) : off } }))))));
}
