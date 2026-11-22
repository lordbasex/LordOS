// tokens.js: Lord Celeste's tokens for the apps (sdk/theme.js), in dark and light, built on every render from the
// settings (mode, accent, effects) and the colour scheme of the theme's options (cae.js). The apps' accent is the
// user's accent from Settings → Appearance, with the names the bundled themes use so the choice survives a switch.
import { lordosWallpapers } from '@lordos/sdk';
import { caeTokens } from './cae.js';

export const accents = {
  azul: '#7aa7ff',
  violeta: '#b69df8',
  rosa: '#f48fb1',
  naranja: '#f2b45a',
  verde: '#6fcf97',
  grafito: '#9a96a3',
};

export const wallpapers = [...lordosWallpapers];

export function makeTheme(s = {}) {
  const cx = caeTokens(s);
  const dark = cx.mode === 'dark';
  const effects = s.effects !== false;
  const accent = accents[s.accent] || cx.primary;
  const popup = dark ? (effects ? 'rgba(29,27,32,0.92)' : '#1d1b20') : (effects ? 'rgba(254,247,255,0.94)' : '#fef7ff');
  return {
    mode: cx.mode,
    accent,
    accents,
    wallpapers,
    effects,

    text: cx.text,
    textDim: dark ? 'rgba(230,224,233,0.64)' : 'rgba(29,27,32,0.62)',
    windowBg: dark ? '#1d1b20' : '#fffbfe',
    sidebarBg: dark ? '#17151a' : '#f3edf7',
    separator: dark ? 'rgba(255,255,255,0.10)' : 'rgba(0,0,0,0.09)',
    hover: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)',
    control: dark ? 'rgba(255,255,255,0.10)' : '#e8e0e8',
    inputBg: dark ? 'rgba(255,255,255,0.07)' : '#ffffff',

    controlRadius: 12,
    controlRadiusSmall: 8,
    groupRadius: 16,
    menuRadius: 16,
    menuItemRadius: 8,

    menuBg: popup,
    menuText: cx.text,
    menuTextDim: cx.dim,
    menuHover: dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.07)',
    panelBg: popup,
    popupBg: popup,
    windowBorder: dark ? 'rgba(255,255,255,0.10)' : 'rgba(0,0,0,0.12)',
    titleBg: dark ? '#211f26' : '#f3edf7',
    titleBgBlur: dark ? '#1b1a1f' : '#ece6f0',
    lightOff: dark ? '#4a4650' : '#cac4d0',
    lightGlyph: dark ? '#141218' : '#1d1b20',
    shadow: dark ? '0 22px 56px rgba(0,0,0,0.55)' : '0 18px 44px rgba(0,0,0,0.22)',
    shadowDim: dark ? '0 10px 28px rgba(0,0,0,0.34)' : '0 8px 22px rgba(0,0,0,0.14)',
    scrollThumb: dark ? 'rgba(255,255,255,0.28)' : 'rgba(0,0,0,0.25)',
    scrollThumbHover: dark ? 'rgba(255,255,255,0.48)' : 'rgba(0,0,0,0.42)',
    blur: effects ? 24 : 0,
  };
}
