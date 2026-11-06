// search.js — the Store's search rules (0.8.8.4, #37), pure functions with no SDK imports
// (server/tests/store_search_test.mjs). The same rules as the package engine's lord.pkg.search
// (server/src/pkg/match.c): the query is split into words; case and accents don't matter; the words
// may be written joined or apart ("vs code" finds "vscode", "vscode" finds "VS Code"); an item with
// every word comes first, ranked by where the words are (the exact name, the name's start, a word's
// start, inside a word); with two words or more an item with only some of them still comes, after
// all of those, when one of its words starts a word of its name or keywords.

// Lower case, accents removed (é → e, ñ → n, ł → l), every other character kept.
const EXTRA = { 'ł': 'l', 'đ': 'd', 'ø': 'o', 'æ': 'a', 'œ': 'o', 'ß': 's', 'þ': 't', 'ı': 'i', 'ħ': 'h', 'ŧ': 't' };
export function fold(s) {
  let t = String(s == null ? '' : s).toLowerCase();
  try { t = t.normalize('NFD').replace(/[̀-ͯ]/g, ''); } catch (e) { /* no normalize: keep accents */ }
  return t.replace(/[łđøæœßþıħŧ]/g, (c) => EXTRA[c]);
}

const SEP = /[\s!-/:-@[-`{-~ -¿‐-‧]+/;      // spaces and ASCII/Latin-1 punctuation
// Without spaces or punctuation: "VS Code" → "vscode".
export const compact = (s) => fold(s).split(SEP).join('');

// The query's words (folded, at most 8) and the same joined.
export function parseQuery(q) {
  const words = fold(q).split(SEP).filter(Boolean).slice(0, 8);
  return { words, joined: words.join('') };
}

// How well a word appears in a folded text: 4 the whole text, 3 its start, 2 the start of a word,
// 1 inside a word (3 letters or more) or across words (`comp`), 0 not at all.
export function level(text, w, comp) {
  if (!text || !w) return 0;
  if (text === w) return 4;
  let best = 0;
  for (let i = text.indexOf(w); i >= 0; i = text.indexOf(w, i + 1)) {
    if (i === 0) return 3;
    if (SEP.test(text[i - 1])) best = 2;
    else if (!best && w.length >= 3) best = 1;
  }
  if (!best && w.length >= 3 && comp && comp.includes(w)) best = 1;
  return best;
}

// Points per level for the item's names (its name, id, package and keywords) and the rest.
const NAME_PTS = [0, 120, 300, 500, 1000];
const KEY_PTS = [0, 100, 260, 420, 850];
const TEXT_PTS = [0, 20, 50, 50, 50];

// Prepares an item's texts once: `names` (strongest first: the name, then the ids and keywords) and
// `texts` (summaries, vendor).
export function prepare(names, texts) {
  const n = (names || []).filter(Boolean).map((s) => ({ f: fold(s), c: compact(s) }));
  return { names: n, texts: (texts || []).filter(Boolean).map(fold) };
}

// The score of a prepared item for a parsed query: 0 when it doesn't match; `full` above 10000.
export function score(item, q) {
  const { words, joined } = q;
  if (!words.length) return 0;
  let total = 0, matched = 0, starts = 0;
  for (const w of words) {
    let s = 0, start = false;
    item.names.forEach((n, i) => {
      const l = level(n.f, w, n.c);
      if (l >= 2 && w.length >= 2) start = true;
      const p = (i === 0 ? NAME_PTS : KEY_PTS)[l];
      if (p > s) s = p;
    });
    if (start) starts++;
    if (!s) for (const t of item.texts) { const p = TEXT_PTS[level(t, w)]; if (p > s) s = p; }
    if (s) matched++;
    total += s;
  }
  if (words.length >= 2) {
    const phrase = words.join(' ');
    if (item.names.some((n) => n.c === joined)) { total += 1400; matched = words.length; }
    else if (item.names.some((n) => n.c.includes(joined))) { total += 600; matched = words.length; }
    // else the words together, in that order, in a summary ("based on VS Code")
    else if (matched === words.length && item.texts.some((t) => t.split(SEP).join(' ').includes(phrase))) total += 400;
  }
  if (matched === words.length) return total + 10000;
  if (words.length < 2 || starts < 1) return 0;
  return Math.max(1, Math.min(9000, Math.floor(total / 4)));
}

// The items that match a query, best first (ties keep the list's order). `fields(item)` gives
// { names: [...], texts: [...] }.
export function search(items, query, fields) {
  const q = parseQuery(query);
  if (!q.words.length) return items.slice();
  const out = [];
  items.forEach((it, i) => {
    const f = fields(it);
    const s = score(prepare(f.names, f.texts), q);
    if (s > 0) out.push({ it, s, i });
  });
  out.sort((a, b) => b.s - a.s || a.i - b.i);
  return out.map((x) => x.it);
}

// Whether one item matches (the pages that filter a list in place).
export const matches = (query, names, texts) => {
  const q = parseQuery(query);
  return !q.words.length || score(prepare(names, texts), q) > 0;
};

// A catalog app's texts (apt/catalog.json): its name, id, package and keywords, then its summary
// in the given language and in English, and its vendor.
export const catalogFields = (a, lang) => ({
  names: [a.name, a.id, a.package, ...(a.keywords || [])],
  texts: [a.summary && a.summary[lang], a.summary && a.summary.en, a.vendor, a.kind],
});
