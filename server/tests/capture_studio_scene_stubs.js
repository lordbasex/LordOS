// capture_studio_scene_stubs.js — goes first in capture_studio_scene.mjs: the native calls the SDK modules make while
// loading, before the scene's own. LordOS Studio is installed (lord.studio.status().available) with no preview source
// (the scene never opens a camera), and the sound system has one microphone and Noise Reduction loaded.
const L = globalThis.lord;
const calls = { preview: 0, setDefault: [] };
globalThis.__studioCalls = calls;
L.studio = {
  status: () => ({ available: true, on: false, loopback: 'missing', sources: [], apps: [], gpu: false, fps: 0 }),
  set: () => {}, setBackground: () => {}, endPreview: () => {},
  preview: () => { calls.preview++; return null; },
};
const SOURCES = [
  { index: 1, name: 'alsa_input.usb-mic', description: 'USB Microphone', monitorOf: -1, default: true },
  { index: 2, name: 'lordos_voice_isolation', description: 'LordOS Noise Reduction', monitorOf: -1 },
];
L.sound = {
  state: () => ({ connected: true, sinks: [], sources: SOURCES, inputs: [], outputs: [], defaultSource: 'alsa_input.usb-mic' }),
  devices: () => [],
  setDefault: (kind, name) => { calls.setDefault.push([kind, name]); return true; },
};
L.media = { cameras: () => [{ device: '/dev/video90', name: 'Test camera', bus: 'usb-test-1' }], on: () => () => {}, close: () => {}, view: () => {} };
const ns = (o, fns) => { if (typeof L[o] !== 'object' || !L[o]) L[o] = {}; for (const [k, v] of Object.entries(fns)) if (typeof L[o][k] !== 'function') L[o][k] = v; };
ns('keyboard', { current: () => ({}), layouts: () => [] });
ns('input', { configure: () => [] });
for (const k of Object.keys(L)) {
  const o = L[k];
  if (!o || typeof o !== 'object' || Array.isArray(o)) continue;
  L[k] = new Proxy(o, { get: (t, p) => { const v = t[p]; return v !== undefined || typeof p !== 'string' ? v : () => []; } });
}
const noop = () => new Proxy({}, { get: (t, p) => (p in t ? t[p] : typeof p === 'string' ? () => [] : undefined) });
globalThis.lord = new Proxy(L, { get: (t, p) => { const v = t[p]; if (v !== undefined || typeof p !== 'string') return v; t[p] = noop(); return t[p]; } });
