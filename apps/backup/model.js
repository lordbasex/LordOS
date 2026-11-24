// model.js — Backups' Settings without a screen (#200): what a folder chosen in the file dialog, or typed, becomes as an
// exclusion. Backups copies the home folder only, so an exclusion is a folder inside it, kept relative to it ("Videos/Large"),
// as lord-server's backup.json stores it (bk_rel_valid: no empty, "." or ".." element).
// node server/tests/backup_model_test.mjs

// input: an absolute path (the dialog's), "~/…" or a path relative to the home folder (typed).
// → null (nothing typed) | { rel } | { error } with error a backup.* key:
//   excludeOutside  not inside the home folder (or written with "." or ".."), so not something Backups copies
//   excludeHome     the home folder itself: nothing would be left to back up
export function exclusionFor(input, home) {
  let s = String(input ?? '').trim();
  if (!s) return null;
  const h = String(home || '').replace(/\/+$/, '');
  if (s === '~' || s.startsWith('~/')) s = s.slice(1);
  else if (s.startsWith('/')) {
    const p = s.replace(/\/+$/, '');
    if (!h || (p !== h && !p.startsWith(`${h}/`))) return { error: 'backup.excludeOutside' };
    s = p.slice(h.length);
  }
  const parts = s.split('/').filter(Boolean);
  if (parts.some((p) => p === '.' || p === '..')) return { error: 'backup.excludeOutside' };
  if (!parts.length) return { error: 'backup.excludeHome' };
  return { rel: parts.join('/') };
}

// The exclusions after adding rel: once, in the order added.
export const addExclusion = (list, rel) => {
  const l = Array.isArray(list) ? list.filter((x) => typeof x === 'string' && x) : [];
  return !rel || l.includes(rel) ? l : [...l, rel];
};

// What Restore sends for the rows picked in a folder of a backup (#231): one row → its path (as before), several →
// the list of their paths (lord.backup.restore takes up to MAX_RESTORE at once, each checked as one), none → the
// folder shown (rel), or null at the top (Restore All has its own button and confirmation). Names that can't be a
// path's element ("", ".", "..", with "/") are dropped: lord-server refuses them anyway.
export const MAX_RESTORE = 64;
const okName = (n) => typeof n === 'string' && n !== '' && n !== '.' && n !== '..' && !n.includes('/');
export function restoreTarget(rel, picked) {
  const base = String(rel || '');
  const names = [...new Set((Array.isArray(picked) ? picked : []).filter(okName))];
  const join = (n) => (base ? `${base}/${n}` : n);
  if (names.length === 1) return join(names[0]);
  if (names.length > 1) return names.slice(0, MAX_RESTORE).map(join);
  return base || null;
}

// The last element of a path, for "Restoring 2 of 5: name" and a failed item's line.
export const baseName = (rel) => String(rel || '').split('/').filter(Boolean).pop() || '';
