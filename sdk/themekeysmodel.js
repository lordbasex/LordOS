// themekeysmodel.js — when a community theme's own key listeners hear a key (sdk/themeapi.js keysForTheme), pure.
// Only when nothing of LordOS's takes it: no keyboard layer open (a system prompt, LordOS's search, a sheet), no text
// field with the keyboard, no LordOS window in front. Holding the keyboard (the theme's menu) only adds a Linux app's
// window in front, whose keys the hold keeps from the app anyway. docs/THEMES-GUIDE.md §6.
export function themeHearsKeys({ layerOpen, textFocused, focusedApp, holds }) {
  if (layerOpen || textFocused) return false;
  if (focusedApp == null) return true;
  return holds > 0 && focusedApp === 'linux';
}
