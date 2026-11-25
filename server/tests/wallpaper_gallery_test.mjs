// wallpaper_gallery_test.mjs — Settings → Wallpaper's gallery (sdk/wallpapergallery.js): the catalog's still pictures
// (codec jpeg) apart from the moving wallpapers, the cat:<id> ids, Auto/HD/4K per display, what each display draws from
// what is on disk, the entries, the search and its filters with counts, the collections, the sizes, what Free up space
// deletes (never one in use), and the shipped catalog. node server/tests/wallpaper_gallery_test.mjs
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  STILL_SECTIONS, GALLERY_FILTERS, stillId, stillOf, stillVariants, videoVariants, isStill, stillCatalog, motionCatalog, storeName,
  storeIndex, onDisk, qualityOf, autoQuality, displayQuality, variantFor, variantsToGet, drawnVariant, qualityPlan, stillWallpaper,
  stillEntry, motionEntry, builtinEntry, mineEntry, matchesQuery, filterItems, filterCounts, collectionOrder, groupCollections,
  collectionCard, collectionBytes, downloadPlan, filesOnDisk, usedBytes, idsInUse, freeUpPlan, twinMap, catalogOnly, twinExtra, withTwin,
} from '../../sdk/wallpapergallery.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
const H = (c) => c.repeat(64);
const still = (id, section, a = 'a', b = 'b') => ({
  id, section, name: id.replace(/-/g, ' '), author: 'LordOS (AI-generated)', license: 'CC0',
  variants: [
    { name: '2160.jpeg', codec: 'jpeg', w: 3840, h: 2160, fps: 0, seconds: 0, file: `stills/${section}/${id}/2160.jpg`, size: 1200, sha256: H(b) },
    { name: '1080.jpeg', codec: 'jpeg', w: 1920, h: 1080, fps: 0, seconds: 0, file: `stills/${section}/${id}/1080.jpg`, size: 400, sha256: H(a) },
  ],
});
const video = (id, section) => ({
  id, section, name: id, author: 'NASA', license: 'Public domain',
  variants: [
    { name: '1080.hevc', codec: 'hevc', w: 1920, h: 1080, file: `${id}/1080.hevc.mp4`, size: 3000, sha256: H('c') },
    { name: '2160.hevc', codec: 'hevc', w: 3840, h: 2160, file: `${id}/2160.hevc.mp4`, size: 9000, sha256: H('d') },
    { name: '1080.h264', codec: 'h264', w: 1920, h: 1080, file: `${id}/1080.h264.mp4`, size: 5000, sha256: H('e') },
  ],
});
const king = still('epic-01-king', 'epic', '1', '2'), lake = still('abstract-lake', 'abstract', '3', '4'), sakura = still('anime-sakura', 'anime', '5', '6');
const earth = video('earth-orbit', 'earth');
const catalog = [earth, king, lake, sakura];

// ---- ids and kinds ----
eq('a still id', stillId('epic-01-king'), 'cat:epic-01-king');
eq('back to the catalog id', stillOf('cat:epic-01-king'), 'epic-01-king');
eq('not a still id', [stillOf('crystal-crown'), stillOf('image:0123456789abcdef'), stillOf('cat:'), stillOf('cat:../x'), stillOf('cat:A'), stillOf(null)], [null, null, null, null, null, null]);
eq('still variants, smallest first', stillVariants(king).map((v) => v.name), ['1080.jpeg', '2160.jpeg']);
eq('a video has no still variants', stillVariants(earth), []);
eq('a still has no video variants (motion ignores jpeg)', videoVariants(king), []);
eq('kinds', [isStill(king), isStill(earth)], [true, false]);
eq('the still catalog', stillCatalog(catalog).map((i) => i.id), ['epic-01-king', 'abstract-lake', 'anime-sakura']);
eq('the moving catalog', motionCatalog(catalog).map((i) => i.id), ['earth-orbit']);
eq('kept as .jpg (a picture) or .mp4 (a video)', [storeName(king.variants[0]), storeName(earth.variants[0])], [`${H('2')}.jpg`, `${H('c')}.mp4`]);

// ---- on disk ----
const idx = storeIndex([{ name: `${H('1')}.jpg`, size: 400 }, { name: `${H('2')}.jpg`, size: 7 }, { name: `${H('c')}.mp4`, size: 3000 }, { name: 'login-still', size: 16 }, { name: 'sub', dir: true }]);
eq('on disk: the catalog size', onDisk(king.variants[1], idx), true);
eq('a file of another size is not it', onDisk(king.variants[0], idx), false);
eq('nothing listed', onDisk(king.variants[1], storeIndex([])), false);

// ---- quality ----
const FHD = { id: 1, connector: 'HDMI-A-1', w: 1920, h: 1080 }, UHD = { id: 2, connector: 'DP-1', w: 3840, h: 2160 };
const WQ = { id: 3, connector: 'DP-2', w: 2560, h: 1440 }, TALL = { id: 4, connector: 'eDP-1', w: 1920, h: 1200 };
const HIDPI = { id: 5, connector: 'eDP-2', w: 1440, h: 900, scale: 200 };
eq('auto: HD up to 1920 × 1080', autoQuality(FHD), 'hd');
eq('auto: 4K above 1920 wide', [autoQuality(WQ), autoQuality(UHD)], ['4k', '4k']);
eq('auto: 4K above 1080 tall', autoQuality(TALL), '4k');
eq('auto: device pixels (scale 200 %)', autoQuality(HIDPI), '4k');
eq('a forced quality wins', [displayQuality('hd', UHD), displayQuality('4k', FHD), displayQuality('bogus', FHD)], ['hd', '4k', 'hd']);
eq('quality: unknown is auto', [qualityOf('x'), qualityOf('4k')], ['auto', '4k']);
eq('the variant of a quality', [variantFor(king, 'hd').name, variantFor(king, '4k').name], ['1080.jpeg', '2160.jpeg']);
eq('to get for two displays on auto: both', variantsToGet(king, 'auto', [FHD, UHD]), ['1080.jpeg', '2160.jpeg']);
eq('to get on HD: one file', variantsToGet(king, 'hd', [FHD, UHD]), ['1080.jpeg']);
eq('to get with no display known: HD', variantsToGet(king, 'auto', []), ['1080.jpeg']);
const both = storeIndex([{ name: `${H('1')}.jpg`, size: 400 }, { name: `${H('2')}.jpg`, size: 1200 }]);
const hdOnly = storeIndex([{ name: `${H('1')}.jpg`, size: 400 }]);
const k4Only = storeIndex([{ name: `${H('2')}.jpg`, size: 1200 }]);
eq('drawn: each display its own on auto', [drawnVariant(king, 'auto', FHD, both).name, drawnVariant(king, 'auto', UHD, both).name], ['1080.jpeg', '2160.jpeg']);
eq('drawn: 4K wanted, only HD here: HD', drawnVariant(king, 'auto', UHD, hdOnly).name, '1080.jpeg');
eq('drawn: HD wanted, only 4K here: 4K', drawnVariant(king, 'hd', FHD, k4Only).name, '2160.jpeg');
eq('drawn: nothing here', drawnVariant(king, 'auto', FHD, storeIndex([])), null);
eq('the plan per display', qualityPlan(king, 'auto', [UHD, FHD]).map((p) => [p.display.connector, p.quality, p.size.w]), [['DP-1', '4k', 3840], ['HDMI-A-1', 'hd', 1920]]);

// ---- what a theme draws ----
const STORE = '/var/lib/lordos/wallpapers';
const thumb = (it) => `asset:wallpapers/motion/${it.id}.thumb.jpg`;
const w = stillWallpaper(king, both, { store: STORE, thumb });
eq('a theme entry: the cat id, its collection as category', [w.id, w.category], ['cat:epic-01-king', 'epic']);
eq('auto: every variant on disk, smallest first', w.variants.map((v) => v.w), [1920, 3840]);
eq('the files in the store', w.variants[0].src, `${STORE}/${H('1')}.jpg`);
eq('the thumbnail ships with the package', w.thumb, 'asset:wallpapers/motion/epic-01-king.thumb.jpg');
eq('HD: only the HD file', stillWallpaper(king, both, { quality: 'hd', store: STORE, thumb }).variants.map((v) => v.w), [1920]);
eq('4K: only the 4K file', stillWallpaper(king, both, { quality: '4k', store: STORE, thumb }).variants.map((v) => v.w), [3840]);
eq('4K wanted but not here: what is', stillWallpaper(king, hdOnly, { quality: '4k', store: STORE, thumb }).variants.map((v) => v.w), [1920]);
eq('nothing on disk: no entry (the theme\'s default shows)', stillWallpaper(king, storeIndex([]), { store: STORE, thumb }), null);

// ---- entries, search, filters ----
const builtins = [{ id: 'crystal-crown', category: 'signature', name: 'Crystal Crown', thumb: 't1', variants: [{ w: 3840, h: 2160 }] },
  { id: 'lumo-celestial', category: 'lumo', name: 'Celestial', thumb: 't2', variants: [] }];
const files = { 'earth-orbit': '1080.hevc' };
const list = [
  ...stillCatalog(catalog).map((it) => stillEntry(it, hdOnly, it.name, thumb(it))),
  ...builtins.map((b) => builtinEntry(b, b.name)),
  ...motionCatalog(catalog).map((it) => motionEntry(it, storeIndex([]), 'Orbit', thumb(it), files)),
  mineEntry({ id: 'image:0123456789abcdef', thumb: 'x', name: 'Your picture' }, 'Your picture'),
];
const byId = Object.fromEntries(list.map((e) => [e.id, e]));
eq('a still: here once a file is', [byId['cat:epic-01-king'].here, byId['cat:abstract-lake'].here], [true, false]);
eq('a still: 4K badge and both sizes', [byId['cat:epic-01-king'].k4, byId['cat:epic-01-king'].sizes], [true, { hd: 400, k4: 1200 }]);
eq('a built-in: always here, 4K from its variants', [byId['crystal-crown'].here, byId['crystal-crown'].k4, byId['lumo-celestial'].k4], [true, true, false]);
eq('a moving one: here by settings.motionFiles, HEVC sizes', [byId['earth-orbit'].here, byId['earth-orbit'].sizes], [true, { hd: 3000, k4: 9000 }]);
const names = { epic: 'Épicos', abstract: 'Abstractos', anime: 'Anime', signature: 'Signature', lumo: 'Lumo', earth: 'La Tierra' };
const colName = (c) => names[c] || c;
eq('search: the name', filterItems(list, { query: 'king' }).map((e) => e.id), ['cat:epic-01-king']);
eq('search: the collection\'s translated name, without accents', filterItems(list, { query: 'epicos', colName }).map((e) => e.id), ['cat:epic-01-king']);
eq('search: every word', filterItems(list, { query: 'anime sakura', colName }).map((e) => e.id), ['cat:anime-sakura']);
eq('search: nothing', filterItems(list, { query: 'zzz' }), []);
eq('filter: on this computer', filterItems(list, { filter: 'here' }).map((e) => e.id), ['cat:epic-01-king', 'crystal-crown', 'lumo-celestial', 'earth-orbit', 'image:0123456789abcdef']);
eq('filter: pictures (stills and built-ins)', filterItems(list, { filter: 'still' }).length, 5);
eq('filter: moving', filterItems(list, { filter: 'motion' }).map((e) => e.id), ['earth-orbit']);
eq('filter: my photos', filterItems(list, { filter: 'mine' }).map((e) => e.id), ['image:0123456789abcdef']);
eq('filter: all leaves my photos to their own filter', filterItems(list, { filter: 'all' }).length, 6);
eq('counts', filterCounts(list, ''), { all: 6, here: 5, still: 5, motion: 1, mine: 1 });
eq('counts with a query', filterCounts(list, 'king'), { all: 1, here: 1, still: 1, motion: 0, mine: 0 });
eq('every filter counted', Object.keys(filterCounts(list)), GALLERY_FILTERS);
eq('matches the id too', matchesQuery(byId['cat:anime-sakura'], 'cat:anime'), true);

// ---- collections ----
const order = collectionOrder(list, ['signature', 'lumo', 'cosmos']);
eq('order: stills (abstract, epic, anime), built-ins, moving', order, ['abstract', 'epic', 'anime', 'signature', 'lumo', 'earth']);
eq('a newer still section goes after the known ones', collectionOrder([{ kind: 'still', col: 'cars' }, { kind: 'still', col: 'epic' }]), ['epic', 'cars']);
eq('the still sections', STILL_SECTIONS, ['abstract', 'epic', 'anime']);
const groups = groupCollections(filterItems(list, { query: 'a' }), order);
eq('groups in order, only those with entries', groups.map(([c]) => c)[0], 'abstract');
const card = collectionCard('epic', groups.find(([c]) => c === 'epic')[1]);
eq('a card: count, here, new badge', [card.count, card.here, card.isNew, card.kind, card.thumbs.length], [1, 1, true, 'still', 1]);
eq('a built-in card is not new', collectionCard('signature', [byId['crystal-crown']]).isNew, false);
const stills = list.filter((e) => e.kind === 'still');
eq('download all: what is missing (HD, 4K)', collectionBytes(stills), { hd: 800, k4: 2400, missing: 2 });
eq('download all: the plan for HD leaves out what is here', downloadPlan(stills, 'hd', hdOnly).map((p) => [p.it.id, p.name]), [['abstract-lake', '1080.jpeg'], ['anime-sakura', '1080.jpeg']]);
eq('download all: the plan for 4K', downloadPlan(stills, '4k', hdOnly).map((p) => p.name), ['2160.jpeg', '2160.jpeg', '2160.jpeg']);

// ---- the disk ----
const disk = storeIndex([{ name: `${H('1')}.jpg`, size: 400 }, { name: `${H('2')}.jpg`, size: 1200 }, { name: `${H('3')}.jpg`, size: 400 },
  { name: `${H('c')}.mp4`, size: 3000 }, { name: `${H('9')}.jpg`, size: 99 }]);
eq('files on disk: only the catalog\'s', filesOnDisk(catalog, disk).map((f) => f.v.name), ['1080.hevc', '2160.jpeg', '1080.jpeg', '1080.jpeg']);
eq('space used', usedBytes(catalog, disk), 3000 + 1200 + 400 + 400);
const s = { wallpaper: 'cat:epic-01-king', wallpapers: { 'DP-1': 'crystal-crown' }, loginWallpaper: 'cat:abstract-lake', motion: { '*': 'earth-orbit' }, motionFiles: {} };
eq('in use: desktop, login, motion', [...idsInUse(s, catalog)].sort(), ['abstract-lake', 'crystal-crown', 'earth-orbit', 'epic-01-king']);
eq('free up space never deletes one in use', freeUpPlan(catalog, disk, idsInUse(s, catalog)).map((f) => f.it.id), []);
eq('free up space: the rest', freeUpPlan(catalog, disk, idsInUse({ wallpaper: 'cat:epic-01-king' }, catalog)).map((f) => f.it.id), ['earth-orbit', 'abstract-lake']);
eq('a slideshow keeps its collection', [...idsInUse({ slideshow: { on: true, category: 'abstract' } }, catalog)], ['abstract-lake']);
eq('a slideshow that is off keeps nothing', [...idsInUse({ slideshow: { on: false, category: 'abstract' } }, catalog)], []);
eq('random keeps every downloaded video', [...idsInUse({ motion: { '*': 'random' }, motionFiles: { 'earth-orbit': '1080.hevc' } }, catalog)].sort(), ['earth-orbit', 'random']);

// ---- twins: a built-in picture and the catalog's item of the same id ----
const pano = { id: 'cyberpunk-panorama', category: 'cyberpunk', name: 'Panorama', thumb: 'tp', src: 'asset:wallpapers/cyberpunk/p.jpg',
  variants: [{ w: 1920, h: 1080, src: 'asset:wallpapers/cyberpunk/p.jpg' }] };
const road = { id: 'cyberpunk-road', category: 'cyberpunk', name: 'Road', thumb: 'tr', src: 'asset:r.jpg', variants: [{ w: 1920, h: 1080, src: 'asset:r.jpg' }, { w: 3840, h: 2160, src: 'asset:r4.jpg' }] };
const panoTwin = still('cyberpunk-panorama', 'cyberpunk', '7', '8'), roadTwin = still('cyberpunk-road', 'cyberpunk', 'a', 'b');
const cat2 = [...catalog, panoTwin, roadTwin];
const builtinIds = ['cyberpunk-panorama', 'cyberpunk-road', 'crystal-crown'];
const twins = twinMap(cat2, builtinIds);
eq('twins: the catalog items with a built-in id', [...twins.keys()], ['cyberpunk-panorama', 'cyberpunk-road']);
eq('twins are not pictures of their own (no cat: duplicate)', catalogOnly(cat2, builtinIds).map((i) => i.id), ['epic-01-king', 'abstract-lake', 'anime-sakura']);
eq('a twin adds its 4K to an HD built-in', twinExtra(pano, panoTwin).name, '2160.jpeg');
eq('a built-in with its own 4K gets nothing more', twinExtra(road, roadTwin), null);
const k4Here = storeIndex([{ name: `${H('8')}.jpg`, size: 1200 }]);
const none = storeIndex([]);
const tw = (q, idx) => withTwin(pano, panoTwin, idx, { quality: q, store: STORE });
eq('auto, 4K not downloaded: the built-in alone (never a missing picture)', tw('auto', none), pano);
eq('auto, 4K here: both, each display takes the one that covers it', tw('auto', k4Here).variants.map((v) => v.w), [1920, 3840]);
eq('auto, 4K here: the store file', tw('auto', k4Here).variants[1].src, `${STORE}/${H('8')}.jpg`);
eq('the id stays the built-in\'s', tw('auto', k4Here).id, 'cyberpunk-panorama');
eq('HD: the built-in file only, even with 4K here', tw('hd', k4Here).variants.map((v) => v.src), ['asset:wallpapers/cyberpunk/p.jpg']);
eq('4K, here: only the 4K file', tw('4k', k4Here).variants.map((v) => v.w), [3840]);
eq('4K, not downloaded: falls back to the built-in HD', tw('4k', none).variants.map((v) => v.w), [1920]);
eq('no twin: unchanged', withTwin(builtins[0], null, k4Here, { store: STORE }), builtins[0]);
const pe = builtinEntry(pano, 'Panorama', panoTwin, none), pe4 = builtinEntry(pano, 'Panorama', panoTwin, k4Here);
eq('one tile: built-in, here (HD), 4K badge, the 4K size', [pe.kind, pe.here, pe.k4, pe.sizes.k4, pe.k4Here], ['builtin', true, true, 1200, false]);
eq('its 4K once downloaded', pe4.k4Here, true);
eq('the collection: one entry per picture', groupCollections([pe, builtinEntry(road, 'Road', roadTwin, none)], ['cyberpunk']).map(([c, es]) => [c, es.length]), [['cyberpunk', 2]]);
eq('download all · 4K: the twins\' 4K still missing', collectionBytes([pe, pe4]), { hd: 0, k4: 1200, missing: 1 });
eq('download all · 4K plan', downloadPlan([pe], '4k', none).map((p) => [p.it.id, p.name]), [['cyberpunk-panorama', '2160.jpeg']]);
eq('download all · HD: nothing (built in)', downloadPlan([pe], 'hd', none), []);
eq('free up: a twin shown on a display stays', freeUpPlan(cat2, k4Here, idsInUse({ wallpapers: { 'DP-1': 'cyberpunk-panorama' } }, cat2)).map((f) => f.it.id), []);
eq('free up: the theme\'s default (setting unset) stays', freeUpPlan(cat2, k4Here, idsInUse({ wallpaper: null }, cat2, ['cyberpunk-panorama'])).length, 0);
eq('free up: a twin nobody shows goes (the built-in HD stays)', freeUpPlan(cat2, k4Here, idsInUse({ wallpaper: 'crystal-crown' }, cat2, ['lumo-celestial'])).map((f) => f.it.id), ['cyberpunk-panorama']);
eq('free up: a slideshow of that collection keeps it', freeUpPlan(cat2, k4Here, idsInUse({ wallpaper: 'x', slideshow: { on: true, category: 'cyberpunk' } }, cat2)).length, 0);

// ---- the shipped catalog ----
const path = fileURLToPath(new URL('../../assets/wallpapers/motion.json', import.meta.url));
if (existsSync(path)) {
  const shipped = JSON.parse(readFileSync(path, 'utf8'));
  const st = stillCatalog(shipped.items), mv = motionCatalog(shipped.items);
  eq('shipped: every item is a still or a moving wallpaper', st.length + mv.length, shipped.items.length);
  eq('shipped: ids fit cat:<id>', st.every((it) => stillOf(stillId(it.id)) === it.id), true);
  eq('shipped: every still has HD and 4K', st.every((it) => variantFor(it, 'hd').w === 1920 && variantFor(it, '4k').w === 3840), true);
  // a picture of its own is in a still section; a built-in's twin has the built-in's id and its category as section
  const index = JSON.parse(readFileSync(fileURLToPath(new URL('../../assets/wallpapers/index.json', import.meta.url)), 'utf8'));
  const builtin = new Map(index.map((e) => [e.id, e.category]));
  eq('shipped: every still is in a known section, or the twin of a built-in in its category',
    st.filter((it) => !(STILL_SECTIONS.includes(it.section) || builtin.get(it.id) === it.section)).map((it) => it.id), []);
  console.log(`shipped twins of built-in pictures: ${twinMap(shipped.items, [...builtin.keys()]).size}`);
  const thumbs = (it) => existsSync(fileURLToPath(new URL(`../../assets/wallpapers/motion/${it.id}.thumb.jpg`, import.meta.url)));
  eq('shipped: every item has its thumbnail', shipped.items.filter((it) => !thumbs(it)).map((it) => it.id), []);
  console.log(`shipped catalog: ${st.length} still pictures, ${mv.length} moving wallpapers`);
}

console.log(bad ? `wallpaper-gallery-test: ${bad} FAILED` : `wallpaper-gallery-test: ${ok} ok`);
if (bad) process.exit(1);
