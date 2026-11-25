// Settings → Wallpaper (0.8.9.6.2): a gallery, like Appearance's desktop styles. At the top what shows now (a large
// picture, its name, collection and license) with For (Desktop / Login screen), Display (All / each one) and Quality
// (Auto / HD / 4K); then a search with the filters All, On this computer, Pictures, Moving and My photos (each with its
// count); the collections as cards and one row of each; a collection's own page (Download all, Use as slideshow); a
// sheet per wallpaper (a large picture, its credits, the real sizes of its files, Use on desktop / Login screen / Only
// this display); and at the bottom the space the downloads take (Free up space never deletes one in use), the
// thumbnails' size, the slideshow and the moving wallpapers' options.
//
// Every collection is here: LordOS's built-in pictures (assets/wallpapers/index.json), the catalog's still pictures
// (Abstract, Epic, Anime: downloaded when chosen, saved as "cat:<id>", sdk/wallpapers.js), the moving wallpapers (a
// compatible GPU only, sdk/motion.jsx) and your own pictures. The pure parts are sdk/wallpapergallery.js.
import {
  View, Text, Image, useTheme, useSettings, useLocale, tOptional, useStore, direction, useAppWindow, useKeyboard, Button, Slider, SearchField, Segmented,
  RadioGroup, Layer, FilePicker, wm, spacing, typeScale, settings, findWallpaper, wallpaperThumb, wallpaperIdFor, wallpaperPatch, slideshowStop,
  slideshowPatch, slideshowPrefs, themes, displayLabel, displayFor, WALLPAPER_CATEGORIES, GREETER_WALLPAPER, DEFAULT_WALLPAPER, setLoginStill, setLoginImage,
  addWallpaperImage, removeWallpaperImage, userWallpaperName, MOTION_RATES, motionBusy, motionItems, motionThumb, motionDownloads, useCodecs,
  downloadMotion, removeMotion, setLoginMotion, variantFor as motionVariantFor, userVideoProblem, media, screenDisplays, errorText,
  stillItems, twinItems, stillThumb, stillStore, stillFiles, fetchCatalogFile, removeCatalogFile, formatBytes,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import {
  GALLERY_FILTERS, QUALITIES, stillOf, qualityOf, variantsToGet, qualityPlan, stillEntry, motionEntry, builtinEntry, mineEntry, filterItems,
  filterCounts, collectionOrder, groupCollections, collectionCard, collectionBytes, downloadPlan, usedBytes, idsInUse, freeUpPlan, twinExtra,
  displayQuality,
} from '../../sdk/wallpapergallery.js';
import { Stack, Section, SettingRow, Note, Actions, PopUp, tint } from './ui.jsx';
import { subPage } from './subpage.js';
import { SlideshowSettings } from './Slideshow.jsx';

const GAP = 10, CARD = 170, CARD_GAP = 12;
// collection cards fill their row: as many columns of at least CARD px as fit, each stretched to share the width
export const cardWidth = (avail) => { const cols = Math.max(1, Math.floor((avail + CARD_GAP) / (CARD + CARD_GAP))); return Math.floor((avail - (cols - 1) * CARD_GAP) / cols); };
// The grids' thumbnail width (issue #58, Thumbnail size): 80-240 px, 100 by default; 16:9 like the thumbnails.
const thumbWidth = (s) => Math.max(80, Math.min(240, Math.round(+s.wallpaperThumbs) || 100));
// A theme names its wallpapers with a locale key (nameKey) and an English fallback (name).
const wallpaperName = (w) => (w && w.nameKey && tOptional(w.nameKey)) || (w && w.name) || '';
// every catalog item: its own pictures, the built-ins' 4K twins, the moving wallpapers
const allCatalog = () => [...stillItems, ...twinItems.values(), ...motionItems];
const sizeText = (w, h) => `${w} × ${h}`;

// The width of a box after layout (the page column narrows with the window), as Appearance's gallery does.
function useWidth(ref, fallback) {
  const [w, setW] = useState(fallback);
  useEffect(() => {
    const measure = () => {
      const n = ref.current;
      if (!n) return;
      const x = Math.round(lord.getLayout(n.id)[2]);
      if (x > 0) setW((old) => (Math.abs(x - old) > 1 ? x : old));
    };
    measure();
    const id = setTimeout(measure, 60);
    return () => clearTimeout(id);
  });
  return w;
}

// While a chosen wallpaper is starting; it turns only then.
function TileSpinner() {
  const [a, setA] = useState(0);
  useEffect(() => { const id = setInterval(() => setA((x) => (x + 30) % 360), 80); return () => clearInterval(id); }, []);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.45)' }}>
      <Text style={{ fontSize: 22, color: '#fff', transform: `rotate(${a}deg)` }}>⟳</Text>
    </View>
  );
}

function Badge({ label }) {
  return (
    <View style={{ paddingHorizontal: 4, height: 14, borderRadius: 4, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.62)' }}>
      <Text style={{ fontSize: 9, fontWeight: 700, color: '#fff' }}>{label}</Text>
    </View>
  );
}

// One wallpaper: its thumbnail, its state (☁ downloads when chosen, ✓ on this computer), 4K or HD, ▶ when it moves, and
// the download's progress along its bottom edge.
function Tile({ e, tw, on, percent, busy, dim, onOpen }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const th = Math.round(tw * 9 / 16);
  const loading = percent != null;
  return (
    <View onClick={onOpen} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ width: tw + 4, gap: 5, opacity: dim ? 0.45 : 1 }}>
      <View style={{ width: tw + 4, height: th + 4, borderRadius: 9, border: `2px solid ${on ? t.accent : hover ? t.separator : 'transparent'}` }}>
        <View style={{ width: tw, height: th, borderRadius: 7, overflow: 'hidden', border: `1px solid ${t.separator}` }}>
          <Image src={e.thumb} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          {busy ? <TileSpinner /> : null}
          {e.kind === 'mine' ? null : (
            <View style={{ position: 'absolute', top: 4, insetInlineEnd: 4, flexDirection: 'row', gap: 3 }}>
              {e.kind === 'motion' ? <Badge label="▶" /> : null}
              <Badge label={tr(e.k4 ? 'settings.wallpaperGallery.quality4k' : 'settings.wallpaperGallery.qualityHd')} />
            </View>
          )}
          {loading || e.kind === 'mine' ? null : (
            <View style={{ position: 'absolute', bottom: 4, insetInlineStart: 4, width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center',
              backgroundColor: e.here ? tint.ok : 'rgba(0,0,0,0.62)' }}>
              <Text style={{ fontSize: 10, fontWeight: 700, color: e.here ? '#08240f' : '#fff' }}>{e.here ? '✓' : '☁'}</Text>
            </View>
          )}
          {loading ? (
            <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 4, backgroundColor: 'rgba(0,0,0,0.5)', direction: 'ltr' }}>
              <View style={{ width: `${Math.max(2, percent)}%`, height: 4, backgroundColor: t.accent }} />
            </View>
          ) : null}
        </View>
      </View>
      <Text style={{ fontSize: typeScale.caption, color: on ? t.text : t.textDim, fontWeight: on ? 600 : 400, whiteSpace: 'nowrap', overflow: 'hidden',
        textOverflow: 'ellipsis', maxWidth: tw + 4, textAlign: 'start' }}>{e.name}</Text>
    </View>
  );
}

function Grid({ entries, ctx, limit }) {
  const shown = limit ? entries.slice(0, limit) : entries;
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP, paddingVertical: spacing.m }}>
      {shown.map((e) => (
        <Tile key={e.id} e={e} tw={ctx.tw} on={ctx.current === e.id} percent={ctx.downloads[e.item?.id ?? '']} busy={ctx.busy && ctx.current === e.id && e.kind === 'motion'}
          dim={e.kind === 'motion' && !ctx.canPlay} onOpen={() => ctx.open(e)} />
      ))}
    </View>
  );
}

// A collection's card: a stack of three thumbnails, its name (NEW for this release's), its count and kind.
function CollectionCard({ card, name, onOpen, w = CARD }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const thumbs = [...card.thumbs].reverse();
  // the stack scales with the card (its width fills the row: see the grid below)
  const inner = w - 18, sw = Math.round(inner * 0.78), sh = Math.round(sw * 9 / 16), step = Math.round(inner * 0.11);
  const at = [{ right: 0, top: 0, opacity: 0.55 }, { right: step, top: Math.round(step * 0.55), opacity: 0.8 }, { left: 0, bottom: 0, opacity: 1 }].slice(3 - thumbs.length);
  return (
    <View onClick={onOpen} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: w, padding: 8, gap: 6, borderRadius: 11, border: `1px solid ${hover ? t.accent : t.separator}`, backgroundColor: hover ? t.hover : t.control }}>
      <View style={{ width: inner, height: Math.round(inner * 9 / 16), direction: 'ltr' }}>
        {thumbs.map((src, i) => (
          <View key={i} style={{ position: 'absolute', ...at[i], width: sw, height: sh, borderRadius: 7, overflow: 'hidden', border: `2px solid ${t.control}` }}>
            <Image src={src} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          </View>
        ))}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{name}</Text>
        {card.isNew ? (
          <View style={{ paddingHorizontal: 5, height: 15, borderRadius: 5, justifyContent: 'center', backgroundColor: t.accent }}>
            <Text style={{ fontSize: 9, fontWeight: 700, color: '#fff' }}>{tr('settings.wallpaperGallery.new')}</Text>
          </View>
        ) : null}
      </View>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`settings.wallpaperGallery.cardMeta.${card.kind}`, { count: card.count })}</Text>
    </View>
  );
}

function Link({ label, onClick }) {
  const t = useTheme();
  return <Text onClick={onClick} style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.accent, whiteSpace: 'nowrap' }}>{label}</Text>;
}

// The sheet of one wallpaper, over this Settings window.
// onStep(-1 | 1): the previous or next wallpaper of the list shown (a collection, or the gallery's filter and search),
// from the arrows on the picture or the ← and → keys; null at either end.
// a round arrow on one side of the sheet's picture (the picture box is LTR: start is its left edge)
function StepArrow({ side, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={(ev) => { ev?.stopPropagation?.(); onClick(); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ position: 'absolute', top: '50%', marginTop: -18, [side === 'start' ? 'left' : 'right']: 10, width: 36, height: 36, borderRadius: 18,
        alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? 'rgba(0,0,0,0.75)' : 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.25)' }}>
      <Text style={{ fontSize: 22, lineHeight: 1, marginTop: -2, color: '#fff' }}>{side === 'start' ? '‹' : '›'}</Text>
    </View>
  );
}

function Sheet({ e, ctx, onClose, onStep }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const [q, setQ] = useState(ctx.quality);
  useEffect(() => { setQ(ctx.quality); }, [e.id]);       // a new wallpaper starts from the page's quality again
  useKeyboard((ev) => {
    if (ev.key === 'Escape') onClose();
    // ← and → as the arrows draw them (mirrored in right-to-left)
    else if ((ev.key === 'ArrowLeft' || ev.key === 'ArrowRight') && onStep) onStep((ev.key === 'ArrowRight') !== (direction() === 'rtl') ? 1 : -1);
  });
  const g = wm.frame(wm.get(win.id) || {}) || {};
  const W = g.w || 720, H = g.h || 560;
  const wide = W >= 700;
  const pw = wide ? Math.min(480, Math.round(W * 0.55)) : Math.min(W - 64, 480), ph = Math.round(pw * 9 / 16);
  const percent = ctx.downloads[e.item?.id ?? ''];
  const still = e.kind === 'still', motion = e.kind === 'motion';
  const blocked = motion && !ctx.canPlay;
  const local = e.kind === 'builtin' ? e.item.src : still && e.here ? findWallpaper([], e.id)?.src : e.kind === 'mine' ? e.item.src : null;
  const src = local ? `thumb:${Math.round(pw * 2)}:${local}` : e.thumb;
  const credit = still ? tr('settings.wallpaperGallery.credit.still')
    : motion ? `${e.author} · ${e.license}`
      : e.kind === 'builtin' ? tr('settings.wallpaperGallery.credit.builtin') : tr('settings.wallpaperImage.hint');
  const opts = [['auto', tr('settings.wallpaperGallery.option.auto')]];
  if (e.twin) opts.push(['hd', tr('settings.wallpaperGallery.option.builtinHd')]);    // HD: the built-in file
  else if (e.sizes.hd) opts.push(['hd', tr('settings.wallpaperGallery.option.hd', { size: formatBytes(e.sizes.hd) })]);
  if (e.sizes.k4) opts.push(['4k', tr('settings.wallpaperGallery.option.4k', { size: formatBytes(e.sizes.k4) })]);
  const only = ctx.onlyDisplay;
  const inUse = ctx.inUse.has(e.item?.id) || ctx.inUse.has(e.id);
  return (
    <Layer name="modal">
      <View onClick={onClose} style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: W, height: H, alignItems: 'center', justifyContent: 'center',
        backgroundColor: 'rgba(0,0,0,0.45)' }}>
        <View onClick={(ev) => ev?.stopPropagation?.()} style={{ flexDirection: wide ? 'row' : 'column', maxWidth: W - 32, maxHeight: H - 32, borderRadius: 14, overflow: 'hidden',
          backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
          <View style={{ width: pw, height: ph, backgroundColor: '#000', direction: 'ltr' }}>
            <Image src={src} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
            {onStep ? <StepArrow side="start" onClick={() => onStep(direction() === 'rtl' ? 1 : -1)} /> : null}
            {onStep ? <StepArrow side="end" onClick={() => onStep(direction() === 'rtl' ? -1 : 1)} /> : null}
          </View>
          <View style={{ width: wide ? Math.min(320, W - 64 - pw) : pw, padding: spacing.l, gap: spacing.m }}>
            <View style={{ gap: 2 }}>
              <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{e.name}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
                {`${ctx.colName(e.col)} · ${tr(motion ? 'settings.wallpaperGallery.kindMotion' : 'settings.wallpaperGallery.kindStill')}`}
              </Text>
            </View>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{credit}</Text>
            {still || motion || e.twin ? <RadioGroup value={q} onChange={setQ} options={opts} /> : null}
            {percent != null ? (
              <View style={{ gap: 4 }}>
                <View style={{ height: 6, borderRadius: 3, backgroundColor: t.control, overflow: 'hidden', direction: 'ltr' }}>
                  <View style={{ width: `${Math.max(2, percent)}%`, height: 6, backgroundColor: t.accent }} />
                </View>
                <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('settings.wallpaperGallery.downloading', { percent })}</Text>
              </View>
            ) : null}
            {blocked ? <Note>{tr('settings.motion.needsGpu')}</Note> : null}
            {ctx.note ? <Note tone="warn">{ctx.note}</Note> : null}
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s }}>
              <Button primary small disabled={blocked || percent != null} label={tr(e.here ? 'settings.wallpaperGallery.useDesktop' : 'settings.wallpaperGallery.downloadUse')}
                onClick={() => ctx.use(e, 'desktop', q)} />
              <Button small disabled={blocked || percent != null} label={tr('settings.wallpaperGallery.useLogin')} onClick={() => ctx.use(e, 'login', q)} />
              {only ? <Button small disabled={blocked || percent != null} label={tr('settings.wallpaperGallery.onlyDisplay', { display: only.label })} onClick={() => ctx.use(e, 'display', q)} /> : null}
            </View>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, alignItems: 'center' }}>
              {((still || motion) && e.here || (e.twin && e.k4Here)) && !inUse ? <Button small label={tr('settings.motion.remove')} onClick={() => ctx.remove(e)} /> : null}
              {e.kind === 'mine' && !inUse ? <Button small label={tr('settings.wallpaperImage.remove')} onClick={() => ctx.remove(e)} /> : null}
              <Link label={tr('settings.wallpaperGallery.close')} onClick={onClose} />
            </View>
          </View>
        </View>
      </View>
    </Layer>
  );
}

// Your own pictures (My photos): add one, choose it, remove it. On the desktop a copy in your home is enough; the login
// screen gets one of its own.
const MAX_IMAGES = 12;
function MyPhotos({ entries, ctx }) {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const win = useAppWindow();
  const [picking, setPicking] = useState(false);
  const [busy, setBusy] = useState(false);
  const ids = s.wallpaperImages || [];
  const add = (path) => {
    setPicking(false);
    if (!path) return;
    setBusy(true);
    ctx.setNote('');
    addWallpaperImage(path).then((id) => {
      set({ wallpaperImages: [id, ...ids.filter((x) => x !== id)].slice(0, MAX_IMAGES) });
      return ctx.use(mineEntry({ id }, ''), ctx.login ? 'login' : 'desktop', 'auto');
    }).catch((e) => ctx.setNote(errorText(e))).finally(() => setBusy(false));
  };
  const g = picking ? wm.frame(wm.get(win.id) || {}) : null;
  return (
    <Section title={tr('settings.wallpaperCategory.mine')} footer={tr('settings.wallpaperImage.hint')}>
      {entries.length ? <Grid entries={entries} ctx={ctx} /> : null}
      <Actions style={{ paddingVertical: spacing.s }}>
        <Button small disabled={busy} label={tr(busy ? 'settings.wallpaperImage.working' : 'settings.wallpaperImage.choose')} onClick={() => setPicking(true)} />
        {ctx.login && userWallpaperName(s.loginWallpaper) ? <Button small label={tr('settings.motion.loginDefault')} onClick={ctx.loginDefault} /> : null}
      </Actions>
      {picking ? (
        <Layer name="modal">
          <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 640, height: g.h || 480 }}>
            <FilePicker computer title={tr('settings.wallpaperImage.pick')} filter={/\.(png|jpe?g)$/i} onClose={() => setPicking(false)} onPick={(p) => add([].concat(p)[0])} />
          </View>
        </Layer>
      ) : null}
    </Section>
  );
}

// The moving wallpapers' own options: speed, random, an added video, none, credits.
function MotionOptions({ ctx }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const [credits, setCredits] = useState(false);
  const [pick, setPick] = useState(false);
  const motion = s.motion || {};
  const addFile = (path) => {
    setPick(false);
    if (!path) return;
    media.probe(path).then((info) => {
      const why = userVideoProblem(info, ctx.codecs);
      if (why) { ctx.setNote(tr(`settings.motion.problem.${why}`)); return; }
      const id = `user:${Date.now()}`;
      const m = { ...motion, [ctx.scopeKey]: id };
      set({ motionUser: [...(s.motionUser || []), { id, name: path.split('/').pop(), path }], motion: m, ...slideshowStop(s) });
    });
  };
  const choose = (id) => { const m = { ...motion }; if (id) m[ctx.scopeKey] = id; else delete m[ctx.scopeKey]; set({ motion: m, ...(id ? slideshowStop(s) : null) }); };
  return (
    <Section title={tr('settings.motion.title')} footer={ctx.canPlay ? tr('settings.motion.hint') : undefined}>
      {!ctx.canPlay ? <Note>{tr('settings.motion.needsGpu')}</Note> : null}
      {ctx.canPlay && !ctx.login ? (
        <SettingRow label={tr('settings.motion.speed')}>
          <Segmented small options={MOTION_RATES.map((r) => [r, `${r / 100}×`])} value={MOTION_RATES.includes(s.motionRate) ? s.motionRate : 100} onChange={(v) => set({ motionRate: v })} />
        </SettingRow>
      ) : null}
      <Actions style={{ paddingVertical: spacing.s }}>
        {ctx.canPlay && ctx.login ? <Button small label={tr('settings.motion.loginDefault')} onClick={ctx.loginDefault} /> : null}
        {ctx.canPlay && !ctx.login && motion[ctx.scopeKey] ? <Button small label={tr('settings.motion.none')} onClick={() => choose(null)} /> : null}
        {ctx.canPlay && !ctx.login ? <Button small label={tr('settings.motion.random')} disabled={Object.keys(s.motionFiles || {}).length < 2} onClick={() => choose('random')} /> : null}
        {ctx.canPlay && !ctx.login ? <Button small label={tr('settings.motion.add')} onClick={() => setPick(true)} /> : null}
        <Button small label={tr('settings.motion.credits')} onClick={() => setCredits(!credits)} />
      </Actions>
      {credits ? (
        <View style={{ gap: 6, paddingVertical: spacing.s }}>
          {motionItems.map((it) => (
            <Text key={it.id} style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{`${tOptional(`settings.motion.item.${it.id}`) || it.name} — ${it.author} — ${it.license} — ${it.source}`}</Text>
          ))}
        </View>
      ) : null}
      {pick ? <FilePicker onPick={(p) => addFile(Array.isArray(p) ? p[0] : p)} onClose={() => setPick(false)} /> : null}
    </Section>
  );
}

// The space the downloads take, and Free up space (never one in use by this account).
function Space({ ctx }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const [ask, setAsk] = useState(false);
  const [working, setWorking] = useState(false);
  const [done, setDone] = useState('');
  const items = allCatalog();
  const used = usedBytes(items, ctx.index);
  const plan = freeUpPlan(items, ctx.index, ctx.inUse);
  const size = plan.reduce((a, f) => a + f.size, 0);
  const free = async () => {
    setWorking(true);
    setDone('');
    ctx.setNote('');
    let freed = 0;
    try {
      for (const f of plan) { await removeCatalogFile(f.it, f.v.name); freed += f.size; }
      const gone = new Set(plan.map((f) => f.it.id));
      const files = Object.fromEntries(Object.entries(s.motionFiles || {}).filter(([id]) => !gone.has(id)));
      set({ motionFiles: files });
      setDone(tr('settings.wallpaperGallery.freed', { size: formatBytes(freed) }));
    } catch (e) { ctx.setNote(errorText(e)); }
    setWorking(false);
    setAsk(false);
  };
  return (
    <Section title={tr('settings.wallpaperGallery.spaceTitle')} footer={tr('settings.wallpaperGallery.spaceHint')}>
      <View style={{ gap: spacing.s, paddingVertical: spacing.m }}>
        <View style={{ height: 7, borderRadius: 4, backgroundColor: t.control, overflow: 'hidden', direction: 'ltr' }}>
          <View style={{ width: `${Math.min(100, (used / (1024 * 1024 * 1024)) * 100)}%`, height: 7, backgroundColor: t.accent }} />
        </View>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('settings.wallpaperGallery.spaceUsed', { size: formatBytes(used) })}</Text>
        {done ? <Text style={{ fontSize: typeScale.callout, color: tint.ok, textAlign: 'start' }}>{done}</Text> : null}
        {ask ? (
          <View style={{ gap: spacing.s }}>
            <Text style={{ fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('settings.wallpaperGallery.freeConfirm', { count: plan.length, size: formatBytes(size) })}</Text>
            <Actions start>
              <Button small destructive disabled={working} label={tr('settings.wallpaperGallery.freeDo')} onClick={free} />
              <Button small disabled={working} label={tr('settings.wallpaperGallery.cancel')} onClick={() => setAsk(false)} />
            </Actions>
          </View>
        ) : (
          <Actions start>
            <Button small disabled={!plan.length} label={tr('settings.wallpaperGallery.freeUp')} onClick={() => { setDone(''); setAsk(true); }} />
          </Actions>
        )}
      </View>
    </Section>
  );
}

// A collection's own page.
function CollectionPage({ col, entries, ctx, onBack }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const [getting, setGetting] = useState(false);
  useKeyboard((ev) => { if (ev.key === 'Escape' && !ctx.sheetOpen) onBack(); });
  const kind = entries[0] ? entries[0].kind : 'builtin';
  const here = entries.filter((e) => e.here).length;
  const bytes = collectionBytes(entries);
  const credit = tr(`settings.wallpaperGallery.credit.${kind === 'motion' ? 'motion' : kind === 'still' ? 'still' : 'builtin'}`);
  const desc = tOptional(`settings.wallpaperGallery.desc.${col}`);
  const canShow = ctx.slideshow && kind !== 'motion' && here >= 2;
  const showing = slideshowPrefs(s).on && slideshowPrefs(s).category === col;
  const getAll = async (q) => {
    setGetting(true);
    ctx.setNote('');
    try {
      for (const p of downloadPlan(entries, q, ctx.index)) await fetchCatalogFile(p.it, p.name);
    } catch (e) { ctx.setNote(errorText(e)); }
    setGetting(false);
  };
  return (
    <View style={{ gap: spacing.l }}>
      {/* where you are, as a browser shows it: "‹ Wallpaper › Anime"; the first part goes back (so do Esc and the mouse's Back) */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <View focusable onClick={onBack} onKeyDown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') onBack(); }}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8, paddingVertical: 4, marginInlineStart: -8, borderRadius: 6 }}>
          <Text style={{ fontSize: 17, color: t.accent, transform: direction() === 'rtl' ? 'scaleX(-1)' : undefined }}>‹</Text>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.accent }}>{tr('settings.section.wallpaper')}</Text>
        </View>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>›</Text>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{ctx.colName(col)}</Text>
      </View>
      <Section>
        <View style={{ gap: spacing.m, paddingVertical: spacing.m }}>
          <View style={{ gap: 3 }}>
            <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 700, color: t.text, textAlign: 'start' }}>{ctx.colName(col)}</Text>
            {desc ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{desc}</Text> : null}
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
              {`${tr('settings.wallpaperGallery.collectionInfo', { count: entries.length, here })} · ${credit}`}
            </Text>
          </View>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s }}>
            {kind === 'builtin' && bytes.missing ? (
              <Button small primary disabled={getting} label={tr('settings.wallpaperGallery.downloadAll', { quality: tr('settings.wallpaperGallery.quality4k'), size: formatBytes(bytes.k4) })} onClick={() => getAll('4k')} />
            ) : null}
            {kind === 'still' && bytes.missing ? (
              <>
                <Button small disabled={getting} label={tr('settings.wallpaperGallery.downloadAll', { quality: tr('settings.wallpaperGallery.qualityHd'), size: formatBytes(bytes.hd) })} onClick={() => getAll('hd')} />
                <Button small primary disabled={getting} label={tr('settings.wallpaperGallery.downloadAll', { quality: tr('settings.wallpaperGallery.quality4k'), size: formatBytes(bytes.k4) })} onClick={() => getAll('4k')} />
              </>
            ) : null}
            {ctx.slideshow && kind !== 'motion' ? (
              <Button small disabled={!canShow || showing || ctx.login} label={tr('settings.wallpaperGallery.useSlideshow')} onClick={() => set(slideshowPatch(s, { on: true, category: col }))} />
            ) : null}
          </View>
        </View>
        {ctx.slideshow && kind === 'still' && here < 2 ? <Note>{tr('settings.wallpaperGallery.slideshowNeedsTwo')}</Note> : null}
      </Section>
      {ctx.note ? <Note tone="warn">{ctx.note}</Note> : null}
      <Grid entries={entries} ctx={ctx} />
    </View>
  );
}

export function WallpaperGallery() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const win = useAppWindow();
  const codecs = useCodecs();
  const downloads = useStore(motionDownloads);
  const busy = useStore(motionBusy);
  useStore(stillStore);                                // a download or a removal lists the store again
  const [target, setTarget] = useState('desktop');
  const [scope, setScope] = useState('*');
  const [note, setNote] = useState('');
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState('all');
  const view = useStore(subPage);                      // a collection's page, or null: the gallery (in Settings' back/forward history)
  const setView = (v) => subPage.set(v || null);
  const [sheet, setSheet] = useState(null);            // the id of the wallpaper whose sheet is open
  const page = useRef(null);
  const width = useWidth(page, 640);
  const list = t.wallpapers || [];
  if (!list.length) return <Stack><Note>{tr('settings.noWallpapers')}</Note></Stack>;   // after every hook (React's order)

  const login = target === 'login';
  const canPlay = codecs.length > 0;
  const displays = screenDisplays() || [];
  const scopeDisplay = !login && displays.length > 1 ? displays.find((d) => d.id === scope) || null : null;
  const scopeKey = scopeDisplay ? scopeDisplay.id : '*';
  const quality = qualityOf(s.wallpaperQuality);
  const index = stillFiles();
  const tw = thumbWidth(s);
  const colName = (c) => tOptional(`settings.wallpaperCategory.${c}`) || tr('settings.wallpaperCategory.other');

  // every wallpaper as a gallery entry
  const entries = [
    ...stillItems.map((it) => stillEntry(it, index, tOptional(`settings.wallpaperItem.${it.id}`) || it.name, stillThumb(it))),
    ...list.map((w) => builtinEntry(w, wallpaperName(w), twinItems.get(w.id) || null, index)),
    ...motionItems.map((it) => motionEntry(it, index, tOptional(`settings.motion.item.${it.id}`) || it.name, motionThumb(it), s.motionFiles || {})),
    ...(s.wallpaperImages || []).map((id) => findWallpaper([], id)).filter(Boolean).map((w) => mineEntry(w, tr('settings.wallpaperImage.name'))),
  ];

  // what shows now, for the target and the display chosen
  const motion = s.motion || {};
  const motionNow = login ? (motionItems.some((it) => it.id === s.loginWallpaper) ? s.loginWallpaper : null)
    : canPlay ? (scopeDisplay && scopeDisplay.id in motion ? motion[scopeDisplay.id] : motion['*']) || null : null;
  const desk = wallpaperIdFor(s, scopeDisplay, t.defaultWallpaper);
  const shownId = login ? s.loginWallpaper : desk;
  const stillNow = findWallpaper(list, login ? (list.some((w) => w.id === shownId) || userWallpaperName(shownId) || stillOf(shownId) ? shownId : GREETER_WALLPAPER) : desk, t.defaultWallpaper);
  const nowEntry = (motionNow && entries.find((e) => e.id === motionNow)) || entries.find((e) => e.id === stillNow?.id)
    || (stillNow ? builtinEntry(stillNow, wallpaperName(stillNow), twinItems.get(stillNow.id) || null, index) : null);
  const current = nowEntry ? nowEntry.id : null;
  const inUse = idsInUse(s, allCatalog(), [t.defaultWallpaper || DEFAULT_WALLPAPER, GREETER_WALLPAPER]);
  for (const id of [s.wallpaper, ...Object.values(s.wallpapers || {}), s.loginWallpaper]) if (id) inUse.add(id);

  const slideshow = !!themes.info()?.wallpaperSlideshow;
  const winDisplay = (() => { try { return displayFor(wm.frame(wm.get(win.id) || {})); } catch (e) { return null; } })();
  const onlyD = displays.length > 1 ? scopeDisplay || winDisplay : null;
  const loginDefault = () => setLoginMotion(null).then(() => { set({ loginWallpaper: null }); setNote(tr('settings.motion.loginReset')); }, (e) => setNote(errorText(e)));

  // Choosing: downloads what it needs first (a still: the files the quality wants for the displays it goes on).
  const use = async (e, where, q) => {
    setNote('');
    const d = where === 'display' ? onlyD : where === 'desktop' ? scopeDisplay : null;
    const ds = d ? [d] : displays;
    try {
      if (e.kind === 'motion') {
        const it = e.item;
        if (!(s.motionFiles || {})[it.id]) {
          const box = q === '4k' ? { w: 3840, h: 2160 } : q === 'hd' ? { w: 1920, h: 1080 } : (d || displays[0] || { w: 1920, h: 1080 });
          const v = motionVariantFor(it, codecs, box);
          if (!v) return;
          await downloadMotion(it, v);
        }
        if (where === 'login') { await setLoginMotion(it); set({ loginWallpaper: it.id }); setNote(tr('settings.motion.loginDone')); }
        else { const m = { ...(settingsNow().motion || {}) }; m[d ? d.id : '*'] = it.id; set({ motion: m, ...slideshowStop(settingsNow()) }); }
      } else {
        // a built-in with a catalog twin: its HD is here; the twin's 4K is fetched when a display it goes on wants it
        const extra = e.kind === 'builtin' && e.twin ? twinExtra(e.item, e.twin) : null;
        if (extra && !e.k4Here && (where === 'login' ? displays : ds).some((x) => displayQuality(q, x) === '4k')) await fetchCatalogFile(e.twin, extra.name);
        if (e.kind === 'still') for (const name of variantsToGet(e.item, q, ds)) {
          const v = e.item.variants.find((x) => x.name === name);
          if (v && !(index.get(`${v.sha256}.jpg`) === v.size)) await fetchCatalogFile(e.item, name);
        }
        if (where === 'login') {
          await (e.kind === 'mine' ? setLoginImage(e.id) : setLoginStill(e.id));
          set({ loginWallpaper: e.id });
          setNote(tr('settings.wallpaperImage.loginDone'));
        } else {
          const now = settingsNow();
          set({ ...wallpaperPatch(now, e.id, d), ...slideshowStop(now) });
        }
      }
      setSheet(null);
    } catch (err) { setNote(errorText(err)); }
  };
  const remove = async (e) => {
    setNote('');
    try {
      if (e.kind === 'motion') await removeMotion(e.item);
      else if (e.kind === 'builtin' && e.twin) { const v = twinExtra(e.item, e.twin); if (v) await removeCatalogFile(e.twin, v.name); }
      else if (e.kind === 'still') { for (const v of e.item.variants) if (index.get(`${v.sha256}.jpg`) === v.size) await removeCatalogFile(e.item, v.name); }
      else if (e.kind === 'mine') {
        await removeWallpaperImage(e.id);
        const now = settingsNow();
        const own = Object.fromEntries(Object.entries(now.wallpapers || {}).filter(([, v]) => v !== e.id));
        set({ wallpaperImages: (now.wallpaperImages || []).filter((x) => x !== e.id), wallpapers: own, ...(now.wallpaper === e.id ? { wallpaper: null } : {}) });
      }
      setSheet(null);
    } catch (err) { setNote(errorText(err)); }
  };
  const ctx = {
    tw, current, downloads, busy, canPlay, codecs, index, quality, colName, inUse, note, setNote, login, scopeKey, slideshow, use, remove, loginDefault,
    onlyDisplay: onlyD ? { display: onlyD, label: displayLabel(onlyD, displays) } : null,
    open: (e) => { setNote(''); setSheet(e.id); },
    sheetOpen: !!sheet,
  };

  // ---- the top: what shows now ----
  const plan = qualityPlan(nowEntry && nowEntry.kind === 'still' ? nowEntry.item : nowEntry && nowEntry.twin ? nowEntry.twin : null, quality, displays.length ? (scopeDisplay ? [scopeDisplay] : displays) : [{ w: 1920, h: 1080 }]);
  const qLabel = (q) => tr(q === '4k' ? 'settings.wallpaperGallery.quality4k' : 'settings.wallpaperGallery.qualityHd');
  // one line per display: a single long text wrapped past the card's bottom with three displays
  const planLines = plan.length > 1
    ? plan.map((p) => tr('settings.wallpaperGallery.planItem', { quality: qLabel(p.quality), display: displayLabel(p.display, displays), size: sizeText(p.size.w, p.size.h) }))
    : plan.length ? [tr('settings.wallpaperGallery.planOne', { quality: qLabel(plan[0].quality), size: sizeText(plan[0].size.w, plan[0].size.h) })] : [];
  const big = Math.max(200, Math.min(340, Math.round(width * 0.42)));
  const nowSub = nowEntry ? [colName(nowEntry.col), tr(nowEntry.kind === 'motion' ? 'settings.wallpaperGallery.kindMotion' : 'settings.wallpaperGallery.kindStill'),
    nowEntry.license].filter(Boolean).join(' · ') : '';
  const nowCard = (
    <Section footer={quality === 'auto' ? tr('settings.wallpaperGallery.qualityHint') : undefined}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.l, paddingVertical: spacing.m }}>
        <View style={{ width: big, height: Math.round(big * 9 / 16), borderRadius: 9, overflow: 'hidden', border: `3px solid ${t.mode === 'dark' ? '#0b0a11' : '#1c1c1e'}`, backgroundColor: '#000' }}>
          <Image src={nowEntry ? nowEntry.thumb : wallpaperThumb(stillNow)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          <View style={{ position: 'absolute', insetInlineStart: 6, bottom: 6, paddingHorizontal: 6, height: 18, borderRadius: 5, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.6)' }}>
            <Text style={{ fontSize: 10, color: '#fff' }}>{login ? tr('settings.wallpaperFor.login') : scopeDisplay ? displayLabel(scopeDisplay, displays) : tr('settings.motion.allDisplays')}</Text>
          </View>
        </View>
        <View style={{ flex: 1, minWidth: 240, gap: spacing.s }}>
          <View style={{ gap: 2 }}>
            <Text style={{ fontSize: typeScale.title, fontWeight: 700, color: t.text, textAlign: 'start' }}>{nowEntry ? nowEntry.name : ''}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{nowSub}</Text>
          </View>
          <SettingRow label={tr('settings.wallpaperGallery.for')}>
            <Segmented small value={target} onChange={(v) => { setTarget(v); setNote(''); }} options={[['desktop', tr('settings.wallpaperFor.desktop')], ['login', tr('settings.wallpaperFor.login')]]} />
          </SettingRow>
          {!login && displays.length > 1 ? (
            <SettingRow label={tr('settings.wallpaperFor.display')}>
              {displays.length > 2
                ? <PopUp width={Math.min(260, Math.max(170, width - big - 200))} value={scope} onChange={setScope} options={[['*', tr('settings.wallpaperGallery.allDisplays')], ...displays.map((d) => [d.id, displayLabel(d, displays)])]} />
                : <Segmented small value={scope} onChange={setScope} options={[['*', tr('settings.wallpaperGallery.allDisplays')], ...displays.map((d) => [d.id, displayLabel(d, displays)])]} />}
            </SettingRow>
          ) : null}
          <SettingRow label={tr('settings.wallpaperGallery.quality')}>
            <Segmented small value={quality} onChange={(v) => set({ wallpaperQuality: v })}
              options={QUALITIES.map((q) => [q, tr(`settings.wallpaperGallery.quality${q === 'auto' ? 'Auto' : q === 'hd' ? 'Hd' : '4k'}`)])} />
          </SettingRow>
          <View style={{ gap: 1 }}>
            {planLines.map((l, i) => <Text key={i} style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{l}</Text>)}
          </View>
        </View>
      </View>
    </Section>
  );

  // ---- the gallery ----
  const counts = filterCounts(entries, query, colName);
  const shown = filterItems(entries, { filter, query, colName });
  const order = collectionOrder(entries, WALLPAPER_CATEGORIES);
  const groups = groupCollections(shown, order);
  const allGroups = groupCollections(entries, order);
  const perRow = Math.max(1, Math.floor((width - 32 + GAP) / (tw + 4 + GAP)));     // inside a section: its padding
  const cardW = cardWidth(width - 32);
  const toolbar = (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s }}>
      <SearchField value={query} onChange={(q) => { setQuery(q); setView(null); }} placeholder={tr('settings.wallpaperGallery.search')} style={{ flex: 1, minWidth: 200 }} />
      <Segmented small appearance="neutral" value={filter} onChange={(f) => { setFilter(f); setView(null); }}
        options={GALLERY_FILTERS.map((f) => [f, tr('settings.wallpaperGallery.chip', { name: tr(`settings.wallpaperGallery.filter.${f}`), count: counts[f] })])} />
    </View>
  );
  const sheetEntry = sheet ? entries.find((e) => e.id === sheet) : null;
  const viewEntries = view ? (allGroups.find(([c]) => c === view) || [view, []])[1] : null;
  // the sheet's arrows walk what is shown: the open collection, else the gallery's filtered and searched list; they wrap
  const sheetList = viewEntries || shown;
  const stepSheet = (d) => {
    const i = sheetList.findIndex((x) => x.id === sheet);
    if (i < 0 || !sheetList.length) return;
    setNote('');
    setSheet(sheetList[(i + d + sheetList.length) % sheetList.length].id);
  };

  let body;
  if (viewEntries) body = <CollectionPage col={view} entries={viewEntries} ctx={ctx} onBack={() => setView(null)} />;
  else if (filter === 'mine') body = <MyPhotos entries={shown} ctx={ctx} />;
  else if (!groups.length) {
    body = (
      <Text style={{ paddingVertical: spacing.xl, fontSize: typeScale.callout, color: t.textDim, textAlign: 'center' }}>
        {query.trim() ? tr('settings.wallpaperGallery.noMatch', { query: query.trim() }) : tr('settings.wallpaperGallery.noneHere')}
      </Text>
    );
  } else {
    body = (
      <View style={{ gap: spacing.l }}>
        {!query.trim() && filter === 'all' ? (
          <Section title={tr('settings.wallpaperGallery.collections')}
            accessory={<Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('settings.wallpaperGallery.collectionsCount', { count: entries.filter((e) => e.kind !== 'mine').length, collections: allGroups.length })}</Text>}>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: CARD_GAP, paddingVertical: spacing.m }}>
              {groups.map(([c, es]) => <CollectionCard key={c} w={cardW} card={collectionCard(c, es)} name={colName(c)} onOpen={() => setView(c)} />)}
            </View>
          </Section>
        ) : null}
        {groups.map(([c, es]) => {
          const cut = query.trim() ? es.length : perRow;
          const desc = tOptional(`settings.wallpaperGallery.desc.${c}`);
          return (
            <Section key={c} title={desc ? `${colName(c)} · ${desc}` : colName(c)}
              accessory={es.length > cut ? <Link label={tr('settings.wallpaperGallery.seeAll', { count: es.length })} onClick={() => setView(c)} /> : null}>
              <Grid entries={es} ctx={ctx} limit={cut} />
            </Section>
          );
        })}
      </View>
    );
  }

  const cap = { fontSize: typeScale.caption, color: t.textDim };
  return (
    <Stack>
      <View ref={page} style={{ gap: 20 }}>
        {nowCard}
        {note && !sheetEntry && !viewEntries ? <Note tone="warn">{note}</Note> : null}
        {toolbar}
        {body}
        <Space ctx={ctx} />
        <Section>
          <SettingRow label={tr('settings.wallpaperThumbs.size')}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, direction: 'ltr' }}>
              <Text style={cap}>{tr('settings.wallpaperThumbs.small')}</Text>
              <Slider value={tw} min={80} max={240} width={150} onChange={(v) => set({ wallpaperThumbs: Math.round(v) })} />
              <Text style={cap}>{tr('settings.wallpaperThumbs.large')}</Text>
            </View>
          </SettingRow>
        </Section>
        {slideshow && !login ? <SlideshowSettings displays={displays} /> : null}
        <MotionOptions ctx={ctx} />
      </View>
      {sheetEntry ? <Sheet e={sheetEntry} ctx={ctx} onClose={() => setSheet(null)} onStep={sheetList.length > 1 ? stepSheet : null} /> : null}
    </Stack>
  );
}

// the settings as they are now (after an await, the render's copy may be old)
const settingsNow = () => settings.get();
