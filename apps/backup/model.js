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
