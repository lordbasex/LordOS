// themefocusmodel.js — a community theme's own text focus (sdk/themeapi.js), pure: no lord.* calls. Tests in
// server/tests/theme_focus_model_test.mjs.
//
// A theme's menu takes the keys the way an app's own field does (takeTextFocus), but never LordOS's (security scan of
// 0.8.9.6.2, F2: a theme that had LordOS's own focus helpers could point the keys of the keyring prompt's password
// field at itself). So:
//   - the theme's tokens are its own values; the facade registers each one under a token it made (never one of
//     LordOS's), and never hands LordOS's real tokens out: while a field of LordOS's has the keys, the theme sees the
//     same frozen stand-in (`other`), which no call takes back;
//   - taking the keys (or giving them back to one of its tokens) is refused while a field of LordOS's has them, or
//     while `blocked()`: the same policy as a theme's own key listeners (themekeysmodel.js themeHearsKeys): a keyboard
//     layer is open (a system prompt, LordOS's search, a sheet), a dialog owns the keyboard, or a LordOS window has the
//     focus (a Linux app's only while the theme holds the keyboard);
//   - a key reaches the theme's handler only while nothing blocks it, checked again at each key;
//   - never sticky: drop() (the facade calls it on a pointerdown on an app window) takes the keys back from it.
// The facade's real calls: take(real, handler), release(real), owner() → the real token with the keys or null,
// restore(real). wrap(fn) → a handler that calls the theme's fn under its policy.
import { themeHearsKeys } from './themekeysmodel.js';

export const THEME_FOCUS_MAX = 64;

// The theme's text focus is blocked (refused, and no key delivered) when its own key listeners wouldn't hear the key:
// any keyboard layer open, a dialog with the keyboard, a LordOS window focused (a Linux app's unless the theme holds the
// keyboard). The text focus it holds itself doesn't count.
export function themeFocusBlocked({ layerOpen, dialogOwner, focusedApp, holds }) {
  if (layerOpen || dialogOwner) return true;
  return !themeHearsKeys({ layerOpen: false, textFocused: false, focusedApp, holds });
}

export function createThemeFocus({ take, release, owner, restore, blocked, wrap }) {
  const byTheme = new Map();             // the theme's token → { real, fn }
  const byReal = new Map();              // a real token the facade made → the theme's token
  const other = Object.freeze({});       // what the theme sees while LordOS's field has the keys
  const nobody = (o) => o === null || o === undefined;
  const lordHolds = () => { const o = owner(); return !nobody(o) && !byReal.has(o); };
  const busy = () => lordHolds() || !!blocked();
  return {
    other,
    // → true when the theme's token has the keys
    take(tok, handler) {
      if (nobody(tok) || typeof handler !== 'function' || busy()) return false;
      let e = byTheme.get(tok);
      if (!e) {
        if (byTheme.size >= THEME_FOCUS_MAX) return false;
        e = { real: Object.freeze({}), fn: null };
        byTheme.set(tok, e);
        byReal.set(e.real, tok);
      }
      const fn = wrap(handler);
      e.fn = fn;
      take(e.real, (ev) => { if (!blocked() && byTheme.get(tok) === e) fn(ev); });
      return true;
    },
    release(tok) {
      const e = nobody(tok) ? null : byTheme.get(tok);
      if (!e) return;
      byTheme.delete(tok);
      byReal.delete(e.real);
      release(e.real);
    },
    // the theme's own token with the keys, `other` when LordOS's field has them, null for none
    owner() {
      const o = owner();
      if (nobody(o)) return null;
      return byReal.has(o) ? byReal.get(o) : other;
    },
    // back to one of the theme's own tokens (still registered); anything else, or while LordOS's field has the keys or
    // something blocks them, is ignored
    restore(tok) {
      const e = nobody(tok) ? null : byTheme.get(tok);
      if (!e || busy()) return false;
      restore(e.real);
      return true;
    },
    // a click on an app window: the theme's token with the keys lets go of them (and is forgotten)
    drop() {
      const o = owner();
      if (nobody(o) || !byReal.has(o)) return false;
      const tok = byReal.get(o);
      byTheme.delete(tok);
      byReal.delete(o);
      release(o);
      return true;
    },
    size: () => byTheme.size,
  };
}
