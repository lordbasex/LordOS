// menubarmodel.js — what the bar shows, with no screen (issue #58). Lord Flow's menu bar and Control Center and the
// Classic taskbar's tray read these choices; Settings → Desktop & Dock → Menu Bar (or Taskbar) writes them. Tested by
// server/tests/menubar_model_test.mjs.
//
//   menuBarPrefs(settings)        { keyboard, osk, bell, calendar, ccSystem, ccNetwork, ccWifi, ccBluetooth }
//   menuBarPatch(settings, k, v)  the settings change that turns one of them on or off
//   outputChoices(soundState)     the outputs a sound panel offers, the default one marked
//   monthCells(year, month, first) the days of a month calendar, padded to whole weeks
//
// A bar on every display (0.8.8.6): Lord Flow draws one full menu bar per display, each with its own clock, status
// items and panels; only the display with the focused window has the app's menus lit.
//
//   menuBarEveryDisplay(settings)            on unless the user turned it off (settings.menuBarAllDisplays)
//   barDisplays(displays, every, mirror)     the displays that get a bar (the main one alone when off or mirrored)
//   hasBar(display, every, mirror)           whether a display's work area reserves the bar's height
//   displayIdFor(displays, rect)             the display holding most of a rectangle (else the nearest one)
//   activeBarId(displays, bars, focus, pin)  the bar whose app menus are lit
//   pinBar(displays, focus, id)              what a click on a dimmed bar leaves, so that bar is lit
//   panelLeft(at, width, area, margin)       a panel's left edge under the item that opened it, inside the display
//   inBar(x, y, area, height)                a point on that display's bar
//   insideDisplay(x, y, w, h, display, m)    a menu's top-left corner moved inside one display
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
  if (key === 'everyDisplay') return { menuBarAllDisplays: !!on };
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

// ---- a bar on every display ----

// On by default since 0.8.8.6. The key is the one Settings → Displays wrote before ("Show menu bar on all
// displays", off unless turned on): a settings.json without it now gets a bar on every display, and one where the
// user turned it off keeps it off. No migration: the absent value is what changed meaning.
export const menuBarEveryDisplay = (s) => !s || s.menuBarAllDisplays !== false;

const mainOf = (displays) => (displays || []).find((d) => d && d.main) || (displays || [])[0] || null;

// Mirrored displays show the same picture, so a single bar (the main display's) is drawn.
export function barDisplays(displays, every, mirror) {
  const list = (displays || []).filter(Boolean);
  if (every && !mirror) return list;
  const m = mainOf(list);
  return m ? [m] : [];
}

export const hasBar = (d, every, mirror) => !!d && (!!d.main || (!!every && !mirror));

// The same rule as runtime/index.js displayFor (most of the rectangle; else the display nearest its center), on a
// plain list so it can be tested. Returns the display's id, or null without displays.
export function displayIdFor(displays, r) {
  const list = (displays || []).filter(Boolean);
  if (!list.length) return null;
  if (!r) return mainOf(list).id;
  let best = null, area = 0;
  for (const d of list) {
    const w = Math.min(r.x + r.w, d.x + d.w) - Math.max(r.x, d.x), h = Math.min(r.y + r.h, d.y + d.h) - Math.max(r.y, d.y);
    if (w > 0 && h > 0 && w * h > area) { area = w * h; best = d; }
  }
  if (best) return best.id;
  const cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  let dist = Infinity;
  for (const d of list) {
    const dx = Math.max(d.x - cx, 0, cx - (d.x + d.w - 1)), dy = Math.max(d.y - cy, 0, cy - (d.y + d.h - 1));
    if (dx * dx + dy * dy < dist) { dist = dx * dx + dy * dy; best = d; }
  }
  return best.id;
}

// The bar whose app menus are lit: the one on the focused window's display (the main display's without a focused
// window). focus: { id, rect } or null. pin: { display, focus, from } from a click on a dimmed bar, which lights that
// bar until the focus moves to another window or the focused window to another display (then it is stale).
export function activeBarId(displays, bars, focus, pin) {
  const list = (bars || []).filter(Boolean);
  if (!list.length) return null;
  const has = (id) => list.some((b) => b.id === id);
  const base = focusDisplayId(displays, focus) ?? list[0].id;
  if (pin && has(pin.display) && pin.focus === (focus ? focus.id : null) && pin.from === base) return pin.display;
  if (has(base)) return base;
  const m = mainOf(list);
  return m.id;
}

// The display of the focused window (focus: { id, rect } or null); the main display's id without one.
export function focusDisplayId(displays, focus) {
  if (focus && focus.rect) return displayIdFor(displays, focus.rect);
  const m = mainOf(displays);
  return m ? m.id : null;
}

// The pin a click on a dimmed bar leaves: that bar is lit while the same window keeps the focus on the same display.
export const pinBar = (displays, focus, id) => ({ display: id, focus: focus ? focus.id : null, from: focusDisplayId(displays, focus) });

// A panel's left edge under the item that opened it (at = [x, width] of the item, global): aligned to the item's
// right edge when the item is on the display's right half, to its left edge otherwise, and always inside the display
// with a margin. Without an item: against the display's right edge, as the panels opened before.
export function panelLeft(at, width, area, margin = 10) {
  if (!area) return null;
  const lo = area.x + margin, hi = area.x + area.w - width - margin;
  if (!at) return Math.round(Math.max(lo, hi));
  const [x, w] = at;
  const want = x + w / 2 >= area.x + area.w / 2 ? x + w - width : x;
  return Math.round(Math.max(lo, Math.min(want, hi)));
}

// A point on a display's bar: a click there belongs to the bar's items (they toggle their panel themselves).
export const inBar = (x, y, area, height) => !!area && x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + height;

// A menu's top-left corner moved so the w×h menu stays inside one display (a menu near a display's edge must not
// spill onto the next one). Bigger than the display: its top-left corner, with the margin.
export function insideDisplay(x, y, w, h, d, m = 4) {
  if (!d) return { x, y };
  return {
    x: Math.max(d.x + m, Math.min(x, d.x + d.w - w - m)),
    y: Math.max(d.y + m, Math.min(y, d.y + d.h - h - m)),
  };
}
