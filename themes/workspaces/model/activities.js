// activities.js — Lord Workspaces' settings as plain data (tested by server/tests/workspaces_model_test.mjs).
//
// An activity is one of the SDK's desktops (sdk/desktops.js, at most MAX_DESKTOPS): its name is the desktop's own
// (settings.desktopNames, renameDesktop), so the Overview and Settings → Desktop & Dock show the same name. What a
// desktop doesn't have lives in this theme's own key, `settings.workspaces` (version WS_VERSION; a new key, so no
// migration): the icon, the colour, the pinned apps and the layouts of each activity, by position like the desktops'
// names (desktops are numbered again at every start), and the theme's own choices.
//
//   settings.workspaces = { v: 1, activities: [ { icon, color, pins, layouts, setup, groups, group, updatedAt } | null ] }
//   (groups: up to MAX_GROUPS more activities that share this desktop, the proposal's Activity Islands and its way past
//   nine: [{ id, name, icon, color, apps }]; group: the one in view, or none for the activity's own; apps: the launcher
//   ids of each group's windows, so they join their group again when they reopen)
//   (layouts: { [display connector]: { layout, ratio, apps?, locked? } }; locked: the padlock is closed)
//   settings.themeOptions.workspaces = { surface, spine, spineEvery, contrast }   (theme.json "options": Settings shows them,
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
export const MAX_GROUPS = 3;          // per activity: with each one's own, 9 × 4 = 36 activities on 9 desktops
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

const isGroupId = (g) => typeof g === 'string' && /^g[0-9]{1,3}$/.test(g);
function cleanGroups(list) {
  if (!Array.isArray(list)) return [];
  const out = [];
  for (const g of list) {
    if (!g || typeof g !== 'object' || !isGroupId(g.id) || out.some((x) => x.id === g.id)) continue;
    out.push({ id: g.id, name: typeof g.name === 'string' ? g.name.replace(/[\u0000-\u001f]/g, ' ').trim().slice(0, 60) : '',
      icon: ACTIVITY_ICONS.includes(g.icon) ? g.icon : ACTIVITY_ICONS[out.length % ACTIVITY_ICONS.length],
      color: isHex(g.color) ? g.color.toUpperCase() : ACTIVITY_COLORS[(out.length + 3) % ACTIVITY_COLORS.length],
      apps: Array.isArray(g.apps) ? [...new Set(g.apps.filter(isKey))].slice(0, 12) : [] });
    if (out.length >= MAX_GROUPS) break;
  }
  return out;
}

export function cleanActivity(a, index = 0) {
  const d = defaultActivity(index);
  if (!a || typeof a !== 'object') return d;
  const groups = cleanGroups(a.groups);
  return {
    icon: ACTIVITY_ICONS.includes(a.icon) ? a.icon : d.icon,
    color: isHex(a.color) ? a.color.toUpperCase() : d.color,
    pins: Array.isArray(a.pins) ? [...new Set(a.pins.filter(isKey))].slice(0, MAX_PINS) : [],
    layouts: cleanLayouts(a.layouts),
    setup: a.setup === true,
    // when the person last changed it (the proposal's updatedAt), an ISO date
    ...(typeof a.updatedAt === 'string' && /^\d{4}-\d{2}-\d{2}T[\d:.]+Z$/.test(a.updatedAt) ? { updatedAt: a.updatedAt } : null),
    ...(groups.length ? { groups, ...(groups.some((g) => g.id === a.group) ? { group: a.group } : null) } : null),
  };
}

// ---- groups (activities that share a desktop) ----

// A new group for activity m: the first unused id, icon and colour (named by the caller).
export function newGroup(m, name) {
  const used = new Set((m.groups || []).map((g) => g.id));
  let n = 1;
  while (used.has(`g${n}`)) n++;
  const taken = [m, ...(m.groups || [])];
  const icon = ACTIVITY_ICONS.find((x) => !taken.some((g) => g.icon === x)) || ACTIVITY_ICONS[n % ACTIVITY_ICONS.length];
  const color = ACTIVITY_COLORS.find((x) => !taken.some((g) => g.color === x)) || ACTIVITY_COLORS[n % ACTIVITY_COLORS.length];
  return { id: `g${n}`, name: name || '', icon, color, apps: [] };
}
// The group a window belongs to: its tag while it is open; a window not tagged yet, the group whose saved apps have its
// app, else the one in view (null: the activity's own).
export function groupOfWindow(m, tag, app) {
  if (tag !== undefined) return tag;
  const saved = (m.groups || []).find((g) => g.apps.includes(app));
  return saved ? saved.id : m.group ?? null;
}
// What showing group g of a desktop does: the windows of other groups that show go (hide), the ones of g that the groups
// hid come back (show). wins: that desktop's windows; tagOf(w): its group; hidden: ids the groups hid.
export function groupPlan(wins, tagOf, hidden, g) {
  const hide = [], show = [];
  for (const w of wins) {
    if (w.pip) continue;
    const mine = (tagOf(w) ?? null) === (g ?? null);
    if (!mine && !w.minimized) hide.push(w.id);
    else if (mine && w.minimized && hidden.has(w.id)) show.push(w.id);
  }
  return { hide, show };
}
// Each group's apps from its open windows (appOf(w): the launcher id), for the settings.
export function groupApps(groups, wins, tagOf, appOf) {
  return groups.map((g) => {
    const now = [...new Set(wins.filter((w) => tagOf(w) === g.id).map(appOf).filter(isKey))];
    return now.length ? { ...g, apps: now.slice(0, 12) } : g;
  });
}
// Every activity, the groups included: [{ desk, group (null: the activity's own), name, icon, color }].
export function everyActivity(list, meta, nameOf) {
  const out = [];
  for (const desk of list) {
    const m = meta[desk];
    if (!m) continue;
    out.push({ desk, group: null, name: nameOf(desk), icon: m.icon, color: m.color });
    for (const g of m.groups || []) out.push({ desk, group: g.id, name: g.name, icon: g.icon, color: g.color });
  }
  return out;
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
    contrast: o.contrast === true,
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
    const plain = m.icon === d.icon && m.color === d.color && !m.pins.length && !Object.keys(m.layouts).length && !m.setup && !m.groups?.length;
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


// The Focus Surface's summary of an activity: its windows (wins: the open ones, without the theme's own panels), the ones
// in front first and minimized ones last, at most `max` of them; total and how many more there are.
export function activitySummary(wins, desk, max = 6) {
  const on = wins.filter((w) => w.desktop === desk);
  const order = [...on.filter((w) => !w.minimized).reverse(), ...on.filter((w) => w.minimized)];
  return { total: on.length, shown: order.slice(0, max), more: Math.max(0, on.length - max) };
}

// The bars' keyboard (barfocus.js): the control after (dir 1) or before (-1) cur, round the list; ±Infinity: the
// last or the first. A control that went away (an activity removed) starts again from the first.
export function barStep(ids, cur, dir) {
  if (!ids.length) return null;
  if (dir === Infinity) return ids[ids.length - 1];
  if (dir === -Infinity) return ids[0];
  const i = ids.indexOf(cur);
  if (i < 0) return ids[0];
  return ids[(i + dir + ids.length) % ids.length];
}

// The onboarding shows on the only activity while nothing was set up and nothing is open.
export const needsOnboarding = (meta, count, windows) => count === 1 && !!meta && !meta.setup && windows === 0;
