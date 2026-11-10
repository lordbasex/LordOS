// collate.js — the order of names shown to people. QuickJS has no Intl, and its localeCompare compares code points and
// ignores the options (sensitivity, numeric), so "go-link Desk" sorted after every name starting with a capital (#64).
// compareNames: case and accents aside ("éte" with "Ete"), numbers by value ("Drive 2" before "Drive 10"). Text pieces
// still go through localeCompare, so an engine with a language's order uses it.
const nameKey = (s) => String(s == null ? '' : s).normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
const isDigits = (s) => s.charCodeAt(0) >= 48 && s.charCodeAt(0) <= 57;
export function compareNames(a, b) {
  const x = nameKey(a).match(/\d+|\D+/g) || [], y = nameKey(b).match(/\d+|\D+/g) || [];
  for (let i = 0; i < Math.min(x.length, y.length); i++) {
    const p = x[i], q = y[i];
    let d;
    if (isDigits(p) && isDigits(q)) {
      const u = p.replace(/^0+(?=\d)/, ''), v = q.replace(/^0+(?=\d)/, '');
      d = u.length - v.length || (u < v ? -1 : u > v ? 1 : 0) || p.length - q.length;
    } else d = p.localeCompare(q);
    if (d) return d < 0 ? -1 : 1;
  }
  return Math.sign(x.length - y.length);
}
