// Tokens of the "Lord Classic" theme. The base keys (text, accent, windowBg…) are the contract with the
// apps (sdk/theme.js); the shape keys (controlRadius, menuHighlight, selection, inputFocus,
// buttonBorder, popupBg) restyle the SDK's shared controls, so every app takes this look without
// being changed. The rest belongs to this theme (docs/DESIGN.md, "Lord Classic").
import { lordosWallpapers } from '@lordos/sdk';

// The same names as the default theme's, so the user's accent survives a theme switch.
export const accents = {
  azul: '#2f6fed',
  violeta: '#8b5cf6',
  rosa: '#e11d74',
  naranja: '#ea7a0c',
  verde: '#16a34a',
  grafito: '#6b7280',
};

export const wallpapers = lordosWallpapers;

export function makeTheme({ mode, accent, effects }) {
  const dark = mode === 'dark';
  const glass = effects !== false;
  return {
    mode,
    accent: accents[accent] || accents.azul,
    accents,
    wallpapers,
    effects: glass,
    text: dark ? '#f1f1f4' : '#1b1b20',
    textDim: dark ? 'rgba(241,241,244,0.6)' : 'rgba(27,27,32,0.6)',
    windowBg: dark ? '#202024' : '#fbfbfc',
    sidebarBg: dark ? '#1a1a1e' : '#f2f2f5',
    separator: dark ? 'rgba(255,255,255,0.085)' : 'rgba(0,0,0,0.09)',
    hover: dark ? 'rgba(255,255,255,0.065)' : 'rgba(0,0,0,0.045)',
    control: dark ? '#2c2c32' : '#f4f4f6',
    inputBg: dark ? '#26262b' : '#ffffff',
    radius: 6,
    // shared controls (sdk/theme.js): small radii, bordered buttons, light menu highlight,
    // selection with an accent bar, an accent underline on the focused field
    controlRadius: 4,
    controlRadiusSmall: 4,
    groupRadius: 6,
    menuRadius: 8,
    menuItemRadius: 4,
    menuHighlight: 'subtle',
    selection: 'indicator',
    inputFocus: 'underline',
    buttonBorder: true,
    popupBg: dark ? '#2b2b30' : '#fafafb',
    gtkButtons: ':minimize,maximize,close',     // GTK apps: buttons on the right, like this frame
    // overlay scroll bars (sdk/components.jsx ScrollView): a squarer, solid thumb with a hairline
    scrollThumb: dark ? '#55555c' : '#a9a9b1', scrollThumbHover: dark ? '#6a6a72' : '#8e8e97',
    scrollThumbBorder: dark ? '#202024' : '#fbfbfc', scrollRadius: 2,
    // This theme's own:
    titleBg: dark ? '#202024' : '#fbfbfc',
    titleBgBlur: dark ? '#1b1b1f' : '#f1f1f4',   // the title bar of a window that isn't in front
    windowBorder: dark ? 'rgba(255,255,255,0.11)' : 'rgba(0,0,0,0.13)',
    closeHover: '#d83b33',
    barBg: dark ? (glass ? 'rgba(30,30,35,0.80)' : '#1e1e23') : (glass ? 'rgba(242,243,246,0.82)' : '#eff0f3'),
    barBorder: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)',
    panelBg: dark ? (glass ? 'rgba(38,38,43,0.88)' : '#26262b') : (glass ? 'rgba(248,248,250,0.90)' : '#f6f6f8'),
    panelFoot: dark ? 'rgba(0,0,0,0.18)' : 'rgba(0,0,0,0.035)',
    tile: dark ? 'rgba(255,255,255,0.06)' : 'rgba(255,255,255,0.75)',
    tileBorder: dark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.07)',
    blur: glass ? 28 : 0,
    shadow: dark ? '0 16px 40px rgba(0,0,0,0.55)' : '0 16px 40px rgba(0,0,0,0.22)',
  };
}
