// spell.js — the spell checker for LordOS's own text fields (docs/SPELLING.md): the languages, the cache of words
// already asked about, the person's dictionary and "Ignore", and the batches sent to lord.spell (lord-server, which asks
// the sandboxed lord-spell helper). The pure part is spellmodel.js.
//
// Settings: spelling.on (true unless the person turns it off, Settings → Keyboard) and spelling.langs (the
// dictionaries chosen; none: the interface language's and English, the rule lord-web uses too). An isolated app has no
// lord.spell: its fields are never underlined.
import { useEffect, useState } from 'react';
import { createStore } from './store.js';
import { settings } from './settings.js';
import { locale } from './i18n.js';
import { spellWords, scriptOfTag, unknownWords, badRanges, spellLanguages, spellLocale, DEBOUNCE_MS, BATCH } from './spellmodel.js';

const api = () => (typeof lord !== 'undefined' && lord.spell && typeof lord.spell.check === 'function' ? lord.spell : null);
const CACHE_MAX = 20000;

// bumped whenever verdicts, languages or the person's words change: the fields draw their lines again
export const spellVersion = createStore(0);
const bump = () => spellVersion.set((n) => n + 1);

let langs = [], langsKey = '', scripts = new Set();
let cache = new Map();                       // word → true | false
let personal = new Set();                    // the person's dictionary (enchant's lists, shared with lord-web)
const ignored = new Set();                   // "Ignore": this session, every field
let failedUntil = 0;

export const spellInstalled = () => { try { return api()?.installed() || []; } catch (e) { return []; } };
export const spellDefaults = () => {
  const l = locale.get();
  try { return api()?.defaults(spellLocale(l.lang, l.region)) || []; } catch (e) { return []; }
};
export const spellOn = () => !!api() && (settings.get().spelling || {}).on !== false;
export const spellLangs = () => langs;

// the languages from the settings, told to lord-server; the cache starts again when they change
function sync() {
  const a = api();
  if (!a) return;
  const s = settings.get().spelling || {};
  const next = spellLanguages(s.langs, spellInstalled(), spellDefaults());
  const key = next.join(',');
  if (key === langsKey) return;
  langs = next;
  langsKey = key;
  scripts = new Set(next.map(scriptOfTag));
  cache = new Map();
  pending.clear();
  try { a.setLanguages(next); } catch (e) { /* older server */ }
  loadWords();
  bump();
}
function loadWords() {
  try { personal = new Set(api()?.words() || []); } catch (e) { personal = new Set(); }
}
let started = false;
function start() {
  if (started || !api()) return;
  started = true;
  sync();
  settings.subscribe(sync);
  locale.subscribe(sync);
}

const verdict = (w) => (ignored.has(w) ? true : cache.get(w));
const personalAll = () => (ignored.size ? new Set([...personal, ...ignored]) : personal);

// ---- the batches ----

const pending = new Set();
let timer = 0, inFlight = false;
function schedule() {
  if (timer || inFlight) return;
  timer = setTimeout(flush, DEBOUNCE_MS);
}
function flush() {
  timer = 0;
  const a = api();
  if (!a || !pending.size || Date.now() < failedUntil) return;
  const words = [...pending].slice(0, BATCH);
  let id = 0;
  try { id = a.check(words); } catch (e) { id = 0; }
  if (!(id > 0)) { schedule(); return; }
  inFlight = true;
  const key = langsKey;
  const until = Date.now() + 8000;
  const poll = () => {
    let r = null;
    try { r = a.take(id); } catch (e) { r = { ok: false }; }
    if (!r && Date.now() < until) { setTimeout(poll, 30); return; }
    inFlight = false;
    for (const w of words) pending.delete(w);
    if (key === langsKey && r && r.ok && Array.isArray(r.good) && r.good.length === words.length) {
      if (cache.size > CACHE_MAX) cache = new Map();
      words.forEach((w, i) => cache.set(w, r.good[i] === 1));
      bump();
    } else if (!r || !r.ok) failedUntil = Date.now() + 30000;          // no dictionary or the helper failed: rest a while
    if (pending.size) schedule();
  };
  setTimeout(poll, 10);
}

// The ranges to underline in a text ([[start, end]] in code points), asking lord-spell about the words not known yet
// (after a pause in typing). caret: the field's caret while it has the keys (the word being typed isn't underlined).
export function spellRanges(text, caret = -1) {
  start();
  if (!spellOn() || !langs.length || !text) return [];
  const tokens = spellWords(text, scripts);
  const want = unknownWords(tokens, verdict, BATCH * 4);
  if (want.length) { for (const w of want) pending.add(w); schedule(); }
  return badRanges(tokens, verdict, { personal: personalAll(), caret });
}

// useSpellRanges(text, enabled, caret): the same for a component, drawn again when the verdicts come
export function useSpellRanges(text, enabled, caret = -1) {
  const [, redraw] = useState(0);
  useEffect(() => (enabled ? spellVersion.subscribe(() => redraw((n) => n + 1)) : undefined), [enabled]);
  return enabled ? spellRanges(text, caret) : [];
}

// A word's suggestions: a promise of [strings] (empty when there are none or the helper isn't there)
export function spellSuggest(word) {
  const a = api();
  return new Promise((resolve) => {
    let id = 0;
    try { id = a ? a.suggest(word) : 0; } catch (e) { id = 0; }
    if (!(id > 0)) { resolve([]); return; }
    const until = Date.now() + 6000;
    const poll = () => {
      let r = null;
      try { r = a.take(id); } catch (e) { r = { ok: false }; }
      if (!r && Date.now() < until) { setTimeout(poll, 30); return; }
      resolve(r && r.ok && Array.isArray(r.suggestions) ? r.suggestions.filter((s) => typeof s === 'string') : []);
    };
    setTimeout(poll, 10);
  });
}

// "Add to dictionary": into the person's lists (every language in use, as lord-web's WebKit does)
export function spellAddWord(word) {
  try { api()?.addWord(word); } catch (e) { return false; }
  loadWords();
  personal.add(word);
  bump();
  return true;
}
export function spellRemoveWord(word) {
  try { api()?.removeWord(word); } catch (e) { return false; }
  loadWords();
  bump();
  return true;
}
export const spellWordsList = () => { start(); loadWords(); return [...personal]; };
// "Ignore": good in every field until the session ends
export function spellIgnore(word) { ignored.add(word); bump(); }
