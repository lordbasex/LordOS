// browserprofiles.js — a Chromium-family browser's profiles at runtime (the rules are in browserprofilemodel.js): read
// from the browser's own Local State when a window list or a picker needs them, at most once every REFRESH_MS per
// browser, never at startup and never in a loop. Only { dir, name, color } is kept.
//
// launchedProfile(id): the profile LordOS launched a window for. It is filled only by LordOS's own launch of a profile
// (lord.apps.launchProfile, awaiting the maintainer's approval: docs/WORKSPACES.md "Browser profiles"); until then it
// stays empty and no window shows a profile (the title is never used, browserprofilemodel.js windowProfile).
import { createStore } from './store.js';
import * as fs from './fs.js';
import { BROWSERS, LOCAL_STATE_MAX, parseLocalState, isProfileBrowser, windowProfile as profileOf } from './browserprofilemodel.js';

export const browserProfiles = createStore({});      // launcher id → [{ dir, name, color }]
const readAt = new Map();
const REFRESH_MS = 30000;
const launched = new Map();                          // window id → profile folder
export const launchedProfiles = createStore(0);      // a revision, for re-rendering

export function refreshBrowserProfiles(key, now = Date.now()) {
  if (!isProfileBrowser(key)) return [];
  const at = readAt.get(key);
  if (at != null && now - at < REFRESH_MS && now >= at) return browserProfiles.get()[key] || [];
  readAt.set(key, now);
  let list = [];
  try {
    const path = `${fs.home()}/${BROWSERS[key]}`;
    // a regular file of a sane size, not a link (the read is bounded again by parseLocalState)
    const st = fs.stat(path);
    if (st && !st.dir && !st.link && st.size > 0 && st.size <= LOCAL_STATE_MAX) list = parseLocalState(fs.read(path));
  } catch (e) { list = []; }
  const cur = browserProfiles.get()[key] || [];
  if (JSON.stringify(cur) !== JSON.stringify(list)) browserProfiles.set((m) => ({ ...m, [key]: list }));
  return list;
}

// a window LordOS launched for that profile (the launch path's own call; the folder is checked again when read)
export function noteLaunchedProfile(id, dir) {
  if (typeof id !== 'string' || typeof dir !== 'string') return;
  if (launched.size > 256) launched.delete(launched.keys().next().value);
  launched.set(id, dir);
  launchedProfiles.set((n) => n + 1);
}
export const forgetLaunchedProfile = (id) => { if (launched.delete(id)) launchedProfiles.set((n) => n + 1); };

// the profile of window w (key: its launcher id), or null
export function windowProfile(w, key) {
  if (!w || !isProfileBrowser(key) || !launched.has(w.id)) return null;
  refreshBrowserProfiles(key);
  return profileOf(w, key, browserProfiles.get(), launched);
}
