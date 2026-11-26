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
//   minLordOS      (0.8.9.6.2) the oldest LordOS release it runs on ("0.8.9.6.2": two to six numbers, nothing else);
//                  on an older one Settings → Appearance says "Needs LordOS ‹version› or later" instead of Apply and
//                  themes.use refuses it (lordosAtLeast, themeNeedsNewer)
//
// theme.json is data (at most 4 KB, read by lord-server), never code: every field is checked here, labels must be
// locale keys of the shell catalog or (0.8.9.6.2) of the theme's own namespace "theme-<id>" (themeNamespace: a
// community theme's locales/<lang>.json, never another theme's), values short plain strings, and anything else is
// dropped. The texts behind a theme's own keys are the theme's, so they are shown through themeText (one line, no
// control or bidi-override characters, cut at a fixed length).
const KEY = /^[A-Za-z][A-Za-z0-9]{0,31}$/;
const LABEL = /^shell\.[A-Za-z0-9_.-]{1,80}$/;
const VALUE = /^[a-z][a-z0-9-]{0,23}$/;
const SKETCH = ['rail', 'pills', 'frame', 'menubar', 'dock', 'taskbar'];
const WALLPAPER = /^[a-z][a-z0-9-]{0,47}$/;
const LICENSE = /^[A-Za-z0-9.+-]{1,40}( (OR|AND|WITH) [A-Za-z0-9.+-]{1,40}){0,3}$/;
const VERSION = /^[0-9]{1,5}(\.[0-9]{1,5}){0,3}([-+][0-9A-Za-z.-]{1,20})?$/;
// A LordOS release, as package.json writes it without a suffix: "0.8.9.6.2", "0.9", "1.0.0".
const LORDOS_VERSION = /^[0-9]{1,5}(\.[0-9]{1,5}){1,5}$/;
// A theme id that has a namespace of its own: "theme-" + id fits lord.locale.catalog's 32 characters (valid_id_dot in
// server/src/ui/js_sys.c), so at most 26.
const NS_ID = /^[A-Za-z0-9_-]{1,26}$/;
const OWN_KEY = /^[A-Za-z0-9_.-]{1,80}$/;
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

// (0.8.9.6.2) The namespace of a theme's own texts: "theme-<id>" ('' for an id too long or with other characters).
// lordos-theme-kit copies a theme's locales/<lang>.json to assets/locales/<lang>/theme-<id>.json of a development tree.
export function themeNamespace(id) {
  return typeof id === 'string' && NS_ID.test(id) ? `theme-${id}` : '';
}

// A label key a theme may use: one of the shell catalog's, or one of its own namespace's (never another theme's).
function labelKey(s, ns) {
  if (typeof s !== 'string') return false;
  if (LABEL.test(s)) return true;
  return !!ns && s.startsWith(`${ns}.`) && OWN_KEY.test(s.slice(ns.length + 1));
}

// The most characters a theme's own text may show (themeText); the shell catalog's are LordOS's and shown whole.
export const THEME_TEXT_MAX = { name: 48, description: 240, label: 48, hint: 160, choice: 24 };

// A text a theme brought (its translations, its theme.json name): one line, without control or bidi-override
// characters, at most `max` characters (cut with "…"); '' for anything that isn't a string.
// eslint-disable-next-line no-control-regex
const UNSAFE_ALL = /[\u0000-\u001f\u007f-\u009f‎‏‪-‮⁦-⁩]/g;
export function themeText(v, max) {
  if (typeof v !== 'string') return '';
  const s = v.replace(UNSAFE_ALL, ' ').replace(/\s+/g, ' ').trim();
  const chars = [...s];
  return chars.length <= max ? s : `${chars.slice(0, Math.max(1, max - 1)).join('').trimEnd()}…`;
}

// The text of an option's label, hint or choice: `translate` is t(); a theme's own key (theme-<id>.…) goes through
// themeText with the length for its kind (THEME_TEXT_MAX), a shell.* key is shown as LordOS translates it.
export function themeLabel(translate, key, kind = 'label') {
  const s = translate(key);
  if (typeof key !== 'string' || !key.startsWith('theme-')) return s;
  return themeText(String(s ?? ''), THEME_TEXT_MAX[kind] || THEME_TEXT_MAX.label);
}

function cleanOption(o, ns) {
  if (!o || typeof o !== 'object' || !KEY.test(o.key) || !labelKey(o.label, ns)) return null;
  const out = { key: o.key, label: o.label };
  if (labelKey(o.hint, ns)) out.hint = o.hint;
  if (o.toggle === true) return { ...out, toggle: true, default: o.default === true };
  if (!Array.isArray(o.choices)) return null;
  const choices = o.choices.filter((c) => Array.isArray(c) && VALUE.test(c[0]) && labelKey(c[1], ns)).slice(0, 6).map((c) => [c[0], c[1]]);
  if (choices.length < 2) return null;
  return { ...out, choices, default: choices.some((c) => c[0] === o.default) ? o.default : choices[0][0] };
}

// `id` is the theme's folder name: with it, labels of its own namespace (themeNamespace) are kept too.
export function cleanThemeMeta(meta, id = '') {
  const m = meta && typeof meta === 'object' && !Array.isArray(meta) ? meta : {};
  const ns = themeNamespace(id);
  const s = m.sketch && typeof m.sketch === 'object' ? m.sketch : null;
  const sketch = s ? Object.fromEntries(SKETCH.filter((k) => s[k] === true).map((k) => [k, true])) : null;
  const options = Array.isArray(m.options) ? m.options.slice(0, 8).map((o) => cleanOption(o, ns)).filter(Boolean) : [];
  const wp = m.wallpaper && typeof m.wallpaper === 'object' && !Array.isArray(m.wallpaper) ? m.wallpaper : {};
  return { sketch: sketch && Object.keys(sketch).length ? sketch : null, options, desktopArrows: m.desktopArrows === true, displayApps: m.displayApps === true,
    wallpaperDefault: typeof wp.default === 'string' && WALLPAPER.test(wp.default) ? wp.default : null, wallpaperSlideshow: wp.slideshow === true,
    community: m.community === true, author: themeAuthor(m.author),
    license: typeof m.license === 'string' && LICENSE.test(m.license) ? m.license : '',
    version: typeof m.version === 'string' && VERSION.test(m.version) ? m.version : '',
    minLordOS: typeof m.minLordOS === 'string' && LORDOS_VERSION.test(m.minLordOS) ? m.minLordOS : '' };
}

// (0.8.9.6.2) Does LordOS `current` (package.json's version: "0.8.9.6.2", "0.9.0-beta", "0.8.9.6.1+ws155") run a
// theme that needs `min`? Numbers are compared one by one, a missing one is 0 and the suffix is ignored; true when
// `min` is '' or invalid (the theme doesn't say) or `current` isn't a version (a build that doesn't know its own).
const numbers = (v) => v.split('.').map((x) => Number(x));
export function lordosAtLeast(current, min) {
  if (typeof min !== 'string' || !LORDOS_VERSION.test(min)) return true;
  const base = typeof current === 'string' ? current.split(/[-+~]/)[0] : '';
  if (!/^[0-9]{1,5}(\.[0-9]{1,5}){0,5}$/.test(base)) return true;
  const a = numbers(base), b = numbers(min);
  for (let i = 0; i < Math.max(a.length, b.length); i++) if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0);
  return true;
}
// The release a theme entry (themes.list) asks for when this LordOS is older than it, else ''.
export function themeNeedsNewer(theme, current) {
  const min = theme && typeof theme.minLordOS === 'string' ? theme.minLordOS : '';
  return min && !lordosAtLeast(current, min) ? min : '';
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
