// voicemanager.js — the voices installed for reading aloud, downloading and removing them, the one chosen for each
// language, and the person's own voices (docs/VOICE.md, phase 3; Settings → AI → Voice, apps/settings/AI.jsx).
//
//   scanVoices()                        → { es: { standard, hd, voices: { standard: Set, hd: Set } }, …, own: [names] }
//                                         what `say` finds: /usr/share/lordos/voice (the packages) and
//                                         ~/.local/share/lordos/voice (the person's), the same rules as say.c
//   installVoices(names, onProgress)    pkg-install of lordos-voice-* packages (an administrator, the signed APT
//                                         repository); onProgress({ done, total }) while downloading
//   removeVoices(names)                 pkg-remove
//   voiceConf() / setVoiceConf(patch)   ~/.config/lordos/voice.json, which `say` reads: { voices: { es: 'lola' },
//                                         quality: 'auto'|'standard'|'hd', backend: 'auto'|'cpu'|'gpu' }
//   addOwnVoice(name, wavPath)          a voice to clone: copies a WAV into ~/.local/share/lordos/voice/voices
//   ownVoiceName(text)                  a file name from what the person typed
//   removeOwnVoice(name)                to the Trash (it can be restored there)
//
// Package names come from sdk/voicecatalog.js. Nothing here plays sound: previews are sdk/voice.js's previewVoice.
import * as fs from './fs.js';
import { admin } from './admin.js';
import { VOICE_LANGS, modelPackage, voicePackage } from './voicecatalog.js';

const SYS = '/usr/share/lordos/voice';
const userRoot = () => `${fs.home()}/.local/share/lordos/voice`;
const CONF = '.config/lordos/voice.json';

const isFile = (p) => { try { const s = fs.stat(p); return !!s && !s.dir; } catch (e) { return false; } };
function names(dir, ext) {
  try { return fs.list(dir, { hidden: false }).filter((e) => !e.dir && e.name.endsWith(ext)).map((e) => e.name.slice(0, -ext.length)); } catch (e) { return []; }
}

export function scanVoices() {
  const out = {};
  const roots = [userRoot(), SYS];
  for (const L of VOICE_LANGS) {
    const r = { standard: false, hd: false, voices: { standard: new Set(), hd: new Set() } };
    for (const root of roots) {
      const m = `${root}/models/${L.id}`;
      if (isFile(`${m}/standard.gguf`)) { r.standard = true; for (const v of names(`${m}/embeddings`, '.safetensors')) r.voices.standard.add(v); }
      if (isFile(`${m}/hd/model.safetensors`)) { r.hd = true; for (const v of names(`${m}/hd/embeddings`, '.safetensors')) r.voices.hd.add(v); }
    }
    out[L.id] = r;
  }
  out.own = [...new Set(roots.flatMap((root) => names(`${root}/voices`, '.wav')))].sort();
  return out;
}

// "progress download <done> <total>" from the package engine (docs/PACKAGES.md)
export function parseProgress(line) {
  const m = /^progress download (\d+) (\d+)/.exec(String(line || ''));
  return m ? { done: Number(m[1]), total: Number(m[2]) } : null;
}

const pkgNames = (list) => {
  const ok = (list || []).filter((n) => typeof n === 'string' && /^lordos-voice-[a-z0-9-]+$/.test(n));
  if (!ok.length || ok.length !== list.length) throw new Error('not a voice package');
  return ok.join(' ');
};

export function installVoices(list, onProgress) {
  return admin.run('pkg-install', pkgNames(list), (line) => { const p = parseProgress(line); if (p && onProgress) onProgress(p); });
}

export const removeVoices = (list) => admin.run('pkg-remove', pkgNames(list));

// what a language and quality need: the model and the voice, those not installed
export function packagesFor(lang, voice, hd, scan) {
  const s = scan[lang];
  const out = [];
  if (!s || !(hd ? s.hd : s.standard)) out.push(modelPackage(lang, hd));
  if (voice && !(s && s.voices[hd ? 'hd' : 'standard'].has(voice))) out.push(voicePackage(lang, voice, hd));
  return out.filter(Boolean);
}

// ---- voice.json ----

export function voiceConf() {
  try { const j = JSON.parse(fs.read(CONF)); return j && typeof j === 'object' && !Array.isArray(j) ? j : {}; } catch (e) { return {}; }
}

export function setVoiceConf(patch) {
  const cur = voiceConf();
  const next = { ...cur, ...patch, ...(patch.voices ? { voices: { ...(cur.voices || {}), ...patch.voices } } : null) };
  try { fs.mkdir('.config/lordos'); } catch (e) { /* already there */ }
  fs.write(CONF, `${JSON.stringify(next, null, 2)}\n`);
  return next;
}

// ---- the person's own voices (voices/<name>.wav, cloned by the model) ----

// a plain name say.c accepts: letters, digits, - and _, at most 60
export function ownVoiceName(text) {
  const s = String(text || '').normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/\s+/g, '_').replace(/[^A-Za-z0-9_-]/g, '').slice(0, 60);
  return s || null;
}

export function addOwnVoice(name, wavPath) {
  const n = ownVoiceName(name);
  if (!n) throw new Error('name');
  const dir = userRoot() + '/voices';
  for (const d of ['.local', '.local/share', '.local/share/lordos', '.local/share/lordos/voice', '.local/share/lordos/voice/voices']) {
    try { fs.mkdir(d); } catch (e) { /* there */ }
  }
  fs.write(`${dir}/${n}.wav`, fs.read(wavPath, { binary: true }));
  return n;
}

export function removeOwnVoice(name) {
  const n = ownVoiceName(name);
  if (n) fs.trash(`${userRoot()}/voices/${n}.wav`);
}
