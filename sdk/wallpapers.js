// The LordOS wallpaper collection (assets/wallpapers/, built by tools/gen-wallpapers.mjs; see its
// README.md). Themes add it to their own wallpapers (tokens `wallpapers`), and Settings → Wallpaper
// groups them by `category`. Each entry: { id, category, nameKey, name, src, thumb, variants }.
//   src       the base image (the original size), `asset:` path
//   thumb     320×180 for grids (Settings, Control Center); never decode the full image for those
//   variants  [{ w, h, src }] from small to large: wallpaperSrc() picks the one that fits a screen
import index from '../assets/wallpapers/index.json';

// The desktop of a new user (sdk/settings.js DEFAULTS) and the fallback for an unknown id.
export const DEFAULT_WALLPAPER = 'lumo-celestial';
// The login screen's background (themes/greeter; the remote login page serves the same image
// through themes/greeter/assets/background.jpg, a link to it).
export const GREETER_WALLPAPER = 'crystal-crown';

// Categories in the order Settings shows them; a theme's own wallpapers come after.
export const WALLPAPER_CATEGORIES = ['signature', 'lumo', 'cosmos', 'gradients'];

// English fallback for an id: "crystal-crown" → "Crystal Crown", "lumo-celestial" → "Celestial"
const englishName = (id) => id.replace(/^lumo-/, '').split('-').map((w) => w[0].toUpperCase() + w.slice(1)).join(' ');

export const lordosWallpapers = index.map((e) => ({
  id: e.id,
  category: e.category,
  nameKey: `shell.wallpaper.${e.id}`,
  name: englishName(e.id),
  src: `asset:wallpapers/${e.file}`,
  thumb: `asset:wallpapers/${e.thumb}`,
  variants: e.variants.map((v) => ({ w: v.w, h: v.h, src: `asset:wallpapers/${v.file}` })),
}));

// The image to draw a wallpaper with on an area of `width`×`height` device pixels (objectFit
// 'cover'): the smallest variant that covers the area, or the largest one there is. With only the
// original (1672×941 today) that is always the original; a later @3840x2160 file is used by itself
// on screens bigger than the original.
export function wallpaperSrc(w, width, height) {
  const list = (w && w.variants) || [];
  if (!list.length) return w ? w.src : null;
  for (const v of list) {
    const s = Math.max(width / v.w, height / v.h);    // cover scale
    if (s <= 1) return v.src;
  }
  return list[list.length - 1].src;
}

// A picture of the user's own (Settings → Wallpaper): the id is `image:<name>`, and the copy lives in their home, made
// by the wallpaper-image action. It works in every theme, because themes all go through findWallpaper and wallpaperSrc.
export const USER_WALLPAPER = /^image:([0-9a-f]{16})$/;
export const userWallpaperName = (id) => (USER_WALLPAPER.exec(id || '') || [])[1] || null;
function userWallpaper(id) {
  const name = userWallpaperName(id);
  if (!name) return null;
  const base = `${lord.fs.home()}/.local/share/lordos/wallpapers/${name}`;
  try { if (!lord.fs.stat(`${base}.jpg`)) return null; } catch (e) { return null; }    // deleted: the default one
  return { id, category: 'mine', nameKey: 'settings.wallpaperImage.name', name: 'Your picture', src: `${base}.jpg`, thumb: `${base}.thumb.jpg` };
}

// The wallpaper chosen in the settings, among a theme's list.
export const findWallpaper = (list, id) => (list || []).find((w) => w.id === id) || userWallpaper(id)
  || (list || []).find((w) => w.id === DEFAULT_WALLPAPER) || (list || [])[0] || null;

// The small image for a picker (falls back to the full one for theme wallpapers without a thumb).
export const wallpaperThumb = (w) => (w && (w.thumb || w.src)) || null;
