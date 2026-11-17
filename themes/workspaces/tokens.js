// Tokens of the "Lord Workspaces" theme. The base keys (text, accent, windowBg…) are the contract with the apps
// (sdk/theme.js); the shape keys restyle the SDK's shared controls; the rest is this theme's own, above all the three
// effects styles of its surfaces (the Context Spine, the activity strip, the status cluster, panels and menus):
//
//   opaque       solid panels, no blur: the cheapest to paint
//   translucent  the classic look: a tinted panel that blurs what is behind it
//   crystal      the translucent panel plus light: a soft two-stop sheen from the top, a darker
//                inner bottom edge (the light bending at the glass's lower rim) and a depth shadow. Tuned per mode.
//
// Settings → Appearance → Transparency off (settings.effects === false) is always opaque. Colours are the brand's
// violets (#7C5CFF, #5B3FE0; the gradient #B46CFF → #4F46E5) and neutrals with a violet bias; contrast is checked in
// server/tests/workspaces_model_test.mjs. Built from useSettings() (memoized in index.jsx), with the SDK's wallpapers passed
// in, so this file is plain data that node can test.
import { SPINE_BLUR } from './model/geometry.js';

// The shared accent names, so the person's accent survives a theme switch. 'violeta' is the brand's own violet.
export const accents = {
  azul: '#3D7BFD',
  violeta: '#7C5CFF',
  rosa: '#E5487F',
  naranja: '#E2711A',
  verde: '#22A565',
  grafito: '#7A7891',
  // the proposal's amber (origin.accent #F4AE48 / #A9650A): deeper in dark mode so the white text apps put on the
  // accent keeps 3:1; with it the focus ring stays the brand's violet (origin.focus)
  ambar: '#C27A12',
};
// The same hues one step deeper: text and white-on-accent fills in light mode keep 4.5:1 or close to it.
export const deep = {
  azul: '#2563EB',
  violeta: '#5B3FE0',
  rosa: '#C8306A',
  naranja: '#C2570C',
  verde: '#15803D',
  grafito: '#5D5B78',
  ambar: '#A9650A',
};

export const BRAND_GRADIENT = 'linear-gradient(135deg, #B46CFF, #4F46E5)';

// One material for every surface of the theme, per effects style and mode.
export function material(surface, dark) {
  if (surface === 'opaque') {
    return {
      bg: dark ? '#221F38' : '#F7F5FD', bgStill: dark ? '#221F38' : '#F7F5FD', border: dark ? 'rgba(255,255,255,0.09)' : 'rgba(20,16,60,0.11)', blur: 0,
      sheen: null, edge: null, rim: null,
      shadow: dark ? '0 10px 26px rgba(5,3,20,0.45)' : '0 10px 26px rgba(40,30,110,0.14)',
    };
  }
  if (surface === 'translucent') {
    return {
      bg: dark ? 'rgba(26,22,50,0.74)' : 'rgba(255,255,255,0.78)', bgStill: dark ? 'rgba(26,22,50,0.86)' : 'rgba(255,255,255,0.88)', border: dark ? 'rgba(255,255,255,0.10)' : 'rgba(20,16,60,0.10)', blur: SPINE_BLUR,
      sheen: null, edge: null, rim: null,
      shadow: dark ? '0 12px 30px rgba(5,3,20,0.42)' : '0 12px 30px rgba(40,30,110,0.16)',
    };
  }
  return {
    bg: dark ? 'rgba(32,27,64,0.50)' : 'rgba(255,255,255,0.52)',
    // the same glass where it doesn't blur (the top band, docs/WORKSPACES.md): thicker, so text stays readable
    bgStill: dark ? 'rgba(32,27,64,0.80)' : 'rgba(255,255,255,0.80)',
    border: dark ? 'rgba(255,255,255,0.16)' : 'rgba(255,255,255,0.70)',
    blur: SPINE_BLUR,
    // the sheen: brighter at the top, almost nothing at the bottom (two stops: what the renderer draws)
    sheen: dark ? 'linear-gradient(180deg, rgba(255,255,255,0.15), rgba(255,255,255,0.01))' : 'linear-gradient(180deg, rgba(255,255,255,0.70), rgba(255,255,255,0.12))',
    // read by nothing now (the group frame dropped its accent top line in 0.8.8.10; surfaces draw no bright top edge)
    edge: dark ? 'rgba(255,255,255,0.42)' : 'rgba(255,255,255,0.98)',
    rim: dark ? 'rgba(0,0,0,0.30)' : 'rgba(40,30,110,0.12)',
    shadow: dark ? '0 18px 40px rgba(6,3,26,0.50)' : '0 18px 38px rgba(52,38,140,0.20)',
  };
}

// High contrast (the theme's option, Settings → Appearance): solid black or white surfaces, pure text, a visible line
// round every surface and control, an accent that holds 7:1 against them, no blur and no shadows. Checked in
// server/tests/workspaces_model_test.mjs (text 21:1, dim text and the accent ≥ 7:1, the text on the accent ≥ 7:1).
export function highContrast(t, dark) {
  const fg = dark ? '#FFFFFF' : '#000000', bg = dark ? '#000000' : '#FFFFFF', line = dark ? '#FFFFFF' : '#000000';
  const accent = dark ? '#C9BBFF' : '#3B1FB0', onAccent = dark ? '#000000' : '#FFFFFF';
  const mat = { bg, bgStill: bg, border: line, blur: 0, sheen: null, edge: null, rim: null, shadow: 'none' };
  return {
    ...t, contrast: true, accent, onAccent, focusRing: accent, effects: false, surface: 'opaque', mat, blur: 0, panelBlur: 0,
    text: fg, textDim: dark ? '#E6E6E6' : '#1F1F1F', windowBg: bg, sidebarBg: bg, separator: line, hover: dark ? 'rgba(255,255,255,0.22)' : 'rgba(0,0,0,0.16)',
    iconFg: fg, iconFgHover: fg, itemHover: dark ? 'rgba(255,255,255,0.28)' : 'rgba(0,0,0,0.20)', barSeparator: line, control: dark ? '#1A1A1A' : '#F0F0F0',
    inputBg: bg, popupBg: bg, solidBg: bg, panelBg: bg, windowBorder: line, titleBg: bg, titleBgBlur: bg, chipBg: bg, frameBg: 'transparent', frameText: fg,
    shadow: 'none', spineShadow: 'none', shadowSoft: 'none', buttonBorder: true,
  };
}

export function makeTheme({ mode, accent, effects, surface, contrast = false }, wallpapers = []) {
  if (contrast) return highContrast(makeTheme({ mode, accent, effects: false, surface: 'opaque' }, wallpapers), mode === 'dark');
  const dark = mode === 'dark';
  const s = effects === false ? 'opaque' : surface || 'crystal';
  const m = material(s, dark);
  const name = accents[accent] ? accent : 'violeta';
  const acc = dark ? accents[name] : deep[name];
  return {
    mode,
    accent: acc,
    accents,
    wallpapers,
    effects: s !== 'opaque',
    surface: s,
    text: dark ? '#F1EFFC' : '#191733',
    textDim: dark ? 'rgba(241,239,252,0.64)' : 'rgba(25,23,51,0.66)',
    windowBg: dark ? '#1C1A2E' : '#FFFFFF',
    sidebarBg: dark ? '#191729' : '#F4F2FB',
    separator: dark ? 'rgba(255,255,255,0.09)' : 'rgba(20,16,60,0.10)',
    hover: dark ? 'rgba(255,255,255,0.075)' : 'rgba(20,16,60,0.06)',
    // the bars' controls (Context Spine, activity strip, status cluster; components/BarButton.jsx): the glyphs' colour
    // at rest and under the pointer, the pill that shows under the pointer, the line between groups
    iconFg: dark ? 'rgba(241,239,252,0.64)' : 'rgba(25,23,51,0.66)',
    iconFgHover: dark ? '#F1EFFC' : '#191733',
    itemHover: dark ? 'rgba(255,255,255,0.14)' : 'rgba(20,16,60,0.09)',
    barSeparator: dark ? 'rgba(255,255,255,0.14)' : 'rgba(20,16,60,0.12)',
    control: dark ? '#2D2A45' : '#ECE9F7',
    inputBg: dark ? '#232039' : '#FFFFFF',
    radius: 10,
    // shared controls (sdk/theme.js): soft radii, accent menu highlight, filled selection, a focus ring
    controlRadius: 8,
    controlRadiusSmall: 6,
    groupRadius: 12,
    menuRadius: 12,
    menuItemRadius: 7,
    menuHighlight: 'accent',
    selection: 'fill',
    inputFocus: 'ring',
    buttonBorder: false,
    popupBg: dark ? '#252239' : '#FBFAFF',
    gtkButtons: ':minimize,maximize,close',
    scrollThumb: dark ? 'rgba(255,255,255,0.30)' : 'rgba(20,16,60,0.28)',
    scrollThumbHover: dark ? 'rgba(255,255,255,0.48)' : 'rgba(20,16,60,0.44)',
    // This theme's own:
    mat: m,
    // an opaque colour close to the surfaces, for glyphs that cut a hole
    solidBg: dark ? '#221F38' : '#F7F5FD',
    blur: s === 'opaque' ? 0 : 20,
    panelBg: s === 'opaque' ? m.bg : dark ? 'rgba(30,26,56,0.86)' : 'rgba(251,250,255,0.88)',
    panelBlur: s === 'opaque' ? 0 : 20,
    windowBorder: dark ? 'rgba(255,255,255,0.12)' : 'rgba(20,16,60,0.14)',
    titleBg: dark ? '#221F37' : '#F4F2FB',
    titleBgBlur: dark ? '#1D1B30' : '#EEEBF8',
    closeHover: '#E5484D',
    // the group frame is only its border (a fill over the whole work area is the costly part of an activity switch on
    // the CPU path, docs/WORKSPACES.md); its header's name and actions sit on small tinted chips, never blurred
    chipBg: dark ? (s === 'opaque' ? '#221F38' : 'rgba(22,18,44,0.78)') : (s === 'opaque' ? '#F7F5FD' : 'rgba(255,255,255,0.84)'),
    // the Control Center's preview of the frame
    frameBg: dark ? (s === 'opaque' ? 'rgba(16,13,34,0.62)' : 'rgba(16,13,34,0.30)') : (s === 'opaque' ? 'rgba(246,244,253,0.70)' : 'rgba(255,255,255,0.26)'),
    frameText: dark ? '#F1EFFC' : '#191733',
    // windows: short shadows (they reach WINDOW_SHADOW px; model/geometry.js keeps that clear of the spine's margin,
    // and a short shadow is cheaper to paint on every frame a window moves)
    shadow: dark ? '0 8px 16px rgba(6,3,24,0.55)' : '0 8px 16px rgba(40,30,110,0.22)',
    // the spine's shadow stays short: whatever its blur and shadow reach repaints the whole spine when it changes, and
    // the top band's glide sits just past that (model/geometry.js)
    spineShadow: 'none',
    shadowSoft: dark ? '0 4px 12px rgba(6,3,24,0.40)' : '0 4px 12px rgba(40,30,110,0.14)',
    onAccent: '#FFFFFF',
    ...(name === 'ambar' ? { focusRing: dark ? '#7C5CFF' : '#5B3FE0' } : null),
    brandGradient: BRAND_GRADIENT,
  };
}

// The style of a surface's own box (the caller adds position and size). Crystal's sheen, edge and rim are children
// (<Crystal />) so this stays one node with no gradient of its own.
// still: no live blur (a surface next to another blurred one, or that changes often), the thicker glass instead.
export function surfaceStyle(t, radius = 16, still = false) {
  const m = t.mat;
  const blur = !still && m.blur;
  return {
    borderRadius: radius, backgroundColor: blur ? m.bg : m.bgStill, border: `1px solid ${m.border}`, boxShadow: m.shadow, overflow: 'hidden',
    ...(blur ? { backdropFilter: `blur(${m.blur}px)` } : null),
  };
}
