// menubarmodel.js — what the bar shows, with no screen (issue #58). Lord Flow's menu bar and Control Center and the
// Classic taskbar's tray read these choices; Settings → Desktop & Dock → Menu Bar (or Taskbar) writes them. Tested by
// server/tests/menubar_model_test.mjs.
//
//   menuBarPrefs(settings)        { keyboard, osk, bell, calendar, ccSystem, ccNetwork, ccWifi, ccBluetooth }
//   menuBarPatch(settings, k, v)  the settings change that turns one of them on or off
//   outputChoices(soundState)     the outputs a sound panel offers, the default one marked
//   monthCells(year, month, first) the days of a month calendar, padded to whole weeks
//
// Everything is on unless the user turned it off, so a settings.json without "menuBar" (every version before
// 0.8.8.6) shows the bar as it was: no migration. The Cleaner item (the crown) keeps its own key,
// cleaner.menuItem, which Cleaner → Settings already writes; the Settings row writes the same key.
export const MENUBAR_DEFAULTS = {
  keyboard: true,       // the keyboard layout (FR, ES…): one click goes to the next layout
  osk: true,            // the on-screen keyboard's item
  bell: true,           // the notifications' bell next to the clock
  calendar: true,       // the month calendar in the clock's panel
  ccSystem: true,       // Lord Flow's Control Center: CPU and memory
  ccNetwork: true,      //   the network and VPN tile
  ccWifi: true,         //   the Wi-Fi tile (only with a Wi-Fi radio)
  ccBluetooth: true,    //   the Bluetooth tile (only with an adapter)
};

export function menuBarPrefs(s) {
  const own = s && typeof s.menuBar === 'object' && s.menuBar ? s.menuBar : {};
  const p = { ...MENUBAR_DEFAULTS };
  for (const k of Object.keys(MENUBAR_DEFAULTS)) if (typeof own[k] === 'boolean') p[k] = own[k];
  return p;
}

// The Cleaner item (the crown): on unless Cleaner → Settings (or Settings → Desktop & Dock) turned it off.
export const cleanerItemShown = (s) => ((s && s.cleaner) || {}).menuItem !== false;

// A shallow patch for settings.set: only "menuBar" (or "cleaner") changes, every other choice in it is kept.
export function menuBarPatch(s, key, on) {
  if (key === 'cleaner') return { cleaner: { ...((s && s.cleaner) || {}), menuItem: !!on } };
  if (!(key in MENUBAR_DEFAULTS)) return {};
  const own = s && typeof s.menuBar === 'object' && s.menuBar ? s.menuBar : {};
  return { menuBar: { ...own, [key]: !!on } };
}

// The outputs of a sound state (sdk/sound.js): [{ index, name, label, default }], the default one marked (the first
// one when PipeWire has not said). PipeWire keeps a volume per output, so choosing another output also brings back
// the level it had there.
export function outputChoices(st) {
  const sinks = (st && Array.isArray(st.sinks) ? st.sinks : []).filter((x) => x && x.name);
  const def = sinks.find((x) => x.default) || sinks[0] || null;
  return sinks.map((x) => ({ index: x.index, name: x.name, label: x.description || x.name, default: x === def }));
}

// The week starts on Sunday in English and on Monday elsewhere (the Classic clock panel's rule).
export const weekStartFor = (lang) => (String(lang || 'en').startsWith('en') ? 0 : 1);

// The cells of a month (month 0-11): null before the 1st and after the last day, so there are whole weeks.
export function monthCells(year, month, first = 0) {
  const lead = (new Date(year, month, 1).getDay() - first + 7) % 7;
  const days = new Date(year, month + 1, 0).getDate();
  const cells = [...Array(lead).fill(null), ...Array.from({ length: days }, (_, i) => i + 1)];
  while (cells.length % 7) cells.push(null);
  return cells;
}
