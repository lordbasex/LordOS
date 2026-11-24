// voicecatalog.js — the voices and models LordOS offers for reading aloud (docs/VOICE.md, phase 3), and the names of
// the packages that carry them. Pure data: Settings → AI → Voice (apps/settings/AI.jsx) and the package builder
// (tools/voice/mkdeb-voice.mjs) read it, so the two never disagree. Tested by server/tests/voicecatalog_test.mjs.
//
// Every voice is Kyutai's (Pocket TTS, CC-BY-4.0), prepared from a recording whose own license allows redistribution
// and commercial use (https://huggingface.co/kyutai/tts-voices): VCTK (CC-BY-4.0), voice-zero (CC0), Alba McKenna
// (CC-BY-4.0), or Kyutai's own voices shipped with the model. Left out on purpose: jean (EARS: non-commercial only),
// cosette, javert and marius (their recordings' origin isn't documented).
//
//   VOICE_LANGS                    [{ id: 'es', hd: 'spanish_24l', standard: 'spanish', own: 'lola', mb, sample }, …]
//                                  standardDir: the standard model is Kyutai's own folder (6 layers), as audio.cpp
//                                  publishes no GGUF of it (Dutch): models/<lang>/standard/
//   VOICES                         [{ id: 'lola', gender: 'f', source: 'kyutai' }, …]   every voice speaks every language;
//                                  hdOnly: only the HD models carry it, but in the standardIn languages
//   hasVoice(voice, hd, lang)      whether that quality (of that language) offers the voice
//   modelPackage(lang, hd)         'lordos-voice-es' | 'lordos-voice-es-hd'
//   voicePackage(lang, voice, hd)  'lordos-voice-es-lola' | 'lordos-voice-es-lola-hd'
//   PREVIEWS_PACKAGE               'lordos-voice-previews': a recorded sample of every voice in every language
//   voiceLabel(id)                 'Bill Boerst' from 'bill_boerst' (a proper name: not translated)

// sample: what a voice reads to be tried, and what its recorded preview says (tools/voice/make-previews.sh); in the
// language itself, not the interface's. mb: download sizes, rounded up. Each language's own voice is Kyutai's voice of
// that language (Lola, Rafael, Estelle, Juergen, Giovanni, Daan); English's is Alba.
export const VOICE_LANGS = [
  { id: 'es', hd: 'spanish_24l', standard: 'spanish', own: 'lola', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Hola, soy una de las voces de LordOS. Puedo leerte en voz alta cualquier texto que selecciones.' },
  { id: 'en', hd: 'english_2026-09_24l', standard: 'english', own: 'alba', mb: { standard: 128, hd: 1304, voice: 7, voiceHd: 32 },
    sample: 'Hello, I am one of the LordOS voices. I can read aloud any text you select.' },
  { id: 'pt', hd: 'portuguese_24l', standard: 'portuguese', own: 'rafael', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Olá, eu sou uma das vozes do LordOS. Posso ler em voz alta qualquer texto que você selecionar.' },
  { id: 'fr', hd: 'french_24l', standard: 'french', own: 'estelle', mb: { standard: 129, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Bonjour, je suis l\'une des voix de LordOS. Je peux lire à voix haute tout texte que vous sélectionnez.' },
  { id: 'de', hd: 'german_24l', standard: 'german', own: 'juergen', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Hallo, ich bin eine der Stimmen von LordOS. Ich kann jeden Text, den du auswählst, laut vorlesen.' },
  { id: 'it', hd: 'italian_24l', standard: 'italian', own: 'giovanni', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Ciao, sono una delle voci di LordOS. Posso leggere ad alta voce qualsiasi testo che selezioni.' },
  { id: 'nl', hd: 'dutch_24l', standard: 'dutch', standardDir: true, own: 'daan', mb: { standard: 220, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Hallo, ik ben een van de stemmen van LordOS. Ik kan elke tekst die je selecteert hardop voorlezen.' },
];

// gender as Kyutai's demo lists it; source: where the recording comes from (its license above)
export const VOICES = [
  { id: 'lola', gender: 'f', source: 'kyutai' },
  { id: 'juergen', gender: 'm', source: 'kyutai' },
  { id: 'rafael', gender: 'm', source: 'kyutai' },
  { id: 'estelle', gender: 'f', source: 'kyutai' },
  { id: 'giovanni', gender: 'm', source: 'kyutai' },
  // not among the standard models' voices, but French's (audio.cpp's GGUF) and Dutch's (Kyutai's own)
  { id: 'daan', gender: 'm', source: 'kyutai', hdOnly: true, standardIn: ['fr', 'nl'] },
  { id: 'alba', gender: 'm', source: 'alba-mackenna' },
  { id: 'bill_boerst', gender: 'm', source: 'voice-zero' },
  { id: 'caro_davy', gender: 'f', source: 'voice-zero' },
  { id: 'peter_yearsley', gender: 'm', source: 'voice-zero' },
  { id: 'stuart_bell', gender: 'm', source: 'voice-zero' },
  { id: 'jane', gender: 'f', source: 'vctk' },
  { id: 'anna', gender: 'f', source: 'vctk' },
  { id: 'azelma', gender: 'f', source: 'vctk' },
  { id: 'charles', gender: 'm', source: 'vctk' },
  { id: 'eponine', gender: 'f', source: 'vctk' },
  { id: 'eve', gender: 'f', source: 'vctk' },
  { id: 'fantine', gender: 'f', source: 'vctk' },
  { id: 'george', gender: 'm', source: 'vctk' },
  { id: 'mary', gender: 'f', source: 'vctk' },
  { id: 'michael', gender: 'm', source: 'vctk' },
  { id: 'paul', gender: 'm', source: 'vctk' },
  { id: 'vera', gender: 'f', source: 'vctk' },
];

export const SOURCE_LICENSES = {
  kyutai: 'CC-BY-4.0',
  'alba-mackenna': 'CC-BY-4.0',
  'voice-zero': 'CC0-1.0',
  vctk: 'CC-BY-4.0',
};

export const PREVIEWS_PACKAGE = 'lordos-voice-previews';

const LANG_IDS = new Set(VOICE_LANGS.map((l) => l.id));
const VOICE_IDS = new Set(VOICES.map((v) => v.id));
const pkgPart = (s) => String(s).replace(/_/g, '-');        // Debian package names have no underscores

export function modelPackage(lang, hd) {
  if (!LANG_IDS.has(lang)) return null;
  return `lordos-voice-${lang}${hd ? '-hd' : ''}`;
}

export function voicePackage(lang, voice, hd) {
  if (!LANG_IDS.has(lang) || !VOICE_IDS.has(voice) || !hasVoice(voice, hd, lang)) return null;
  return `lordos-voice-${lang}-${pkgPart(voice)}${hd ? '-hd' : ''}`;
}

export const voiceLabel = (id) => String(id).split('_').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
export const hasVoice = (voice, hd, lang) =>
  VOICES.some((v) => v.id === voice && (hd || !v.hdOnly || (v.standardIn || []).includes(lang)));
export const langOf = (id) => VOICE_LANGS.find((l) => l.id === id) || null;
