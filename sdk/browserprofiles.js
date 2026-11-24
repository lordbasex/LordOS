// browserprofiles.js — a Chromium-family browser's profiles at runtime (the rules are in browserprofilemodel.js): read
// from the browser's own Local State only on demand (a menu opening, a launch, the empty card's quick launch showing),
// never on a draw or per frame: one stat, and the file is read and parsed again only when its mtime or size changed.
// Only { dir, name, color } is kept.
//
// launchedProfile(id): the profile LordOS launched a window for. It is filled only by LordOS's own launch of a profile
// (lord.apps.launchProfile, approved by the maintainer 2026-11-24: docs/WORKSPACES.md "Browser profiles"); without it it
// stays empty and no window shows a profile (the title is never used, browserprofilemodel.js windowProfile).
import { createStore } from './store.js';
import * as fs from './fs.js';
import { BROWSERS, LOCAL_STATE_MAX, parseLocalState, isProfileBrowser, windowProfile as profileOf } from './browserprofilemodel.js';

export const browserProfiles = createStore({});      // launcher id → [{ dir, name, color }]
const readAt = new Map();                            // launcher id → "mtime|size" of the Local State read
const launched = new Map();                          // window id → profile folder
export const launchedProfiles = createStore(0);      // a revision, for re-rendering

export function refreshBrowserProfiles(key, now = Date.now()) {
  if (!isProfileBrowser(key)) return [];
  void now;
  let list = [];
  try {
    const path = `${fs.home()}/${BROWSERS[key]}`;
    // a regular file of a sane size, not a link (the read is bounded again by parseLocalState)
    const st = fs.stat(path);
    const sig = st ? `${st.mtime}|${st.size}` : '';
    if (sig && readAt.get(key) === sig) return browserProfiles.get()[key] || [];
    readAt.set(key, sig);
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
  return profileOf(w, key, browserProfiles.get(), launched);
}
