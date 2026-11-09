// editor.jsx — the markup editor (issue #30): a screenshot right after the capture, or any picture opened
// from Photos or Files' Quick Preview (media.js openMarkup). Arrow, box, line, text and blur; redaction
// (pixelate, solid fill); crop with aspect presets, resize and quarter turns; Save (in place, the previous
// file goes to the Trash) and Save as copy. Transcribe copies the text in a dragged area (or the whole picture
// with a click) to the clipboard: the picture without the annotations, read by the sandboxed lord-ocr (media.ocr).
//
// The pixels live in lord-server: each crop, resize or turn is a new picture (media.crop/resize/rotate,
// server/src/media/imgops.c) and the annotations of each step are drawn over it (media.annotate). Undo
// goes back over the annotations, then over the steps (the last MAX_STEPS pictures are kept). The math
// (rectangles, presets, sizes, file names) is in sdk/imageeditmodel.js, tested in node.
import {
  View, Text, MediaView, Button, Toggle, Segmented, TextInput, Toolbar, ToolbarButton, ToolbarGroup, ToolbarDivider, ToolbarSpacer, Hairline,
  useTheme, useAppWindow, useKeyboard, useStore, windows, wm, t as tr, media, fs, addListener, errorText, imageEdit as M,
  clipboard, settings,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { ocrLangs, cleanText, transcribeRect } from './ocr.js';

const COLORS = ['#ff3b30', '#ffcc00', '#34c759', '#0a84ff', '#ffffff', '#1c1c1e'];
const MAX_STEPS = 6;               // pictures kept for undoing crops, resizes and turns
const TOOLS = [
  ['arrow', '↗', 'screenshots.tool.arrow'], ['box', '▢', 'screenshots.tool.box'], ['line', '╱', 'screenshots.tool.line'],
  ['text', 'T', 'screenshots.tool.text'], ['blur', '▒', 'screenshots.tool.blur'], ['pixelate', '▦', 'screenshots.tool.pixelate'],
  ['fill', '■', 'screenshots.tool.fill'], ['crop', '⌗', 'screenshots.tool.crop'],
];
const exists = (p) => { try { fs.stat(p); return true; } catch (e) { return false; } };
const nameOf = (p) => String(p || '').split('/').pop();

// props: handle (a capture, already decoded) and/or path (the file it is, or the picture to open);
// newCapturePath() gives a free name in the Screenshots folder for a capture that was never saved.
export function Editor({ handle, path: firstPath, app, newCapturePath, onSaved }) {
  const th = useTheme();
  const win = useAppWindow();
  useStore(windows, (ws) => { const w = ws.find((x) => x.id === win.id); return w ? `${w.w}x${w.h}` : ''; });   // redraw the overlays on resize
  const [path, setPath] = useState(firstPath || null);
  const [steps, setSteps] = useState(handle ? [{ id: handle, ops: [] }] : []);
  const [error, setError] = useState(null);
  const [tool, setTool] = useState('arrow');
  const [color, setColor] = useState(COLORS[0]);
  const [draft, setDraft] = useState(null);
  const [text, setText] = useState(null);           // { x, y, value } (image coordinates)
  const [preview, setPreview] = useState(0);
  const [msg, setMsg] = useState(null);
  const [crop, setCrop] = useState(null);           // { preset, r } (image pixels)
  const [size, setSize] = useState(null);           // { width, height, keep, wText, hText }
  const [lay, setLay] = useState(null);             // the box's layout as last drawn (for the overlays)
  const box = useRef(null);
  const drag = useRef(null);
  const live = useRef({});
  const top = steps[steps.length - 1];
  const info = (top && media.info(top.id)) || { width: 1, height: 1 };
  const iw = info.width, ih = info.height;
  const ops = top ? top.ops : [];
  live.current = { steps, preview };

  // the layout is known only after a frame: the overlays follow it one frame later (bars shown, window resized)
  useEffect(() => {
    const check = () => {
      const l = box.current ? lord.getLayout(box.current.id) : null;
      if (l && (!lay || l.some((x, i) => x !== lay[i]))) setLay(l);
    };
    const tms = [30, 120, 300].map((ms) => setTimeout(check, ms));
    // while a crop or a text box is drawn over the picture, keep following it (a window being resized)
    const iv = crop || text ? setInterval(check, 250) : 0;
    return () => { tms.forEach(clearTimeout); if (iv) clearInterval(iv); };
  });
  useEffect(() => { wm.setTitle(win.id, tr('screenshots.markupTitle', { name: path ? nameOf(path) : tr('apps.screenshots') })); }, [path]);

  // a picture file: decoded by lord-media, copied into lord-server's memory and turned upright (EXIF)
  useEffect(() => {
    if (handle || !firstPath) return undefined;
    const img = media.image(app, firstPath);
    if (!img) { setError('media.noHelper'); return undefined; }
    let pending = true;
    const off = media.on(img, (type, d) => {
      if (type !== 'ready' && type !== 'error') return;
      pending = false;
      off();
      if (type === 'error') { media.close(img); setError(d.message || 'media.imageUnreadable'); return; }
      let id = media.crop(img, 0, 0, d.width, d.height);
      const turns = Math.round((d.orientation || 0) / 90) % 4;
      if (id && turns) { const r = media.rotate(id, turns); media.close(id); id = r; }
      media.close(img);
      if (id) setSteps([{ id, ops: [] }]); else setError('media.imageUnreadable');
    });
    return () => { if (pending) { off(); media.close(img); } };
  }, []);
  useEffect(() => () => { for (const s of live.current.steps) media.close(s.id); }, []);
  useEffect(() => () => { if (preview) media.close(preview); }, [preview]);

  // the preview: the step's picture with its annotations (and the one being drawn)
  useEffect(() => {
    if (!top) return;
    const all = draft ? [...ops, draft] : ops;
    const id = media.annotate(top.id, all, preview);
    if (id && id !== preview) setPreview(id);
    if (id) media.view(id, { fit: 'contain', background: 0 });
  }, [steps, draft]);

  const flash = (m) => { setMsg(m); setTimeout(() => setMsg(null), 2600); };
  const current = () => preview || top?.id;
  const setOps = (f) => setSteps((l) => l.map((s, i) => (i === l.length - 1 ? { ...s, ops: f(s.ops) } : s)));
  const push = (id) => {
    if (!id) { flash(tr('screenshots.editError')); return; }
    let l = [...live.current.steps, { id, ops: [] }];
    while (l.length > MAX_STEPS) { media.close(l[0].id); l = l.slice(1); }
    setSteps(l);
    setCrop(null); setSize(null); setTool((k) => (k === 'crop' ? 'arrow' : k));
  };
  const undo = () => {
    const l = live.current.steps, last = l[l.length - 1];
    if (!last) return;
    if (crop || size) { setCrop(null); setSize(null); if (tool === 'crop') setTool('arrow'); return; }
    if (last.ops.length) setOps((o) => o.slice(0, -1));
    else if (l.length > 1) { media.close(last.id); setSteps(l.slice(0, -1)); }
  };

  // where the picture is drawn in the box
  const layout = () => (box.current ? lord.getLayout(box.current.id) : [0, 0, 1, 1]);
  const view = () => { const [, , bw, bh] = layout(); return M.fitView(iw, ih, bw, bh); };
  const at = (x, y) => { const [lx, ly] = layout(); return M.toImagePoint(x - lx, y - ly, view()); };
  const bounds = { x: 0, y: 0, w: iw, h: ih };
  const ratio = crop ? M.presetRatio(crop.preset, iw, ih) : 0;

  const pickTool = (k) => {
    setText(null); setSize(null);
    if (k === 'crop') setCrop({ preset: 'free', r: { x: 0, y: 0, w: iw, h: ih } }); else setCrop(null);
    if (k === 'fill' && tool !== 'fill') setColor('#1c1c1e');        // redaction: black unless another color is picked
    setTool(k);
  };
  const setPreset = (p) => setCrop((c) => ({ preset: p, r: p === 'free' && c ? c.r : M.fitRatio(bounds, M.presetRatio(p, iw, ih)) }));
  const applyCrop = () => {
    const r = crop && M.imageRect(crop.r, iw, ih);
    if (!r) return;
    if (r.x === 0 && r.y === 0 && r.w === iw && r.h === ih) { setCrop(null); setTool('arrow'); return; }
    push(media.crop(current(), r.x, r.y, r.w, r.h));
  };
  const openResize = () => { setCrop(null); setText(null); if (tool === 'crop') setTool('arrow'); setSize({ width: iw, height: ih, keep: true, wText: String(iw), hText: String(ih) }); };
  const typeSide = (side, txt) => setSize((s) => {
    const v = M.parseSide(txt);
    const key = side === 'width' ? 'wText' : 'hText', other = side === 'width' ? 'hText' : 'wText';
    if (v == null) return { ...s, [key]: txt };
    const r = M.resizeSize(iw, ih, side, v, s.keep);
    return { ...s, ...r, [key]: txt, [other]: s.keep ? String(side === 'width' ? r.height : r.width) : s[other] };
  });
  const applyResize = () => {
    if (!size) return;
    const w = M.parseSide(size.wText), h = M.parseSide(size.hText);
    if (!w || !h) { flash(tr('screenshots.sizeInvalid', { max: M.MAX_SIDE })); return; }
    if (w === iw && h === ih) { setSize(null); return; }
    push(media.resize(current(), w, h));
  };
  const turn = (q) => push(media.rotate(current(), q));

  // pointer: drawing, or the crop rectangle (new, a handle, or moving it)
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      const p = at(e.x, e.y);
      if (d.kind === 'draw' || d.kind === 'ocr') { setDraft((o) => o && { ...o, x2: p.x, y2: p.y }); return; }
      if (d.kind === 'new') setCrop((c) => c && { ...c, r: M.dragRect(d.x, d.y, p.x, p.y, ratio, bounds) });
      else if (d.kind === 'move') setCrop((c) => c && { ...c, r: M.moveRect(d.r, p.x - d.x, p.y - d.y, bounds) });
      else setCrop((c) => c && { ...c, r: M.resizeRect(d.r, d.kind, p.x, p.y, ratio, bounds) });
    });
    const offUp = addListener('pointerup', () => {
      const d = drag.current;
      if (!d) return;
      drag.current = null;
      if (d.kind === 'draw') setDraft((o) => { if (o && Math.hypot(o.x2 - o.x1, o.y2 - o.y1) > 3) setOps((l) => [...l, finish(o)]); return null; });
      else if (d.kind === 'ocr') setDraft((o) => { if (o) transcribe(o); return null; });
      else setCrop((c) => (c && (c.r.w < 2 || c.r.h < 2) ? { ...c, r: M.fitRatio(bounds, ratio) } : c));
    });
    return () => { offMove(); offUp(); };
  });
  const finish = (o) => (o.type === 'fill' || o.type === 'pixelate' ? M.redactOp(o.type, o.x1, o.y1, o.x2, o.y2, iw, ih, o.color) : o);
  const width = Math.max(3, Math.round(Math.min(iw, ih) / 220));
  const down = (e) => {
    if (!top || size) return;
    const p = at(e.x, e.y);
    if (tool === 'crop' && crop) {
      // inside moves the rectangle, unless it is the whole picture (then a new one is drawn)
      const whole = crop.r.w >= iw - 0.5 && crop.r.h >= ih - 0.5;
      const h = M.handleAt(crop.r, p.x, p.y, 12 / view().s, ratio);
      drag.current = !h || (h === 'move' && whole) ? { kind: 'new', x: p.x, y: p.y } : h === 'move' ? { kind: 'move', x: p.x, y: p.y, r: crop.r } : { kind: h, r: crop.r };
      return;
    }
    if (tool === 'text') { setText({ x: p.x, y: p.y, value: '' }); return; }
    if (tool === 'transcribe') {                       // the area shows as a blue box while dragged, never kept
      drag.current = { kind: 'ocr' };
      setDraft({ type: 'box', x1: p.x, y1: p.y, x2: p.x, y2: p.y, color: '#0a84ff', width });
      return;
    }
    drag.current = { kind: 'draw' };
    const o = { type: tool, x1: p.x, y1: p.y, x2: p.x, y2: p.y, color, width: tool === 'blur' ? 6 : width };
    setDraft(tool === 'pixelate' ? { ...o, size: M.pixelBlock(iw, ih) } : o);
  };
  const addText = () => {
    if (text && text.value.trim()) setOps((o) => [...o, { type: 'text', x1: text.x, y1: text.y, text: text.value, color, size: Math.max(18, width * 7) }]);
    setText(null);
  };

  // ---- saving ----
  const plan = () => M.savePlan(path, tr('screenshots.edited'));
  const saved = (p, copy) => { flash(tr(copy ? 'screenshots.savedCopy' : 'screenshots.savedName', { name: nameOf(p) })); onSaved?.(p); };
  // the reason goes with it (the media helper missing, a full disk…), never a bare "Could not save" (issue #77)
  const saveFailed = (error) => flash(error ? `${tr('screenshots.saveError')}: ${errorText(error)}` : tr('screenshots.saveError'));
  const saveCopy = async () => {
    const pl = plan();
    const out = path ? M.freeCopy(pl, exists) : newCapturePath();
    if (!out || !current()) { flash(tr('screenshots.saveError')); return; }
    const r = await media.saveImage(current(), out, { format: pl.format });
    if (!r.ok) { saveFailed(r.error); return; }
    if (!path) setPath(r.path);                      // a capture only on the clipboard now has a file
    saved(r.path, !!path);
  };
  const save = async () => {
    const pl = plan();
    if (!path || !pl.replace) { await saveCopy(); return; }
    const r = await media.saveImage(current(), pl.temp, { format: pl.format });
    if (!r.ok) { saveFailed(r.error); return; }
    try { fs.trash(path); } catch (e) {
      // the original can't go to the Trash: the edit stays, as a copy
      const out = M.freeCopy(pl, exists);
      try { fs.rename(r.path, out); saved(out, true); } catch (x) { flash(tr('screenshots.saveError')); }
      return;
    }
    try { fs.rename(r.path, path); saved(path, false); } catch (e) { flash(tr('screenshots.saveError')); }
  };
  // Transcribe: the text of an area of the picture (its pixels, not the annotations) to the clipboard
  const transcribe = async (o) => {
    const r = top && transcribeRect(o.x1, o.y1, o.x2, o.y2, iw, ih);
    if (!r) return;
    const c = media.crop(top.id, r.x, r.y, r.w, r.h);
    if (!c) { flash(tr('screenshots.editError')); return; }
    flash(tr('screenshots.reading'));
    try {
      const s = settings.get();
      const txt = cleanText(await media.ocr(c, ocrLangs(s.languages || [s.language])));
      if (txt) { clipboard.set(txt); flash(tr('screenshots.textCopied', { count: [...txt].length })); } else flash(tr('screenshots.noText'));
    } catch (e) { flash(errorText(e)); } finally { media.close(c); }
  };
  const copy = async () => { const r = await media.copyImage(current()); flash(r.ok ? tr('screenshots.copied') : `${tr('screenshots.notCopied')}: ${errorText(r.error || 'media.noHelper')}`); };

  useKeyboard((e) => {
    const k = String(e.base || '').toLowerCase();
    if ((e.ctrl || e.meta) && k === 'z') undo();
    else if ((e.ctrl || e.meta) && k === 's') (e.shift ? saveCopy : save)();
    else if (e.key === 'Enter' && crop && !text) applyCrop();
    else if (e.key === 'Escape' && (crop || size)) { setCrop(null); setSize(null); if (tool === 'crop') setTool('arrow'); }
  });

  const v = top && lay ? M.fitView(iw, ih, lay[2], lay[3]) : null;
  const tp = text && v ? { left: v.ox + text.x * v.s, top: v.oy + text.y * v.s } : null;
  const cr = crop && v ? M.toViewRect(crop.r, v) : null;
  const ir = crop ? M.imageRect(crop.r, iw, ih) : null;
  const inPlace = plan().replace;
  const dim = 'rgba(0,0,0,0.55)';
  const knob = (x, y) => <View style={{ position: 'absolute', left: x - 5, top: y - 5, width: 10, height: 10, borderRadius: 2, backgroundColor: '#fff', border: '1px solid rgba(0,0,0,0.5)' }} />;
  const label = (k) => <Text style={{ fontSize: 12, color: th.textDim }}>{tr(k)}</Text>;
  return (
    <View style={{ flex: 1 }}>
      <Toolbar>
        <ToolbarGroup>
          {TOOLS.map(([k, g, key]) => <ToolbarButton key={k} icon={g} title={tr(key)} active={tool === k} onClick={() => pickTool(k)} />)}
        </ToolbarGroup>
        <ToolbarDivider />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}>
          {COLORS.map((c) => (
            <View key={c} onClick={() => setColor(c)} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
              border: `2px solid ${color === c ? th.accent : 'transparent'}` }}>
              <View style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: c, border: `1px solid ${th.separator}` }} />
            </View>
          ))}
        </View>
        <ToolbarDivider />
        <ToolbarButton icon="⟲" title={tr('screenshots.rotateLeft')} onClick={() => turn(-1)} />
        <ToolbarButton icon="⟳" title={tr('screenshots.rotateRight')} onClick={() => turn(1)} />
        <ToolbarButton icon="⤡" title={tr('screenshots.resize')} active={!!size} onClick={() => (size ? setSize(null) : openResize())} />
        <ToolbarSpacer />
        <ToolbarButton icon="Ⓣ" title={tr('screenshots.tool.transcribe')} active={tool === 'transcribe'} onClick={() => pickTool(tool === 'transcribe' ? 'arrow' : 'transcribe')} />
        <ToolbarButton icon="↶" title={tr('screenshots.undo')} onClick={undo} />
        <ToolbarButton icon="⧉" title={tr('screenshots.copy')} onClick={copy} />
        {inPlace && <Button label={tr('screenshots.saveCopy')} onClick={saveCopy} />}
        <Button primary label={tr(inPlace || !path ? 'screenshots.save' : 'screenshots.saveCopy')} onClick={inPlace ? save : saveCopy} />
      </Toolbar>
      {crop && (
        <View style={{ height: 44, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12 }}>
          <Hairline edge="bottom" />
          {label('screenshots.crop.aspect')}
          <Segmented small value={crop.preset} onChange={setPreset}
            options={M.CROP_PRESETS.map((p) => [p, p === 'free' || p === 'original' || p === 'square' ? tr(`screenshots.crop.${p}`) : p])} />
          <Text style={{ fontSize: 12, color: th.textDim, direction: 'ltr' }}>{ir ? `${ir.w} × ${ir.h}` : ''}</Text>
          <View style={{ flex: 1 }} />
          <Button label={tr('screenshots.cancel')} onClick={() => { setCrop(null); setTool('arrow'); }} />
          <Button primary label={tr('screenshots.crop.apply')} onClick={applyCrop} />
        </View>
      )}
      {size && (
        <View style={{ height: 44, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12 }}>
          <Hairline edge="bottom" />
          {label('screenshots.width')}
          <View style={{ width: 72 }}><TextInput value={size.wText} onChange={(t) => typeSide('width', t)} onSubmit={applyResize} selectAll /></View>
          <Text style={{ fontSize: 12, color: th.textDim }}>×</Text>
          {label('screenshots.height')}
          <View style={{ width: 72 }}><TextInput value={size.hText} onChange={(t) => typeSide('height', t)} onSubmit={applyResize} selectAll /></View>
          {label('screenshots.pixels')}
          <View style={{ width: 8 }} />
          <Toggle small on={size.keep} onChange={(k) => setSize((s) => ({ ...s, keep: k }))} />
          {label('screenshots.keepRatio')}
          <View style={{ flex: 1 }} />
          <Text style={{ fontSize: 11, color: th.textDim, direction: 'ltr' }}>{`${iw} × ${ih}`}</Text>
          <Button label={tr('screenshots.cancel')} onClick={() => setSize(null)} />
          <Button primary label={tr('screenshots.resizeApply')} onClick={applyResize} />
        </View>
      )}
      <View ref={box} style={{ flex: 1, backgroundColor: th.mode === 'dark' ? '#111' : '#ddd', overflow: 'hidden', direction: 'ltr' }} onMouseDown={down}>
        {top ? <MediaView media={current()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} /> : null}
        {!top && (
          <View style={{ position: 'absolute', left: 0, right: 0, top: '42%', alignItems: 'center', gap: 4 }}>
            <Text style={{ fontSize: 13, color: error ? '#ff6b60' : th.textDim }}>{tr(error ? 'screenshots.cantOpen' : 'screenshots.loading')}</Text>
            {error ? <Text style={{ fontSize: 11, color: th.textDim }}>{errorText(error)}</Text> : null}
          </View>
        )}
        {cr && (
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none' }}>
            <View style={{ position: 'absolute', left: 0, top: 0, right: 0, height: cr.y, backgroundColor: dim }} />
            <View style={{ position: 'absolute', left: 0, top: cr.y + cr.h, right: 0, bottom: 0, backgroundColor: dim }} />
            <View style={{ position: 'absolute', left: 0, top: cr.y, width: cr.x, height: cr.h, backgroundColor: dim }} />
            <View style={{ position: 'absolute', left: cr.x + cr.w, top: cr.y, right: 0, height: cr.h, backgroundColor: dim }} />
            <View style={{ position: 'absolute', left: cr.x, top: cr.y, width: cr.w, height: cr.h, border: '1px solid #fff' }}>
              {/* rule of thirds */}
              <View style={{ position: 'absolute', left: cr.w / 3, top: 0, bottom: 0, width: 1, backgroundColor: 'rgba(255,255,255,0.35)' }} />
              <View style={{ position: 'absolute', left: (cr.w * 2) / 3, top: 0, bottom: 0, width: 1, backgroundColor: 'rgba(255,255,255,0.35)' }} />
              <View style={{ position: 'absolute', top: cr.h / 3, left: 0, right: 0, height: 1, backgroundColor: 'rgba(255,255,255,0.35)' }} />
              <View style={{ position: 'absolute', top: (cr.h * 2) / 3, left: 0, right: 0, height: 1, backgroundColor: 'rgba(255,255,255,0.35)' }} />
            </View>
            {knob(cr.x, cr.y)}{knob(cr.x + cr.w, cr.y)}{knob(cr.x, cr.y + cr.h)}{knob(cr.x + cr.w, cr.y + cr.h)}
          </View>
        )}
        {text && tp && (
          <View style={{ position: 'absolute', left: tp.left, top: tp.top, width: 260 }}>
            <TextInput value={text.value} autoFocus onChange={(val) => setText({ ...text, value: val })} onSubmit={addText} onCancel={() => setText(null)}
              placeholder={tr('screenshots.textHint')} />
          </View>
        )}
        {msg && <View style={{ position: 'absolute', left: 0, right: 0, bottom: 20, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 14, height: 30, justifyContent: 'center', borderRadius: 15, backgroundColor: 'rgba(0,0,0,0.75)' }}><Text style={{ fontSize: 12, color: '#fff' }}>{msg}</Text></View>
        </View>}
      </View>
    </View>
  );
}
