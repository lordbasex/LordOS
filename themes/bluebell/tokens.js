// Tokens of "Lord Bluebell", a tribute to the desktops of 2001 (docs/THEMES-GUIDE.md, "Style packs" and "App skins").
// The base keys are the contract with the apps (sdk/theme.js); the richer tokens (toolbarBg, buttonBg, bevel,
// stateBorders) give every app's shared controls the era's raised buttons and soft toolbars; the rest is this theme's.
// Every colour here is our own pick for the style; nothing is taken from another system's files.
import { lordosWallpapers } from '@lordos/sdk';

// The same names as the other themes', so the person's accent survives a switch.
export const accents = {
  azul: '#316ac5',
  verde: '#3c9a2a',
  naranja: '#e0731e',
  violeta: '#6b4fbb',
  rosa: '#c6306f',
  grafito: '#5f6b7a',
};

export const wallpapers = lordosWallpapers;

export function makeTheme({ mode, accent }) {
  const dark = mode === 'dark';
  const a = accents[accent] || accents.azul;
  return {
    mode,
    accent: a,
    accentName: accents[accent] ? accent : 'azul',
    accents,
    wallpapers,
    effects: false,
    text: dark ? '#eef1f6' : '#000000',
    textDim: dark ? 'rgba(238,241,246,0.62)' : '#5a5a5a',
    windowBg: dark ? '#23272e' : '#ffffff',
    sidebarBg: dark ? '#1e2530' : '#ece9d8',
    separator: dark ? 'rgba(255,255,255,0.12)' : '#aca899',
    hover: dark ? 'rgba(120,160,230,0.18)' : 'rgba(49,106,197,0.12)',
    control: dark ? '#353b45' : '#f4f3ee',
    inputBg: dark ? '#1b1f25' : '#ffffff',
    radius: 3,
    controlRadius: 3,
    controlRadiusSmall: 3,
    groupRadius: 4,
    menuRadius: 0,
    menuItemRadius: 0,
    menuHighlight: 'accent',
    selection: 'fill',
    inputFocus: 'ring',
    buttonBorder: true,
    popupBg: dark ? '#2a2f38' : '#ffffff',
    gtkButtons: ':minimize,maximize,close',
    scrollThumb: dark ? '#4a5260' : '#c3d3fd', scrollThumbHover: dark ? '#5c6575' : '#b0c6fb',
    scrollThumbBorder: dark ? '#23272e' : '#ffffff', scrollRadius: 3,
    // richer tokens (sdk/theme.js): soft toolbars, raised buttons, a border per state
    toolbarBg: dark ? 'linear-gradient(180deg, #2c323c, #252a32)' : 'linear-gradient(180deg, #fbfbf8, #ece9d8)',
    buttonBg: dark ? 'linear-gradient(180deg, #424a57, #343a44)' : 'linear-gradient(180deg, #ffffff, #ecebe6)',
    bevel: null,
    stateBorders: dark
      ? { normal: '#5d6b80', hover: '#e8a33c', pressed: '#7aa0e0', disabled: '#3c434e', toolbar: 'transparent' }
      : { normal: '#003c74', hover: '#e8a33c', pressed: '#8fa8d8', disabled: '#c9c7ba', toolbar: 'transparent' },
    // This theme's own:
    fontFamily: undefined,                        // set by index.jsx from the style pack (useThemeFont)
    titleBg: dark ? 'linear-gradient(180deg, #2c4c84, #142b55)'
      : 'linear-gradient(180deg, #2d7cf2, #0349c1)',
    titleBgBlur: dark ? 'linear-gradient(180deg, #3a4454, #2c3440)' : 'linear-gradient(180deg, #9ab6ec, #7393dc)',
    titleText: '#ffffff',
    titleTextBlur: dark ? 'rgba(255,255,255,0.65)' : '#d8e4f8',
    frameEdge: dark ? '#1a2e52' : '#0831d9',
    frameEdgeBlur: dark ? '#3a4454' : '#7a96df',
    windowBorder: dark ? '#1a2e52' : '#0831d9',
    closeBg: 'linear-gradient(180deg, #ea8a6c, #c33a20)',
    frameButtonBg: dark ? 'linear-gradient(180deg, #4a6aa8, #2c4d8c)' : 'linear-gradient(180deg, #5a9bff, #1a54d0)',
    barBg: dark ? 'linear-gradient(180deg, #2a4473, #152649)'
      : 'linear-gradient(180deg, #3f84f0, #1c47b0)',
    trayBg: dark ? 'linear-gradient(180deg, #24508f, #1b3f74)' : 'linear-gradient(180deg, #19a6ee, #0f6fd1)',
    startBg: 'linear-gradient(180deg, #4cb84c, #25772a)',
    taskBtn: dark ? 'linear-gradient(180deg, #3f5f9a, #30508a)' : 'linear-gradient(180deg, #3c81f3, #2a6ae8)',
    taskBtnActive: dark ? 'linear-gradient(180deg, #1a2c4f, #20365f)' : 'linear-gradient(180deg, #1e4bb0, #1941a5)',
    panelBg: dark ? '#262b33' : '#ffffff',
    panelFoot: dark ? 'rgba(0,0,0,0.25)' : '#d5e3fa',
    tile: dark ? 'rgba(255,255,255,0.06)' : '#ffffff',
    tileBorder: dark ? 'rgba(255,255,255,0.1)' : '#aca899',
    blur: 0,
    shadow: '0 6px 18px rgba(0,0,0,0.38)',
    barBorder: 'rgba(0,0,0,0.2)',
    // the Files skin's task pane (skins/files.jsx)
    paneBg: dark ? 'linear-gradient(180deg, #2d3b55, #232c3d)' : 'linear-gradient(180deg, #7ba2e7, #6375d6)',
    paneHead: dark ? 'linear-gradient(90deg, #3a4b6b, #2f3e5a)' : 'linear-gradient(90deg, #ffffff, #c6d3f7)',
    paneHeadText: dark ? '#cfe0ff' : '#215dc6',
    paneBody: dark ? '#29344a' : '#d6dff7',
    paneLink: dark ? '#a9c6ff' : '#215dc6',
    paneEdge: dark ? 'rgba(255,255,255,0.12)' : '#ffffff',
    barHover: dark ? 'linear-gradient(180deg, #3c4450, #2f353f)' : 'linear-gradient(180deg, #ffffff, #e7e5dc)',
    barPressed: dark ? 'linear-gradient(180deg, #252a32, #313740)' : 'linear-gradient(180deg, #e3e1d6, #f4f3ee)',
    barHoverEdge: dark ? '#5d6b80' : '#c2bfb0',
    addressBg: dark ? '#2a2f38' : '#ece9d8',
    statusBg: dark ? '#2a2f38' : '#ece9d8',
    listHead: dark ? 'linear-gradient(180deg, #343a44, #2b3038)' : 'linear-gradient(180deg, #ffffff, #ebeadb)',
    selectBg: dark ? '#2f5aa8' : '#316ac5',
    selectText: '#ffffff',
  };
}

// The desktop when no picture was chosen: an original sky of our own, deep blue above, paler toward the horizon, with a
// few soft clouds. Drawn with nodes (themes/bluebell/index.jsx), no image file.
export const sky = (dark) => (dark
  ? { top: '#0d1b33', bottom: '#2a4a78', cloud: 'rgba(170,195,235,0.07)' }
  : { top: '#2c6fd6', bottom: '#a9d0f5', cloud: 'rgba(255,255,255,0.22)' });
