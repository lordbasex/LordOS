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

// The editor's Transcribe: what a drag from (x1, y1) to (x2, y2) reads, in the picture's pixels, clamped to it; a
// plain click (under 6 pixels either way) reads the whole picture.
export function transcribeRect(x1, y1, x2, y2, iw, ih) {
  if (!(iw >= 1) || !(ih >= 1)) return null;
  const vals = [x1, y1, x2, y2];
  if (!vals.every(Number.isFinite) || (Math.abs(x2 - x1) < 6 && Math.abs(y2 - y1) < 6)) return { x: 0, y: 0, w: Math.round(iw), h: Math.round(ih) };
  const cl = (v, m) => Math.max(0, Math.min(m, Math.round(v)));
  const x0 = cl(Math.min(x1, x2), iw), x3 = cl(Math.max(x1, x2), iw), y0 = cl(Math.min(y1, y2), ih), y3 = cl(Math.max(y1, y2), ih);
  return x3 - x0 < 2 || y3 - y0 < 2 ? null : { x: x0, y: y0, w: x3 - x0, h: y3 - y0 };
}
