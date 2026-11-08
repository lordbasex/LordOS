// voicerecmodel.js — the recordings of a personal voice (Settings → AI → Voice, "Create a Personal Voice";
// docs/VOICE.md): the room's noise, where speech starts and ends in a phrase, and the phrases joined into the one WAV
// the model clones from (16-bit mono, at most 30 s; lord-tts's read_voice). Pure; tested by
// server/tests/voicerec_model_test.mjs.
//
//   parseWav(arrayBuffer)            → { rate, channels, samples: Int16Array (mono) } or null
//   levelDb(samples)                 → RMS in dBFS (-120 for silence)
//   quietEnough(samples)             → the room's noise is under QUIET_DB
//   trimSilence(samples, rate)       → the samples between the first and the last speech, with SPEECH_PAD_MS around
//   joinPhrases(clips, rate)         → an ArrayBuffer: a WAV of the clips, GAP_MS of silence between, MAX_MS at most
//   phraseDone(state, level, nowMs)  → the recording of a phrase can stop: speech was heard, then quiet for END_QUIET_MS
//                                      (level: lord-media's live level, 0..1); state is kept by the caller

export const QUIET_DB = -45;          // a quiet room; above it the voice copies the noise too
export const SPEECH_DB = -38;         // a 20 ms frame louder than this is speech
export const SPEECH_PAD_MS = 150;
export const GAP_MS = 350;
export const MAX_MS = 30000;
export const END_QUIET_MS = 1200;
export const MIN_PHRASE_MS = 1500;

export function parseWav(buf) {
  if (!buf || buf.byteLength < 44) return null;
  const v = new DataView(buf);
  const tag = (o) => String.fromCharCode(v.getUint8(o), v.getUint8(o + 1), v.getUint8(o + 2), v.getUint8(o + 3));
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  let fmt = null, data = null;
  for (let at = 12; at + 8 <= buf.byteLength;) {
    const size = v.getUint32(at + 4, true);
    if (size > buf.byteLength - at - 8) return null;
    if (tag(at) === 'fmt ' && size >= 16) fmt = { format: v.getUint16(at + 8, true), channels: v.getUint16(at + 10, true), rate: v.getUint32(at + 12, true), bits: v.getUint16(at + 22, true) };
    else if (tag(at) === 'data') { data = { at: at + 8, size }; break; }
    at += 8 + size + (size & 1);
  }
  if (!fmt || !data || fmt.format !== 1 || fmt.bits !== 16 || fmt.channels < 1 || fmt.channels > 2 || fmt.rate < 8000 || fmt.rate > 96000) return null;
  const frames = Math.floor(data.size / (2 * fmt.channels));
  const samples = new Int16Array(frames);
  for (let i = 0; i < frames; i++) {
    let s = 0;
    for (let c = 0; c < fmt.channels; c++) s += v.getInt16(data.at + (i * fmt.channels + c) * 2, true);
    samples[i] = Math.round(s / fmt.channels);
  }
  return { rate: fmt.rate, channels: fmt.channels, samples };
}

export function levelDb(samples) {
  if (!samples || !samples.length) return -120;
  let e = 0;
  for (let i = 0; i < samples.length; i++) e += samples[i] * samples[i];
  const rms = Math.sqrt(e / samples.length) / 32768;
  return rms > 0 ? Math.max(-120, 20 * Math.log10(rms)) : -120;
}

export const quietEnough = (samples) => levelDb(samples) < QUIET_DB;

export function trimSilence(samples, rate) {
  const hop = Math.max(1, Math.floor(rate * 0.02));
  let first = -1, last = -1;
  for (let at = 0; at + hop <= samples.length; at += hop) {
    if (levelDb(samples.subarray(at, at + hop)) > SPEECH_DB) { if (first < 0) first = at; last = at + hop; }
  }
  if (first < 0) return new Int16Array(0);
  const pad = Math.floor((rate * SPEECH_PAD_MS) / 1000);
  return samples.slice(Math.max(0, first - pad), Math.min(samples.length, last + pad));
}

export function joinPhrases(clips, rate) {
  const gap = Math.floor((rate * GAP_MS) / 1000), max = Math.floor((rate * MAX_MS) / 1000);
  const parts = [];
  let n = 0;
  for (const c of clips || []) {
    if (!c || !c.length) continue;
    if (n) { const g = Math.min(gap, max - n); if (g <= 0) break; parts.push(new Int16Array(g)); n += g; }
    const take = Math.min(c.length, max - n);
    if (take <= 0) break;
    parts.push(c.subarray(0, take));
    n += take;
  }
  const buf = new ArrayBuffer(44 + n * 2);
  const v = new DataView(buf);
  const str = (o, s) => { for (let i = 0; i < 4; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); v.setUint32(4, 36 + n * 2, true); str(8, 'WAVE');
  str(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, rate, true); v.setUint32(28, rate * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  str(36, 'data'); v.setUint32(40, n * 2, true);
  let at = 44;
  for (const p of parts) for (let i = 0; i < p.length; i++, at += 2) v.setInt16(at, p[i], true);
  return buf;
}

// state: { start, peak, quietSince } (start: ms when the recording began)
export function phraseDone(state, level, now) {
  const s = state;
  if (!Number.isFinite(level)) return false;
  s.peak = Math.max(s.peak || 0, level);
  const speaking = s.peak > 0.02 && level > s.peak * 0.18;
  if (speaking) { s.heard = true; s.quietSince = null; return false; }
  if (!s.heard || now - s.start < MIN_PHRASE_MS) return false;
  if (s.quietSince == null) s.quietSince = now;
  return now - s.quietSince >= END_QUIET_MS;
}
