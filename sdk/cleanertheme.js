// cleanertheme.js — Cleaner's looks, as data: one token object per theme, so a theme adds no logic. Noche follows
// the system's mode; Aurora and Cristal have one fixed look. Chosen in Cleaner → Settings (settings.cleaner.theme);
// the app and its panel by the clock use the same one.
const NOCHE = {
  stars: true, orbit: 'dark', sky: 'sky-dark', text: '#f2f5ff', sub: '#b6c0e6', side: 'rgba(5,8,28,0.5)', glass: 'rgba(120,150,255,0.09)', line: 'rgba(150,175,255,0.2)',
  accent: '#7fe3ff', good: '#7ff0c4', warn: '#ffd36b', glow: 'rgba(90,200,255,0.45)', btnA: '#39c5ff', btnB: '#5a5cff', btnText: '#06102e', ringLine: '#bfeeff', barA: '#39c5ff', barB: '#8a6bff',
  track: 'rgba(150,175,255,0.18)', panel: '#10143f', star: '#ffffff', danger: '#d92d4a', scrim: 'rgba(8,4,30,0.5)',
};
const NOCHE_LIGHT = {
  stars: true, orbit: 'light', sky: 'sky-light', text: '#141a4d', sub: '#444c80', side: 'rgba(255,255,255,0.55)', glass: 'rgba(255,255,255,0.66)', line: 'rgba(60,80,180,0.2)',
  accent: '#0a6fc2', good: '#0b7a4b', warn: '#9a5b00', glow: 'rgba(60,120,255,0.35)', btnA: '#2a8fe8', btnB: '#4a48e0', btnText: '#ffffff', ringLine: '#ffffff', barA: '#2a8fe8', barB: '#6a4be0',
  track: 'rgba(60,80,180,0.14)', panel: '#f8f9ff', star: '#4a55c8', danger: '#d92d4a', scrim: 'rgba(8,4,30,0.35)',
};
const AURORA = {
  bg: 'linear-gradient(135deg, #190846 0%, #37109a 42%, #7419c9 78%, #b832d8 100%)', text: '#ffffff', sub: 'rgba(255,255,255,0.8)', side: 'rgba(14,4,50,0.32)', glass: 'rgba(255,255,255,0.1)',
  line: 'rgba(255,255,255,0.18)', accent: '#f0c6ff', good: '#b9f7a8', warn: '#ffe08a', glow: 'rgba(214,120,255,0.55)', btnA: '#8a4be0', btnB: '#5a23c8', btnText: '#ffffff', ringLine: '#ffffff',
  barA: '#ff7ad9', barB: '#b45cff', track: 'rgba(255,255,255,0.16)', panel: '#2c0f7c', star: '#ffffff', danger: '#d92d4a', scrim: 'rgba(8,4,30,0.5)',
};
const CRISTAL = {
  pane: true, bg: 'linear-gradient(140deg, #fdf0fb 0%, #ecdfff 46%, #d2d9ff 100%)', text: '#27125e', sub: '#54428c', side: 'rgba(255,255,255,0.45)', glass: 'rgba(255,255,255,0.62)',
  line: 'rgba(90,60,170,0.18)', accent: '#6a22d6', good: '#0b7a4b', warn: '#9a5b00', glow: 'rgba(130,70,230,0.35)', btnA: '#8a3df0', btnB: '#5a23c8', btnText: '#ffffff', ringLine: '#ffffff',
  barA: '#8a3df0', barB: '#d94fc0', track: 'rgba(90,60,170,0.14)', panel: '#fbf7ff', star: '#6a22d6', danger: '#d92d4a', scrim: 'rgba(8,4,30,0.35)',
};

// the window's tint per section, and each module's own colour (Noche keeps its sky under the tint)
const TINT_DARK = { cleanup: 'rgba(255,95,162,0.17)', large: 'rgba(247,168,27,0.13)', storage: 'rgba(47,130,255,0.2)', duplicates: 'rgba(30,200,190,0.17)' };
const TINT_LIGHT = { cleanup: 'rgba(255,95,162,0.13)', large: 'rgba(247,168,27,0.13)', storage: 'rgba(47,130,255,0.13)', duplicates: 'rgba(30,200,190,0.15)' };
const MODULE = { cleanup: '#c2316f', storage: '#1f6fd0', duplicates: '#0f8f86' };
for (const t of [NOCHE, AURORA]) t.tints = TINT_DARK;
for (const t of [NOCHE_LIGHT, CRISTAL]) t.tints = TINT_LIGHT;
for (const t of [NOCHE, NOCHE_LIGHT, AURORA, CRISTAL]) t.module = MODULE;

export const CLEANER_THEMES = ['noche', 'aurora', 'cristal'];
export function cleanerPalette(settings, mode) {
  const id = (settings && settings.cleaner && settings.cleaner.theme) || 'noche';
  if (id === 'aurora') return AURORA;
  if (id === 'cristal') return CRISTAL;
  return mode === 'light' ? NOCHE_LIGHT : NOCHE;
}
