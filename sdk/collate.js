// collate.js — the order of names shown to people. QuickJS has no Intl, and its localeCompare compares code points and
// ignores the options (sensitivity, numeric), so "go-link Desk" sorted after every name starting with a capital (#64).
// compareNames: case and accents aside ("éte" with "Ete"), numbers by value ("Drive 2" before "Drive 10"). Text pieces
// still go through localeCompare, so an engine with a language's order uses it.
const nameKey = (s) => String(s == null ? '' : s).normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
const isDigits = (s) => s.charCodeAt(0) >= 48 && s.charCodeAt(0) <= 57;
// A name cut into its pieces (case and accents aside): what compareNames compares. Making them is most of its cost
// (normalize and two regular expressions), so a list sorts by them made once per item (sortByName), not twice per
// comparison: the Store's Debian page sorted ~1600 names in ~150 ms, now in a few.
// The pieces of the last names asked for are kept (PARTS_MAX of them), so the many sorts that call compareNames
// directly get most of that too.
const PARTS_MAX = 8192;
let partsOf = new Map();
export function nameParts(s) {
  const k = String(s == null ? '' : s);
  let p = partsOf.get(k);
  if (p === undefined) {
    p = nameKey(k).match(/\d+|\D+/g) || [];
    if (partsOf.size >= PARTS_MAX) partsOf = new Map();
    partsOf.set(k, p);
  }
  return p;
}
export function compareParts(x, y) {
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
export function compareNames(a, b) {
  return compareParts(nameParts(a), nameParts(b));
}
// A new array of the items in compareNames order of nameOf(item); equal names keep their order (a stable sort).
export function sortByName(list, nameOf = (x) => x) {
  const keyed = list.map((item) => [nameParts(nameOf(item)), item]);
  keyed.sort((p, q) => compareParts(p[0], q[0]));
  return keyed.map((p) => p[1]);
}
