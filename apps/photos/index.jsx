// Photos — image viewer and library (our own design, in the style of desktop viewers).
//
// The library is the XDG pictures folder, ~/Pictures by default (and its folders), and the cloud folders kept on this
// computer (places.js), with thumbnails in ~/.cache/lordos/thumbs. The
// viewer decodes with lord-media (PNG, JPEG, animated GIF, WebP, BMP; HEIC/AVIF if FFmpeg can)
// and C draws it: zoom, panning, rotation, full screen and slideshow. Edits (crop with aspect presets
// and handles, a new size, rotation, brightness and contrast) are saved as a copy: the original is left
// untouched. Markup opens the picture in the markup editor (arrows, text, redaction…; issue #30).
import {
  View, Text, Image, MediaView, ScrollView, SidebarItem, SectionTitle, Button, Slider, useTheme, useAppWindow, useKeyboard, useWindowState, wm,
  t as tr, errorText, media, useThumb, userDirs, fs, addListener, direction, print, ThemeProvider, ToolbarButton, ToolbarGroup, EmptyState, useStore, windows,
  Segmented, TextInput, Toggle, openMarkup, imageEdit as M, useCloud,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { keptPlaces, inPlaces } from './places.js';

const APP = 'photos';
const handledNonce = new Map();      // «Open with» already handled, per window
const IMAGE = /\.(png|jpe?g|gif|webp|bmp|heic|heif|avif|tiff?)$/i;
const baseName = (p) => String(p || '').split('/').pop();
const dirOf = (p) => String(p || '').split('/').slice(0, -1).join('/');

export function Icon({ size: s }) {
  const petal = (c, x, y) => <View style={{ position: 'absolute', left: s * x, top: s * y, width: s * 0.3, height: s * 0.3, borderRadius: s * 0.15, backgroundColor: c, opacity: 0.88 }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, backgroundColor: '#fbfbfd', boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* a flower with six colored petals, our own */}
      {petal('#ffb340', 0.35, 0.16)}{petal('#ff5f57', 0.55, 0.27)}{petal('#c86dd7', 0.55, 0.47)}
      {petal('#4c8dff', 0.35, 0.58)}{petal('#33c7a8', 0.15, 0.47)}{petal('#9bd44a', 0.15, 0.27)}
      <View style={{ position: 'absolute', left: s * 0.42, top: s * 0.4, width: s * 0.16, height: s * 0.16, borderRadius: s * 0.08, backgroundColor: '#fff' }} />
    </View>
  );
}

function listImages(dir, depth, out) {
  if (depth < 0 || out.length > 4000) return;
  let list = [];
  try { list = fs.list(dir); } catch (e) { return; }
  for (const e of list) {
    const p = `${dir}/${e.name}`;
    if (e.dir) { if (depth > 0) listImages(p, depth - 1, out); }
    else if (IMAGE.test(e.name)) out.push({ path: p, name: e.name, size: e.size, mtime: e.mtime });
  }
}

// the same, without waiting on the compositor's thread: a cloud folder is on a network mount, where a listing can wait
// for the provider (the session must never freeze for it)
async function listImagesAsync(dir, depth, out, alive) {
  if (depth < 0 || out.length > 4000 || !alive()) return;
  let list = [];
  try { list = await fs.listAsync(dir); } catch (e) { return; }
  for (const e of list) {
    if (!alive()) return;
    const p = `${dir}/${e.name}`;
    if (e.dir) { if (depth > 0 && !e.name.startsWith('.')) await listImagesAsync(p, depth - 1, out, alive); }
    else if (IMAGE.test(e.name)) out.push({ path: p, name: e.name, size: e.size, mtime: e.mtime });
  }
}

function subfolders(dir) {
  try { return fs.list(dir).filter((e) => e.dir).map((e) => `${dir}/${e.name}`); } catch (e) { return []; }
}

function Thumb({ e, size, selected, onOpen, onSelect }) {
  const th = useTheme();
  const thumb = useThumb(e.path, 256);
  return (
    <View onClick={onSelect} onDoubleClick={onOpen} style={{ width: size, height: size, borderRadius: 6, overflow: 'hidden', backgroundColor: th.control,
      border: selected ? `3px solid ${th.accent}` : '0px solid transparent', alignItems: 'center', justifyContent: 'center' }}>
      {thumb ? <Image src={thumb} style={{ width: selected ? size - 6 : size, height: selected ? size - 6 : size, objectFit: 'cover', borderRadius: selected ? 3 : 0 }} />
        : <Text style={{ fontSize: 10, color: th.textDim }}>{e.name.split('.').pop().toUpperCase()}</Text>}
    </View>
  );
}

function ToolBtn({ glyph, label, onClick, active, dark }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const col = dark ? '#fff' : th.text;
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, minWidth: 30, paddingHorizontal: 8, borderRadius: 7, flexDirection: 'row', gap: 6, alignItems: 'center', justifyContent: 'center',
        backgroundColor: active ? th.accent : hover ? 'rgba(127,127,127,0.2)' : 'transparent' }}>
      <Text style={{ fontSize: 14, color: active ? '#fff' : col }}>{glyph}</Text>
      {label ? <Text style={{ fontSize: 12, color: active ? '#fff' : col }}>{label}</Text> : null}
    </View>
  );
}

// ---- viewer ----

function Viewer({ list, index, setIndex, onClose, winId, mode }) {
  const th = useTheme();
  const path = list[index]?.path;
  const [id, setId] = useState(0);
  const [info, setInfo] = useState(null);
  const [error, setError] = useState(null);
  const [zoom, setZoom] = useState(0);          // 0 = fit
  const [center, setCenter] = useState({ cx: 0.5, cy: 0.5 });
  const [rot, setRot] = useState(0);            // user rotation (on top of EXIF)
  const [slide, setSlide] = useState(false);
  const [edit, setEdit] = useState(null);        // { bright, contrast, crop: {x,y,w,h} in view px, preset, size }
  const [toast, setToast] = useState(null);
  const box = useRef(null);
  const drag = useRef(null);
  const pinchK = useRef(1);           // trackpad pinch: scale already applied
  const idRef = useRef(0);

  // File → Print menu (the dialog belongs to the system, to lord-doc)
  useEffect(() => {
    if (!path) return;
    wm.setMenus(winId, [{ label: tr('photos.menu.file'), items: [
      { label: tr('photos.menu.print'), hint: '❖P', onClick: () => print.open({ title: baseName(path), image: path }) },
      { label: tr('photos.menu.slideshow'), onClick: () => setSlide(true) },
    ] }]);
  }, [path]);

  useEffect(() => {
    if (!path) return undefined;
    setInfo(null); setError(null); setZoom(0); setCenter({ cx: 0.5, cy: 0.5 }); setRot(0); setEdit(null);
    // animated GIF: with the player (at its original size, looping, muted)
    const nid = /\.gif$/i.test(path) ? media.play(APP, path, { native: true, loop: true, audio: false }) : media.image(APP, path);
    setId(nid || 0);
    idRef.current = nid;
    wm.setTitle(winId, baseName(path));
    const off = nid ? media.on(nid, (type, d) => {
      if (type === 'ready') setInfo(d);
      else if (type === 'info') {
        const v = d.streams?.find((x) => x.type === 'video') || {};
        setInfo({ width: v.width, height: v.height, sourceWidth: v.width, sourceHeight: v.height, orientation: 0, animated: true });
      }
      else if (type === 'error') setError(d.message);
    }) : null;
    return () => { off?.(); media.close(nid); };
  }, [path]);

  const orient = info?.orientation || 0;
  const totalRot = (orient + rot + 360) % 360;
  useEffect(() => {
    if (!id) return;
    media.view(id, { fit: 'contain', zoom, cx: center.cx, cy: center.cy, rotate: totalRot, background: 0,
      brightness: edit?.bright || 0, contrast: edit?.contrast || 0 });
  }, [id, zoom, center, totalRot, edit?.bright, edit?.contrast]);

  // slideshow
  useEffect(() => {
    if (!slide) return undefined;
    const tmr = setInterval(() => setIndex((i) => (i + 1) % list.length), 4000);
    return () => clearInterval(tmr);
  }, [slide, list.length]);
  useEffect(() => { if (slide && mode !== 'fullscreen') wm.setFullscreen(winId, true); }, [slide]);
  useEffect(() => { if (mode !== 'fullscreen') setSlide(false); }, [mode]);

  const layout = () => (box.current ? lord.getLayout(box.current.id) : [0, 0, 1, 1]);
  // size of the (rotated) image and current scale
  const dims = () => {
    const w0 = info?.width || 1, h0 = info?.height || 1;
    const rw = totalRot % 180 ? h0 : w0, rh = totalRot % 180 ? w0 : h0;
    const [, , bw, bh] = layout();
    const fit = Math.min(bw / rw, bh / rh);
    return { rw, rh, bw, bh, fit, scale: zoom || fit };
  };
  const setZoomAt = (z) => {
    const { fit } = dims();
    if (!z || z <= fit * 1.001) { setZoom(0); setCenter({ cx: 0.5, cy: 0.5 }); return; }
    setZoom(Math.min(16, z));
  };
  const zoomBy = (f) => { const { scale } = dims(); setZoomAt(scale * f); };

  const prev = () => setIndex((i) => (i > 0 ? i - 1 : i));
  const next = () => setIndex((i) => (i < list.length - 1 ? i + 1 : i));
  useKeyboard((e) => {
    const rtl = direction() === 'rtl';
    if (e.key === 'ArrowRight') (rtl ? prev : next)();
    else if (e.key === 'ArrowLeft') (rtl ? next : prev)();
    else if (e.char === '+' || e.char === '=') zoomBy(1.25);
    else if (e.char === '-') zoomBy(0.8);
    else if (e.char === '0') setZoomAt(0);
    else if (e.char === '1') setZoomAt(1);
    else if (String(e.base || '').toLowerCase() === 'r' && !e.shift) setRot((r) => r + 90);
    else if (String(e.base || '').toLowerCase() === 'f') wm.setFullscreen(winId, mode !== 'fullscreen');
    else if (e.key === ' ') setSlide((v) => !v);
    else if ((e.meta || e.ctrl) && String(e.base || '').toLowerCase() === 'p') print.open({ title: baseName(path), image: path });
    else if (e.key === 'Escape' && mode !== 'fullscreen') { if (edit) setEdit(null); else onClose(); }
  });

  // dragging: pan when zoomed, or mark the crop while editing
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      if (d.crop) {
        const [lx, ly] = layout();
        const x = e.x - lx, y = e.y - ly, b = imageRect(), ratio = cropRatio();
        const c = d.crop === 'new' ? M.dragRect(d.x, d.y, x, y, ratio, b) : d.crop === 'move' ? M.moveRect(d.r, x - d.x, y - d.y, b) : M.resizeRect(d.r, d.crop, x, y, ratio, b);
        setEdit((ed) => ed && ({ ...ed, crop: c }));
        return;
      }
      const { rw, rh, scale } = dims();
      setCenter({ cx: Math.max(0, Math.min(1, d.cx - (e.x - d.x) / (rw * scale))), cy: Math.max(0, Math.min(1, d.cy - (e.y - d.y) / (rh * scale))) });
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });

  const imageRect = () => {
    const { rw, rh, bw, bh, fit } = dims();
    return { x: (bw - rw * fit) / 2, y: (bh - rh * fit) / 2, w: rw * fit, h: rh * fit };
  };
  const cropRatio = () => { const { rw, rh } = dims(); return M.presetRatio(edit?.preset || 'free', rw, rh); };
  // the crop in the picture's pixels (after rotation), or all of it
  const cropPixels = () => {
    const { rw, rh } = dims(), r = imageRect(), k = rw / (r.w || 1);
    const c = edit?.crop && edit.crop.w > 4 && edit.crop.h > 4 ? M.imageRect({ x: (edit.crop.x - r.x) * k, y: (edit.crop.y - r.y) * k, w: edit.crop.w * k, h: edit.crop.h * k }, rw, rh) : null;
    return c || { x: 0, y: 0, w: rw, h: rh, all: true };
  };
  const startEdit = () => { setZoomAt(0); setEdit({ bright: 0, contrast: 0, crop: null, preset: 'free', size: null }); };
  const setPreset = (p) => {
    const { rw, rh } = dims();
    setEdit((ed) => ({ ...ed, preset: p, crop: p === 'free' ? ed.crop : M.fitRatio(imageRect(), M.presetRatio(p, rw, rh)) }));
  };
  // the new size: follows the crop until a side is typed
  const outSize = () => {
    const c = cropPixels(), z = edit?.size;
    if (!z) return { width: c.w, height: c.h, wText: String(c.w), hText: String(c.h), keep: true, touched: false };
    if (z.keep) { const r = M.resizeSize(c.w, c.h, 'width', z.width, true); return { ...z, height: r.height, hText: String(r.height) }; }
    return z;
  };
  const typeSide = (side, txt) => {
    const c = cropPixels(), cur = outSize(), v = M.parseSide(txt);
    const key = side === 'width' ? 'wText' : 'hText';
    if (v == null) { setEdit((ed) => ({ ...ed, size: { ...cur, [key]: txt, touched: true } })); return; }
    const r = M.resizeSize(c.w, c.h, side, v, cur.keep);
    setEdit((ed) => ({ ...ed, size: { ...cur, ...(cur.keep ? r : { [side]: v }), [key]: txt, ...(cur.keep ? { [side === 'width' ? 'hText' : 'wText']: String(side === 'width' ? r.height : r.width) } : {}), touched: true } }));
  };

  const saveCopy = async () => {
    const ext = /\.jpe?g$/i.test(path) ? 'jpeg' : 'png';
    const base = baseName(path).replace(/\.[^.]+$/, '');
    let out = '';
    for (let n = 1; n < 100; n++) {
      const name = `${base} (${tr('photos.edited')}${n > 1 ? ` ${n}` : ''}).${ext === 'jpeg' ? 'jpg' : 'png'}`;
      out = `${dirOf(path)}/${name}`;
      try { fs.stat(out); } catch (e) { break; }
    }
    const p = cropPixels(), z = outSize();
    const c = p.all ? {} : { cx: p.x, cy: p.y, cw: p.w, ch: p.h, refw: dims().rw };
    const w = M.parseSide(z.wText), h = M.parseSide(z.hText);
    if (z.touched && (!w || !h)) { setToast(tr('photos.sizeInvalid', { max: M.MAX_SIDE })); setTimeout(() => setToast(null), 3000); return; }
    const sz = z.touched && (w !== p.w || h !== p.h) ? { width: w, height: h } : {};
    const res = await media.edit(APP, path, out, { rotate: rot, brightness: edit.bright, contrast: edit.contrast, format: ext, ...c, ...sz });
    setToast(res.ok ? tr('photos.saved', { name: baseName(res.path) }) : tr('photos.saveError'));
    setTimeout(() => setToast(null), 3000);
    if (res.ok) setEdit(null);
  };

  const full = mode === 'fullscreen';
  // the viewer is always dark (photos look best on a dark surround), in light mode too
  return (
    <ThemeProvider tokens={viewerTokens(th)}>
    <View style={{ flex: 1, backgroundColor: full ? '#000' : '#141416' }}>
      {!full && (
        <View style={{ height: 52, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 12, gap: 8, backgroundColor: '#1d1d20' }}>
          <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={18} label={tr('photos.library')} onClick={onClose} />
          <View style={{ flex: 1, alignItems: 'center', overflow: 'hidden' }}>
            <Text style={{ fontSize: 13, fontWeight: 600, color: '#f5f5f7', whiteSpace: 'nowrap' }}>{baseName(path)}</Text>
            <Text style={{ fontSize: 11, color: 'rgba(235,235,245,0.55)', whiteSpace: 'nowrap' }}>{info?.sourceWidth ? `${info.sourceWidth} × ${info.sourceHeight}` : ''}{list.length > 1 ? `  ·  ${index + 1} / ${list.length}` : ''}</Text>
          </View>
          <ToolbarGroup>
            <ToolbarButton icon="−" small title={tr('photos.zoomOut')} onClick={() => zoomBy(0.8)} />
            <View onClick={() => setZoomAt(zoom ? 0 : 1)} style={{ minWidth: 56, alignItems: 'center' }}>
              <Text style={{ fontSize: 11, fontWeight: 500, color: '#f5f5f7', whiteSpace: 'nowrap' }}>{zoom ? `${Math.round(zoom * 100)} %` : tr('photos.fit')}</Text>
            </View>
            <ToolbarButton icon="+" small title={tr('photos.zoomIn')} onClick={() => zoomBy(1.25)} />
          </ToolbarGroup>
          <ToolbarButton icon="⟲" title={tr('photos.rotateLeft')} onClick={() => setRot((r) => r - 90)} />
          <ToolbarButton icon="⟳" title={tr('photos.rotateRight')} onClick={() => setRot((r) => r + 90)} />
          <ToolbarButton icon="▶" iconSize={13} title={tr('photos.slideshow')} onClick={() => setSlide(true)} />
          <ToolbarButton icon="⛶" title={tr('photos.fullscreen')} onClick={() => wm.setFullscreen(winId, true)} />
          {M.canMarkup(path) && !info?.animated && !error ? <ToolbarButton icon="✐" label={tr('photos.markup')} title={tr('photos.markupHint')} onClick={() => openMarkup(path)} /> : null}
          <ToolbarButton icon="✎" label={tr('photos.edit')} active={!!edit} onClick={() => (edit ? setEdit(null) : startEdit())} />
        </View>
      )}
      <View ref={box} style={{ flex: 1, overflow: 'hidden' }}
        onWheel={(e) => { if (e.precise && !e.notches) return; zoomBy(e.deltaY < 0 ? 1.15 : 1 / 1.15); }}
        onPinch={(e) => { if (e.phase === 'begin') pinchK.current = 1; else { zoomBy(e.scale / pinchK.current); pinchK.current = e.scale; } }}
        onDoubleClick={() => setZoomAt(zoom ? 0 : 1)}
        onMouseDown={(e) => {
          if (edit) {
            const [lx, ly] = layout(), x = e.x - lx, y = e.y - ly;
            const h = edit.crop && edit.crop.w > 4 ? M.handleAt(edit.crop, x, y, 10, cropRatio()) : null;
            drag.current = !h ? { crop: 'new', x, y } : h === 'move' ? { crop: 'move', x, y, r: edit.crop } : { crop: h, r: edit.crop };
            return;
          }
          if (zoom) drag.current = { x: e.x, y: e.y, cx: center.cx, cy: center.cy };
        }}>
        <MediaView media={id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
        {error && <View style={{ position: 'absolute', left: 0, right: 0, top: '45%', alignItems: 'center', gap: 4 }}>
          <Text style={{ fontSize: 13, color: '#ffb0b0' }}>{tr('photos.cantOpen')}</Text>
          <Text style={{ fontSize: 11, color: '#999' }}>{errorText(error)}</Text>
        </View>}
        {edit?.crop && edit.crop.w > 2 && (() => {
          const c = edit.crop, dim = 'rgba(0,0,0,0.5)';
          return (
            <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none', direction: 'ltr' }}>
              <View style={{ position: 'absolute', left: 0, top: 0, right: 0, height: c.y, backgroundColor: dim }} />
              <View style={{ position: 'absolute', left: 0, top: c.y + c.h, right: 0, bottom: 0, backgroundColor: dim }} />
              <View style={{ position: 'absolute', left: 0, top: c.y, width: c.x, height: c.h, backgroundColor: dim }} />
              <View style={{ position: 'absolute', left: c.x + c.w, top: c.y, right: 0, height: c.h, backgroundColor: dim }} />
              <View style={{ position: 'absolute', left: c.x, top: c.y, width: c.w, height: c.h, border: '2px solid #fff' }} />
              {[[c.x, c.y], [c.x + c.w, c.y], [c.x, c.y + c.h], [c.x + c.w, c.y + c.h]].map(([x, y], i) => (
                <View key={i} style={{ position: 'absolute', left: x - 6, top: y - 6, width: 12, height: 12, borderRadius: 3, backgroundColor: '#fff', border: '1px solid rgba(0,0,0,0.5)' }} />
              ))}
            </View>
          );
        })()}
        {list.length > 1 && !edit && (
          <>
            <View onClick={prev} style={{ position: 'absolute', left: 8, top: '45%', width: 36, height: 36, borderRadius: 18, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
              <Text style={{ fontSize: 18, color: '#fff' }}>‹</Text>
            </View>
            <View onClick={next} style={{ position: 'absolute', right: 8, top: '45%', width: 36, height: 36, borderRadius: 18, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
              <Text style={{ fontSize: 18, color: '#fff' }}>›</Text>
            </View>
          </>
        )}
        {full && slide && <View style={{ position: 'absolute', right: 16, bottom: 16, flexDirection: 'row', gap: 6 }}>
          <ToolBtn dark glyph="⏸" onClick={() => setSlide(false)} />
          <ToolBtn dark glyph="✕" onClick={() => wm.setFullscreen(winId, false)} />
        </View>}
        {toast && <View style={{ position: 'absolute', left: 0, right: 0, bottom: 20, alignItems: 'center' }}>
          <View style={{ paddingHorizontal: 14, paddingVertical: 8, borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.75)' }}>
            <Text style={{ fontSize: 12, color: '#fff' }}>{toast}</Text>
          </View>
        </View>}
      </View>
      {edit && !full && (() => {
        const cp = cropPixels(), z = outSize();
        return (
          <View style={{ paddingVertical: 8, paddingHorizontal: 16, gap: 8, backgroundColor: '#1d1d20' }}>
            <View style={{ height: 30, flexDirection: 'row', alignItems: 'center', gap: 10 }}>
              <Text style={{ fontSize: 12, color: '#ccc' }}>{tr('photos.crop')}</Text>
              <Segmented small value={edit.preset} onChange={setPreset}
                options={M.CROP_PRESETS.map((p) => [p, p === 'free' || p === 'original' || p === 'square' ? tr(`photos.crop.${p}`) : p])} />
              <Text style={{ fontSize: 11, color: '#999', direction: 'ltr' }}>{`${cp.w} × ${cp.h}`}</Text>
              <View style={{ flex: 1 }} />
              <Text style={{ fontSize: 12, color: '#ccc' }}>{tr('photos.size')}</Text>
              <View style={{ width: 70 }}><TextInput value={z.wText} onChange={(t) => typeSide('width', t)} selectAll /></View>
              <Text style={{ fontSize: 12, color: '#999' }}>×</Text>
              <View style={{ width: 70 }}><TextInput value={z.hText} onChange={(t) => typeSide('height', t)} selectAll /></View>
              <Toggle small on={z.keep} onChange={(k) => setEdit((ed) => ({ ...ed, size: { ...outSize(), keep: k, touched: true } }))} />
              <Text style={{ fontSize: 12, color: '#ccc' }}>{tr('photos.keepRatio')}</Text>
            </View>
            <View style={{ height: 30, flexDirection: 'row', alignItems: 'center', gap: 12 }}>
              <Text style={{ fontSize: 12, color: '#ccc' }}>{tr('photos.brightness')}</Text>
              <Slider value={edit.bright} min={-100} max={100} width={130} onChange={(v) => setEdit({ ...edit, bright: Math.round(v) })} />
              <Text style={{ fontSize: 12, color: '#ccc', marginLeft: 8 }}>{tr('photos.contrast')}</Text>
              <Slider value={edit.contrast} min={-100} max={100} width={130} onChange={(v) => setEdit({ ...edit, contrast: Math.round(v) })} />
              <Text style={{ fontSize: 11, color: '#999', flex: 1 }}>{edit.crop ? tr('photos.cropReady') : tr('photos.cropHint')}</Text>
              <Button label={tr('photos.reset')} onClick={() => { setEdit({ bright: 0, contrast: 0, crop: null, preset: 'free', size: null }); setRot(0); }} />
              <Button primary label={tr('photos.saveCopy')} onClick={saveCopy} />
            </View>
          </View>
        );
      })()}
    </View>
    </ThemeProvider>
  );
}

// Tokens for the viewer's always-dark chrome (toolbar buttons, tooltips, sliders).
const viewerTokens = (th) => ({ ...th, mode: 'dark', text: '#f5f5f7', textDim: 'rgba(235,235,245,0.55)', hover: 'rgba(255,255,255,0.1)',
  control: 'rgba(255,255,255,0.1)', separator: 'rgba(255,255,255,0.1)', windowBg: '#1f1f23', inputBg: '#2a2a2f' });

// Small folder icon for the sidebar (the folder emoji is a color picture; a line glyph fits better).
function FolderGlyph({ on }) {
  const th = useTheme();
  const c = on ? '#fff' : th.accent;
  return (
    <View style={{ width: 18, alignItems: 'center' }}>
      <View style={{ width: 15, height: 11, marginTop: 2 }}>
        <View style={{ position: 'absolute', left: 0, top: 0, width: 6, height: 3, borderRadius: 1, backgroundColor: c }} />
        <View style={{ position: 'absolute', left: 0, top: 2, width: 15, height: 9, borderRadius: 2, border: `1.5px solid ${c}` }} />
      </View>
    </View>
  );
}

export default function Photos() {
  const th = useTheme();
  const win = useAppWindow();
  const dirs = userDirs();
  const [folder, setFolder] = useState(dirs.pictures);
  const [all, setAll] = useState(true);
  const [items, setItems] = useState([]);
  const [sel, setSel] = useState(null);
  const [viewer, setViewer] = useWindowState('viewer', null);    // { list, index } (survives full screen)
  const [size, setSize] = useState(132);
  const [rev, setRev] = useState(0);
  const winW = useStore(windows, (ws) => ws.find((w) => w.id === win.id)?.w || 980);

  const places = keptPlaces(useCloud().accounts);

  useEffect(() => {
    const out = [];
    const byMtime = (a, b) => (b.mtime || 0) - (a.mtime || 0);
    if (!inPlaces(places, folder)) {
      listImages(folder, all ? 4 : 0, out);
      setItems(out.sort(byMtime));
      return undefined;
    }
    let live = true;
    setItems([]);
    listImagesAsync(folder, 4, out, () => live).then(() => { if (live) setItems(out.sort(byMtime)); });
    return () => { live = false; };
  }, [folder, all, rev]);

  // open what Files sent: the image and its neighbors in the same folder
  useEffect(() => {
    const p = win.args?.path;
    if (!p || handledNonce.get(win.id) === win.args.nonce) return;
    handledNonce.set(win.id, win.args.nonce);
    const out = [];
    listImages(dirOf(p), 0, out);
    out.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
    const i = Math.max(0, out.findIndex((x) => x.path === p));
    setViewer({ list: out.length ? out : [{ path: p, name: baseName(p) }], index: i });
  }, [win.args?.nonce]);

  if (viewer) {
    return <Viewer list={viewer.list} index={viewer.index} winId={win.id} mode={win.mode}
      setIndex={(f) => setViewer((v) => ({ ...v, index: typeof f === 'function' ? f(v.index) : f }))}
      onClose={() => { setViewer(null); wm.setTitle(win.id, null); wm.setFullscreen(win.id, false); }} />;
  }

  // narrow window (close to minSize): a smaller title so it is not clipped
  const narrow = winW < 700;
  // grouped by month, like a photo library
  const groups = [];
  for (const e of items) {
    const d = new Date(e.mtime || 0);
    const k = `${d.getFullYear()}-${d.getMonth()}`;
    if (!groups.length || groups[groups.length - 1].k !== k) groups.push({ k, date: d, list: [] });
    groups[groups.length - 1].list.push(e);
  }
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 210, backgroundColor: th.sidebarBg, paddingTop: 8, paddingHorizontal: 10, gap: 2 }}>
        <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: th.separator }} />
        <SectionTitle>{tr('photos.library')}</SectionTitle>
        <SidebarItem label={tr('photos.allPhotos')} icon="▦" selected={all && folder === dirs.pictures} onClick={() => { setFolder(dirs.pictures); setAll(true); }} />
        <View style={{ height: 8 }} />
        <SectionTitle>{tr('photos.folders')}</SectionTitle>
        {subfolders(dirs.pictures).map((f) => (
          <SidebarItem key={f} label={baseName(f)} icon={<FolderGlyph on={!all && folder === f} />} selected={!all && folder === f} onClick={() => { setFolder(f); setAll(false); }} />
        ))}
        {places.length > 0 && <View style={{ height: 8 }} />}
        {places.length > 0 && <SectionTitle>{tr('photos.cloud')}</SectionTitle>}
        {places.map((k) => (
          <SidebarItem key={k.path} label={k.label} icon={<FolderGlyph on={folder === k.path} />} selected={folder === k.path} onClick={() => { setFolder(k.path); setAll(true); }} />
        ))}
      </View>
      <View style={{ flex: 1 }}>
        <View style={{ height: 64, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 20, gap: 12 }}>
          <View style={{ flex: 1, overflow: 'hidden' }}>
            <Text style={{ fontSize: narrow ? 17 : 22, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' }}>{places.find((k) => k.path === folder)?.label || (all ? tr('photos.allPhotos') : baseName(folder))}</Text>
            <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap' }}>{tr('photos.count', { count: items.length })}</Text>
          </View>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, direction: 'ltr' }}>
            {!narrow && <View style={{ width: 8, height: 8, borderRadius: 2, border: `1.5px solid ${th.textDim}` }} />}
            <Slider value={size} min={80} max={240} width={narrow ? 72 : 96} onChange={(v) => setSize(Math.round(v))} />
            {!narrow && <View style={{ width: 13, height: 13, borderRadius: 3, border: `1.5px solid ${th.textDim}` }} />}
          </View>
          <ToolbarButton icon="↻" title={tr('photos.refresh')} onClick={() => setRev((r) => r + 1)} />
        </View>
        <ScrollView style={{ flex: 1, paddingHorizontal: 20 }}>
          {!items.length && (
            <EmptyState icon="▦" title={tr('photos.emptyTitle')} hint={tr('photos.empty')} style={{ paddingTop: 80 }} />
          )}
          {groups.map((g) => (
            <View key={g.k} style={{ gap: 10, marginBottom: 24 }}>
              <Text style={{ fontSize: 15, fontWeight: 600, color: th.text }}>{tr('photos.monthYear', { month: tr(`photos.month.${g.date.getMonth()}`), year: g.date.getFullYear() })}</Text>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
                {g.list.map((e) => (
                  <Thumb key={e.path} e={e} size={size} selected={sel === e.path} onSelect={() => setSel(e.path)}
                    onOpen={() => setViewer({ list: items, index: items.indexOf(e) })} />
                ))}
              </View>
            </View>
          ))}
        </ScrollView>
      </View>
    </View>
  );
}
