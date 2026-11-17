// Settings → Users → Avatar → Choose image…: a picture from the file chooser, fitted to the round avatar. The picture is
// opened by lord-media (any size or format it reads, turned upright by its EXIF), shown in a circle; the slider zooms,
// dragging moves it. Save cuts the square seen in the circle, scales it to AVATAR_PX and writes a JPEG to
// ~/.cache/lordos/avatar.jpg (a 100 MB photo becomes a few dozen KB; the same file each time), which the user-avatar action then copies to
// /var/lib/lordos/users. The original is never changed. All the picture work is lord-media's (sdk/media.js: image, crop,
// resize, saveImage), no pixels go through JavaScript.
import { View, Text, Button, Slider, Layer, FilePicker, MediaView, media, fs, addListener, useTheme, useLocale, useAppWindow, wm, spacing, typeScale } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { avatarCrop } from './avatarmodel.js';

export const AVATAR_PX = 512;
const STAGE = 260;
const PREVIEW_PX = 1200;
const APP = 'settings';
const IMAGES = /\.(png|jpe?g|webp|heic|heif|avif|gif|bmp|tiff?)$/i;

// onSave(path): the picture written, ready for user-avatar; onClose(): nothing chosen or cancelled
export function AvatarEditor({ onSave, onClose }) {
  const th = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const [picking, setPicking] = useState(true);
  const [src, setSrc] = useState(null);             // { id, w, h }: the upright picture
  const [zoom, setZoom] = useState(0);              // 0..1 of the slider
  const [center, setCenter] = useState(null);       // { x, y } in picture pixels
  const [preview, setPreview] = useState(0);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const drag = useRef(null);

  const open = (path) => {
    setPicking(false);
    const img = media.image(APP, path);
    if (!img) { setErr(tr('settings.users.avatarUnreadable')); return; }
    const off = media.on(img, (type, d) => {
      if (type !== 'ready' && type !== 'error') return;
      off();
      if (type === 'error') { media.close(img); setErr(tr('settings.users.avatarUnreadable')); return; }
      let id = media.crop(img, 0, 0, d.width, d.height);
      const turns = Math.round((d.orientation || 0) / 90) % 4;
      if (id && turns) { const r = media.rotate(id, turns); media.close(id); id = r; }
      media.close(img);
      if (!id) { setErr(tr('settings.users.avatarUnreadable')); return; }
      const w = turns % 2 ? d.height : d.width, h = turns % 2 ? d.width : d.height;
      // the preview is cut from a copy at most PREVIEW_PX on its long side (a 100 MB photo isn't copied at every move);
      // Save cuts from the full picture
      const k = Math.min(1, PREVIEW_PX / Math.max(w, h));
      const pid = k < 1 ? media.resize(id, Math.round(w * k), Math.round(h * k)) : id;
      setSrc({ id, w, h, pid: pid || id, k: pid ? k : 1 });
      setCenter({ x: w / 2, y: h / 2 });
    });
  };
  useEffect(() => () => { if (src) { if (src.pid !== src.id) media.close(src.pid); media.close(src.id); } }, [src]);
  useEffect(() => () => { if (preview) media.close(preview); }, [preview]);

  // the square seen in the circle (avatarmodel.js), cut by lord-media; shown a moment after the last change
  const box = src && center ? avatarCrop(src.w, src.h, zoom, center) : null;
  useEffect(() => {
    if (!box) return undefined;
    const tm = setTimeout(() => {
      const id = media.crop(src.pid, Math.round(box.x * src.k), Math.round(box.y * src.k), Math.max(1, Math.round(box.size * src.k)), Math.max(1, Math.round(box.size * src.k)));
      if (id) { media.view(id, { fit: 'cover', background: 0 }); setPreview(id); }
    }, 30);
    return () => clearTimeout(tm);
  }, [box?.x, box?.y, box?.size]);

  // dragging: the picture follows the pointer (one stage pixel is box.size / STAGE picture pixels)
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      const k = d.size / STAGE;
      setCenter({ x: d.cx - (e.x - d.x) * k, y: d.cy - (e.y - d.y) * k });
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  }, []);

  const save = async () => {
    if (!box) return;
    setBusy(true);
    const cut = media.crop(src.id, box.x, box.y, box.size, box.size);
    const small = cut && box.size > AVATAR_PX ? media.resize(cut, AVATAR_PX, AVATAR_PX) : cut;
    const out = `${fs.home()}/.cache/lordos/avatar.jpg`;          // one file, written again each time
    try { fs.mkdir(`${fs.home()}/.cache/lordos`); } catch (e) { /* already there */ }
    const r = small ? await media.saveImage(small, out, { format: 'jpeg', quality: 88 }) : null;
    if (small && small !== cut) media.close(small);
    if (cut) media.close(cut);
    setBusy(false);
    if (!r || r.ok === false) { setErr(tr('settings.users.avatarUnreadable')); return; }
    onSave(out);
  };

  const g = wm.frame(wm.get(win.id) || {});
  return (
    <Layer name="modal">
      <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 720, height: g.h || 560, alignItems: 'center',
        justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
        {picking ? (
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }}>
            <FilePicker title={tr('settings.users.avatarPick')} filter={IMAGES}
              onClose={() => { setPicking(false); onClose(); }} onPick={(p) => open(Array.isArray(p) ? p[0] : p)} />
          </View>
        ) : (
          <View style={{ width: 360, maxWidth: '94%', padding: 20, gap: spacing.m, borderRadius: 12, alignItems: 'center', backgroundColor: th.windowBg,
            border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
            <Text style={{ alignSelf: 'stretch', fontSize: 15, fontWeight: 600, color: th.text, textAlign: 'start' }}>{tr('settings.users.avatarEdit')}</Text>
            <View onMouseDown={(e) => { if (box) drag.current = { x: e.x, y: e.y, cx: center.x, cy: center.y, size: box.size }; }}
              style={{ width: STAGE, height: STAGE, borderRadius: STAGE / 2, overflow: 'hidden', backgroundColor: th.control, alignItems: 'center', justifyContent: 'center' }}>
              {preview ? <MediaView media={preview} style={{ position: 'absolute', left: 0, top: 0, width: STAGE, height: STAGE }} />
                : <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{err || tr('settings.users.avatarLoading')}</Text>}
            </View>
            <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'center' }}>{tr('settings.users.avatarHint')}</Text>
            <View style={{ alignSelf: 'stretch', flexDirection: 'row', alignItems: 'center', gap: spacing.s, direction: 'ltr' }}>
              <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{tr('settings.users.avatarZoom')}</Text>
              <Slider value={zoom} width={240} onChange={setZoom} />
            </View>
            {err && preview ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a' }}>{err}</Text> : null}
            <View style={{ alignSelf: 'stretch', flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
              <Button label={tr('settings.users.avatarOther')} onClick={() => { setErr(''); setPreview(0); setSrc(null); setPicking(true); }} disabled={busy} />
              <Button label={tr('settings.users.cancel')} onClick={onClose} disabled={busy} />
              <Button primary label={tr('settings.users.avatarSave')} onClick={save} disabled={!box || busy} />
            </View>
          </View>
        )}
      </View>
    </Layer>
  );
}
