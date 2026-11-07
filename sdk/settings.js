// settings.js — user settings, stored in ~/.config/lordos/settings.json.
import { createStore, useStore } from './store.js';
import { DEFAULT_WALLPAPER } from './wallpapers.js';

const DEFAULTS = {
  mode: 'dark',          // 'dark' | 'light'
  accent: 'azul',
  effects: true,         // transparency with blur
  reduceMotion: false,   // decorative animations off (Settings → Appearance)
  browserTabsInTitlebar: true,   // 0.8.4, Settings → Appearance: Chromium and Firefox draw their own bar (browserframe.js)
  browserFramePending: false,    // that switch is still waiting for Chromium to quit
  scrollBars: 'auto',    // 0.8.3, Settings → Appearance → Show scroll bars: 'auto' | 'scrolling' | 'always'
  motion: {},            // moving wallpaper per display: { '*': id, <display id>: id } (sdk/motion.jsx)
  motionFiles: {},       // id → the variant on disk
  motionUser: [],        // added videos: [{ id: 'user:…', name, path }]
  wallpaperImages: [],   // the user's own pictures, newest first: ids 'image:<name>' (sdk/wallpapers.js)
  wallpaper: DEFAULT_WALLPAPER,  // 0.8.2: Lumo Celestial (before: 'aurora', still available)
  keyboard: 'latam',     // before M5; now keyboards (see keyboard.js)
  keyboards: null,       // [{ layout, variant }] from xkb-data
  keyboardIndex: 0,
  language: null,        // interface language ('es', 'en', 'pt', 'fr', 'it'…); null = the region's
  languages: null,       // preferred languages in order, the first is `language` (Settings → Language & Region); null = en, es, pt
  region: '',            // formats region ('es_AR'…); '' = the language's
  theme: null,           // id of the chosen theme (folder in themes/)
  desktopCount: 1,            // 0.8.7, how many desktops (sdk/desktops.js)
  desktopNames: null,         // 0.8.8.3, their names by position, '' for the default (sdk/desktops.js)
  desktopArrows: true,        // 0.8.7, Settings → Keyboard: Lord Flow's ⌃ arrows are the Desktops keys
  switcher: null,             // 0.8.7.1, Settings → Keyboard: the app switcher (switchermodel.js)
  sessionRestore: 'always',   // 0.8.7, Settings → Session: 'off' | 'ask' | 'always' (sessionrestore.jsx)
  sessionBrowsers: true,      // browsers reopen their tabs too (the browser-restore admin action)
  remoteDesks: null,          // 0.8.7, the RDP and VNC apps' saved computers (no passwords)
  files: null,                // 0.8.7, Files → Settings (apps/files/prefs.js)
  pickerView: null,           // 0.8.8.3, the system file dialog's List / Thumbnails: { files, images } (sdk/filepickermodel.js viewFor)
  sessionSaved: null,         // { clean, windows } what was open; written by sessionrestore.jsx only
};

// Per-user settings migrations (docs/UPDATES.md, "Migrations"): when an update changes the shape or
// meaning of a setting, it appends [schema, (settings) => newSettings] here. Each runs once per user,
// in order, when the session loads the file; "schema" records the last one applied. The system-wide
// migrations (as root) are the lordos package's (packaging/migrations).
export const SETTINGS_SCHEMA = 2;
// A Terminal key mapping that sends what a US keyboard's key types anyway (keys.js names keys by their
// US position): on a US keyboard it does nothing, on any other it types the wrong character. The iTerm2
// importer made them from keypad entries before 0.8.8.6 ("-" typed "/" on a Latin American keyboard).
const US_SHIFTED = { 8: '*', '=': '+' };
export function usNoopMapping(m) {
  if (!m || m.action !== 'text' || typeof m.combo !== 'string') return false;
  const k = m.combo.startsWith('Shift+') ? US_SHIFTED[m.combo.slice(6)] : /^[0-9./*+=-]$/.test(m.combo) ? m.combo : null;
  return k != null && m.param === k;
}
function dropUsNoops(list) { return Array.isArray(list) ? list.filter((m) => !usNoopMapping(m)) : list; }
const MIGRATIONS = [
  [1, (s) => s],          // 0.8.1: the schema number starts being recorded; nothing changes
  [2, (s) => {            // 0.8.8.6: Terminal mappings imported from iTerm2's keypad (usNoopMapping)
    const t = s.terminal;
    if (!t || typeof t !== 'object') return s;
    const profiles = Array.isArray(t.profiles) ? t.profiles.map((p) => (p && p.keyMap ? { ...p, keyMap: dropUsNoops(p.keyMap) } : p)) : t.profiles;
    return { ...s, terminal: { ...t, profiles, keyMap: dropUsNoops(t.keyMap) } };
  }],
];
export function migrateSettings(s) {
  let out = s;
  for (const [to, fn] of MIGRATIONS) if ((out.schema || 0) < to) out = { ...fn(out), schema: to };
  return out;
}

function load() {
  try {
    const txt = lord.settings.load();
    return txt ? migrateSettings({ ...DEFAULTS, ...JSON.parse(txt) }) : { ...DEFAULTS, schema: SETTINGS_SCHEMA };
  } catch (e) {
    console.warn('unreadable settings, using the defaults:', e);
    return { ...DEFAULTS };
  }
}

export const settings = createStore(load());
// Developer timing (settings.json "debugTiming": true, no UI): opening an app logs how long its
// first window took (sdk/components.jsx AppHost, sdk/packages.js isolatedApp, docs/PRIVACY.md).
globalThis.__lordTiming = !!settings.get().debugTiming;

let pending = 0;
settings.subscribe(() => {
  clearTimeout(pending);
  pending = setTimeout(() => lord.settings.save(JSON.stringify(settings.get(), null, 2)), 300);
});

// Now, not in 300 ms; durable: on the disk before this returns (session restore's marker must survive
// a power cut). Blocks the frame for that write, so it's only for that.
export function saveSettingsNow(durable = false) {
  clearTimeout(pending);
  return lord.settings.save(JSON.stringify(settings.get(), null, 2), durable);
}

export function useSettings() {
  return [useStore(settings), settings.set];
}
