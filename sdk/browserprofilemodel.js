// browserprofilemodel.js — a Chromium-family browser's profiles as plain data (pure; tested with node:
// server/tests/browser_profiles_test.mjs). Used to tell two windows of the same browser apart at a glance (a dot in the
// profile's colour on its icon, its name in the window lists) and to name the profile an activity or a display opens;
// launching a profile waits for the maintainer's approval of its launch path (docs/WORKSPACES.md, "Browser profiles").
//
// The browser's own `Local State` (JSON, in its user-data folder) lists them under profile.info_cache: folder name →
// { name, profile_highlight_color, default_avatar_fill_color, … }. It is the browser's file, written by whatever runs
// as the person, so it is untrusted data: read only up to LOCAL_STATE_MAX bytes and only when it is a regular file; parsed
// with JSON.parse (no eval, no reviver); at most MAX_PROFILES profiles; a folder name kept only when it is one the
// browser itself makes ("Default", "Profile N") — never a path, and nothing from it is ever used as one here; a name
// cut to PROFILE_NAME_MAX characters with control and bidi-override characters dropped (windowlistmodel.js); a colour
// only from an integer (ARGB, as Chromium stores SkColor), as #rrggbb. Anything else in the file is ignored.
import { cleanWindowLabel } from './windowlistmodel.js';

export const LOCAL_STATE_MAX = 1024 * 1024;
export const MAX_PROFILES = 32;
export const PROFILE_NAME_MAX = 40;
export const PROFILE_DIR = /^(Default|Profile [1-9][0-9]{0,3})$/;

// The browsers whose profiles are read: launcher id → its Local State, relative to the home folder (fixed here, never
// from the caller). Debian's Chromium and the vendors' packages from the Store (docs/PACKAGES.md "Browser defaults").
export const BROWSERS = Object.freeze({
  'desktop:google-chrome.desktop': '.config/google-chrome/Local State',
  'desktop:google-chrome-beta.desktop': '.config/google-chrome-beta/Local State',
  'desktop:chromium.desktop': '.config/chromium/Local State',
  'desktop:brave-browser.desktop': '.config/BraveSoftware/Brave-Browser/Local State',
  'desktop:microsoft-edge.desktop': '.config/microsoft-edge/Local State',
});
export const isProfileBrowser = (key) => Object.prototype.hasOwnProperty.call(BROWSERS, key);

const hex2 = (n) => n.toString(16).padStart(2, '0');
export function skColor(v) {
  if (typeof v !== 'number' || !Number.isInteger(v) || v < -2147483648 || v > 4294967295) return null;
  const u = v >>> 0;
  return `#${hex2((u >>> 16) & 255)}${hex2((u >>> 8) & 255)}${hex2(u & 255)}`;
}

// text: the file's contents → [{ dir, name, color }] in the browser's own order (Default first), or [] for anything
// that isn't the expected shape.
export function parseLocalState(text) {
  if (typeof text !== 'string' || !text || text.length > LOCAL_STATE_MAX) return [];
  let d;
  try { d = JSON.parse(text); } catch (e) { return []; }
  const cache = d && typeof d === 'object' && d.profile && typeof d.profile === 'object' ? d.profile.info_cache : null;
  if (!cache || typeof cache !== 'object' || Array.isArray(cache)) return [];
  const out = [];
  for (const dir of Object.keys(cache)) {
    if (out.length >= MAX_PROFILES) break;
    if (!PROFILE_DIR.test(dir)) continue;
    const p = cache[dir];
    if (!p || typeof p !== 'object') continue;
    const name = cleanWindowLabel(typeof p.name === 'string' ? p.name : '', PROFILE_NAME_MAX);
    out.push({ dir, name: name || dir, color: skColor(p.profile_highlight_color) || skColor(p.default_avatar_fill_color) });
  }
  const rank = (x) => (x.dir === 'Default' ? 0 : Number(x.dir.slice(8)) || 0);
  return out.sort((a, b) => rank(a) - rank(b));
}

// Which profile a browser window belongs to. Checked against Chromium's source (2026-11): on Linux a browser window's
// title is "<page title> - Google Chrome" (window_metadata_controller.cc, IDS_BROWSER_WINDOW_TITLE_FORMAT) with no
// profile name, its Wayland app_id / X11 WM_CLASS is the same for every profile ("google-chrome"; only installed web
// apps carry the profile in theirs, "chrome-<app>-<profile dir>"), and one process serves every profile. The title is
// the page's to choose, so it is never used: a page titled "Work" would get Work's colour. What LordOS can know is
// the profile it launched a window for (its own --profile-directory launch, pending the maintainer's approval:
// docs/WORKSPACES.md "Browser profiles"; the launch placement then ties the next window of that browser to it,
// themes/workspaces/model/launches.js). launched: window id → profile folder. Returns { dir, name, color } or null.
export function windowProfile(w, key, profilesByKey, launched = null) {
  if (!w || !isProfileBrowser(key) || !launched || typeof launched.get !== 'function') return null;
  const dir = launched.get(w.id);
  if (typeof dir !== 'string' || !PROFILE_DIR.test(dir)) return null;
  return (profilesByKey[key] || []).find((p) => p.dir === dir) || null;
}

// ---- which profile a launch from an activity or a display opens (stored; used once the launch path is approved) ----
// An activity's choice comes first ("in Desarrollo, Chrome opens Dev"), then the display's, by connector name (the
// SDK's per-display rule: every theme reads the same settings key, browserProfiles.displays). A choice is a profile
// folder name of the browser's own form (PROFILE_DIR) and nothing else; a browser outside BROWSERS has none.
// map: { [browser launcher id]: folder }.
export const MAX_PROFILE_CHOICES = 8;
export function cleanProfileChoices(map) {
  if (!map || typeof map !== 'object' || Array.isArray(map)) return {};
  const out = {};
  for (const [k, v] of Object.entries(map)) {
    if (Object.keys(out).length >= MAX_PROFILE_CHOICES) break;
    if (isProfileBrowser(k) && typeof v === 'string' && PROFILE_DIR.test(v)) out[k] = v;
  }
  return out;
}
export const CONNECTOR = /^[A-Za-z0-9-]{1,32}$/;
export function cleanDisplayProfiles(v) {
  const d = v && typeof v === 'object' && v.displays && typeof v.displays === 'object' && !Array.isArray(v.displays) ? v.displays : {};
  const displays = {};
  for (const [c, m] of Object.entries(d).slice(0, 16)) {
    const clean = CONNECTOR.test(c) ? cleanProfileChoices(m) : {};
    if (Object.keys(clean).length) displays[c] = clean;
  }
  return { displays };
}
// key: the browser's launcher id; activity: that activity's choices; prefs: the settings' browserProfiles; connector:
// the display's. → a folder name, or null (the browser's own default: its last-used profile, or its picker).
export function profileChoice(key, activity, prefs, connector) {
  if (!isProfileBrowser(key)) return null;
  const a = cleanProfileChoices(activity)[key];
  if (a) return a;
  const d = cleanDisplayProfiles(prefs).displays[CONNECTOR.test(connector || '') ? connector : ''];
  return (d && d[key]) || null;
}
