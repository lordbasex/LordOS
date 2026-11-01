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
  wallpaper: DEFAULT_WALLPAPER,  // 0.8.2: Lumo Celestial (before: 'aurora', still available)
  keyboard: 'latam',     // before M5; now keyboards (see keyboard.js)
  keyboards: null,       // [{ layout, variant }] from xkb-data
  keyboardIndex: 0,
  language: null,        // interface language ('es', 'en', 'pt'…); null = the region's
  region: '',            // formats region ('es_AR'…); '' = the language's
  theme: null,           // id of the chosen theme (folder in themes/)
  desktopCount: 1,            // 0.8.7, how many desktops (sdk/desktops.js)
  desktopArrows: true,        // 0.8.7, Settings → Keyboard: Lord Flow's ⌃ arrows are the Desktops keys
  sessionRestore: 'always',   // 0.8.7, Settings → Session: 'off' | 'ask' | 'always' (sessionrestore.jsx)
  sessionBrowsers: true,      // browsers reopen their tabs too (the browser-restore admin action)
  files: null,                // 0.8.7, Files → Settings (apps/files/prefs.js)
  sessionSaved: null,         // { clean, windows } what was open; written by sessionrestore.jsx only
};

// Per-user settings migrations (docs/UPDATES.md, "Migrations"): when an update changes the shape or
// meaning of a setting, it appends [schema, (settings) => newSettings] here. Each runs once per user,
// in order, when the session loads the file; "schema" records the last one applied. The system-wide
// migrations (as root) are the lordos package's (packaging/migrations).
export const SETTINGS_SCHEMA = 1;
const MIGRATIONS = [
  [1, (s) => s],          // 0.8.1: the schema number starts being recorded; nothing changes
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
