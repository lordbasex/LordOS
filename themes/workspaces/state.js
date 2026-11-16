// state.js — Lord Workspaces' runtime state: the activities' own data (icon, colour, pins, layouts) by desktop id, kept
// in settings.workspaces by position (model/activities.js), and what the shell has open. The desktops themselves (how
// many, which one shows, their names) are the SDK's (sdk/desktops.js), so the Overview, Settings and session restore
// keep working as in the other themes.
import { createStore, settings, desktopList, desktopNames, activeDesktop, renameDesktop, addDesktopAndShow, wm, launchApp } from '@lordos/sdk';
import { wsPrefs, metaFromSaved, savedFromMeta, wsPatch, freshActivity, cleanActivity, togglePin } from './model/activities.js';

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

let started = false;
export function startActivities() {
  if (started) return;
  started = true;
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
  activities.set({ ...cur, [id]: cleanActivity(fn(m), desktopList.get().indexOf(id)) });
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
