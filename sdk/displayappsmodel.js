// displayappsmodel.js — which apps are used on each display, as plain functions with no imports
// (server/tests/display_apps_model_test.mjs); sdk/displayapps.js runs it. For every theme (a per-display feature is
// the SDK's, keyed by connector: DP-2, HDMI-A-1…, never by the model's name, which identical monitors share).
//
//   settings.displayApps = { v: 1, off?: true, displays: { [display key]: {
//       apps: [{ id, n, t }],                      // launcher ids opened there: how many times, when last (ms)
//       sets: [{ apps, layout?, ratio?, n, t }],   // the apps seen open there together (in tile order) and the
//                                                  // theme's layout for them, how many times, when last
//       quiet?: true,                              // "Don't suggest on this display"
//   } } }
//
// Small and pruned: at most MAX_DISPLAYS displays, MAX_APPS apps and MAX_SETS sets per display, MAX_SET_APPS apps per
// set; what wasn't seen for FORGET_DAYS goes, and the rest is ranked by use that fades (half every HALF_LIFE_DAYS).
// It stays on this computer (the settings file); Settings can turn it off (off: nothing is recorded or shown) and
// clear it. Nothing here opens anything: the suggestion is only offered, and the theme opens the apps on a click.
//
//   cleanDisplayApps(raw)                         the cleaned key (an empty record for anything wrong)
//   noteApp(rec, key, id, now)                    an app opened on display `key`
//   noteSet(rec, key, { apps, layout, ratio }, now)   apps open there together for a while
//   quickApps(rec, key, now, { have, max })       the apps to offer there first: most used, recent first
//   suggestionFor(rec, key, now, { have })        the usual set to suggest there ({ apps, layout, ratio }) or null
//   quietDisplay(rec, key), clearDisplayApps(rec), setDisplayAppsOff(rec, off)
export const DA_VERSION = 1;
export const MAX_DISPLAYS = 8;
export const MAX_APPS = 12;
export const MAX_SETS = 6;
export const MAX_SET_APPS = 6;
export const QUICK_APPS = 5;
export const MIN_SEEN = 2;            // a set seen at least this many times is "the usual"
export const HALF_LIFE_DAYS = 14;
export const FORGET_DAYS = 90;
const DAY = 86400000;

const isAppId = (k) => typeof k === 'string' && k.length > 0 && k.length <= 200 && !/[\u0000-\u001f]/.test(k);
const isDisplayKey = (k) => typeof k === 'string' && /^[A-Za-z0-9#._:-]{1,64}$/.test(k);
const isLayout = (l) => typeof l === 'string' && /^[a-z][a-z0-9-]{0,23}$/.test(l);
const count = (n) => (Number.isFinite(n) && n >= 1 ? Math.min(Math.floor(n), 1e6) : 1);
const time = (t) => (Number.isFinite(t) && t > 0 ? Math.floor(t) : 0);
// An app id that names something the launcher can open again: not a guest's window (vm:) nor a window without a
// .desktop (linux:).
export const recordable = (id) => isAppId(id) && !/^(vm|linux):/.test(id);

export const emptyDisplayApps = () => ({ v: DA_VERSION, displays: {} });

// Use that fades: n, halved every HALF_LIFE_DAYS since it was last seen.
export function score(e, now) {
  const age = Math.max(0, (now - (e.t || 0)) / DAY);
  return (e.n || 0) * Math.pow(0.5, age / HALF_LIFE_DAYS);
}
const ranked = (list, now) => [...list].sort((a, b) => score(b, now) - score(a, now) || b.t - a.t);
const fresh = (e, now) => !now || now - e.t < FORGET_DAYS * DAY;
const sig = (apps) => [...apps].sort().join('\n');

function cleanDisplay(d) {
  if (!d || typeof d !== 'object' || Array.isArray(d)) return null;
  const apps = [];
  for (const a of Array.isArray(d.apps) ? d.apps : []) {
    if (!a || !recordable(a.id) || apps.some((x) => x.id === a.id)) continue;
    apps.push({ id: a.id, n: count(a.n), t: time(a.t) });
    if (apps.length >= MAX_APPS) break;
  }
  const sets = [];
  for (const s of Array.isArray(d.sets) ? d.sets : []) {
    if (!s || !Array.isArray(s.apps)) continue;
    const list = [...new Set(s.apps.filter(recordable))].slice(0, MAX_SET_APPS);
    if (!list.length || sets.some((x) => sig(x.apps) === sig(list))) continue;
    const e = { apps: list, n: count(s.n), t: time(s.t) };
    if (isLayout(s.layout)) e.layout = s.layout;
    if (Number.isFinite(s.ratio) && s.ratio > 0 && s.ratio < 1) e.ratio = Math.round(s.ratio * 1000) / 1000;
    sets.push(e);
    if (sets.length >= MAX_SETS) break;
  }
  const out = { apps, sets };
  if (d.quiet === true) out.quiet = true;
  return out;
}
const used = (d) => d.apps.length || d.sets.length || d.quiet;

export function cleanDisplayApps(raw) {
  const r = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
  const out = emptyDisplayApps();
  if (r.off === true) out.off = true;
  const ds = r.displays && typeof r.displays === 'object' && !Array.isArray(r.displays) ? r.displays : {};
  for (const [k, v] of Object.entries(ds)) {
    if (!isDisplayKey(k)) continue;
    const d = cleanDisplay(v);
    if (d && used(d)) out.displays[k] = d;
    if (Object.keys(out.displays).length >= MAX_DISPLAYS) break;
  }
  return out;
}

const lastSeen = (d) => Math.max(0, ...d.apps.map((a) => a.t), ...d.sets.map((s) => s.t));
// at most MAX_DISPLAYS: the one used longest ago goes (never `keep`, the display just written)
function capDisplays(displays, keep) {
  const keys = Object.keys(displays);
  if (keys.length <= MAX_DISPLAYS) return displays;
  const drop = keys.filter((k) => k !== keep).sort((a, b) => lastSeen(displays[a]) - lastSeen(displays[b])).slice(0, keys.length - MAX_DISPLAYS);
  const out = { ...displays };
  for (const k of drop) delete out[k];
  return out;
}
function withDisplay(rec, key, fn) {
  const r = cleanDisplayApps(rec);
  if (r.off || !isDisplayKey(key)) return r;
  const cur = r.displays[key] || { apps: [], sets: [] };
  const next = fn(cur);
  const displays = { ...r.displays };
  if (used(next)) displays[key] = next; else delete displays[key];
  return { ...r, displays: capDisplays(displays, key) };
}

export function noteApp(rec, key, id, now) {
  if (!recordable(id)) return cleanDisplayApps(rec);
  return withDisplay(rec, key, (d) => {
    const was = d.apps.find((a) => a.id === id);
    const apps = [{ id, n: was ? Math.min(was.n + 1, 1e6) : 1, t: time(now) }, ...d.apps.filter((a) => a.id !== id && fresh(a, now))];
    // the newest stays even when it ranks last
    const keep = ranked(apps.slice(1), now).slice(0, MAX_APPS - 1);
    return { ...d, apps: [apps[0], ...keep] };
  });
}

export function noteSet(rec, key, set, now) {
  const list = [...new Set((set?.apps || []).filter(recordable))].slice(0, MAX_SET_APPS);
  if (!list.length) return cleanDisplayApps(rec);
  return withDisplay(rec, key, (d) => {
    const was = d.sets.find((s) => sig(s.apps) === sig(list));
    // the latest order and layout of the same apps win (the person rearranged them)
    const e = { apps: list, n: was ? Math.min(was.n + 1, 1e6) : 1, t: time(now) };
    if (isLayout(set.layout)) e.layout = set.layout;
    if (Number.isFinite(set.ratio) && set.ratio > 0 && set.ratio < 1) e.ratio = Math.round(set.ratio * 1000) / 1000;
    const rest = d.sets.filter((s) => s !== was && fresh(s, now));
    return { ...d, sets: [e, ...ranked(rest, now).slice(0, MAX_SETS - 1)] };
  });
}

// The apps to offer first on display `key`: most used there, recent ones first. have(id): still installed.
export function quickApps(rec, key, now, { have = () => true, max = QUICK_APPS } = {}) {
  const r = cleanDisplayApps(rec);
  const d = r.displays[key];
  if (r.off || !d) return [];
  return ranked(d.apps.filter((a) => fresh(a, now) && have(a.id)), now).slice(0, max).map((a) => a.id);
}

// The usual set of display `key`: the best-ranked one seen at least MIN_SEEN times, without apps no longer
// installed; null while there is none, the record is off, or the display was told not to suggest.
export function suggestionFor(rec, key, now, { have = () => true } = {}) {
  const r = cleanDisplayApps(rec);
  const d = r.displays[key];
  if (r.off || !d || d.quiet) return null;
  for (const s of ranked(d.sets.filter((x) => x.n >= MIN_SEEN && fresh(x, now)), now)) {
    const apps = s.apps.filter(have);
    if (!apps.length) continue;
    return { apps, layout: s.layout || null, ratio: s.ratio ?? null };
  }
  return null;
}

export const quietDisplay = (rec, key) => withDisplay(rec, key, (d) => ({ ...d, quiet: true }));
// Forget every display (suggestions come back where they were turned off); off stays as it was.
export function clearDisplayApps(rec) {
  const r = cleanDisplayApps(rec);
  return r.off ? { v: DA_VERSION, off: true, displays: {} } : emptyDisplayApps();
}
// Off forgets too: nothing recorded is kept while the person asked not to record.
export function setDisplayAppsOff(rec, off) {
  if (off) return { v: DA_VERSION, off: true, displays: {} };
  const { off: _, ...r } = cleanDisplayApps(rec);
  return r;
}
export const displayAppsEmpty = (rec) => !Object.keys(cleanDisplayApps(rec).displays).length;
