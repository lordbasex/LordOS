// wallpaper_model_test.mjs — one wallpaper per display (sdk/wallpapermodel.js): which picture each display shows,
// what choosing one for all displays or for one writes, and the displays' names. node server/tests/wallpaper_model_test.mjs
import { displayWallpaperKey, wallpaperIdFor, wallpaperPatch, displayLabel, DEFAULT_WALLPAPER } from '../../sdk/wallpapermodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const L = { id: 83, name: 'S27F350', connector: 'DP-2' }, M = { id: 89, name: 'S27F350', connector: 'HDMI-A-1' }, R = { id: 95, name: 'S27F350', connector: 'DP-3' };
const s0 = { wallpaper: 'aurora', wallpapers: {}, motion: {} };

eq('key: the connector', displayWallpaperKey(M), 'HDMI-A-1');
eq('key: no connector, the id', displayWallpaperKey({ id: 4 }), '#4');
eq('no own picture: the global one', [L, M, R].map((d) => wallpaperIdFor(s0, d)), ['aurora', 'aurora', 'aurora']);
const s1 = { ...s0, ...wallpaperPatch(s0, 'cosmos', M) };
eq('one display: its own picture, the others keep the global one', [L, M, R].map((d) => wallpaperIdFor(s1, d)), ['aurora', 'cosmos', 'aurora']);
eq('one display: its moving wallpaper off (null wins over the one for all)', s1.motion, { 89: null });
const s2 = { ...s1, ...wallpaperPatch(s1, 'lumo', R) };
eq('two displays, each its own', [L, M, R].map((d) => wallpaperIdFor(s2, d)), ['aurora', 'cosmos', 'lumo']);
const s3 = { ...s2, ...wallpaperPatch(s2, 'gradient', null) };
eq('all displays: the global picture, no own ones, no moving wallpaper', [[L, M, R].map((d) => wallpaperIdFor(s3, d)), s3.wallpapers, s3.motion], [['gradient', 'gradient', 'gradient'], {}, {}]);
eq('no display (one monitor): the global picture', wallpaperIdFor(s2, null), 'aurora');
eq('a bad map is ignored', wallpaperIdFor({ wallpaper: 'a', wallpapers: 'x' }, M), 'a');
eq('an empty entry is ignored', wallpaperIdFor({ wallpaper: 'a', wallpapers: { 'HDMI-A-1': '' } }, M), 'a');
// 0.8.9: never chosen (null) is the theme's default; Lumo Celestial when the theme has none (Flow, Classic)
eq('never chosen: Lumo Celestial', wallpaperIdFor({ wallpaper: null }, M), DEFAULT_WALLPAPER);
eq('never chosen: the theme\'s default', wallpaperIdFor({ wallpaper: null }, M, 'cyberpunk-panorama'), 'cyberpunk-panorama');
eq('never chosen, no display: the theme\'s default', wallpaperIdFor({ wallpaper: null }, null, 'cyberpunk-panorama'), 'cyberpunk-panorama');
eq('chosen: the theme\'s default doesn\'t count', wallpaperIdFor({ wallpaper: 'aurora' }, M, 'cyberpunk-panorama'), 'aurora');
eq('a display\'s own picture over the theme\'s default', wallpaperIdFor({ wallpaper: null, wallpapers: { 'HDMI-A-1': 'lumo' } }, M, 'cyberpunk-panorama'), 'lumo');
eq('labels: three of one model add the connector', [L, M, R].map((d) => displayLabel(d, [L, M, R])), ['S27F350 · DP-2', 'S27F350 · HDMI-A-1', 'S27F350 · DP-3']);
eq('labels: different models, the name alone', displayLabel({ id: 1, name: 'U2720Q', connector: 'DP-1' }, [{ id: 1, name: 'U2720Q', connector: 'DP-1' }, L]), 'U2720Q');
eq('labels: no name, the connector', displayLabel({ id: 2, name: '', connector: 'eDP-1' }, []), 'eDP-1');

console.log(bad ? `wallpaper-model-test: ${bad} FAILED` : `wallpaper-model-test: ${ok} ok`);
if (bad) process.exit(1);
