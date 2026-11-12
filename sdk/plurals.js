// plurals.js — plural rules for the interface catalogs (CLDR 45, simplified to integers).
//
// A language names its rule in meta.json ("plural": "east-slavic"); a text with plurals is an object
// keyed by the categories its rule gives ({ "one": "…", "few": "…", "many": "…", "other": "…" }).
// sdk/i18n.js picks the form with the rule of the language the text came from, falling back to
// "other". Pure: no lord.*; tests in server/tests/plurals_test.mjs, docs/LANGUAGES.md.
export const PLURALS = {
  'one-other': (n) => (n === 1 ? 'one' : 'other'),                   // en, es, it, de, nl, sv, el, tr, hu, fi, bg, az, ur
  'zero-one-other': (n) => (n === 0 || n === 1 ? 'one' : 'other'),   // pt, fr, hi, bn, fa
  none: () => 'other',                                               // ja, zh, ko, th, vi, id, ms
  'east-slavic': (n) => {                                            // ru, uk, be
    const m10 = n % 10, m100 = n % 100;
    if (m10 === 1 && m100 !== 11) return 'one';
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few';
    return 'many';
  },
  polish: (n) => {
    const m10 = n % 10, m100 = n % 100;
    if (n === 1) return 'one';
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few';
    return 'many';
  },
  'czech-slovak': (n) => (n === 1 ? 'one' : n >= 2 && n <= 4 ? 'few' : 'other'),
  lithuanian: (n) => {                                               // lt: 1, 21 vienas; 2–9, 22 keli; 0, 10–20 kitas
    const m10 = n % 10, m100 = n % 100;
    if (m100 >= 11 && m100 <= 19) return 'other';
    if (m10 === 1) return 'one';
    if (m10 >= 2) return 'few';
    return 'other';
  },
  romanian: (n) => {                                                 // ro: 1 → one; 0, 2–19, 101–119 → few; 20, 100 → other
    const m100 = n % 100;
    if (n === 1) return 'one';
    if (n === 0 || (m100 >= 1 && m100 <= 19)) return 'few';
    return 'other';
  },
  arabic: (n) => {                                                   // ar: zero, one, two, few (3–10), many (11–99), other (100, 101…)
    const m100 = n % 100;
    if (n === 0) return 'zero';
    if (n === 1) return 'one';
    if (n === 2) return 'two';
    if (m100 >= 3 && m100 <= 10) return 'few';
    if (m100 >= 11) return 'many';
    return 'other';
  },
};

// The categories a rule gives (what a translated plural object should have), in CLDR's order.
const ORDER = ['zero', 'one', 'two', 'few', 'many', 'other'];
export function pluralCategories(rule) {
  const f = PLURALS[rule];
  if (!f) return null;
  const seen = new Set();
  for (let n = 0; n < 1000; n++) seen.add(f(n));
  return ORDER.filter((c) => seen.has(c));
}
