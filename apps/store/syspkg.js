// syspkg.js — how Store → Updates (and its plans) name LordOS's own data packages: in words instead of
// the package name. Only presentation; what gets installed is the same. The lordos package itself has
// its own card (LordOSCard in debian.js). No imports, so tests/store_syspkg_test.mjs can load it.
//
//   sysPkgKey(name)       the t() key prefix for a known system package, or null
//   snapshotDate(version) the date of a lordos-hwdata version (YYYY.MM.DD or YYYY.MM.DD.N,
//                         tools/iso/mkdeb-hwdata.sh) as a local Date, or null when it isn't one
//   companionsOf(name, known) the Debian packages the Store installs with this one (those known(p) says the index
//                         has): what an app needs to work in LordOS that Debian leaves to Recommends or to nothing

const KEYS = { 'lordos-hwdata': 'store.updates.hwdata' };

export const sysPkgKey = (name) => (typeof name === 'string' && Object.prototype.hasOwnProperty.call(KEYS, name) ? KEYS[name] : null);

export function snapshotDate(version) {
  const m = /^(\d{4})\.(\d{2})\.(\d{2})(?:\.[1-9]\d?)?$/.exec(typeof version === 'string' ? version : '');
  if (!m) return null;
  const y = Number(m[1]), mo = Number(m[2]) - 1, d = Number(m[3]);
  const date = new Date(y, mo, d);
  return date.getFullYear() === y && date.getMonth() === mo && date.getDate() === d ? date : null;
}

// OBS Studio from Debian (Flathub compatibility run, 2026-11-22): without obs-plugins it has no screen capture, no
// encoders and no browser source; without qt6-wayland its Qt can't use the Wayland display and it falls back to
// Xwayland, where it can't capture through the ScreenCast portal. Both are installed with it (and, for an OBS installed
// before, by its page's Update).
const COMPANIONS = { 'obs-studio': ['obs-plugins', 'qt6-wayland'] };

export function companionsOf(name, known = () => true) {
  const list = typeof name === 'string' && Object.prototype.hasOwnProperty.call(COMPANIONS, name) ? COMPANIONS[name] : [];
  return list.filter((p) => known(p));
}
