// voicerec_model_test.mjs — the recordings of a personal voice (sdk/voicerecmodel.js): reading and writing WAV,
// the room's noise, trimming silence, joining phrases, telling when a phrase was read.
//
//   node tests/voicerec_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/voicerecmodel.js'));
const { parseWav, levelDb, quietEnough, trimSilence, joinPhrases, phraseDone, MAX_MS, END_QUIET_MS } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => { n++; const g = JSON.stringify(got), w = JSON.stringify(want); if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); } };
const ok = (name, v) => eq(name, !!v, true);

const R = 48000;
const tone = (ms, amp) => { const a = new Int16Array(Math.floor((R * ms) / 1000)); for (let i = 0; i < a.length; i++) a[i] = Math.round(amp * 32767 * Math.sin((2 * Math.PI * 220 * i) / R)); return a; };
const noise = (ms, amp) => { const a = new Int16Array(Math.floor((R * ms) / 1000)); let s = 7; for (let i = 0; i < a.length; i++) { s = (s * 1103515245 + 12345) >>> 0; a[i] = Math.round(((s / 4294967296) - 0.5) * 2 * amp * 32767); } return a; };
const cat = (...xs) => { const out = new Int16Array(xs.reduce((s, x) => s + x.length, 0)); let at = 0; for (const x of xs) { out.set(x, at); at += x.length; } return out; };

// a written WAV reads back
const w = joinPhrases([tone(500, 0.3)], R);
const p = parseWav(w);
eq('round trip: rate, channels', [p.rate, p.channels], [R, 1]);
eq('round trip: length', p.samples.length, R / 2);
// stereo is mixed down
const st = new ArrayBuffer(44 + 8); const v = new DataView(st);
[...'RIFF'].forEach((c, i) => v.setUint8(i, c.charCodeAt(0))); v.setUint32(4, 44, true); [...'WAVEfmt '].forEach((c, i) => v.setUint8(8 + i, c.charCodeAt(0)));
v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 2, true); v.setUint32(24, R, true); v.setUint32(28, R * 4, true); v.setUint16(32, 4, true); v.setUint16(34, 16, true);
[...'data'].forEach((c, i) => v.setUint8(36 + i, c.charCodeAt(0))); v.setUint32(40, 8, true); v.setInt16(44, 1000, true); v.setInt16(46, 3000, true); v.setInt16(48, -100, true); v.setInt16(50, -300, true);
eq('stereo mixed to mono', Array.from(parseWav(st).samples), [2000, -200]);
eq('not a WAV', parseWav(new ArrayBuffer(100)), null);
eq('a data size past the end', (() => { const b = joinPhrases([tone(10, 0.1)], R); new DataView(b).setUint32(40, 1e9, true); return parseWav(b); })(), null);
eq('too short', parseWav(new ArrayBuffer(10)), null);

// the room
ok('silence is quiet', quietEnough(new Int16Array(R)));
ok('soft hiss is quiet', quietEnough(noise(1000, 0.002)));
ok('a noisy room is not', !quietEnough(noise(1000, 0.05)));
eq('silence is -120 dB', levelDb(new Int16Array(10)), -120);

// trimming: silence around a phrase goes, a little is kept
const phrase = cat(new Int16Array(R), tone(1000, 0.3), new Int16Array(R));
const t = trimSilence(phrase, R);
ok('trimmed to about the speech plus padding', t.length > R && t.length < R * 1.4);
eq('only silence: nothing', trimSilence(new Int16Array(R), R).length, 0);

// joining: gaps between, at most 30 s
const j = parseWav(joinPhrases([tone(1000, 0.3), tone(1000, 0.3)], R));
ok('two phrases and a gap', j.samples.length > R * 2.3 && j.samples.length < R * 2.4);
const long = parseWav(joinPhrases(Array.from({ length: 8 }, () => tone(5000, 0.3)), R));
eq('never past 30 s', long.samples.length, (R * MAX_MS) / 1000);
eq('empty clips are skipped', parseWav(joinPhrases([new Int16Array(0), tone(100, 0.3)], R)).samples.length, R / 10);

// when a phrase was read
const s = { start: 0 };
ok('quiet at the start: go on', !phraseDone(s, 0.001, 300));
ok('speaking: go on', !phraseDone(s, 0.5, 800) && !phraseDone(s, 0.4, 1600));
ok('a short pause: go on', !phraseDone(s, 0.01, 2000) && !phraseDone(s, 0.01, 2000 + END_QUIET_MS - 100));
ok('quiet long enough after speech: done', phraseDone(s, 0.01, 2000 + END_QUIET_MS));
const s2 = { start: 0 };
ok('nothing said: never done by itself', !phraseDone(s2, 0.001, 500) && !phraseDone(s2, 0.001, 9000));

console.log(`voicerec_model_test: ${n - bad}/${n}`);
process.exit(bad ? 1 : 0);
