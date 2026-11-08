// voicecatalog.js — the voices and models LordOS offers for reading aloud (docs/VOICE.md, phase 3), and the names of
// the packages that carry them. Pure data: Settings → AI → Voice (apps/settings/AI.jsx) and the package builder
// (tools/voice/mkdeb-voice.mjs) read it, so the two never disagree. Tested by server/tests/voicecatalog_test.mjs.
//
// Every voice is Kyutai's (Pocket TTS, CC-BY-4.0), prepared from a recording whose own license allows redistribution
// and commercial use (https://huggingface.co/kyutai/tts-voices): VCTK (CC-BY-4.0), voice-zero (CC0), Alba McKenna
// (CC-BY-4.0), or Kyutai's own voices shipped with the model. Left out on purpose: jean (EARS: non-commercial only),
// cosette, javert and marius (their recordings' origin isn't documented).
//
//   VOICE_LANGS                    [{ id: 'es', hd: 'spanish_24l', own: 'lola', mb, sample }, …]
//   VOICES                         [{ id: 'lola', gender: 'f', source: 'kyutai' }, …]   every voice speaks every language;
//                                  hdOnly: only the HD models carry it
//   hasVoice(voice, hd)            whether that quality offers the voice
//   modelPackage(lang, hd)         'lordos-voice-es' | 'lordos-voice-es-hd'
//   voicePackage(lang, voice, hd)  'lordos-voice-es-lola' | 'lordos-voice-es-lola-hd'
//   PREVIEWS_PACKAGE               'lordos-voice-previews': a recorded sample of every voice in every language
//   voiceLabel(id)                 'Bill Boerst' from 'bill_boerst' (a proper name: not translated)

// sample: what a voice reads to be tried, and what its recorded preview says (tools/voice/make-previews.sh); in the
// language itself, not the interface's. mb: download sizes, rounded up
export const VOICE_LANGS = [
  { id: 'es', hd: 'spanish_24l', standard: 'spanish', own: 'lola', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Hola, soy una de las voces de LordOS. Puedo leerte en voz alta cualquier texto que selecciones.' },
  { id: 'en', hd: 'english_2026-09_24l', standard: 'english', own: 'alba', mb: { standard: 128, hd: 1304, voice: 7, voiceHd: 32 },
    sample: 'Hello, I am one of the LordOS voices. I can read aloud any text you select.' },
  { id: 'pt', hd: 'portuguese_24l', standard: 'portuguese', own: 'rafael', mb: { standard: 128, hd: 672, voice: 7, voiceHd: 32 },
    sample: 'Olá, eu sou uma das vozes do LordOS. Posso ler em voz alta qualquer texto que você selecionar.' },
];

// gender as Kyutai's demo lists it; source: where the recording comes from (its license above)
export const VOICES = [
  { id: 'lola', gender: 'f', source: 'kyutai' },
  { id: 'juergen', gender: 'm', source: 'kyutai' },
  { id: 'rafael', gender: 'm', source: 'kyutai' },
  { id: 'estelle', gender: 'f', source: 'kyutai' },
  { id: 'giovanni', gender: 'm', source: 'kyutai' },
  { id: 'daan', gender: 'm', source: 'kyutai', hdOnly: true },     // not among the standard models' voices
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
  if (!LANG_IDS.has(lang) || !VOICE_IDS.has(voice) || !hasVoice(voice, hd)) return null;
  return `lordos-voice-${lang}-${pkgPart(voice)}${hd ? '-hd' : ''}`;
}

export const voiceLabel = (id) => String(id).split('_').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
export const hasVoice = (voice, hd) => VOICES.some((v) => v.id === voice && (hd || !v.hdOnly));
export const langOf = (id) => VOICE_LANGS.find((l) => l.id === id) || null;
