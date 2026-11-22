// cae.js: Lord Celeste's design values, its colour schemes (dark and light), its options and small helpers.
// The layout idea (a screen frame with a bar on one side and drawers that slide out of its edges) is inspired by
// Caelestia (caelestia-dots/shell, GPL-3.0); no code or assets come from it. Everything here was written for LordOS.
import { themeOptionValue } from '@lordos/sdk';

export const THEME_ID = 'celeste';

// The theme's own choices, as declared in theme.json "options" (Settings → Appearance shows them under Desktop style
// and stores them in settings.themeOptions.celeste). Kept here too so the theme reads them with the same defaults.
export const OPTIONS = {
  scheme: { key: 'scheme', default: 'lavender', choices: [['lavender'], ['rose'], ['sky'], ['mint'], ['sand']] },
  surface: { key: 'surface', default: 'solid', choices: [['clear'], ['glass'], ['solid'], ['opaque']] },
  hover: { key: 'hover', toggle: true, default: true },
  wpEffect: { key: 'wpEffect', default: 'fade', choices: [['none'], ['fade'], ['curtain'], ['blinds'], ['shutter'], ['zoom']] },
};
export const option = (s, name) => themeOptionValue(s, THEME_ID, OPTIONS[name]);

const SURFACE = { clear: 0.55, glass: 0.8, solid: 0.94, opaque: 1 };

// Each scheme in both modes: primary (accents on the frame), onPrimary (text on it), container (selected rows,
// pressed buttons) and onContainer.
export const SCHEMES = {
  lavender: {
    dark: { primary: '#d0bcff', onPrimary: '#381e72', container: '#4f378b', onContainer: '#eaddff' },
    light: { primary: '#6750a4', onPrimary: '#ffffff', container: '#eaddff', onContainer: '#21005d' },
  },
  rose: {
    dark: { primary: '#ffb1c8', onPrimary: '#5e1133', container: '#7b2949', onContainer: '#ffd9e2' },
    light: { primary: '#984061', onPrimary: '#ffffff', container: '#ffd9e2', onContainer: '#3e001d' },
  },
  sky: {
    dark: { primary: '#a8c8ff', onPrimary: '#06305f', container: '#254777', onContainer: '#d5e3ff' },
    light: { primary: '#3d5f90', onPrimary: '#ffffff', container: '#d5e3ff', onContainer: '#001b3c' },
  },
  mint: {
    dark: { primary: '#8ed9b8', onPrimary: '#00382a', container: '#00513e', onContainer: '#a9f5d3' },
    light: { primary: '#086c53', onPrimary: '#ffffff', container: '#a9f5d3', onContainer: '#002117' },
  },
  sand: {
    dark: { primary: '#f0bf6f', onPrimary: '#432c00', container: '#614100', onContainer: '#ffddac' },
    light: { primary: '#7c5800', onPrimary: '#ffffff', container: '#ffddac', onContainer: '#271900' },
  },
};

const NEUTRAL = {
  dark: { frame: [20, 18, 24], frameSolid: '#141218', surface: '#1d1b20', surfaceHi: '#2b2930', surfaceHi2: '#36343b',
    text: '#e6e0e9', sub: '#cac4d0', dim: '#938f99', danger: '#f2b8b5', onDanger: '#601410', scrim: 'rgba(0,0,0,0.32)' },
  light: { frame: [254, 247, 255], frameSolid: '#fef7ff', surface: '#f3edf7', surfaceHi: '#e8e0e8', surfaceHi2: '#ddd6df',
    text: '#1d1b20', sub: '#49454f', dim: '#79747e', danger: '#b3261e', onDanger: '#ffffff', scrim: 'rgba(0,0,0,0.18)' },
};

// The colours the frame, the bar and the drawers use. s: the settings (mode, effects, themeOptions).
export function caeTokens(s) {
  const mode = s && s.mode === 'light' ? 'light' : 'dark';
  const id = option(s, 'scheme');
  const sc = (SCHEMES[id] || SCHEMES.lavender)[mode];
  const n = NEUTRAL[mode];
  // "no transparency" (Settings → Appearance) makes the frame opaque whatever the option says
  const a = s && s.effects === false ? 1 : SURFACE[option(s, 'surface')] ?? 0.94;
  return { ...n, ...sc, id, mode, frame: `rgba(${n.frame.join(',')},${a})` };
}

// Ease out (cubic), for the drawers.
export const easeOut = (k) => 1 - Math.pow(1 - k, 3);
export const clampN = (v, a, b) => Math.max(a, Math.min(b, v));

// One style animation without React: calls step(eased 0..1) from animation frames for ms milliseconds, then done().
// Returns a function that stops it. With ms <= 0 (reduce motion) it jumps to the end at once.
export function tween(ms, step, done, ease = easeOut) {
  if (!(ms > 0)) { step(1); done?.(); return () => {}; }
  const t0 = Date.now();
  let raf = 0, live = true;
  const frame = () => {
    if (!live) return;
    const k = Math.min(1, (Date.now() - t0) / ms);
    try { step(ease(k)); } catch (e) { live = false; return; }   // the node went away
    if (k < 1) raf = requestAnimationFrame(frame);
    else { live = false; done?.(); }
  };
  raf = requestAnimationFrame(frame);
  return () => { live = false; cancelAnimationFrame(raf); };
}

// lord.setStyle on a node that may have been unmounted meanwhile.
export const setStyle = (node, k, v) => { if (node) try { lord.setStyle(node.id, k, v); } catch (e) { /* gone */ } };
