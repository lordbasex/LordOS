// lordosmodel.js — Store → Apps LordOS (2026-11-23): LordOS's own store. Its shop lists LordOS's installable apps and
// components from a data file (assets/store/lordos-apps.json: a new one is an entry there and its two texts), each
// installed the way it always was (a package of LordOS's repository through the package engine and its plan, an
// apt/catalog.json item through its own page, the voices from Settings); its second column starts with Installed
// (these items once installed, and the installed .lpk apps), then its categories once there are enough entries.
// Pure logic (node tests: server/tests/store_lordos_test.mjs); apps/store/lordospages.js and index.jsx draw it.

export const KINDS = ['deb', 'catalog', 'settings'];
// below this many entries the second column has no categories (Installed and All only)
export const CATS_AT = 6;

const ID = /^[a-zA-Z][a-zA-Z0-9]{1,40}$/;
const PKG = /^[a-z0-9][a-z0-9+.-]{1,80}$/;
const APP = /^[a-z][a-z0-9-]{0,40}$/;
const PAGE = /^(src:[a-z]+|[a-z]+)$/;

// The entries that are well formed, in the file's order: a known kind, an id the texts can use, a package for the ones
// the package engine installs, an open target that is an app id or a Store page; a settings entry must open its app.
// Anything else is left out (a file edited by hand must not break the Store).
export function nativeItems(data) {
  const list = data && Array.isArray(data.apps) ? data.apps : [];
  const seen = new Set();
  const out = [];
  for (const a of list) {
    if (!a || typeof a !== 'object' || !ID.test(String(a.id)) || seen.has(a.id) || !KINDS.includes(a.kind)) continue;
    const open = openOf(a.open);
    if (a.kind !== 'settings' && !PKG.test(String(a.package || ''))) continue;
    if (a.kind === 'settings' && !(open && open.app)) continue;
    seen.add(a.id);
    out.push({
      id: a.id, kind: a.kind, package: a.kind === 'settings' ? null : a.package, icon: typeof a.icon === 'string' ? a.icon : 'package',
      category: typeof a.category === 'string' && /^[a-z]+$/.test(a.category) ? a.category : null,
      installed: a.installed === 'voices' ? 'voices' : 'package', open,
    });
  }
  return out;
}

function openOf(o) {
  if (!o || typeof o !== 'object') return null;
  if (typeof o.app === 'string' && APP.test(o.app)) {
    return { app: o.app, args: o.args && typeof o.args === 'object' && !Array.isArray(o.args) ? { ...o.args } : null };
  }
  if (typeof o.page === 'string' && PAGE.test(o.page)) return { page: o.page };
  return null;
}

// The texts of an entry.
export const nameKey = (a) => `store.native.${a.id}.name`;
export const summaryKey = (a) => `store.native.${a.id}.summary`;

// Whether an entry is installed. probe: { pkg(name) → bool, voices() → bool } (the package index, the voice models).
export function isInstalled(a, probe) {
  if (!a || !probe) return false;
  if (a.installed === 'voices') return !!(probe.voices && probe.voices());
  return !!(a.package && probe.pkg && probe.pkg(a.package));
}
export const installedItems = (items, probe) => (items || []).filter((a) => isInstalled(a, probe));

// The card's action: Open (installed, with somewhere to open), View (installed, nothing to open: its page has Remove),
// Get (not installed).
export const actionOf = (a, installed) => (installed ? (a && a.open ? 'open' : 'view') : 'get');

// The second column's categories: none below CATS_AT entries; else each category with entries, in the file's order,
// and how many each has.
export function nativeCats(items) {
  const list = items || [];
  if (list.length < CATS_AT) return { cats: [], counts: {} };
  const cats = [], counts = {};
  for (const a of list) {
    if (!a.category) continue;
    if (!counts[a.category]) cats.push(a.category);
    counts[a.category] = (counts[a.category] || 0) + 1;
  }
  return { cats, counts };
}
export const inCategory = (items, cat) => (items || []).filter((a) => !cat || a.category === cat);

// The search: every word of the query in the entry's name, summary, id or package (case and accents ignored).
// text(key): the translated text of a key.
const fold = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
export function searchNative(items, query, text = (k) => k) {
  const words = fold(query).split(/\s+/).filter(Boolean);
  if (!words.length) return items || [];
  return (items || []).filter((a) => {
    const hay = fold([text(nameKey(a)), text(summaryKey(a)), a.id, a.package].join(' '));
    return words.every((w) => hay.includes(w));
  });
}

// The general Installed page's row of an installed entry: a launcher-like entry of kind "native", whose system is
// LordOS's (installedmodel.js), its id "native:<id>".
export const NATIVE_KIND = 'native';
export const rowId = (a) => `native:${a.id}`;
export const fromRowId = (id) => (typeof id === 'string' && id.startsWith('native:') ? id.slice(7) : null);
