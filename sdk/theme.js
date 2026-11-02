// theme.js — contract between themes and apps. The theme provides the tokens; apps use them
// with useTheme() and so look right with any theme.
import { createContext, createElement, useContext, useEffect } from 'react';
import { settings } from './settings.js';
import { createStore } from './store.js';

// Minimal tokens every theme must provide (see themes/README.md).
export const DEFAULT_TOKENS = {
  mode: 'dark',
  accent: '#0a84ff',
  text: '#f5f5f7',
  textDim: 'rgba(235,235,245,0.55)',
  windowBg: '#1f1f23',
  sidebarBg: '#26262b',
  separator: 'rgba(255,255,255,0.08)',
  hover: 'rgba(255,255,255,0.07)',
  control: '#3a3a40',
  inputBg: '#2c2c31',
  radius: 8,
  wallpapers: [],        // [{ id, nameKey (locale key), name (English fallback), src }]
  // Shape and style of the shared controls (sdk/components.jsx, sdk/controls.jsx). A theme changes
  // them to give every app its look without touching the apps (docs/DESIGN.md, "Theme tokens").
  controlRadius: 7,       // Button, TextInput, SearchField, Stepper, dialog buttons
  controlRadiusSmall: 6,  // small buttons, toolbar buttons, sidebar rows
  groupRadius: 10,        // GroupBox (inset forms)
  menuRadius: 8,          // MenuButton menus, tooltips use menuRadius - 2
  menuItemRadius: 5,
  menuHighlight: 'accent',  // 'accent': the hovered item is filled with the accent; 'subtle': a light fill
  selection: 'fill',      // SidebarItem: 'fill' (accent background) or 'indicator' (light fill and an accent bar)
  inputFocus: 'ring',     // TextInput: 'ring' (accent border) or 'underline' (accent line at the bottom)
  buttonBorder: false,    // Button: a 1 px border around neutral buttons
  popupBg: null,          // menus and tooltips; null = a light or dark surface for the mode
};

// The surface of menus, tooltips and popovers for a theme (popupBg, or one for the mode).
export const popupBg = (t) => t.popupBg || (t.mode === 'dark' ? '#2c2c31' : '#ffffff');

const ThemeContext = createContext(DEFAULT_TOKENS);

// The tokens of the outermost provider, the theme's. The on-screen keyboard is mounted beside the
// theme's tree, not inside it (startShell's layers), and reads them here so it follows the mode and accent.
export const shellTokens = createStore(null);

// Isolated .lpk apps (another process) receive these same tokens live through the broker
// (lord.broker.publish, see docs/PRIVACY.md). Only the theme publishes them, not the app host.
let published = '';
function publishTokens(value) {
  try {
    if (typeof lord === 'undefined' || !lord.broker || typeof lord.broker.publish !== 'function') return;
    const json = JSON.stringify(value, (k, v) => (typeof v === 'function' ? undefined : v));
    if (json !== published) { published = json; lord.broker.publish('theme', json); }
  } catch (e) { /* non-serializable tokens */ }
}

// Linux apps (GTK, GNOME, Qt) read the mode, accent and title-bar button order through
// org.freedesktop.portal.Settings, served by axia-server (ui/portal.c). gtkButtons is the
// GTK order: traffic lights on the left, or buttons on the right in a taskbar-style theme.
const GNOME_ACCENTS = { azul: 'blue', violeta: 'purple', rosa: 'pink', naranja: 'orange', verde: 'green', grafito: 'slate',
  rojo: 'red', amarillo: 'yellow', turquesa: 'teal', celeste: 'blue' };
let portalSent = '';
function syncPortal(value) {
  try {
    if (typeof lord === 'undefined' || !lord.portal) return;
    const name = settings.get().accent || '';
    const msg = { dark: value.mode !== 'light', accent: /^#[0-9a-f]{6}$/i.test(value.accent) ? value.accent : '#0a84ff',
      accentName: GNOME_ACCENTS[name] || 'blue', buttons: value.gtkButtons || 'close,minimize,maximize:' };
    const json = JSON.stringify(msg);
    if (json !== portalSent) { portalSent = json; lord.portal.set(msg); }
  } catch (e) { /* no portal */ }
}

// Settings every app's controls follow, carried with the tokens so isolated apps (which have no
// settings file of their own) get them too: the scroll bars' mode (Settings → Appearance → Show
// scroll bars, sdk/scrollbar.js) and reduce motion. Tokens the theme (or, in an isolated app, the
// published tokens) already carry win.
function settingTokens() {
  const s = settings.get();
  return { scrollBars: s.scrollBars || 'auto', reduceMotion: !!s.reduceMotion };
}

export function ThemeProvider({ tokens, children }) {
  const value = { ...DEFAULT_TOKENS, ...settingTokens(), ...tokens };
  const outermost = useContext(ThemeContext) === DEFAULT_TOKENS;
  useEffect(() => { publishTokens(value); syncPortal(value); if (outermost) shellTokens.set(value); });
  return createElement(ThemeContext.Provider, { value }, children);
}

export const useTheme = () => useContext(ThemeContext);

// A translucent line color (rgba) laid over an opaque one (#rrggbb), as one opaque color. A window of an
// app with a transparent background (the Terminal) has nothing behind its border and title separator,
// so those are drawn already mixed: otherwise what is behind the window shows through them.
export function solidOver(rgba, hex) {
  const m = /rgba?\(([\d.]+),\s*([\d.]+),\s*([\d.]+)(?:,\s*([\d.]+))?\)/.exec(String(rgba));
  const h = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex));
  if (!m || !h) return rgba;
  const a = m[4] == null ? 1 : +m[4];
  const c = [1, 2, 3].map((i) => Math.round(+m[i] * a + parseInt(h[i], 16) * (1 - a)));
  return `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
}
