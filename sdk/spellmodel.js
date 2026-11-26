// spellmodel.js — the spell checker's pure part (docs/SPELLING.md; tested by server/tests/spell_model_test.mjs): which
// words of a text are checked, which are underlined, where the wavy lines go, and the right-click menu's rows.
//
// Positions are code points (the text fields' own unit, textfieldmodel.js). A word is a run of letters and marks with
// apostrophes or hyphens inside it; it is checked only when a chosen dictionary is written in its script (no
// dictionary for Arabic or Thai in use: nothing of them is underlined) and it isn't something spell checkers leave
// alone: a link, an e-mail address, a path, code (snake_case, camelCase, a word with digits), an abbreviation in
// capitals, one letter, or a word longer than WORD_MAX bytes. The person's dictionary and "Ignore" make a word good.

export const WORD_MAX = 64;                 // bytes of UTF-8, as lord-spell takes (spellproto.h SPELL_WORD_MAX)
export const BATCH = 64;                    // words in one lord.spell.check
export const DEBOUNCE_MS = 350;             // after the last key, before the new words are asked about
export const SUGGEST_MAX = 8;

const utf8Len = (s) => { let n = 0; for (const ch of s) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; } return n; };

// the script a dictionary is written in, from its tag's language (en_US → latin); anything not listed is Latin
const SCRIPT_OF_LANG = {
  ru: 'cyrillic', uk: 'cyrillic', bg: 'cyrillic', be: 'cyrillic', sr: 'cyrillic', mk: 'cyrillic', kk: 'cyrillic', mn: 'cyrillic',
  el: 'greek', ar: 'arabic', fa: 'arabic', ur: 'arabic', he: 'hebrew', hi: 'devanagari', ne: 'devanagari', mr: 'devanagari',
  bn: 'bengali', th: 'thai', ko: 'hangul', gu: 'gujarati', te: 'telugu', ml: 'malayalam', si: 'sinhala', lo: 'lao', bo: 'tibetan',
  dz: 'tibetan',
};
export const scriptOfTag = (tag) => SCRIPT_OF_LANG[String(tag || '').split(/[_-]/)[0]] || 'latin';

const SCRIPTS = [
  ['latin', /\p{Script=Latin}/u], ['cyrillic', /\p{Script=Cyrillic}/u], ['greek', /\p{Script=Greek}/u], ['arabic', /\p{Script=Arabic}/u],
  ['hebrew', /\p{Script=Hebrew}/u], ['devanagari', /\p{Script=Devanagari}/u], ['bengali', /\p{Script=Bengali}/u], ['thai', /\p{Script=Thai}/u],
  ['hangul', /\p{Script=Hangul}/u], ['gujarati', /\p{Script=Gujarati}/u], ['telugu', /\p{Script=Telugu}/u], ['malayalam', /\p{Script=Malayalam}/u],
  ['sinhala', /\p{Script=Sinhala}/u], ['lao', /\p{Script=Lao}/u], ['tibetan', /\p{Script=Tibetan}/u],
];
// the script of a word (its first letter's), or 'other' (Han, Kana… for which there is no dictionary)
export function scriptOf(word) {
  const first = [...word].find((ch) => /\p{L}/u.test(ch)) || '';
  for (const [name, re] of SCRIPTS) if (re.test(first)) return name;
  return 'other';
}

// a chunk (between spaces) that is a link, an address, a path or code: none of its words are checked
const SKIP_CHUNK = /:\/\/|^www\.|@|^[~/]|[\\_{}<>=#$%^*|]|\.(com|org|net|io|dev|gov|edu|[a-z]{2})(\/|$)/i;

// The words of a text: [{ start, end, word }] in code points, checkable ones only. scripts: the scripts of the
// dictionaries in use (a Set; null checks every script).
export function spellWords(text, scripts = null) {
  const out = [];
  if (!text) return out;
  const cps = [...text];
  let i = 0;
  const isSpace = (ch) => /\s/u.test(ch);
  while (i < cps.length) {
    while (i < cps.length && isSpace(cps[i])) i++;
    const c0 = i;
    while (i < cps.length && !isSpace(cps[i])) i++;
    const chunkCps = cps.slice(c0, i), chunk = chunkCps.join('');
    if (!chunk || SKIP_CHUNK.test(chunk)) continue;
    // the words of the chunk: letters and marks, joined by ' ’ or - between letters
    let j = 0;
    while (j < chunkCps.length) {
      if (!/[\p{L}\p{M}]/u.test(chunkCps[j])) { j++; continue; }
      const s = j;
      while (j < chunkCps.length) {
        if (/[\p{L}\p{M}]/u.test(chunkCps[j])) { j++; continue; }
        if (/['’-]/u.test(chunkCps[j]) && j + 1 < chunkCps.length && /\p{L}/u.test(chunkCps[j + 1])) { j++; continue; }
        break;
      }
      const word = chunkCps.slice(s, j).join('');
      const before = s > 0 ? chunkCps[s - 1] : '', after = j < chunkCps.length ? chunkCps[j] : '';
      if (/\p{N}/u.test(before) || /\p{N}/u.test(after)) continue;                     // R2D2, 3rd, v2beta
      if (checkable(word) && (!scripts || scripts.has(scriptOf(word)))) out.push({ start: c0 + s, end: c0 + j, word });
    }
  }
  return out;
}

// a word a spell checker should look at
export function checkable(word) {
  const n = [...word].length;
  if (n < 2 || utf8Len(word) > WORD_MAX) return false;
  if (/^\p{Lu}+$/u.test(word.replace(/[\p{M}'’-]/gu, ''))) return false;            // NASA, PDF
  if (/\p{Ll}\p{Lu}/u.test(word)) return false;                                     // camelCase, iPhone
  return true;
}

// is the word in the person's dictionary or ignored: exactly, or in lower case when it starts a sentence (a word kept
// in lower case is good capitalised; one kept capitalised is a name and stays so)
export function personalGood(word, words) {
  if (!words || !words.size) return false;
  if (words.has(word)) return true;
  const lower = word.toLowerCase();
  return lower !== word && words.has(lower) && word === lower[0].toUpperCase() + lower.slice(1);
}

// The ranges to underline: the words the cache says are wrong (false), not the person's, and not the word being typed
// (the one that ends at the caret while the field has the keys). verdict(word) → true | false | undefined (not known).
export function badRanges(tokens, verdict, { personal = null, caret = -1 } = {}) {
  const out = [];
  for (const tk of tokens) {
    if (caret >= 0 && tk.end === caret) continue;
    if (verdict(tk.word) !== false || personalGood(tk.word, personal)) continue;
    out.push([tk.start, tk.end]);
  }
  return out;
}

// the words not known yet, once each, at most max (a batch for lord.spell.check)
export function unknownWords(tokens, verdict, max = BATCH) {
  const seen = new Set(), out = [];
  for (const tk of tokens) {
    if (out.length >= max) break;
    if (seen.has(tk.word) || verdict(tk.word) !== undefined) continue;
    seen.add(tk.word);
    out.push(tk.word);
  }
  return out;
}

// the bad range that holds a position (a right click's), or null
export const rangeAt = (ranges, pos) => (ranges || []).find(([a, b]) => pos >= a && pos <= b) || null;

// The wavy lines of a row of text: [{ x, w }] from the row's ranges, with rects(a, b) giving the boxes of a range as
// drawn ([{ x, w }], textfieldmodel.js selRects); ranges outside the row are cut to it.
export function underlines(ranges, rowStart, rowEnd, rects) {
  const out = [];
  for (const [a, b] of ranges || []) {
    const s = Math.max(a, rowStart), e = Math.min(b, rowEnd);
    if (e <= s) continue;
    for (const r of rects(s - rowStart, e - rowStart)) if (r.w > 0.5) out.push({ x: r.x, w: r.w });
  }
  return out;
}

// The spelling rows of a text field's menu, first in it: the suggestions (or a row saying they are on their way, or
// that there are none), "Add to dictionary", "Ignore", then a separator. tr(key) translates.
export function spellMenuItems({ word, suggestions, loading, tr, onReplace, onAdd, onIgnore }) {
  if (!word) return [];
  const rows = [];
  if (loading) rows.push({ label: tr('spell.menu.looking'), disabled: true });
  else if (!suggestions || !suggestions.length) rows.push({ label: tr('spell.menu.none'), disabled: true });
  else for (const s of suggestions.slice(0, SUGGEST_MAX)) rows.push({ label: s, bold: true, onClick: () => onReplace(s) });
  rows.push('-');
  rows.push({ label: tr('spell.menu.add', { word }), onClick: () => onAdd(word) });
  rows.push({ label: tr('spell.menu.ignore'), onClick: () => onIgnore(word) });
  rows.push('-');
  return rows;
}

// The dictionaries to check with when the person chose none: lord-spell's defaults (spellang.h: the interface
// language with its region first, then English, installed ones only). chosen: the person's list (Settings → Keyboard),
// kept to installed ones; at most 4.
export function spellLanguages(chosen, installed, defaults) {
  const have = new Set(installed || []);
  const list = Array.isArray(chosen) && chosen.length ? chosen : defaults || [];
  return [...new Set(list.filter((t) => have.has(t)))].slice(0, 4);
}

// A locale for spellang ("es_AR"): the interface language and the region ("AR", "es-AR", "es_AR" or none).
export function spellLocale(lang, region) {
  const l = String(lang || 'en').split(/[-_]/)[0];
  const m = /([A-Z]{2})$/.exec(String(region || ''));
  return m ? `${l}_${m[1]}` : l;
}

// Code-point ranges as UTF-16 offsets (the unit of editors that index strings directly: Notes, Mail's body)
export function toUtf16(text, ranges) {
  if (!ranges || !ranges.length) return [];
  const at = [0];
  for (const ch of String(text || '')) at.push(at[at.length - 1] + ch.length);
  return ranges.map(([a, b]) => [at[Math.min(a, at.length - 1)], at[Math.min(b, at.length - 1)]]);
}

// Debian 13's dictionaries Settings → Keyboard offers to install (pkg-install, an administrator; only these names):
// [package, interface language, what its installed tags start with, installed size in KiB (0: a metapackage)]
export const SPELL_PACKAGES = [
  ['hunspell-en-us', 'en', 'en_US', 1565], ['hunspell-en-gb', 'en', 'en_GB', 2638], ['hunspell-es', 'es', 'es', 2053],
  ['hunspell-pt-br', 'pt', 'pt_BR', 9642], ['hunspell-pt-pt', 'pt', 'pt_PT', 2406], ['hunspell-fr', 'fr', 'fr', 0],
  ['hunspell-de-de', 'de', 'de_DE', 1928], ['hunspell-it', 'it', 'it', 2703], ['hunspell-nl', 'nl', 'nl', 4399],
  ['hunspell-pl', 'pl', 'pl', 8341], ['hunspell-ru', 'ru', 'ru', 6140], ['hunspell-uk', 'uk', 'uk', 14266],
  ['hunspell-tr', 'tr', 'tr', 71198], ['hunspell-ar', 'ar', 'ar', 10766], ['hunspell-bg', 'bg', 'bg', 3043],
  ['hunspell-bn', 'bn', 'bn', 4194], ['hunspell-ca', 'ca', 'ca', 10158], ['hunspell-el', 'el', 'el', 19104],
  ['hunspell-hi', 'hi', 'hi', 943], ['hunspell-hu', 'hu', 'hu', 7458], ['hunspell-id', 'id', 'id', 1311],
  ['hunspell-lt', 'lt', 'lt', 2512], ['hunspell-ro', 'ro', 'ro', 4256], ['hunspell-sv', 'sv', 'sv', 8208],
  ['hunspell-sw', 'sw', 'sw', 1602], ['hunspell-th', 'th', 'th', 2495], ['hunspell-vi', 'vi', 'vi', 505],
  ['hunspell-ko', 'ko', 'ko', 25796],
].map(([pkg, lang, match, kib]) => ({ pkg, lang, match, bytes: kib * 1024 }));
export const spellPackageOk = (name) => SPELL_PACKAGES.some((p) => p.pkg === name);
// whether a package's dictionaries are among the installed tags
export const packageInstalled = (p, installed) => (installed || []).some((t) => t === p.match || t.startsWith(`${p.match}_`) || (p.match.includes('_') && t === p.match));
// the person's choice after ticking or unticking a tag: from the list in use, at most 4, never empty (none = defaults)
export function toggleLang(current, tag, on) {
  const l = (current || []).filter((t) => t !== tag);
  return (on ? [...l, tag] : l).slice(0, 4);
}
