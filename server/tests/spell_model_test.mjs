// spell_model_test.mjs — the spell checker's pure part (sdk/spellmodel.js, docs/SPELLING.md): which words are checked
// (links, addresses, paths, code, abbreviations, digits and scripts without a dictionary left alone), the underlined
// ranges (the person's words, Ignore, the word being typed), the batches, the wavy lines' boxes per row, the menu's rows,
// the languages and the packages Settings may install. node server/tests/spell_model_test.mjs
import {
  spellWords, checkable, scriptOf, scriptOfTag, personalGood, badRanges, unknownWords, rangeAt, underlines, spellMenuItems, spellLanguages,
  spellLocale, toUtf16, SPELL_PACKAGES, spellPackageOk, packageInstalled, toggleLang, WORD_MAX, BATCH,
} from '../../sdk/spellmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const words = (t, s) => spellWords(t, s).map((w) => w.word);

// ---- which words ----
eq('plain words', words('Hello wrold, this is a tset.'), ['Hello', 'wrold', 'this', 'is', 'tset']);
// (ws170 QA MEJORA-2) a markdown link's target and file names with an extension are no words; the link's text is
eq('a link target and a file name', words('Ver [📎 nota.txt](nota.txt) y [la guía](docs/guia-uso.md) mañana'), ['Ver', 'la', 'guía', 'mañana']);
eq('a file name alone, positions kept after it', spellWords('abrí informe-final.pdf hoy').map((w) => [w.word, w.start]), [['abrí', 0], ['hoy', 23]]);
eq('a sentence ending in a dot still checked', words('Fin. Otra frase.'), ['Fin', 'Otra', 'frase']);
eq('positions are code points', spellWords('😀 cañón x').map((w) => [w.start, w.end]), [[2, 7]]);
eq('apostrophes and hyphens inside a word', words("don't d’accord e-mail -dash- 'quoted'"), ["don't", 'd’accord', 'e-mail', 'dash', 'quoted']);
eq('links, addresses and paths left alone', words('see https://lordos.org/x www.example.com me@mail.com /usr/share ~/Notes lordos.org now'), ['see', 'now']);
eq('code left alone', words('snake_case camelCase iPhone x=1 <div> $HOME #tag ok'), ['ok']);
eq('digits next to a word', words('R2D2 3rd v2beta 2025 fine'), ['fine']);
eq('abbreviations in capitals and one letter', words('NASA PDF a I OK Yes'), ['Yes']);
eq('a word too long for lord-spell', checkable('a'.repeat(WORD_MAX + 1)), false);
eq('multibyte length counted in bytes', checkable('ñ'.repeat(33)), false);
eq('scripts', [scriptOf('hola'), scriptOf('привет'), scriptOf('مرحبا'), scriptOf('שלום'), scriptOf('你好'), scriptOf('สวัสดี')], ['latin', 'cyrillic', 'arabic', 'hebrew', 'other', 'thai']);
eq('a tag\'s script', [scriptOfTag('en_US'), scriptOfTag('ru_RU'), scriptOfTag('ar'), scriptOfTag('es_AR')], ['latin', 'cyrillic', 'arabic', 'latin']);
const latin = new Set(['latin']);
eq('no dictionary for a script: not checked (right to left stays clean)', words('hola مرحبا שלום мир 你好 world', latin), ['hola', 'world']);
eq('Arabic checked when an Arabic dictionary is in use', words('مرحبا hola', new Set(['arabic'])), ['مرحبا']);

// ---- ranges ----
const known = { wrold: false, tset: false, Hello: true, this: true, is: true, lumo: false, Lumo: false, LordOS: false };
const verdict = (w) => known[w];
const text = 'Hello wrold, this is a tset';
const tk = spellWords(text);
eq('the bad ranges', badRanges(tk, verdict), [[6, 11], [23, 27]]);
eq('the word being typed is not underlined', badRanges(tk, verdict, { caret: 27 }), [[6, 11]]);
eq('the person\'s words are good', badRanges(tk, verdict, { personal: new Set(['wrold']) }), [[23, 27]]);
eq('a lower-case word is good capitalised', personalGood('Lumo', new Set(['lumo'])), true);
eq('a capitalised word (a name) stays capitalised', personalGood('lumo', new Set(['Lumo'])), false);
eq('ALL CAPS of a lower-case word is not its capitalisation', personalGood('LUMO', new Set(['lumo'])), false);
eq('unknown words once each', unknownWords(spellWords('one two one three'), () => undefined), ['one', 'two', 'three']);
eq('unknown words capped', unknownWords(spellWords(Array.from({ length: 100 }, (_, i) => `w${'abcdefghij'[i % 10]}${'abcdefghij'[Math.floor(i / 10)]}`).join(' ')), () => undefined).length, BATCH);
eq('known words not asked again', unknownWords(tk, verdict), ['a'].filter(() => false));
eq('a right click in a range', rangeAt([[6, 11]], 8), [6, 11]);
eq('a right click at its end', rangeAt([[6, 11]], 11), [6, 11]);
eq('a right click elsewhere', rangeAt([[6, 11]], 3), null);
eq('UTF-16 offsets for editors', toUtf16('😀 tset', [[2, 6]]), [[3, 7]]);

// ---- the wavy lines of a row ----
const rects = (a, b) => [{ x: a * 7, w: (b - a) * 7 }];
eq('a range inside the row', underlines([[6, 11]], 0, 20, rects), [{ x: 42, w: 35 }]);
eq('a range cut to the row (wrapped)', underlines([[6, 11]], 8, 20, rects), [{ x: 0, w: 21 }]);
eq('a range in another row', underlines([[6, 11]], 12, 20, rects), []);
eq('a range in right-to-left text: several boxes', underlines([[0, 4]], 0, 4, () => [{ x: 30, w: 10 }, { x: 0, w: 12 }]), [{ x: 30, w: 10 }, { x: 0, w: 12 }]);

// ---- the menu ----
const tr = (k, v) => (v ? `${k}:${JSON.stringify(v)}` : k);
let got = null;
const rows = spellMenuItems({ word: 'wrold', suggestions: ['world', 'would', 'wold', 'w', 'x', 'y', 'z', 'q', 'r'], tr, onReplace: (s) => { got = s; }, onAdd: () => {}, onIgnore: () => {} });
eq('suggestions first, at most eight, then Add and Ignore', rows.map((r) => (r === '-' ? '-' : r.label)),
  ['world', 'would', 'wold', 'w', 'x', 'y', 'z', 'q', '-', 'spell.menu.add:{"word":"wrold"}', 'spell.menu.ignore', '-']);
rows[0].onClick();
eq('a suggestion replaces the word', got, 'world');
eq('while suggestions come', spellMenuItems({ word: 'x', loading: true, tr, onReplace() {}, onAdd() {}, onIgnore() {} })[0], { label: 'spell.menu.looking', disabled: true });
eq('no suggestions', spellMenuItems({ word: 'x', suggestions: [], tr, onReplace() {}, onAdd() {}, onIgnore() {} })[0], { label: 'spell.menu.none', disabled: true });
eq('no word: no rows', spellMenuItems({ word: null, tr }), []);

// ---- languages and packages ----
eq('defaults when nothing chosen', spellLanguages([], ['en_US', 'es_ES'], ['es_ES', 'en_US']), ['es_ES', 'en_US']);
eq('the chosen ones, installed only, at most four', spellLanguages(['de_DE', 'es_ES', 'fr_FR', 'en_US', 'pt_BR', 'it_IT'], ['de_DE', 'es_ES', 'fr_FR', 'en_US', 'pt_BR', 'it_IT'], []), ['de_DE', 'es_ES', 'fr_FR', 'en_US']);
eq('a chosen one removed from the system is left out', spellLanguages(['xx_XX', 'es_ES'], ['es_ES'], ['en_US']), ['es_ES']);
eq('spellang\'s locale', [spellLocale('es', 'AR'), spellLocale('pt', 'pt-BR'), spellLocale('zh-Hans', ''), spellLocale('en', null)], ['es_AR', 'pt_BR', 'zh', 'en']);
eq('toggle on, at most four', toggleLang(['a', 'b', 'c', 'd'], 'e', true), ['a', 'b', 'c', 'd']);
eq('toggle off', toggleLang(['a', 'b'], 'a', false), ['b']);
eq('the images\' three dictionaries are in the list', ['hunspell-en-us', 'hunspell-es', 'hunspell-pt-br'].every(spellPackageOk), true);
eq('only Debian\'s hunspell packages, nothing else', [spellPackageOk('hunspell-es; rm -rf /'), spellPackageOk('bash'), SPELL_PACKAGES.every((p) => /^hunspell-[a-z-]+$/.test(p.pkg))], [false, false, true]);
eq('installed by tag', [packageInstalled(SPELL_PACKAGES.find((p) => p.pkg === 'hunspell-es'), ['es_AR', 'en_US']), packageInstalled(SPELL_PACKAGES.find((p) => p.pkg === 'hunspell-pt-pt'), ['pt_BR'])], [true, false]);

console.log(`spell-model-test: ${ok} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
