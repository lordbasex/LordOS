// The LordOS wallpaper collection (assets/wallpapers/, built by tools/gen-wallpapers.mjs; see its
// README.md). Themes add it to their own wallpapers (tokens `wallpapers`), and Settings → Wallpaper
// groups them by `category`. Each entry: { id, category, nameKey, name, src, thumb, variants }.
//   src       the base image (the original size), `asset:` path
//   thumb     320×180 for grids (Settings, Control Center); never decode the full image for those
//   variants  [{ w, h, src }] from small to large: wallpaperSrc() picks the one that fits a screen
import index from '../assets/wallpapers/index.json';
import catalog from '../assets/wallpapers/motion.json';
import { DEFAULT_WALLPAPER } from './wallpapermodel.js';
import { catalogOnly, twinMap, stillOf, stillWallpaper, storeIndex, withTwin } from './wallpapergallery.js';
import { settings } from './settings.js';
import { createStore } from './store.js';

// The desktop of a user who never chose one (settings.wallpaper null, sdk/settings.js) and the fallback for an
// unknown id, unless the theme names its own (theme.json "wallpaper": { "default" }, tokens.defaultWallpaper: Lord
// Workspaces' is Cyberpunk City HD's panorama).
export { DEFAULT_WALLPAPER } from './wallpapermodel.js';
// The login screen's background (themes/greeter; the remote login page serves the same image
// through themes/greeter/assets/background.jpg, a link to it).
export const GREETER_WALLPAPER = 'crystal-crown';

// Categories in the order Settings shows them; a theme's own wallpapers come after.
export const WALLPAPER_CATEGORIES = ['signature', 'lumo', 'cosmos', 'cyberpunk', 'gradients'];

// English fallback for an id: "crystal-crown" → "Crystal Crown", "lumo-celestial" → "Celestial",
// "cyberpunk-sky-terrace" → "Sky Terrace"
const englishName = (id) => id.replace(/^(lumo|cyberpunk)-/, '').split('-').map((w) => w[0].toUpperCase() + w.slice(1)).join(' ');

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

// A still picture of the catalog (Settings → Wallpaper's Abstract, Epic and Anime collections, 0.8.9.6.2): the id is
// `cat:<catalog id>`, and its files are the ones wallpaper-get keeps in /var/lib/lordos/wallpapers/<sha256>.jpg (shared
// by every user, read-only for them). Only the variants on disk are offered, so each display draws the one that covers
// it (wallpaperSrc: Auto) or the one settings.wallpaperQuality names ('hd', '4k'). Not on disk (removed, or another
// computer's settings): null, and the theme's default shows. Works in every theme, on the login screen and in the
// slideshow, which all go through findWallpaper. The rules are in wallpapergallery.js.
export const STILL_STORE = '/var/lib/lordos/wallpapers';
// The catalog's pictures of their own (cat:<id>), and its twins of built-in pictures: an item with a built-in's id is
// that built-in's 4K (Cyberpunk City HD today), never a second wallpaper. The rules are in wallpapergallery.js.
export const stillItems = catalogOnly(catalog.items, index.map((e) => e.id));
export const twinItems = twinMap(catalog.items, index.map((e) => e.id));
export const stillThumb = (it) => `asset:wallpapers/motion/${it.id}.thumb.jpg`;
// The store's listing, read again at most every 10 s (a theme asks on each render), or at once after a change here.
export const stillStore = createStore(0);           // bumped when the listing changes: downloads, removals
let storeList = null, storeAt = 0;
export function stillFiles(fresh = false) {
  if (fresh || !storeList || Date.now() - storeAt > 10000) {
    let list = [];
    try { list = lord.fs.list(STILL_STORE) || []; } catch (e) { list = []; }
    storeList = storeIndex(list);
    storeAt = Date.now();
  }
  return storeList;
}
export function refreshStillFiles() { stillFiles(true); stillStore.set((n) => n + 1); }
const quality = () => { try { return settings.get().wallpaperQuality; } catch (e) { return 'auto'; } };
function catalogWallpaper(id) {
  const cid = stillOf(id);
  if (!cid) return null;
  const it = stillItems.find((x) => x.id === cid);
  return it ? stillWallpaper(it, stillFiles(), { quality: quality(), store: STILL_STORE, thumb: stillThumb }) : null;
}
// Every catalog picture on this computer, as wallpaper entries (the slideshow's collections, sdk/slideshow.jsx).
export const downloadedStills = () => stillItems.map((it) => stillWallpaper(it, stillFiles(), { quality: quality(), store: STILL_STORE, thumb: stillThumb })).filter(Boolean);

// The wallpaper chosen in the settings, among a theme's list; def: the theme's default (tokens.defaultWallpaper).
// A built-in picture with a catalog twin comes with the twin's 4K file once it is on disk (withTwin), per quality.
export const withTwinFile = (w) => (w && twinItems.has(w.id) ? withTwin(w, twinItems.get(w.id), stillFiles(), { quality: quality(), store: STILL_STORE }) : w);
const builtinFind = (list, id) => withTwinFile((list || []).find((w) => w.id === id) || null);
export const findWallpaper = (list, id, def = DEFAULT_WALLPAPER) => builtinFind(list, id || def) || userWallpaper(id) || catalogWallpaper(id)
  || builtinFind(list, def) || builtinFind(list, DEFAULT_WALLPAPER) || withTwinFile((list || [])[0] || null);

// The small image for a picker (falls back to the full one for theme wallpapers without a thumb).
export const wallpaperThumb = (w) => (w && (w.thumb || w.src)) || null;

// One wallpaper per display (every theme): the rules are in wallpapermodel.js, tested with node.
export { displayWallpaperKey, wallpaperIdFor, wallpaperPatch, displayLabel } from './wallpapermodel.js';
