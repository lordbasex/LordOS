// voicecatalog_test.mjs — the voices LordOS offers and their package names (sdk/voicecatalog.js).
//
//   node tests/voicecatalog_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const C = await import(path.join(here, '../../sdk/voicecatalog.js'));
const { VOICE_LANGS, VOICES, SOURCE_LICENSES, modelPackage, voicePackage, voiceLabel, hasVoice } = C;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, v) => eq(name, !!v, true);

eq('23 voices', VOICES.length, 23);
eq('no repeated voice', new Set(VOICES.map((v) => v.id)).size, VOICES.length);
ok('left out: jean (non-commercial), cosette, javert, marius', !VOICES.some((v) => ['jean', 'cosette', 'javert', 'marius'].includes(v.id)));
ok('every voice has a license that allows redistribution', VOICES.every((v) => SOURCE_LICENSES[v.source]));
ok('every gender is f or m', VOICES.every((v) => v.gender === 'f' || v.gender === 'm'));
ok('voice ids are plain names (say.c, speak.c)', VOICES.every((v) => /^[a-z0-9_]{1,60}$/.test(v.id)));
ok('each language\'s own voice is in the list', VOICE_LANGS.every((l) => VOICES.some((v) => v.id === l.own)));
eq('own voices', VOICE_LANGS.map((l) => l.own), ['lola', 'alba', 'rafael']);
eq('HD names as lord-tts accepts them', VOICE_LANGS.map((l) => l.hd), ['spanish_24l', 'english_2026-09_24l', 'portuguese_24l']);

eq('model', modelPackage('es', false), 'lordos-voice-es');
eq('model HD', modelPackage('pt', true), 'lordos-voice-pt-hd');
eq('voice', voicePackage('es', 'lola', false), 'lordos-voice-es-lola');
eq('voice HD, underscore', voicePackage('en', 'bill_boerst', true), 'lordos-voice-en-bill-boerst-hd');
eq('unknown language', modelPackage('fr', true), null);
eq('unknown voice', voicePackage('es', 'jean', true), null);
eq('a name can\'t sneak in', voicePackage('es', 'lola lordos-x', false), null);
eq('daan only in HD', [hasVoice('daan', true), hasVoice('daan', false), voicePackage('es', 'daan', false)], [true, false, null]);
eq('22 standard voices', VOICES.filter((v) => hasVoice(v.id, false)).length, 22);
const all = VOICE_LANGS.flatMap((l) => [false, true].flatMap((hd) => [modelPackage(l.id, hd), ...VOICES.filter((v) => hasVoice(v.id, hd)).map((v) => voicePackage(l.id, v.id, hd))]));
ok('every package name is a valid Debian name', all.every((p) => /^[a-z0-9][a-z0-9+.-]+$/.test(p)));
eq('no two packages share a name', new Set(all).size, all.length);
eq('label', voiceLabel('peter_yearsley'), 'Peter Yearsley');

console.log(`voicecatalog_test: ${n - bad}/${n}`);
process.exit(bad ? 1 : 0);
