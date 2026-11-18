// soundwavemodel.js — the sound wave widget's choices, pure (node tests: server/tests/soundwave_model_test.mjs).
//
// A widget listens to one source: the desktop (what the default output plays), one app (kept by name, since its
// stream's index changes each time it starts playing) or the default microphone. lord.sound.spectrum serves one
// at a time, so with several widgets the most recently shown one decides.
//
//   sourceOf(opts)                 { kind: 'desktop' | 'app' | 'mic', app?, label? } (the desktop by default)
//   appsPlaying(state)             [{ key, label }] one per app with a stream (our own streams are never in the state)
//   resolve(state, src)            [kind, index] for lord.sound.spectrum, or null (the app isn't playing now)
//   PALETTES / colorsOf(name, accent)   three 0xRRGGBB colours, first line to last
//   gainOf(level)                  'low' | 'normal' | 'high' → the node's gain

export function sourceOf(opts) {
  const s = opts && opts.source;
  if (s && s.kind === 'mic') return { kind: 'mic' };
  if (s && s.kind === 'app' && typeof s.app === 'string' && s.app) return { kind: 'app', app: s.app, label: typeof s.label === 'string' ? s.label : s.app };
  return { kind: 'desktop' };
}

const keyOf = (i) => i.appId || i.binary || i.app || '';

export function appsPlaying(state) {
  const out = [];
  for (const i of (state && state.inputs) || []) {
    const key = keyOf(i);
    if (!key || out.some((o) => o.key === key)) continue;
    out.push({ key, label: i.app || i.binary || key });
  }
  return out;
}

export function resolve(state, src) {
  if (!src || src.kind === 'desktop') return ['sink', -1];
  if (src.kind === 'mic') return ['source', -1];
  const ins = ((state && state.inputs) || []).filter((i) => keyOf(i) === src.app);
  if (!ins.length) return null;
  const playing = ins.filter((i) => !i.corked);
  const pick = (playing.length ? playing : ins).reduce((a, b) => (b.index > a.index ? b : a));   // the newest stream
  return ['input', pick.index];
}

export const PALETTES = {
  neon: [0x408cff, 0xa05aff, 0xff3cf0],
  aurora: [0x2fe6a8, 0x2fb8ff, 0x6a5cff],
  fire: [0xffd23c, 0xff7a2f, 0xff2f6a],
  ice: [0xbfe9ff, 0x5ac8ff, 0x2f6aff],
};
export const PALETTE_ORDER = ['neon', 'accent', 'aurora', 'fire', 'ice'];

const hex = (c) => {
  const m = typeof c === 'string' && /^#?([0-9a-f]{6})$/i.exec(c.trim());
  return m ? parseInt(m[1], 16) : null;
};
// the theme's accent: lighter at the first line, the accent in the middle, a warmer turn at the last
function fromAccent(accent) {
  const a = hex(accent);
  if (a == null) return PALETTES.neon;
  const r = (a >> 16) & 255, g = (a >> 8) & 255, b = a & 255;
  const mix = (x, y, t) => Math.round(x + (y - x) * t);
  const light = (mix(r, 255, 0.45) << 16) | (mix(g, 255, 0.45) << 8) | mix(b, 255, 0.45);
  const warm = (mix(r, 255, 0.5) << 16) | (mix(g, 60, 0.4) << 8) | mix(b, 200, 0.3);
  return [light, a, warm];
}
export function colorsOf(name, accent) {
  if (name === 'accent') return fromAccent(accent);
  return PALETTES[name] || PALETTES.neon;
}

export const gainOf = (level) => (level === 'low' ? 0.6 : level === 'high' ? 1.7 : 1);
