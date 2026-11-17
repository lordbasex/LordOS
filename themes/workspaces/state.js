// state.js — Lord Workspaces' runtime state: the activities' own data (icon, colour, pins, layouts) by desktop id, kept
// in settings.workspaces by position (model/activities.js), and what the shell has open. The desktops themselves (how
// many, which one shows, their names) are the SDK's (sdk/desktops.js), so the Overview, Settings and session restore
// keep working as in the other themes.
import { createStore, settings, desktopList, desktopNames, activeDesktop, renameDesktop, addDesktopAndShow, wm, launchApp, windows, getApp, launcherIdOf,
  switchDesktop } from '@lordos/sdk';
import { wsPrefs, metaFromSaved, savedFromMeta, wsPatch, freshActivity, cleanActivity, togglePin, recentOrder, newGroup, groupOfWindow, groupPlan, groupApps,
  MAX_GROUPS } from './model/activities.js';

// desktop id → activity
export const activities = createStore({});
// what is open: null | { name: 'search' | 'control' | 'notif' | 'menu', display, at, mode }
export const panel = createStore(null);
// the activity being renamed in the strip, or asked about before deleting
export const renaming = createStore(null);
export const deleting = createStore(null);
// the last place something was launched from (the spine, the search panel): a window opening right after zooms from it
export const launchOrigin = { x: 0, y: 0, t: 0 };
export function noteLaunch(x, y) { launchOrigin.x = x; launchOrigin.y = y; launchOrigin.t = Date.now(); }

// The activities in the order they were last shown, the one in view first (the spine's "recent" ones are the next few).
// Only for this session: nothing about it is saved.
export const recentActivities = createStore([]);
const noteShown = () => {
  const list = desktopList.get(), now = activeDesktop.get();
  const next = recentOrder(recentActivities.get(), now, list);
  if (next.join('|') !== recentActivities.get().join('|')) recentActivities.set(next);
};

let started = false;
export function startActivities() {
  if (started) return;
  started = true;
  activeDesktop.subscribe(noteShown);
  desktopList.subscribe(noteShown);
  noteShown();
  activities.set(metaFromSaved(desktopList.get(), wsPrefs(settings.get()).activities));
  // a desktop added anywhere (the Overview, Settings, a shortcut) gets an activity of its own
  desktopList.subscribe(() => {
    const list = desktopList.get(), cur = activities.get();
    if (list.every((id) => cur[id])) { save(); return; }
    const next = { ...cur };
    list.forEach((id, i) => { if (!next[id]) next[id] = freshActivity(next, i); });
    activities.set(next);
  });
  activities.subscribe(save);
}

function save() {
  if (!started) return;
  const s = settings.get();
  const want = savedFromMeta(desktopList.get(), activities.get());
  const have = wsPrefs(s).activities;
  if (JSON.stringify(want) !== JSON.stringify(have)) settings.set(wsPatch(s, { activities: want }));
}

export const activityOf = (id) => activities.get()[id] || cleanActivity(null, Math.max(0, desktopList.get().indexOf(id)));
export function updateActivity(id, fn) {
  const cur = activities.get();
  const m = cur[id] || activityOf(id);
  const next = cleanActivity(fn(m), desktopList.get().indexOf(id));
  const { updatedAt: a, ...was } = m, { updatedAt: b, ...now } = next;
  if (JSON.stringify(was) === JSON.stringify(now)) return;            // nothing changed: no new date, no save
  activities.set({ ...cur, [id]: { ...next, updatedAt: new Date().toISOString() } });
}

// "+ New activity": a desktop, shown at once, with a fresh icon and colour, marked set up so it doesn't onboard.
export function newActivity() {
  const id = addDesktopAndShow();
  if (id != null) updateActivity(id, (m) => ({ ...m, setup: true }));
  return id;
}

// The onboarding's last step: name, icon, colour and pins, then the pinned apps open (the tiler places them).
export function setUpActivity(id, { name, icon, color, pins }) {
  if (typeof name === 'string') renameDesktop(id, name);
  updateActivity(id, (m) => ({ ...m, icon: icon || m.icon, color: color || m.color, pins: pins || m.pins, setup: true }));
  for (const key of pins || []) launchApp(key);
}

export const pinToggle = (id, key) => updateActivity(id, (m) => ({ ...m, pins: togglePin(m.pins, key) }));
export function openPins(id) { for (const key of activityOf(id).pins) launchApp(key); }

// a window to another activity, staying where it is (the person follows it with the strip)
export const moveWindowTo = (winId, desk) => wm.moveToDesktop(winId, desk);

export { desktopNames, activeDesktop, desktopList };

// ---- groups: more activities on the same desktop (model/activities.js) ----
// Each open window has its group (null: the activity's own), decided when it first shows: the group whose saved apps
// have its app, else the one in view. Showing a group minimizes the windows of the others and brings back the ones it
// had hidden; those don't count as minimized by the person (the frame's tray and the minimize effect leave them out).
export const windowGroups = createStore(new Map());     // window id → group id | null
export const groupHidden = createStore(new Set());      // ids a group switch minimized
const own = (w) => !w.pip && !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));
const tagOf = (w) => windowGroups.get().get(w.id) ?? null;

function tagWindows() {
  const tags = windowGroups.get(), all = windows.get().filter(own);
  let next = null;
  for (const w of all) {
    const m = activities.get()[w.desktop];
    const want = !tags.has(w.id) ? groupOfWindow(m || {}, undefined, launcherIdOf(w)) : tags.get(w.id);
    // a window moved to another activity keeps no group the activity doesn't have
    const fixed = want && !(m?.groups || []).some((g) => g.id === want) ? null : want;
    if (!tags.has(w.id) || tags.get(w.id) !== fixed) { next = next || new Map(tags); next.set(w.id, fixed); }
  }
  for (const id of tags.keys()) if (!all.some((w) => w.id === id)) { next = next || new Map(tags); next.delete(id); }
  if (next) windowGroups.set(() => next);
  const hid = groupHidden.get();
  if ([...hid].some((id) => !all.some((w) => w.id === id && w.minimized))) groupHidden.set(() => new Set([...hid].filter((id) => all.some((w) => w.id === id && w.minimized))));
}
// what each group has open goes to the settings, so its windows find their group after a restart
function rememberApps() {
  const all = windows.get().filter(own);
  for (const [desk, m] of Object.entries(activities.get())) {
    if (!m.groups?.length) continue;
    const id = desktopList.get().find((d) => String(d) === desk);
    if (id == null) continue;
    const wins = all.filter((w) => String(w.desktop) === String(desk));
    const next = groupApps(m.groups, wins, tagOf, launcherIdOf);
    if (JSON.stringify(next) !== JSON.stringify(m.groups)) updateActivity(id, (x) => ({ ...x, groups: next }));
  }
}
let groupsStarted = false;
export function startGroups() {
  if (groupsStarted) return;
  groupsStarted = true;
  windows.subscribe(tagWindows);
  windowGroups.subscribe(rememberApps);
  tagWindows();
}

export const groupsOf = (desk) => activityOf(desk).groups || [];
export const groupInView = (desk) => activityOf(desk).group ?? null;

// Show group g (null: the activity's own) of desk, going to that desktop if it isn't in view.
export function showGroup(desk, g) {
  updateActivity(desk, (m) => { const x = { ...m }; if (g) x.group = g; else delete x.group; return x; });
  const wins = windows.get().filter((w) => own(w) && w.desktop === desk);
  const plan = groupPlan(wins, tagOf, groupHidden.get(), g);
  // minimized first, then marked (each minimize re-checks the marks: one not minimized yet would lose its own)
  for (const id of plan.hide) wm.minimize(id);
  for (const id of plan.show) wm.raise(id);
  const hid = new Set(groupHidden.get());
  for (const id of plan.hide) hid.add(id);
  for (const id of plan.show) hid.delete(id);
  groupHidden.set(() => hid);
  if (activeDesktop.get() !== desk) switchDesktop(desk);
}
export function addGroup(desk, name) {
  const m = activityOf(desk);
  if ((m.groups || []).length >= MAX_GROUPS) return null;
  const g = newGroup(m, name);
  updateActivity(desk, (x) => ({ ...x, groups: [...(x.groups || []), g] }));
  showGroup(desk, g.id);
  return g.id;
}
export const renameGroup = (desk, id, name) => updateActivity(desk, (m) => ({ ...m, groups: (m.groups || []).map((g) => (g.id === id ? { ...g, name: name.trim().slice(0, 60) } : g)) }));
// A group removed: its windows join the activity's own, and show.
export function removeGroup(desk, id) {
  if (groupInView(desk) === id) showGroup(desk, null);
  const tags = new Map(windowGroups.get());
  for (const [w, g] of tags) if (g === id) tags.set(w, null);
  windowGroups.set(() => tags);
  updateActivity(desk, (m) => { const x = { ...m, groups: (m.groups || []).filter((g) => g.id !== id) }; if (x.group === id) delete x.group; return x; });
  showGroup(desk, groupInView(desk));
}
// A window to another group of its activity: it leaves the view when that group isn't the one shown.
export function moveWindowToGroup(winId, g) {
  const w = wm.get(winId);
  if (!w) return;
  const tags = new Map(windowGroups.get());
  tags.set(winId, g ?? null);
  windowGroups.set(() => tags);
  if ((g ?? null) !== groupInView(w.desktop)) { wm.minimize(winId); groupHidden.set(() => new Set([...groupHidden.get(), winId])); }
}
