// windowbuttons.js — which side of a title bar the close, minimize and maximize buttons go on, no screen (issue #58).
// Settings → Desktop & Dock → Title bars: 'auto' (each theme's own place: Lord Flow's circles at the start, Lord Classic's
// buttons at the end), 'left' or 'right'. Left and right are the screen's sides in every language; 'auto' follows the
// reading direction, as the themes always did (in Arabic Lord Flow's circles are on the right).
// node server/tests/windowbuttons_test.mjs

export const WINDOW_BUTTON_SIDES = ['auto', 'left', 'right'];

// The saved choice (settings.windowButtons), 'auto' for anything else.
export const windowButtonsPref = (s) => (WINDOW_BUTTON_SIDES.includes(s && s.windowButtons) ? s.windowButtons : 'auto');

// Whether the buttons go first in the title bar's row (the engine mirrors a row right to left). themeStart: the
// theme's own place for 'auto' (Lord Flow: true, Lord Classic: false).
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
