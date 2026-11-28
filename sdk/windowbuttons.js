// windowbuttons.js — which side of a title bar the close, minimize and maximize buttons go on, no screen (issue #58).
// Settings → Desktop & Dock → Title bars: 'auto' (each theme's own place: Flow's circles at the start, Classic's
// buttons at the end), 'left' or 'right'. Left and right are the screen's sides in every language; 'auto' follows the
// reading direction, as the themes always did (in Arabic Flow's circles are on the right).
// node server/tests/windowbuttons_test.mjs

export const WINDOW_BUTTON_SIDES = ['auto', 'left', 'right'];

// The saved choice (settings.windowButtons), 'auto' for anything else.
export const windowButtonsPref = (s) => (WINDOW_BUTTON_SIDES.includes(s && s.windowButtons) ? s.windowButtons : 'auto');

// Whether the buttons go first in the title bar's row (the engine mirrors a row right to left). themeStart: the
// theme's own place for 'auto' (Flow: true, Classic: false).
export function buttonsAtStart(pref, themeStart, rtl = false) {
  if (pref === 'left') return !rtl;
  if (pref === 'right') return !!rtl;
  return !!themeStart;
}

// The buttons in the row's order, close always the one nearest the corner: at the start close, minimize, maximize;
// at the end minimize, maximize, close (the orders each side's users know).
export const buttonOrder = (atStart) => (atStart ? ['close', 'minimize', 'maximize'] : ['minimize', 'maximize', 'close']);

// The order Linux apps that draw their own title bar are given (org.gnome.desktop.wm.preferences button-layout,
// served by lord-server's settings portal): the same side as the theme's frames. themeLayout: the theme's own.
export function gtkButtonLayout(pref, themeLayout) {
  if (pref === 'left') return 'close,minimize,maximize:';
  if (pref === 'right') return ':minimize,maximize,close';
  return themeLayout || 'close,minimize,maximize:';
}

// ---- The size of the title bar and its buttons (Settings → Appearance → Window options, GitHub #151 and #219) ----
// 'normal', 'large' or 'extra large', apart from the display's scale: every theme draws its bar, its buttons, their
// glyphs and their hit areas this much bigger, and tells the window manager its new frame (configureWindows' chrome).
export const WINDOW_BUTTON_SIZES = ['normal', 'large', 'xlarge'];
const SIZE_SCALE = { normal: 1, large: 1.25, xlarge: 1.5 };

// The saved choice (settings.windowButtonSize), 'normal' for anything else.
export const windowButtonSizePref = (s) => (WINDOW_BUTTON_SIZES.includes(s && s.windowButtonSize) ? s.windowButtonSize : 'normal');
export const buttonSizeScale = (pref) => SIZE_SCALE[pref] || 1;
// A length of the theme's title bar (its height, a button, a glyph, a gap) at that size: in whole pixels when larger,
// exactly as the theme wrote it at the normal size (a glyph's 1.5 px stroke stays 1.5).
export const scaledBar = (px, pref) => { const k = buttonSizeScale(pref); return k === 1 ? px : Math.round(px * k); };

// ---- The Window menu's tiling layouts (Settings → Appearance → Window options, GitHub #151) ----
// On unless the person turned them off: Move & Resize (halves, quarters, thirds, arrangements, previous size) in the
// Window menu, and each theme's own snap items in its title bar's menu. Dragging to the edges is a separate switch.
export const windowMenuTiling = (s) => !(s && s.windowMenuTiling === false);
