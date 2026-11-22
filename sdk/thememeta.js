// thememeta.js — what a theme's theme.json may tell Settings besides its name (0.8.9, Lord Workspaces), as plain data
// (tested by server/tests/theme_meta_test.mjs). Settings uses it instead of knowing each theme by its id; the bundled
// Lord Flow and Lord Classic declare none of it and keep their own pictures and pages.
//
//   sketch         { rail, pills, frame, menubar, dock, taskbar }: booleans for Settings → Appearance's drawn picture
//   options        [{ key, label, hint, choices: [[value, label]] | toggle: true, default }]: the theme's own choices,
//                  shown under Desktop style while it is the current theme and stored in
//                  settings.themeOptions[<theme id>][key] (one key for every theme: a theme's id can't reach another
//                  setting)
//   desktopArrows  true: the theme follows Settings → Keyboard's ⌃ arrows for desktops (as Lord Flow does)
//   wallpaper      { default: <id>, slideshow: true }: the picture of a user who never chose one (settings.wallpaper
//                  null; the theme's tokens.defaultWallpaper says the same to the SDK), and Settings → Wallpaper's
//                  slideshow (sdk/slideshowmodel.js). Read as wallpaperDefault and wallpaperSlideshow.
//   displayApps    true: the theme offers the apps used on each display (sdk/displayapps.js); Settings shows, with its
//                  options, the switch that records them and "Clear"
//   community      true: a theme LordOS didn't make; Settings → Appearance marks it "Community · unofficial"
//   author         who made it (plain text, at most 80 characters; "author pending" or none reads as an unknown author)
//   license        its license as an SPDX expression ("MIT", "GPL-3.0-or-later", "MIT OR Apache-2.0")
//   version        its own version ("1.2.0", "0.3.1-beta"); the official themes have none and show LordOS's
//
// theme.json is data (at most 4 KB, read by lord-server), never code: every field is checked here, labels must be
// locale keys of the shell catalog, values short plain strings, and anything else is dropped.
const KEY = /^[A-Za-z][A-Za-z0-9]{0,31}$/;
const LABEL = /^shell\.[A-Za-z0-9_.-]{1,80}$/;
const VALUE = /^[a-z][a-z0-9-]{0,23}$/;
const SKETCH = ['rail', 'pills', 'frame', 'menubar', 'dock', 'taskbar'];
const WALLPAPER = /^[a-z][a-z0-9-]{0,47}$/;
const LICENSE = /^[A-Za-z0-9.+-]{1,40}( (OR|AND|WITH) [A-Za-z0-9.+-]{1,40}){0,3}$/;
const VERSION = /^[0-9]{1,5}(\.[0-9]{1,5}){0,3}([-+][0-9A-Za-z.-]{1,20})?$/;
// Plain text on one line: no control or bidi-override characters, at most `max` characters; '' otherwise.
// eslint-disable-next-line no-control-regex
const UNSAFE = /[\u0000-\u001f\u007f-\u009f\u200e\u200f\u202a-\u202e\u2066-\u2069]/;
function plainText(v, max) {
  if (typeof v !== 'string') return '';
  if (UNSAFE.test(v)) return '';
  const s = v.trim().replace(/ +/g, ' ');
  return s && [...s].length <= max ? s : '';
}
// A theme.json author: '' when missing, invalid or still the placeholder "author pending".
export function themeAuthor(v) {
  const s = plainText(v, 80);
  return /^author pending$/i.test(s) ? '' : s;
}

function cleanOption(o) {
  if (!o || typeof o !== 'object' || !KEY.test(o.key) || !LABEL.test(o.label)) return null;
  const out = { key: o.key, label: o.label };
  if (typeof o.hint === 'string' && LABEL.test(o.hint)) out.hint = o.hint;
  if (o.toggle === true) return { ...out, toggle: true, default: o.default === true };
  if (!Array.isArray(o.choices)) return null;
  const choices = o.choices.filter((c) => Array.isArray(c) && VALUE.test(c[0]) && LABEL.test(c[1])).slice(0, 6).map((c) => [c[0], c[1]]);
  if (choices.length < 2) return null;
  return { ...out, choices, default: choices.some((c) => c[0] === o.default) ? o.default : choices[0][0] };
}

export function cleanThemeMeta(meta) {
  const m = meta && typeof meta === 'object' && !Array.isArray(meta) ? meta : {};
  const s = m.sketch && typeof m.sketch === 'object' ? m.sketch : null;
  const sketch = s ? Object.fromEntries(SKETCH.filter((k) => s[k] === true).map((k) => [k, true])) : null;
  const options = Array.isArray(m.options) ? m.options.slice(0, 8).map(cleanOption).filter(Boolean) : [];
  const wp = m.wallpaper && typeof m.wallpaper === 'object' && !Array.isArray(m.wallpaper) ? m.wallpaper : {};
  return { sketch: sketch && Object.keys(sketch).length ? sketch : null, options, desktopArrows: m.desktopArrows === true, displayApps: m.displayApps === true,
    wallpaperDefault: typeof wp.default === 'string' && WALLPAPER.test(wp.default) ? wp.default : null, wallpaperSlideshow: wp.slideshow === true,
    community: m.community === true, author: themeAuthor(m.author),
    license: typeof m.license === 'string' && LICENSE.test(m.license) ? m.license : '',
    version: typeof m.version === 'string' && VERSION.test(m.version) ? m.version : '' };
}

// The value of a theme option in the settings (its default when unset or not one of its choices).
export function themeOptions(settings, themeId) {
  const all = settings && settings.themeOptions && typeof settings.themeOptions === 'object' && !Array.isArray(settings.themeOptions) ? settings.themeOptions : {};
  const own = Object.prototype.hasOwnProperty.call(all, themeId) ? all[themeId] : null;
  return own && typeof own === 'object' && !Array.isArray(own) ? own : {};
}
export function themeOptionValue(settings, themeId, opt) {
  const own = themeOptions(settings, themeId);
  const v = own[opt.key];
  if (opt.toggle) return typeof v === 'boolean' ? v : opt.default;
  return opt.choices.some((c) => c[0] === v) ? v : opt.default;
}
// The settings patch that sets it (the theme's other values stay).
export function themeOptionPatch(settings, themeId, key, value) {
  const all = settings && settings.themeOptions && typeof settings.themeOptions === 'object' && !Array.isArray(settings.themeOptions) ? settings.themeOptions : {};
  return { themeOptions: { ...all, [themeId]: { ...themeOptions(settings, themeId), [key]: value } } };
}
