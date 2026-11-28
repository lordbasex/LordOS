// wallpapergallery.js — Settings → Wallpaper's gallery (0.8.9.6.2) as plain data, with no imports and no screen
// (server/tests/wallpaper_gallery_test.mjs): the still pictures of the catalog (assets/wallpapers/motion.json, variants
// with codec "jpeg"), the id a chosen one is saved under, which file each display draws for a quality, what is on this
// computer, the collections, the search and its filters with their counts, the sizes, and what "Free up space" may
// delete. sdk/wallpapers.js draws them in every theme; apps/settings/WallpaperGallery.jsx shows the page.
//
// The catalog is the one the closed action wallpaper-get reads as root (server/src/hw/wallpaper_admin.c): a moving
// wallpaper's variants are hevc or h264 (kept as /var/lib/lordos/wallpapers/<sha256>.mp4), a still picture's are jpeg
// (kept as <sha256>.jpg). Nothing here depends on how many items there are.

// The still collections, in the gallery's order; new ones in a later catalog come after them. All three are new in
// 0.8.9.6.2 (the NEW badge).
export const STILL_SECTIONS = ['abstract', 'epic', 'anime'];
export const NEW_COLLECTIONS = ['abstract', 'epic', 'anime'];
// The moving wallpapers' sections (sdk/motion.jsx MOTION_SECTIONS), in the gallery's order.
export const MOTION_ORDER = ['landscape', 'urban', 'earth', 'underwater', 'dynamic'];
export const GALLERY_FILTERS = ['all', 'here', 'still', 'motion', 'mine'];
export const QUALITIES = ['auto', 'hd', '4k'];
// A catalog picture chosen as a wallpaper: settings.wallpaper, settings.wallpapers[connector], the slideshow and the
// login screen ("still cat:<id>", kept in /var/lib/lordos/wallpapers/login-still) all say "cat:<catalog id>".
export const STILL_ID = /^cat:([a-z0-9][a-z0-9-]{0,47})$/;
export const stillId = (id) => `cat:${id}`;
export const stillOf = (id) => (STILL_ID.exec(typeof id === 'string' ? id : '') || [])[1] || null;
// The HD and 4K sizes: Auto takes 4K only on a display wider than 1920 or taller than 1080 pixels.
export const HD = { w: 1920, h: 1080 };

const isJpeg = (v) => !!v && v.codec === 'jpeg';
const num = (x) => (typeof x === 'number' && Number.isFinite(x) && x > 0 ? x : 0);

// A catalog item's still variants, smallest first; [] for a moving wallpaper.
export const stillVariants = (it) => ((it && Array.isArray(it.variants) ? it.variants : []).filter(isJpeg)).sort((a, b) => num(a.w) - num(b.w));
// A catalog item's video variants (what sdk/motion.jsx plays); [] for a still picture.
export const videoVariants = (it) => (it && Array.isArray(it.variants) ? it.variants : []).filter((v) => v && !isJpeg(v));
export const isStill = (it) => stillVariants(it).length > 0 && videoVariants(it).length === 0;
export const stillCatalog = (items) => (items || []).filter(isStill);
export const motionCatalog = (items) => (items || []).filter((it) => videoVariants(it).length > 0);

// The name a variant is kept under in the store, from its codec (as the root side decides it).
export const storeName = (v) => `${v.sha256}.${isJpeg(v) ? 'jpg' : 'mp4'}`;
// The store's listing (lord.fs.list: [{ name, size }]) as name → size.
export function storeIndex(list) {
  const m = new Map();
  for (const e of list || []) if (e && typeof e.name === 'string' && !e.dir) m.set(e.name, num(e.size));
  return m;
}
// Is this variant on this computer: its file, at the catalog's size (wallpaper-get keeps nothing else).
export const onDisk = (v, index) => !!v && !!index && index.get(storeName(v)) === v.size;

// ---- quality ----

export const qualityOf = (q) => (QUALITIES.includes(q) ? q : 'auto');
// A display's size in device pixels (its scale applied, as the themes draw the wallpaper).
export const devicePx = (d) => {
  const k = (num(d && d.scale) || 100) / 100;
  return { w: Math.round(num(d && d.w) * k), h: Math.round(num(d && d.h) * k) };
};
export const autoQuality = (d) => { const p = devicePx(d); return p.w > HD.w || p.h > HD.h ? '4k' : 'hd'; };
export const displayQuality = (q, d) => (qualityOf(q) === 'auto' ? autoQuality(d) : qualityOf(q));
// The variant of an item for a quality: HD is the largest that is at most 1920 × 1080 (or the smallest there is), 4K
// the largest.
export function variantFor(it, q) {
  const list = stillVariants(it);
  if (!list.length) return null;
  if (q === '4k') return list[list.length - 1];
  const hd = list.filter((v) => num(v.w) <= HD.w && num(v.h) <= HD.h);
  return hd.length ? hd[hd.length - 1] : list[0];
}
// The variants to download for these displays (none known: one HD screen), by name, without repeats.
export function variantsToGet(it, q, displays) {
  const ds = displays && displays.length ? displays : [HD];
  const out = [];
  for (const d of ds) {
    const v = variantFor(it, displayQuality(q, d));
    if (v && !out.includes(v.name)) out.push(v.name);
  }
  return out;
}
// What a display of this size draws, given what is on disk: the variant of its quality when it is there, else the
// one on disk nearest to it (the largest below, else the smallest above). null when nothing is on disk.
export function drawnVariant(it, q, d, index) {
  const have = stillVariants(it).filter((v) => onDisk(v, index));
  if (!have.length) return null;
  const want = variantFor(it, displayQuality(q, d));
  if (want && have.includes(want)) return want;
  const below = have.filter((v) => num(v.w) <= num(want && want.w));
  return below.length ? below[below.length - 1] : have[0];
}
// One line per display for the page: [{ display, quality, variant }] (variant: { w, h } of the file it uses).
export function qualityPlan(it, q, displays) {
  return (displays || []).map((d) => {
    const quality = displayQuality(q, d);
    const v = it ? variantFor(it, quality) : null;
    return { display: d, quality, size: v ? { w: v.w, h: v.h } : quality === '4k' ? { w: 3840, h: 2160 } : { ...HD } };
  });
}

// ---- the wallpaper a theme draws ----

// A still picture as a theme's wallpaper entry (sdk/wallpapers.js: { id, category, name, src, thumb, variants }), with
// only the variants on disk; with a quality other than Auto, only that one when it is there. null when nothing is on
// disk: findWallpaper then falls back to the theme's default.
export function stillWallpaper(it, index, { quality = 'auto', store, thumb } = {}) {
  const have = stillVariants(it).filter((v) => onDisk(v, index));
  if (!have.length) return null;
  const q = qualityOf(quality);
  const want = q === 'auto' ? null : variantFor(it, q);
  const use = want && have.includes(want) ? [want] : have;
  const at = (v) => `${store}/${storeName(v)}`;
  return {
    id: stillId(it.id), category: it.section, name: it.name, nameKey: `settings.wallpaperItem.${it.id}`,
    src: at(use[use.length - 1]), thumb: thumb ? thumb(it) : at(use[0]),
    variants: use.map((v) => ({ w: v.w, h: v.h, src: at(v) })),
    author: it.author || '', license: it.license || '', catalog: true,
  };
}

// ---- twins: a built-in picture with a catalog item of the same id ----
// A built-in wallpaper (assets/wallpapers/index.json) whose id is also a still item of the catalog is ONE wallpaper:
// its HD is the built-in file (always here, works offline), its 4K the catalog's largest variant (downloaded when
// wanted). It keeps its plain built-in id in the settings; findWallpaper adds the 4K file on disk to its variants
// (withTwin), so every theme, the login screen and the slideshow pick it per display, and fall back to the built-in
// file while it isn't there. Nothing here names a category: any built-in collection whose ids get catalog twins
// behaves the same.

// The still items that are a built-in's twin, by id, and the ones that are pictures of their own (cat:<id>).
export function twinMap(items, builtinIds) {
  const ids = new Set(builtinIds || []);
  const m = new Map();
  for (const it of stillCatalog(items)) if (ids.has(it.id)) m.set(it.id, it);
  return m;
}
export const catalogOnly = (items, builtinIds) => { const ids = new Set(builtinIds || []); return stillCatalog(items).filter((it) => !ids.has(it.id)); };
// The twin's file that adds something to the built-in: its largest variant, when it is larger than every built-in one.
export function twinExtra(w, twin) {
  const v = variantFor(twin, '4k');
  const own = Math.max(0, ...((w && w.variants) || []).map((x) => num(x.w)));
  return v && num(v.w) > own ? v : null;
}
// The built-in entry as a theme draws it for a quality: Auto, every built-in file and the twin's 4K once on disk;
// HD, the built-in files up to 1920 × 1080; 4K, only files of 3840 or more when there is one here (the twin's once on
// disk, or the built-in's own), else the built-in ones. Never an entry with no file.
export function withTwin(w, twin, index, { quality = 'auto', store } = {}) {
  if (!w || !twin) return w;
  const q = qualityOf(quality);
  const base = (w.variants || []).slice().sort((a, b) => num(a.w) - num(b.w));
  const extra = twinExtra(w, twin);
  const got = extra && onDisk(extra, index) ? [{ w: extra.w, h: extra.h, src: `${store}/${storeName(extra)}` }] : [];
  let vars = [...base, ...got];
  if (q === 'hd') { const hd = base.filter((v) => num(v.w) <= HD.w && num(v.h) <= HD.h); if (hd.length) vars = hd; }
  if (q === '4k') { const big = vars.filter((v) => num(v.w) >= 3840); if (big.length) vars = big; }
  if (!got.length && q === 'auto') return w;
  return { ...w, variants: vars, twin: true };
}

// ---- the gallery's entries ----
// { id, kind: 'still' | 'motion' | 'builtin' | 'mine', col, name, thumb, here, k4, sizes: { hd, k4 }, item }
//   id    what is saved when it is chosen: 'cat:<id>' for a still, the catalog id for a moving one, the theme's id
//   here  on this computer: always for a built-in or one of your pictures

export function stillEntry(it, index, name, thumb) {
  const hd = variantFor(it, 'hd'), k4 = variantFor(it, '4k');
  return {
    id: stillId(it.id), kind: 'still', col: it.section, name: name || it.name || it.id, thumb,
    here: stillVariants(it).some((v) => onDisk(v, index)), k4: !!k4 && num(k4.w) >= 3840,
    sizes: { hd: hd ? num(hd.size) : 0, k4: k4 && k4 !== hd ? num(k4.size) : 0 }, item: it,
    author: it.author || '', license: it.license || '',
  };
}
// A moving wallpaper's sizes are its first codec's (HEVC before H.264, as sdk/motionmodel.js pickVariant).
// Is a moving wallpaper on this computer: the variant settings.motionFiles names (or, with none named, any of its
// videos) is in the store at the catalog's size. A file named after the catalog's hash but of another size (the ws170 QA
// found 13 bytes of text named after a video's hash) is not: it counts as not downloaded, and choosing it fetches it
// again (wallpaper-get checks the size and the SHA-256 of what is there and replaces what isn't the catalog's).
export function motionHere(it, index, files) {
  const named = files && files[it.id] ? videoVariants(it).find((v) => v.name === files[it.id]) : null;
  return named ? onDisk(named, index) : videoVariants(it).some((v) => onDisk(v, index));
}
export function motionEntry(it, index, name, thumb, files) {
  const vids = videoVariants(it);
  const codec = vids.some((v) => v.codec === 'hevc') ? 'hevc' : vids[0] ? vids[0].codec : '';
  const mine = vids.filter((v) => v.codec === codec).sort((a, b) => num(a.w) - num(b.w));
  const hd = mine.filter((v) => num(v.w) <= HD.w).pop() || mine[0], k4 = mine.find((v) => num(v.w) >= 3840) || null;
  return {
    id: it.id, kind: 'motion', col: it.section, name: name || it.name || it.id, thumb,
    here: motionHere(it, index, files), k4: !!k4,
    sizes: { hd: hd ? num(hd.size) : 0, k4: k4 ? num(k4.size) : 0 }, item: it,
    author: it.author || '', license: it.license || '',
  };
}
// twin: its catalog item, if any (then the 4K is that item's largest file, downloaded on demand: k4Here says if it is).
export function builtinEntry(w, name, twin = null, index = null) {
  const extra = twin ? twinExtra(w, twin) : null;
  const own4k = (w.variants || []).some((v) => num(v.w) >= 3840);
  return {
    id: w.id, kind: 'builtin', col: w.category || 'other', name: name || w.name || w.id, thumb: w.thumb || w.src,
    here: true, k4: own4k || !!extra, sizes: { hd: 0, k4: extra ? num(extra.size) : 0 }, item: w, author: 'LordOS', license: 'MIT',
    twin: extra ? twin : null, k4Here: own4k || (!!extra && onDisk(extra, index)),
  };
}
export const mineEntry = (w, name) => ({
  id: w.id, kind: 'mine', col: 'mine', name: name || w.name || w.id, thumb: w.thumb || w.src, here: true, k4: false,
  sizes: { hd: 0, k4: 0 }, item: w, author: '', license: '',
});

// ---- search and filters ----

// Lowercase without accents, so "epico" finds "Épico".
export const foldText = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
// Every word of the query is in the name, the id, the collection's id or its (translated) name.
export function matchesQuery(e, query, colName = (c) => c) {
  const words = foldText(query).split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const hay = foldText([e.name, e.id, e.col, colName(e.col)].join(' '));
  return words.every((w) => hay.includes(w));
}
export function matchesFilter(e, filter) {
  if (filter === 'here') return e.here;
  if (filter === 'still') return e.kind === 'still' || e.kind === 'builtin';
  if (filter === 'motion') return e.kind === 'motion';
  if (filter === 'mine') return e.kind === 'mine';
  return e.kind !== 'mine';                              // All: the collections (your pictures have their own filter)
}
export const filterItems = (list, { filter = 'all', query = '', colName } = {}) =>
  (list || []).filter((e) => matchesFilter(e, filter) && matchesQuery(e, query, colName));
export function filterCounts(list, query = '', colName) {
  const out = {};
  for (const f of GALLERY_FILTERS) out[f] = filterItems(list, { filter: f, query, colName }).length;
  return out;
}

// ---- collections ----

// The order of the collections: the still ones (abstract, epic, anime, then any newer), the built-in categories in the
// order given (WALLPAPER_CATEGORIES, then a theme's own), then the moving ones.
export function collectionOrder(list, builtinOrder = []) {
  const kinds = new Map();
  for (const e of list || []) if (e.kind !== 'mine' && !kinds.has(e.col)) kinds.set(e.col, e.kind);
  const rank = (c) => {
    const k = kinds.get(c);
    if (k === 'still') return [0, STILL_SECTIONS.includes(c) ? STILL_SECTIONS.indexOf(c) : STILL_SECTIONS.length];
    if (k === 'builtin') return [1, builtinOrder.includes(c) ? builtinOrder.indexOf(c) : builtinOrder.length];
    return [2, MOTION_ORDER.includes(c) ? MOTION_ORDER.indexOf(c) : MOTION_ORDER.length];
  };
  return [...kinds.keys()].sort((a, b) => {
    const x = rank(a), y = rank(b);
    return x[0] - y[0] || x[1] - y[1] || (a < b ? -1 : a > b ? 1 : 0);
  });
}
// [[col, entries]] in that order, for the entries given (a search or a filter).
export function groupCollections(list, order) {
  const by = new Map();
  for (const e of list || []) { if (!by.has(e.col)) by.set(e.col, []); by.get(e.col).push(e); }
  return (order || [...by.keys()]).filter((c) => by.has(c)).map((c) => [c, by.get(c)]);
}
export const collectionKind = (entries) => (entries && entries[0] ? entries[0].kind : 'builtin');
// A collection's card: its count, how many are here, the three thumbnails of its stack.
export function collectionCard(col, entries) {
  return { col, kind: collectionKind(entries), count: entries.length, here: entries.filter((e) => e.here).length,
    thumbs: entries.slice(0, 3).map((e) => e.thumb), isNew: NEW_COLLECTIONS.includes(col) };
}
// What "Download all" still has to fetch, in bytes, for HD and for 4K (the files not on this computer).
export function collectionBytes(entries) {
  let hd = 0, k4 = 0, missing = 0;
  for (const e of entries || []) {
    if (e.kind === 'builtin' && e.twin && !e.k4Here) { missing++; k4 += e.sizes.k4; continue; }   // HD is built in
    if (e.here || (e.kind !== 'still' && e.kind !== 'motion')) continue;
    missing++;
    hd += e.sizes.hd || 0;
    k4 += e.sizes.k4 || e.sizes.hd || 0;
  }
  return { hd, k4, missing };
}
// The still pictures "Download all" fetches for a quality: [{ it, name }], the ones already here left out.
export function downloadPlan(entries, q, index) {
  const out = [];
  for (const e of entries || []) {
    if (e.kind === 'builtin' && e.twin && q === '4k') {
      const v = twinExtra(e.item, e.twin);
      if (v && !onDisk(v, index)) out.push({ it: e.twin, name: v.name, size: num(v.size) });
      continue;
    }
    if (e.kind !== 'still') continue;
    const v = variantFor(e.item, q === '4k' ? '4k' : 'hd');
    if (v && !onDisk(v, index)) out.push({ it: e.item, name: v.name, size: num(v.size) });
  }
  return out;
}

// ---- the disk ----

// Every catalog file on this computer: [{ it, v, size }] (another user's downloads too: the store is shared).
export function filesOnDisk(items, index) {
  const out = [];
  for (const it of items || []) for (const v of it.variants || []) if (onDisk(v, index)) out.push({ it, v, size: num(v.size) });
  return out;
}
export const usedBytes = (items, index) => filesOnDisk(items, index).reduce((a, f) => a + f.size, 0);
// The catalog ids this user's settings show now: the desktop (every display), the login screen, the moving wallpapers,
// and every picture of a slideshow's collection while it is on. A built-in id counts for its twin (the same id).
// defaults: what shows while a setting is unset (the theme's default wallpaper, the login screen's).
export function idsInUse(s, items, defaults = []) {
  const out = new Set();
  const add = (id) => { if (typeof id !== 'string' || !id) return; out.add(stillOf(id) || id); };
  add(s && s.wallpaper);
  if (!(s && s.wallpaper)) add(defaults[0]);
  if (!(s && s.loginWallpaper)) add(defaults[1]);
  for (const v of Object.values((s && s.wallpapers) || {})) add(v);
  add(s && s.loginWallpaper);
  for (const v of Object.values((s && s.motion) || {})) add(v);
  if (s && s.motion && Object.values(s.motion).includes('random')) for (const id of Object.keys(s.motionFiles || {})) add(id);
  const sl = s && s.slideshow;
  if (sl && sl.on === true && typeof sl.category === 'string') for (const it of items || []) if (isStill(it) && it.section === sl.category) add(it.id);
  return out;
}
// What "Free up space" deletes: every catalog file on disk whose item isn't in use. [{ it, v, size }]
export const freeUpPlan = (items, index, inUse) => filesOnDisk(items, index).filter((f) => !inUse.has(f.it.id));
