// switchermodel.js — the app switcher's rules, with no UI (sdk/appswitcher.jsx, docs/SHORTCUTS.md, tests/switcher_model_test.mjs).

export const SWITCHER_DEFAULTS = {
  keys: 'theme',        // 'theme': ❖Tab in Lord Flow, ⌥Tab in Lord Classic; or 'meta', 'alt', 'both'
  scope: 'all',         // 'all', 'desktop' (this desktop) or 'display' (this display)
  hideMinimized: false, // an app with every window minimized stays out
  numbers: true,        // 1 to 9 on the first apps; pressing one goes to it
  windows: true,        // the row with the selected app's windows
  size: 'm',            // 's', 'm' or 'l'
  skin: 'system',       // 'system', 'glass', 'solid', 'contrast' or 'custom'
  customBg: '#20202a',
  customAccent: '#a855f7',
  excluded: [],         // Dock ids that never show
};

export function switcherPrefs(saved) {
  const p = { ...SWITCHER_DEFAULTS, ...(saved && typeof saved === 'object' ? saved : {}) };
  if (!['theme', 'meta', 'alt', 'both'].includes(p.keys)) p.keys = 'theme';
  if (!['all', 'desktop', 'display'].includes(p.scope)) p.scope = 'all';
  if (!['s', 'm', 'l'].includes(p.size)) p.size = 'm';
  if (!['system', 'glass', 'solid', 'contrast', 'custom'].includes(p.skin)) p.skin = 'system';
  if (!Array.isArray(p.excluded)) p.excluded = [];
  return p;
}

// Which modifiers open it, in a theme whose own is `own` ('meta' or 'alt').
export const switcherMods = (keys, own) => {
  const k = keys === 'theme' || !keys ? own : keys;
  return k === 'both' ? ['meta', 'alt'] : [k];
};
export const switcherBits = (e) => (e.mods & 8 ? 8 : 2);
export const switcherGrabName = (base, keys) => (!keys || keys === 'theme' ? base : `${base}+tab-${keys}`);

// The windows of a group that count here, or none if the group stays out.
export function groupWindows(group, prefs, { inDesktop, inDisplay }) {
  if (prefs.excluded.includes(group.key)) return [];
  let list = group.windows;
  if (prefs.scope === 'desktop') list = list.filter(inDesktop);
  if (prefs.scope === 'display') list = list.filter(inDisplay);
  if (prefs.hideMinimized && list.every((w) => w.minimized)) return [];
  return list;
}

export const SIZES = { s: { icon: 48, cell: 72 }, m: { icon: 72, cell: 104 }, l: { icon: 96, cell: 132 } };

export const validColor = (c) => typeof c === 'string' && /^#[0-9a-fA-F]{6}$/.test(c);

export function skinColors(prefs, th) {
  const dark = th.mode === 'dark';
  switch (prefs.skin) {
    case 'glass': return { bg: dark ? 'rgba(30,30,34,0.6)' : 'rgba(245,245,247,0.6)', blur: 24, accent: th.accent, text: th.text, border: th.separator };
    case 'solid': return { bg: dark ? '#1c1c20' : '#f2f2f4', blur: 0, accent: th.accent, text: th.text, border: th.separator };
    case 'contrast': return { bg: dark ? '#000000' : '#ffffff', blur: 0, accent: dark ? '#ffffff' : '#000000', text: dark ? '#ffffff' : '#000000', border: dark ? '#ffffff' : '#000000' };
    case 'custom': return { bg: validColor(prefs.customBg) ? prefs.customBg : SWITCHER_DEFAULTS.customBg, blur: 0,
      accent: validColor(prefs.customAccent) ? prefs.customAccent : SWITCHER_DEFAULTS.customAccent, text: th.text, border: th.separator };
    default: return { bg: dark ? 'rgba(30,30,34,0.92)' : 'rgba(245,245,247,0.94)', blur: 0, accent: th.accent, text: th.text, border: th.separator };
  }
}

// evdev codes, the same in every layout
export const digitOf = (code) => (code >= 2 && code <= 10 ? code - 2 : -1);     // 1 → 0 … 9 → 8
export const WINDOW_NEXT = new Set([44, 41, 108]);                               // Z, the key under Esc, ↓
export const WINDOW_BACK = 103;                                                  // ↑

export const stepWindow = (index, count, step) => (count ? (index + step + count) % count : 0);

// How much a window shrinks to fit a card.
export const fitScale = (w, h, cw, ch) => Math.min(cw / Math.max(1, w), ch / Math.max(1, h));
