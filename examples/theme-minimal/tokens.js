// Tokens of the "Minimal" example theme. The base keys (mode … inputBg) are the contract with the
// apps (sdk/theme.js, docs/THEMES-GUIDE.md "tokens.js"); every app reads them through useTheme().
// The keys after "This theme's own" are used only by this theme's frame and bar.
import { lordosWallpapers } from '@lordos/sdk';

// The same names as the bundled themes' accents, so the user's choice in Settings → Appearance
// survives a theme switch (Settings shows these swatches).
export const accents = {
  azul: '#3478f6',
  violeta: '#9b5de5',
  rosa: '#e5487f',
  naranja: '#f08c2e',
  verde: '#2fb171',
  grafito: '#7d7d85',
};

export function makeTheme({ mode, accent, effects }) {
  const dark = mode === 'dark';
  return {
    mode,
    accent: accents[accent] || accents.azul,
    accents,
    wallpapers: lordosWallpapers,
    text: dark ? '#f2f2f5' : '#1c1c21',
    textDim: dark ? 'rgba(242,242,245,0.58)' : 'rgba(28,28,33,0.6)',
    windowBg: dark ? '#1e1e22' : '#fcfcfd',
    sidebarBg: dark ? '#242429' : '#f2f2f5',
    separator: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)',
    hover: dark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)',
    control: dark ? '#34343a' : '#e6e6eb',
    inputBg: dark ? '#28282d' : '#ffffff',
    // GTK apps put their buttons on the right, like this frame
    gtkButtons: ':minimize,maximize,close',
    // This theme's own:
    titleBg: dark ? '#26262b' : '#ededf0',
    windowBorder: dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.14)',
    barBg: dark ? (effects !== false ? 'rgba(28,28,32,0.82)' : '#1c1c20') : (effects !== false ? 'rgba(244,244,247,0.85)' : '#f1f1f4'),
    blur: effects !== false ? 20 : 0,
    shadow: dark ? '0 12px 32px rgba(0,0,0,0.5)' : '0 12px 32px rgba(0,0,0,0.2)',
  };
}
