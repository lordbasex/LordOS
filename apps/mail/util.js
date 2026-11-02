// util.js — small helpers for Mail: dates in the list, folder names, color categories, opening
// an attachment the way Files would.
import { t, formatDate, formatTime, weekdayName, openFileWithApps, revealPath } from '@lordos/sdk';

export const CATEGORY_COLORS = ['#ff453a', '#ff9f0a', '#ffd60a', '#30d158', '#0a84ff', '#bf5af2'];
// labels are built at render time (they follow the language)
export const categoryName = (i, cfg) => cfg?.categories?.[i - 1]?.name || t(`mail.cat.${i}`);

export function listDate(ms) {
  const d = new Date(ms), now = new Date();
  if (d.toDateString() === now.toDateString()) return formatTime(d);
  const y = new Date(now); y.setDate(now.getDate() - 1);
  if (d.toDateString() === y.toDateString()) return t('mail.yesterday');
  if (now - d < 6 * 86400000) return weekdayName(d.getDay(), false);
  return formatDate(d, d.getFullYear() === now.getFullYear() ? 'short' : 'numeric');
}

export const fullDate = (ms) => `${formatDate(new Date(ms), 'long')} ${formatTime(new Date(ms))}`;

const SPECIAL_ICONS = { inbox: '📥', drafts: '📝', sent: '📤', archive: '🗄️', junk: '⚠️', trash: '🗑️', all: '📚', flagged: '🚩' };
export const folderIcon = (f) => SPECIAL_ICONS[f.special] || '📁';
export const folderName = (f) => (f.special && f.special !== 'all' && f.special !== 'flagged' ? t(`mail.folder.${f.special}`) : f.display);

export const addrText = (a) => (a?.name ? `${a.name} <${a.email}>` : a?.email || '');
export const addrShort = (a) => a?.name || a?.email || '';

export function formatSize(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}

// The same rules as Files (apps/files/ops.js), in the session (sdk/sysopen.js).
export const openPath = (path, name) => openFileWithApps(path, name);
export { revealPath };

// The same rule as axia-maild (mime_safe_filename): a sender-chosen attachment name becomes a
// plain basename: no path parts, no leading dots, no control, bidi-override or FAT-reserved
// characters, at most 200 bytes. Never empty.
export function safeName(name) {
  let s = String(name || '').split(/[/\\]/).pop();
  s = s.replace(/^[.\s]+/, '').replace(/[\u0000-\u001f\u007f-\u009f<>:"|?*\u200e\u200f\u061c\u202a-\u202e\u2066-\u2069\ufeff]/g, '_');
  const bytes = (x) => { let n = 0; for (const ch of x) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; } return n; };
  while (bytes(s) > 200) s = [...s].slice(0, -1).join('');
  s = s.replace(/[.\s]+$/, '');
  return s && !/^[._\s]+$/.test(s) ? s : 'attachment';
}
