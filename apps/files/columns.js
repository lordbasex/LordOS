// columns.js — the list view's columns as data, no screen: which show, in what order and how wide. Name is always first.
export const ALL_COLUMNS = ['date', 'created', 'opened', 'changed', 'size', 'kind', 'comments', 'tags', 'perms', 'owner'];
const DEFAULT_W = { date: 150, created: 150, opened: 150, changed: 150, size: 80, kind: 120, comments: 180, tags: 80, perms: 70, owner: 110 };
export const MIN_W = 50;
export const MAX_W = 600;
export const NAME_MIN = 200;
export const NAME_MAX = 1400;
const ON = ['date', 'size', 'kind'];

// what is stored is only trusted as far as it names known columns
export function normalColumns(c) {
  const order = (c && Array.isArray(c.order) ? c.order : []).filter((k, i, a) => ALL_COLUMNS.includes(k) && a.indexOf(k) === i);
  const shown = (c && Array.isArray(c.shown) ? c.shown : ON).filter((k, i, a) => ALL_COLUMNS.includes(k) && a.indexOf(k) === i);
  const width = {};
  for (const k of ALL_COLUMNS) width[k] = clampW(c?.width?.[k] ?? DEFAULT_W[k]);
  const nameWidth = c?.nameWidth ? clampName(c.nameWidth) : null;     // null: Name takes the room left
  return { order: [...order, ...ALL_COLUMNS.filter((k) => !order.includes(k))], shown, width, nameWidth };
}
export const clampName = (w) => Math.min(NAME_MAX, Math.max(NAME_MIN, Math.round(Number(w) || NAME_MIN)));
export const clampW = (w) => Math.min(MAX_W, Math.max(MIN_W, Math.round(Number(w) || MIN_W)));
export const visible = (c) => c.order.filter((k) => c.shown.includes(k));
export const toggleColumn = (c, k) => ({ ...c, shown: c.shown.includes(k) ? c.shown.filter((x) => x !== k) : [...c.shown, k] });
export const resize = (c, k, w) => (k === 'name' ? { ...c, nameWidth: clampName(w) } : { ...c, width: { ...c.width, [k]: clampW(w) } });
// moves k to where `before` is among the visible ones; null: the end
export function moveColumn(c, k, before) {
  const rest = c.order.filter((x) => x !== k);
  const at = before == null ? rest.length : rest.indexOf(before);
  return at < 0 ? c : { ...c, order: [...rest.slice(0, at), k, ...rest.slice(at)] };
}
// which column a pointer at x (from the start of the first column after Name) falls before; null past the end
export function dropBefore(c, x) {
  let e = 0;
  for (const k of visible(c)) { const w = c.width[k]; if (x < e + w / 2) return k; e += w; }
  return null;
}
export const totalWidth = (c) => visible(c).reduce((s, k) => s + c.width[k], 0);
