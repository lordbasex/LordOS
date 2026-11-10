// locations.js — what each sidebar place shows and how it is sorted and grouped.
//   { type: 'dir', path } | { type: 'recent' } | { type: 'apps' } | { type: 'trash' }
//   | { type: 'tag', color } | { type: 'search', root, query }
import { fs, compareNames } from '@lordos/sdk';
import { kindOf } from './kinds.js';
import { tagStore, tagsOf, commentOf } from './ops.js';
import { archiveEntries } from './archive.js';

export const sameLoc = (a, b) => a && b && a.type === b.type && a.path === b.path && a.color === b.color && a.query === b.query && a.root === b.root && (a.inner || '') === (b.inner || '');

export const LIST_CAP = 5000;
const norm = (dir) => (e) => ({ ...e, path: `${dir === '/' ? '' : dir}/${e.name}` });

// Entries of a place (unsorted). hidden: show the ones that start with a dot.
export function loadEntries(loc, { hidden, launcherApps }) {
  switch (loc.type) {
    case 'dir': {
      // lord.fs.list stops at 5000 entries a folder: the list says so (`capped`) for Files to tell
      const raw = lord.fs.list(loc.path);
      const out = raw.filter((e) => hidden || !e.name.startsWith('.')).map(norm(loc.path));
      out.capped = raw.length === LIST_CAP;
      return out;
    }
    case 'recent': return fs.recent(80, 30).map((e) => ({ ...e, recent: true }));
    case 'apps': return launcherApps.map((a) => ({ app: true, id: a.id, name: a.name, Icon: a.Icon, path: `app:${a.id}`, dir: false, size: 0, mtime: 0, kindLabel: a.kind }));
    case 'trash': return fs.trashList().map((x) => ({ name: x.name, path: x.id, trash: true, original: x.original, deleted: x.deleted, dir: x.dir, size: x.size, mtime: Date.parse(x.deleted) || 0 }));
    case 'tag': {
      const all = tagStore.get();
      const out = [];
      for (const [p, colors] of Object.entries(all)) {
        if (!colors.includes(loc.color)) continue;
        try { const s = fs.stat(p); out.push({ name: s.name, path: s.path, dir: s.dir, size: s.size, mtime: s.mtime }); } catch (e) { /* no longer exists */ }
      }
      return out;
    }
    case 'search': return loc.query ? fs.search(loc.root, loc.query, { limit: 300, depth: 10 }) : [];
    case 'archive': return archiveEntries(loc);
    default: return [];
  }
}

const collator = compareNames;   // QuickJS ignores localeCompare's options (sdk/collate.js)

// statOf: what a column needs beyond the listing (views.jsx); foldersFirst: Files → Settings
export function sortEntries(list, sort, { statOf, foldersFirst = true } = {}) {
  const dir = sort.desc ? -1 : 1;
  const key = sort.by;
  const num = { created: 'btime', opened: 'atime', changed: 'ctime' }[key];
  const text = { perms: (e) => statOf?.(e)?.mode || '', owner: (e) => statOf?.(e)?.user || '', comments: (e) => commentOf(e.path), tags: (e) => (tagsOf(e.path)[0] || '') }[key];
  const cmp = (a, b) => {
    if (foldersFirst && a.dir !== b.dir && key !== 'kind') return a.dir ? -1 : 1;
    let r = 0;
    if (key === 'date') r = (a.mtime || 0) - (b.mtime || 0);
    else if (key === 'size') r = (a.dir ? -1 : a.size) - (b.dir ? -1 : b.size);
    else if (key === 'kind') r = collator(kindOf(a), kindOf(b));
    else if (num) r = (statOf?.(a)?.[num] || 0) - (statOf?.(b)?.[num] || 0);
    else if (text) r = collator(text(a), text(b));
    if (!r) r = collator(a.name, b.name);
    return r * dir;
  };
  return [...list].sort(cmp);
}

// Groups: [{ key, label, items }] (a single untitled group if group = 'none').
export function groupEntries(list, group, t) {
  if (group === 'none') return [{ key: 'all', label: '', items: list }];
  const buckets = new Map();
  const now = Date.now(), day = 86400000;
  for (const e of list) {
    let k;
    if (group === 'kind') k = kindOf(e);
    else if (group === 'date') {
      const age = now - (e.mtime || 0);
      k = age < day ? 'today' : age < 7 * day ? 'week' : age < 30 * day ? 'month' : age < 365 * day ? 'year' : 'older';
    } else if (group === 'size') {
      k = e.dir ? 'folders' : e.size < 1e5 ? 'small' : e.size < 1e7 ? 'medium' : e.size < 1e9 ? 'large' : 'huge';
    }
    if (!buckets.has(k)) buckets.set(k, []);
    buckets.get(k).push(e);
  }
  const order = { date: ['today', 'week', 'month', 'year', 'older'], size: ['folders', 'small', 'medium', 'large', 'huge'] }[group];
  const label = (k) => t(`files.group.${group}.${k}`);
  // by kind: folders first, then by kind name
  const keys = order ? order.filter((k) => buckets.has(k))
    : [...buckets.keys()].sort((a, b) => (a === 'folder' ? -1 : b === 'folder' ? 1 : collator(label(a), label(b))));
  return keys.map((k) => ({ key: k, label: label(k), items: buckets.get(k) }));
}

export { userDirs } from '../../sdk/fileparts.jsx';
