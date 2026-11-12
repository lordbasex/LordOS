// recent.js — File → Open Recent (#113): the last files the Editor opened or saved, newest first,
// kept in settings.json as "editor": { recent: [absolute paths] }. No imports: tested with node,
// server/tests/editor_recent_test.mjs.

export const RECENT_MAX = 5;

const clean = (list) => (Array.isArray(list) ? list.filter((p) => typeof p === 'string' && p.startsWith('/')) : []);

// The list after opening or saving `path`: it goes first, once, and the list keeps RECENT_MAX.
export function addRecent(list, path, max = RECENT_MAX) {
  const cur = clean(list);
  if (typeof path !== 'string' || !path.startsWith('/')) return cur.slice(0, max);
  return [path, ...cur.filter((p) => p !== path)].slice(0, max);
}

export function removeRecent(list, path) {
  return clean(list).filter((p) => p !== path);
}

// The submenu's rows: [{ path, label, missing }]. A file that is not there now (deleted, or on a
// drive that is not mounted) stays in the list, greyed out. Two files with the same name show their
// folder too, shortened to ~ under the home folder.
export function recentRows(list, { exists = () => true, home = '' } = {}) {
  const paths = clean(list).slice(0, RECENT_MAX);
  const base = (p) => p.split('/').filter(Boolean).pop() || p;
  const counts = new Map();
  for (const p of paths) counts.set(base(p), (counts.get(base(p)) || 0) + 1);
  const short = (d) => (home && (d === home || d.startsWith(`${home}/`)) ? `~${d.slice(home.length)}` : d || '/');
  return paths.map((p) => {
    const name = base(p);
    const label = counts.get(name) > 1 ? `${name} — ${short(p.split('/').slice(0, -1).join('/'))}` : name;
    let missing = false;
    try { missing = !exists(p); } catch (e) { missing = true; }
    return { path: p, label, missing };
  });
}
