// installedmodel.js — the Store's installed apps by package system (2026-11-23): each origin's second column has
// "Installed" at the top, over its categories (the shop comes first), and shows only that system's installed apps;
// the general Installed page lists every installed app, whatever the system, with a filter per system that has any.
// Pure logic (node tests: server/tests/store_installed_test.mjs); index.jsx and origincol.js draw it.

// The second column's "Installed": a value of the Store's `sub` (an origin's category otherwise); no category is
// called that (the categories are freedesktop's: AudioVideo, Graphics, …).
export const MINE = 'installed';
export const isMine = (sub) => sub === MINE;

// The package systems, in the order the filters show them.
export const SYSTEMS = ['lordos', 'debian', 'flathub', 'web'];

// A launcher entry's system (sdk/apps.js's `kind`): LordOS's own apps and installed .lpk packages, the Linux apps
// Debian's packages brought, the Flatpak apps, the web apps. Anything else: null.
export function systemOf(a) {
  const k = a && a.kind;
  if (k === 'lordos' || k === 'package') return 'lordos';
  if (k === 'linux') return 'debian';
  if (k === 'flatpak') return 'flathub';
  if (k === 'web') return 'web';
  return null;
}

// The origin page ('src:debian' | 'src:flathub' | 'src:web') whose system this is, the other way round.
export const originOf = (system) => (system === 'debian' || system === 'flathub' || system === 'web' ? `src:${system}` : null);

// The general Installed page's filters: [key, count] for All, each system that has installed apps (in SYSTEMS' order)
// and Deactivated (always: it says how to turn a LordOS app off again). A filter that is chosen stays even when its
// system has nothing left (the person sees it empty rather than jumping to another).
export function installedFilters(apps, chosen = 'all') {
  const list = apps || [];
  const out = [['all', list.length]];
  for (const s of SYSTEMS) {
    const n = list.filter((a) => systemOf(a) === s).length;
    if (n || chosen === s) out.push([s, n]);
  }
  out.push(['off', list.filter((a) => a.off).length]);
  return out;
}

// The apps a filter keeps: all, one system's, or the deactivated ones.
export function filterInstalled(apps, filter) {
  const list = apps || [];
  if (filter === 'off') return list.filter((a) => a.off);
  if (SYSTEMS.includes(filter)) return list.filter((a) => systemOf(a) === filter);
  return list;
}

// A known filter, else All (a page opened with an old or a wrong `filter` argument).
export const filterOk = (f) => (f === 'all' || f === 'off' || SYSTEMS.includes(f) ? f : 'all');
