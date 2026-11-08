// ocr.js — Screenshots' "copy the text" (media.ocr, the lord-ocr helper), the pure part
// (server/tests/screenshots_model_test.mjs): which Tesseract languages to read with, and how the text is cleaned.

// The interface's languages (settings.languages, "es", "pt-BR"…) as Tesseract's data names, first (they win where
// two read a word differently), then the ones LordOS installs (English: code, commands and most of a screen; and
// Spanish, Portuguese, French and Italian, so "año" keeps its accent with an English interface; a missing one is
// skipped by Tesseract), each once: ['pt'] → 'por+eng+spa+fra+ita'. Five cost ~0.3 s a selection.
const TESS = { en: 'eng', es: 'spa', pt: 'por', fr: 'fra', it: 'ita', de: 'deu', ca: 'cat', nl: 'nld' };
const SHIPPED = ['en', 'es', 'pt', 'fr', 'it'];
export function ocrLangs(languages) {
  const out = [];
  for (const l of [...(Array.isArray(languages) ? languages : []), ...SHIPPED]) {
    const t = TESS[String(l || '').toLowerCase().split(/[-_]/)[0]];
    if (t && !out.includes(t)) out.push(t);
  }
  return out.join('+');
}

// Tesseract's text: no form feed at the end, lines' trailing spaces off, at most one blank line in a row.
export function cleanText(s) {
  return String(s || '').replace(/\f/g, '').split('\n').map((l) => l.replace(/\s+$/, '')).join('\n')
    .replace(/\n{3,}/g, '\n\n').trim();
}
