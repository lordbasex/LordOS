// Tokens of the "Lord Flow" theme. The base keys (text, accent, windowBg…) are the contract
// with apps (see sdk/theme.js); the rest belongs to this theme.
import { lordosWallpapers } from '@lordos/sdk';

export const accents = {
  azul: '#0a84ff',
  violeta: '#bf5af2',
  rosa: '#ff375f',
  naranja: '#ff9f0a',
  verde: '#30d158',
  grafito: '#8e8e93',
};

// The LordOS collection (Signature, Lumo, Cosmos; sdk/wallpapers.js) and this theme's own
// gradients (tools/gen-wallpaper.py). name: English fallback; the shown name is the translation of
// nameKey (assets/locales/*/shell.json). Thumbnails: assets/thumbs/ (320×180).
const gradients = [
  { id: 'aurora', category: 'gradients', nameKey: 'shell.wallpaper.aurora', name: 'Aurora', src: 'assets/aurora.png', thumb: 'assets/thumbs/aurora.jpg' },
  { id: 'ocean', category: 'gradients', nameKey: 'shell.wallpaper.ocean', name: 'Ocean', src: 'assets/ocean.png', thumb: 'assets/thumbs/ocean.jpg' },
  { id: 'sunset', category: 'gradients', nameKey: 'shell.wallpaper.sunset', name: 'Sunset', src: 'assets/sunset.png', thumb: 'assets/thumbs/sunset.jpg' },
  { id: 'graphite', category: 'gradients', nameKey: 'shell.wallpaper.graphite', name: 'Graphite', src: 'assets/graphite.png', thumb: 'assets/thumbs/graphite.jpg' },
];
export const wallpapers = [...lordosWallpapers, ...gradients];

export function makeTheme({ mode, accent, effects }) {
  const dark = mode === 'dark';
  return {
    mode,
    accent: accents[accent] || accents.azul,
    accents,
    wallpapers,
    effects,
    text: dark ? '#f5f5f7' : '#1d1d1f',
    textDim: dark ? 'rgba(235,235,245,0.55)' : 'rgba(60,60,67,0.6)',
    windowBg: dark ? '#1f1f23' : '#ffffff',
    sidebarBg: dark ? '#26262b' : '#f3f3f6',
    separator: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)',
    hover: dark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)',
    control: dark ? '#3a3a40' : '#e3e3e8',
    inputBg: dark ? '#2a2a2f' : '#ffffff',
    // This theme's own:
    menuBg: dark ? 'rgba(24,24,30,0.42)' : 'rgba(250,250,252,0.55)',
    menuText: dark ? '#f5f5f7' : '#1d1d1f',
    menuHover: dark ? 'rgba(255,255,255,0.16)' : 'rgba(0,0,0,0.08)',
    panelBg: dark ? (effects ? 'rgba(36,36,42,0.72)' : '#2a2a30') : (effects ? 'rgba(246,246,248,0.78)' : '#f2f2f5'),
    windowBorder: dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.14)',
    titleBg: dark ? '#2b2b30' : '#ececef',
    dockBg: dark ? 'rgba(46,46,56,0.42)' : 'rgba(255,255,255,0.38)',
    dockBorder: dark ? 'rgba(255,255,255,0.18)' : 'rgba(255,255,255,0.55)',
    lightOff: dark ? '#4a4a50' : '#c8c8cc',
    // overlay scroll bars (sdk/components.jsx ScrollView): a soft pill
    scrollThumb: dark ? 'rgba(255,255,255,0.32)' : 'rgba(0,0,0,0.3)',
    scrollThumbHover: dark ? 'rgba(255,255,255,0.5)' : 'rgba(0,0,0,0.45)',
    blur: effects ? 24 : 0,
  };
}
