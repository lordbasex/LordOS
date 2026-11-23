// snippetsmodel.js — the clipboard's Snippets (GitHub #205): texts the person keeps to paste again, each with a name, in
// the order they chose. Pure, without a screen; sdk/clipboard.js keeps the list, saves it and draws the tab. Tested by
// server/tests/snippets_model_test.mjs.
//
//   cleanText(s), cleanName(s)            plain text (line breaks kept, other control characters dropped), one-line names
//   nameFor(text)                         the name a snippet gets when none is given: its first line, shortened
//   addSnippet(list, { name, text }, { id, time })     → { list, snippet } | { error }   (new ones go first)
//   updateSnippet(list, id, { name, text }, time)       → { list, snippet } | { error }   (edit and rename)
//   removeSnippet(list, id) / moveSnippet(list, id, step) → list
//   matchSnippet(s, q)                    the search: its name or its text
//   parseSnippets(json) / serializeSnippets(list)       the file, read back with every limit applied again
//
// Errors: 'empty' (no text), 'tooMany' (SNIPPET_MAX_COUNT), 'tooLong' (SNIPPET_MAX_BYTES of UTF-8), 'missing' (no such
// id), 'exists' (the same text is already a snippet; `snippet` is that one). The limits keep the file below lord.fs's
// and lord.clipboard.saveStore's 32 MB even with every snippet full and escaped.

export const SNIPPET_MAX_COUNT = 200;
export const SNIPPET_MAX_BYTES = 65536;
export const SNIPPET_MAX_NAME = 80;          // characters (code points)
const AUTO_NAME = 40;

export function utf8Bytes(s) {
  let n = 0;
  for (const ch of String(s)) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
  return n;
}

// Line breaks as \n; tabs kept; NUL and the other C0 controls and DEL dropped (plain text, nothing that could steer a
// terminal it is pasted into).
export function cleanText(s) {
  return String(s ?? '').replace(/\r\n?/g, '\n').replace(/[\u0000-\u0008\u000b-\u001f\u007f]/g, '');
}

export function cleanName(s) {
  const one = cleanText(s).replace(/\s+/g, ' ').trim();
  const cps = [...one];
  return cps.length > SNIPPET_MAX_NAME ? cps.slice(0, SNIPPET_MAX_NAME).join('').trimEnd() : one;
}

export function nameFor(text) {
  const line = cleanText(text).split('\n').map((l) => l.replace(/\s+/g, ' ').trim()).find(Boolean) || '';
  const cps = [...line];
  return cps.length > AUTO_NAME ? `${cps.slice(0, AUTO_NAME - 1).join('').trimEnd()}…` : line;
}

// the text cut at the byte limit, on a character's edge
function capText(text) {
  if (utf8Bytes(text) <= SNIPPET_MAX_BYTES) return text;
  let n = 0, out = '';
  for (const ch of text) {
    const b = utf8Bytes(ch);
    if (n + b > SNIPPET_MAX_BYTES) break;
    n += b; out += ch;
  }
  return out;
}

function checkText(raw) {
  const text = cleanText(raw);
  if (!text.trim()) return { error: 'empty' };
  if (utf8Bytes(text) > SNIPPET_MAX_BYTES) return { error: 'tooLong' };
  return { text };
}

export function addSnippet(list, { name, text } = {}, { id, time = 0 } = {}) {
  const c = checkText(text);
  if (c.error) return c;
  const same = list.find((s) => s.text === c.text);
  if (same) return { error: 'exists', snippet: same };
  if (list.length >= SNIPPET_MAX_COUNT) return { error: 'tooMany' };
  const snippet = { id: String(id), name: cleanName(name) || nameFor(c.text), text: c.text, created: time, updated: time };
  return { list: [snippet, ...list], snippet };
}

export function updateSnippet(list, id, { name, text } = {}, time = 0) {
  const old = list.find((s) => s.id === id);
  if (!old) return { error: 'missing' };
  const c = text === undefined ? { text: old.text } : checkText(text);
  if (c.error) return c;
  const same = list.find((s) => s.id !== id && s.text === c.text);
  if (same) return { error: 'exists', snippet: same };
  const nm = name === undefined ? old.name : cleanName(name) || nameFor(c.text);
  const snippet = { ...old, name: nm, text: c.text, updated: time };
  return { list: list.map((s) => (s.id === id ? snippet : s)), snippet };
}

export const removeSnippet = (list, id) => list.filter((s) => s.id !== id);

// one place up (step < 0) or down; the list as it was at either end
export function moveSnippet(list, id, step) {
  const i = list.findIndex((s) => s.id === id);
  const j = i + (step < 0 ? -1 : 1);
  if (i < 0 || j < 0 || j >= list.length) return list;
  const out = [...list];
  [out[i], out[j]] = [out[j], out[i]];
  return out;
}

export function matchSnippet(s, q) {
  const k = String(q || '').toLowerCase();
  return !k || s.name.toLowerCase().includes(k) || s.text.toLowerCase().includes(k);
}

// A file someone else may have written: only well-formed entries, every limit applied again, ids unique, no text twice.
export function parseSnippets(json) {
  let items;
  try { items = JSON.parse(json)?.items; } catch (e) { return []; }
  if (!Array.isArray(items)) return [];
  const out = [], ids = new Set(), texts = new Set();
  for (const x of items) {
    if (out.length >= SNIPPET_MAX_COUNT) break;
    if (!x || typeof x !== 'object' || typeof x.text !== 'string') continue;
    const text = capText(cleanText(x.text));
    if (!text.trim() || texts.has(text)) continue;
    let id = typeof x.id === 'string' && /^[\w-]{1,40}$/.test(x.id) ? x.id : '';
    if (!id || ids.has(id)) { let k = out.length; do id = `s${k++}`; while (ids.has(id)); }
    const num = (v) => (Number.isFinite(v) && v >= 0 ? v : 0);
    out.push({ id, name: cleanName(typeof x.name === 'string' ? x.name : '') || nameFor(text), text, created: num(x.created), updated: num(x.updated) });
    ids.add(id); texts.add(text);
  }
  return out;
}

export const serializeSnippets = (list) => JSON.stringify({ version: 1, items: list.map(({ id, name, text, created, updated }) => ({ id, name, text, created, updated })) });
