// activities.js — Lord Workspaces' settings as plain data (tested by server/tests/workspaces_model_test.mjs).
//
// An activity is one of the SDK's desktops (sdk/desktops.js, at most MAX_DESKTOPS): its name is the desktop's own
// (settings.desktopNames, renameDesktop), so the Overview and Settings → Desktop & Dock show the same name. What a
// desktop doesn't have lives in this theme's own key, `settings.workspaces` (version WS_VERSION; a new key, so no
// migration): the icon, the colour, the pinned apps and the layouts of each activity, by position like the desktops'
// names (desktops are numbered again at every start), and the theme's own choices.
//
//   settings.workspaces = { v: 1, activities: [ { icon, color, pins, layouts, setup } | null ] }
//   (layouts: { [display connector]: { layout, ratio, apps?, locked? } }; locked: the padlock is closed)
//   settings.themeOptions.workspaces = { surface, spine, spineEvery }   (theme.json "options": Settings shows them,
//                                    sdk/thememeta.js)
//
//   wsPrefs(settings)                the cleaned key (defaults for anything missing or wrong)
//   effectiveSurface(prefs, s)       'opaque' when Settings → Appearance → Transparency is off
//   metaFromSaved(list, saved)       desktop id → activity, from the saved positions
//   savedFromMeta(list, meta)        back to positions, for the settings file
import { cleanLayout, cleanRatio } from './layouts.js';
import { themeOptions, themeOptionPatch } from '../../../sdk/thememeta.js';

export const THEME_ID = 'workspaces';

export const WS_VERSION = 1;
export const SURFACES = ['opaque', 'translucent', 'crystal'];
export const SPINE_PREFS = ['auto', 'expanded', 'collapsed'];
export const ACTIVITY_ICONS = ['spark', 'code', 'design', 'server', 'chat', 'music', 'book', 'home', 'work', 'globe'];
export const ACTIVITY_COLORS = ['#7C5CFF', '#3D7BFD', '#14A89A', '#22A565', '#E59A12', '#F06A24', '#E5487F', '#7A7891'];
export const MAX_PINS = 8;
export const MAX_ACTIVITIES = 9;
export const DEFAULT_SURFACE = 'crystal';

const isHex = (c) => typeof c === 'string' && /^#[0-9a-fA-F]{6}$/.test(c);
const isKey = (k) => typeof k === 'string' && k.length > 0 && k.length <= 200;

export function defaultActivity(index = 0) {
  return { icon: ACTIVITY_ICONS[index % ACTIVITY_ICONS.length], color: ACTIVITY_COLORS[index % ACTIVITY_COLORS.length], pins: [], layouts: {}, setup: false };
}

function cleanLayouts(l) {
  const out = {};
  if (!l || typeof l !== 'object' || Array.isArray(l)) return out;
  for (const [dk, v] of Object.entries(l).slice(0, 8)) {
    if (!isKey(dk) || !v || typeof v !== 'object') continue;
    const layout = cleanLayout(v.layout);
    const e = { layout, ratio: cleanRatio(layout, v.ratio) };
    if (Array.isArray(v.apps)) e.apps = v.apps.filter(isKey).slice(0, 12);
    if (v.locked === true) e.locked = true;
    out[dk] = e;
  }
  return out;
}

export function cleanActivity(a, index = 0) {
  const d = defaultActivity(index);
  if (!a || typeof a !== 'object') return d;
  return {
    icon: ACTIVITY_ICONS.includes(a.icon) ? a.icon : d.icon,
    color: isHex(a.color) ? a.color.toUpperCase() : d.color,
    pins: Array.isArray(a.pins) ? [...new Set(a.pins.filter(isKey))].slice(0, MAX_PINS) : [],
    layouts: cleanLayouts(a.layouts),
    setup: a.setup === true,
  };
}

export function wsPrefs(s) {
  const w = s && typeof s.workspaces === 'object' && s.workspaces && !Array.isArray(s.workspaces) ? s.workspaces : {};
  const list = Array.isArray(w.activities) ? w.activities.slice(0, MAX_ACTIVITIES) : [];
  const o = themeOptions(s, THEME_ID);
  return {
    v: WS_VERSION,
    surface: SURFACES.includes(o.surface) ? o.surface : DEFAULT_SURFACE,
    spine: SPINE_PREFS.includes(o.spine) ? o.spine : 'auto',
    spineEvery: o.spineEvery === true,
    activities: list.map((a, i) => (a ? cleanActivity(a, i) : null)),
  };
}

// Transparency off (Settings → Appearance, settings.effects === false) is always Opaque.
export const effectiveSurface = (prefs, s) => (s && s.effects === false ? 'opaque' : prefs.surface);

// list: the desktop ids in order (sdk desktopList); saved: activities by position
export function metaFromSaved(list, saved) {
  const out = {};
  list.forEach((id, i) => { out[id] = cleanActivity(saved?.[i], i); });
  return out;
}
export function savedFromMeta(list, meta) {
  const arr = list.map((id, i) => {
    const m = meta[id];
    if (!m) return null;
    const d = defaultActivity(i);
    const plain = m.icon === d.icon && m.color === d.color && !m.pins.length && !Object.keys(m.layouts).length && !m.setup;
    return plain ? null : m;
  });
  while (arr.length && arr[arr.length - 1] == null) arr.pop();
  return arr;
}

// The settings patch for the activities (the rest of the key stays as it is).
export function wsPatch(s, patch) {
  const cur = s && typeof s.workspaces === 'object' && s.workspaces ? s.workspaces : {};
  return { workspaces: { ...cur, ...patch, v: WS_VERSION } };
}
// The settings patch for one of the theme's choices (surface, spine, spineEvery), where Settings keeps them too.
export const optPatch = (s, key, value) => themeOptionPatch(s, THEME_ID, key, value);

// The first unused icon and colour for a new activity, so new ones don't all look alike.
export function freshActivity(meta, index) {
  const used = Object.values(meta || {});
  const icon = ACTIVITY_ICONS.find((x) => !used.some((m) => m && m.icon === x)) || ACTIVITY_ICONS[index % ACTIVITY_ICONS.length];
  const color = ACTIVITY_COLORS.find((x) => !used.some((m) => m && m.color === x)) || ACTIVITY_COLORS[index % ACTIVITY_COLORS.length];
  return { ...defaultActivity(index), icon, color };
}

// Pinning: at most MAX_PINS, no repeats; toggling an app that is there removes it.
export function togglePin(pins, key) {
  if (!isKey(key)) return pins;
  if (pins.includes(key)) return pins.filter((k) => k !== key);
  return [...pins, key].slice(0, MAX_PINS);
}

// The order the activities were last shown in: the one in view first, then the others from the most recent; a new
// desktop goes last and a removed one goes away. prev: the previous order; now: the desktop in view; list: every desktop.
export function recentOrder(prev, now, list) {
  const have = new Set(list);
  const rest = (prev || []).filter((id) => id !== now && have.has(id));
  const fresh = list.filter((id) => id !== now && !rest.includes(id));
  return [...(have.has(now) ? [now] : []), ...rest, ...fresh];
}
// How many recent activities the spine has room for on a display of this height (none on short ones).
export const spineRecentRoom = (h) => (h >= 860 ? 3 : h >= 720 ? 2 : h >= 620 ? 1 : 0);

// The Focus Surface's summary of an activity: its windows (wins: the open ones, without the theme's own panels), the ones
// in front first and minimized ones last, at most `max` of them; total and how many more there are.
export function activitySummary(wins, desk, max = 6) {
  const on = wins.filter((w) => w.desktop === desk);
  const order = [...on.filter((w) => !w.minimized).reverse(), ...on.filter((w) => w.minimized)];
  return { total: on.length, shown: order.slice(0, max), more: Math.max(0, on.length - max) };
}

// The onboarding shows on the only activity while nothing was set up and nothing is open.
export const needsOnboarding = (meta, count, windows) => count === 1 && !!meta && !meta.setup && windows === 0;
