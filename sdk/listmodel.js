// listmodel.js — what the pointer and the keys do to the selection of a List or a Table (sdk/lists.jsx), with no
// screen. Pure logic: tests in server/tests/list_model_test.mjs.
//
// A selection is { selected, anchor, lead } over the keys of the rows, in the order they are shown: the selected keys
// (in that order), the row a Shift range starts from and the row the keyboard is on. A key that is no longer shown
// drops out (keepShown).
//
//   clickSelect(s, keys, key, { shift, toggle, multiple })   a press on a row: that row alone, a range from the anchor
//                                                              (Shift) or that row in or out (Ctrl or ⌘)
//   keySelect(e, s, keys, { multiple, page })                 ↑ ↓, Home, End, Page Up and Page Down move the lead and
//                                                              select it; with Shift they extend from the anchor, with
//                                                              Ctrl or ⌘ they move only the lead, and Ctrl+Space then
//                                                              toggles it; Ctrl+A selects every row. null: not its key
//   findSelect(s, keys, texts, buf)                           type-to-find: the next row whose text starts with what
//                                                              was typed (sdk/menunav.js typeAhead keeps the letters)
import { matchIndex } from './menunav.js';

export const NO_SELECTION = Object.freeze({ selected: [], anchor: null, lead: null });

// The keys from a to b (either way round), as they are shown.
export function rangeOf(keys, a, b) {
  const i = keys.indexOf(a), j = keys.indexOf(b);
  if (j < 0) return [];
  if (i < 0) return [b];
  return keys.slice(Math.min(i, j), Math.max(i, j) + 1);
}

// The selection with only the keys still shown; the anchor and the lead go when their row does.
export function keepShown(s, keys) {
  const shown = new Set(keys);
  const selected = keys.filter((k) => (s.selected || []).includes(k));
  const anchor = shown.has(s.anchor) ? s.anchor : null, lead = shown.has(s.lead) ? s.lead : null;
  if (selected.length === (s.selected || []).length && anchor === s.anchor && lead === s.lead) return s;
  return { selected, anchor, lead };
}

// in the order of keys
const ordered = (keys, set) => keys.filter((k) => set.has(k));

export function clickSelect(s, keys, key, { shift = false, toggle = false, multiple = false } = {}) {
  if (!keys.includes(key)) return s;
  if (multiple && shift) {
    const anchor = s.anchor != null && keys.includes(s.anchor) ? s.anchor : key;
    return { selected: rangeOf(keys, anchor, key), anchor, lead: key };
  }
  if (multiple && toggle) {
    const set = new Set(s.selected);
    if (set.has(key)) set.delete(key); else set.add(key);
    return { selected: ordered(keys, set), anchor: key, lead: key };
  }
  return { selected: [key], anchor: key, lead: key };
}

const plainish = (e) => !e.alt;
const cmd = (e) => !!(e.ctrl || e.meta);

// Where a move key takes the lead from index i among n rows (page: the rows that fit in the view); null otherwise.
export function moveIndex(key, i, n, page = 10) {
  if (!(n > 0)) return null;
  const p = Math.max(1, page | 0);
  switch (key) {
    case 'ArrowDown': return i < 0 ? 0 : Math.min(n - 1, i + 1);
    case 'ArrowUp': return i < 0 ? n - 1 : Math.max(0, i - 1);
    case 'Home': return 0;
    case 'End': return n - 1;
    case 'PageDown': return i < 0 ? Math.min(n - 1, p - 1) : Math.min(n - 1, i + p);
    case 'PageUp': return i < 0 ? 0 : Math.max(0, i - p);
    default: return null;
  }
}

export function keySelect(e, s, keys, { multiple = false, page = 10 } = {}) {
  if (!e || !plainish(e) || !keys.length) return null;
  const k = String(e.base || e.key || '').toLowerCase();
  if (cmd(e) && k === 'a' && !e.shift) return multiple ? { selected: keys.slice(), anchor: keys[0], lead: s.lead ?? keys[0] } : null;
  const lead = keys.indexOf(s.lead);
  if (e.key === ' ') {
    if (lead < 0) return null;
    if (cmd(e) && multiple) return clickSelect(s, keys, s.lead, { toggle: true, multiple });
    if (cmd(e) || e.shift) return null;
    return s.selected.length === 1 && s.selected[0] === s.lead ? s : { selected: [s.lead], anchor: s.lead, lead: s.lead };
  }
  const to = moveIndex(e.key, lead, keys.length, page);
  if (to == null) return null;
  const key = keys[to];
  if (multiple && e.shift) {
    const anchor = s.anchor != null && keys.includes(s.anchor) ? s.anchor : s.lead != null && lead >= 0 ? s.lead : key;
    return { selected: rangeOf(keys, anchor, key), anchor, lead: key };
  }
  if (multiple && cmd(e)) return { ...s, lead: key };
  if (cmd(e) || e.shift) return null;
  return { selected: [key], anchor: key, lead: key };
}

// Type-to-find: buf is what was typed so far (lowercase); texts[i] the text of the row keys[i]. The row found is
// selected alone, as an arrow would; null when no row starts with it.
export function findSelect(s, keys, texts, buf) {
  const items = texts.map((x) => ({ label: String(x ?? '') }));
  const i = matchIndex(items, keys.indexOf(s.lead), buf);
  if (i < 0) return null;
  return { selected: [keys[i]], anchor: keys[i], lead: keys[i] };
}

// A printable character with no Ctrl, ⌘ or ⌥ (Shift is fine): a letter for type-to-find. Space only while a search
// is already going on (it then belongs to the name), otherwise it selects.
export function findChar(e, searching = false) {
  if (!e || e.ctrl || e.meta || e.alt) return '';
  const ch = typeof e.char === 'string' ? e.char : '';
  if ([...ch].length !== 1) return '';
  if (ch === ' ') return searching ? ' ' : '';
  return /\S/u.test(ch) ? ch : '';
}
