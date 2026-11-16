// wallpapermodel.js — one wallpaper per display, as plain functions with no imports (server/tests/wallpaper_model_test.mjs);
// sdk/wallpapers.js re-exports them.
// settings.wallpaper is every display's picture; settings.wallpapers = { [display key]: id } gives a display its own.
// A display's key is its connector (DP-2, HDMI-A-1…), never its model's name: three monitors of one model share
// that. The moving wallpapers keep their own per-display map (settings.motion, sdk/motion.jsx).
export const displayWallpaperKey = (d) => (d && typeof d.connector === 'string' && d.connector ? d.connector : `#${d?.id ?? 0}`);
// the id of the picture display d shows
export function wallpaperIdFor(s, d) {
  const m = s && s.wallpapers && typeof s.wallpapers === 'object' ? s.wallpapers : null;
  const own = m && d ? m[displayWallpaperKey(d)] : null;
  return typeof own === 'string' && own ? own : s?.wallpaper;
}
// The settings patch for choosing picture `id` for display d, or for all of them (d null). For all: the global
// picture, no display of its own, no moving wallpaper. For one: its own picture, and that display's moving
// wallpaper off (null in settings.motion hides the one for all displays there).
export function wallpaperPatch(s, id, d) {
  if (!d) return { wallpaper: id, wallpapers: {}, motion: {} };
  const k = displayWallpaperKey(d);
  return { wallpapers: { ...(s.wallpapers || {}), [k]: id }, motion: { ...(s.motion || {}), [d.id]: null } };
}
// A display's name in a list of several: its model, and its connector too when two share the model.
export function displayLabel(d, list) {
  const name = d?.name || '';
  const twin = (list || []).filter((x) => (x.name || '') === name).length > 1;
  if (d?.connector && (twin || !name)) return name ? `${name} · ${d.connector}` : d.connector;
  return name || String(d?.id ?? '');
}
