// studio_voice_test.mjs — LordOS Studio's Noise Reduction drop-in (E-S6; docs/CAMERA-STUDIO-PLAN.md 7):
// tools/iso/rootfs/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf, parsed here the way PipeWire reads its
// configuration (SPA JSON: '=' or ':', bare words, '#' comments, no commas needed). It never touches this machine's
// PipeWire: no daemon is started, nothing is loaded. Checks: one module, PipeWire's own echo-cancel with WebRTC, the
// processing D8 chose (no echo reference fed), the node names that sdk/studiomodel.js (Mic Mode, the sound lists) and
// server/src/sec/pwguard.c (the capture indicator) rely on, the package installing it, and, when the dev machine has
// them, that Debian's installed module and WebRTC plugin know every key used. Run from server/.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { VOICE_SOURCE, VOICE_CAPTURE, VOICE_INTERNAL_SINKS, VOICE_INTERNAL_STREAMS } from '../../sdk/studiomodel.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const CONF = path.join(root, 'tools/iso/rootfs/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf');

// SPA JSON, strict enough for a static file of ours: objects, arrays, strings, bare words, numbers, booleans
export function parseSpa(text) {
  let i = 0;
  const ws = () => {
    for (;;) {
      while (i < text.length && /[\s,]/.test(text[i])) i++;
      if (text[i] === '#') { while (i < text.length && text[i] !== '\n') i++; continue; }
      return;
    }
  };
  const word = () => {
    ws();
    if (text[i] === '"') {
      let s = '';
      i++;
      while (i < text.length && text[i] !== '"') { if (text[i] === '\\') i++; s += text[i++]; }
      if (text[i] !== '"') throw new Error('unterminated string');
      i++;
      return { s, quoted: true };
    }
    const m = /^[^\s{}\[\]=:,#"]+/.exec(text.slice(i));
    if (!m) throw new Error(`unexpected "${text[i]}" at ${i}`);
    i += m[0].length;
    return { s: m[0], quoted: false };
  };
  const value = () => {
    ws();
    if (text[i] === '{') { i++; return object('}'); }
    if (text[i] === '[') {
      i++;
      const a = [];
      for (;;) { ws(); if (text[i] === ']') { i++; return a; } if (i >= text.length) throw new Error('unterminated array'); a.push(value()); }
    }
    const w = word();
    if (w.quoted) return w.s;
    if (w.s === 'true') return true;
    if (w.s === 'false') return false;
    if (w.s === 'null') return null;
    if (/^-?\d+(\.\d+)?$/.test(w.s)) return Number(w.s);
    return w.s;
  };
  const object = (end) => {
    const o = {};
    for (;;) {
      ws();
      if (end && text[i] === end) { i++; return o; }
      if (i >= text.length) { if (end) throw new Error('unterminated object'); return o; }
      const k = word().s;
      ws();
      if (text[i] !== '=' && text[i] !== ':') throw new Error(`expected = after ${k}`);
      i++;
      if (k in o) throw new Error(`duplicate key ${k}`);
      o[k] = value();
    }
  };
  return object(null);
}

let n = 0;
const t = (name, fn) => { fn(); n++; };

const text = fs.readFileSync(CONF, 'utf8');
const conf = parseSpa(text);

t('one module: PipeWire\'s own echo-cancel with WebRTC, nothing else', () => {
  assert.deepEqual(Object.keys(conf), ['context.modules']);
  assert.equal(conf['context.modules'].length, 1);
  const m = conf['context.modules'][0];
  assert.equal(m.name, 'libpipewire-module-echo-cancel');
  assert.deepEqual(Object.keys(m).sort(), ['args', 'name']);                       // no flags: { nofail }, no condition
  assert.equal(m.args['library.name'], 'aec/libspa-aec-webrtc');
  assert.equal(m.args['monitor.mode'], false);                                       // no sink monitor taken as the echo
});

const args = conf['context.modules'][0].args;

t('the processing: noise, transients, high-pass, gain; no voice activity detection', () => {
  const a = args['aec.args'];
  assert.equal(a['webrtc.noise_suppression'], true);
  assert.equal(a['webrtc.transient_suppression'], true);
  assert.equal(a['webrtc.high_pass_filter'], true);
  assert.equal(a['webrtc.gain_control'], true);
  assert.equal(a['webrtc.voice_detection'], false);
  assert.deepEqual(Object.keys(a).every((k) => k.startsWith('webrtc.')), true);
});

t('the names Mic Mode, the sound lists and the capture indicator rely on', () => {
  assert.equal(args['source.props']['node.name'], VOICE_SOURCE);
  assert.equal(args['capture.props']['node.name'], VOICE_CAPTURE);
  assert.equal(args['capture.props']['media.name'], VOICE_CAPTURE);                // the name pulse clients see
  assert.ok(VOICE_INTERNAL_SINKS.includes(args['sink.props']['node.name']));
  assert.ok(VOICE_INTERNAL_STREAMS.includes(args['capture.props']['node.name']));
  assert.ok(VOICE_INTERNAL_STREAMS.includes(args['playback.props']['node.name']));
  // the echo reference never keeps the speakers awake, and nothing becomes the default output by itself
  assert.equal(args['sink.props']['node.passive'], true);
  assert.equal(args['playback.props']['node.passive'], true);
  assert.ok(args['sink.props']['priority.session'] <= 1 && args['source.props']['priority.session'] <= 1);
  const pw = fs.readFileSync(path.join(root, 'server/src/sec/pwguard.c'), 'utf8');
  assert.ok(pw.includes(`"${VOICE_CAPTURE}"`), 'pwguard.c knows the filter\'s capture by the same name');
});

t('the package installs it in PipeWire\'s own drop-in folder', () => {
  const mk = fs.readFileSync(path.join(root, 'tools/iso/mkdeb.sh'), 'utf8');
  assert.ok(mk.includes('tools/iso/rootfs/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf "$PKG/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf"'));
});

t('the parser itself', () => {
  assert.deepEqual(parseSpa('a = 1 b = { c = "x y" d: [ 1 2 true ] } # comment\n e = bare'), { a: 1, b: { c: 'x y', d: [1, 2, true] }, e: 'bare' });
  assert.throws(() => parseSpa('a = { b = 1'));
  assert.throws(() => parseSpa('a = 1 a = 2'));
});

// Debian's own files on the dev machine (read only): every key we use is one they know
const lib = ['/usr/lib/x86_64-linux-gnu', '/usr/lib/aarch64-linux-gnu'].find((d) => fs.existsSync(path.join(d, 'spa-0.2/aec/libspa-aec-webrtc.so')));
if (lib) {
  t('Debian\'s module and WebRTC plugin know every key used', () => {
    const aec = fs.readFileSync(path.join(lib, 'spa-0.2/aec/libspa-aec-webrtc.so')).toString('latin1');
    for (const k of Object.keys(args['aec.args'])) assert.ok(aec.includes(k), `libspa-aec-webrtc doesn't know ${k}`);
    const mod = fs.readFileSync(path.join(lib, 'pipewire-0.3/libpipewire-module-echo-cancel.so')).toString('latin1');
    for (const k of Object.keys(args)) assert.ok(mod.includes(k), `module-echo-cancel doesn't know ${k}`);
  });
} else console.log('studio voice: skipped the installed module check: needs libspa-0.2-modules');

console.log(`studio voice: ${n} groups ok`);
