// Camera: webcam preview (V4L2), photos and videos (our own design, in the style of desktop camera
// apps). axia-server opens the device after the camera permission; axia-media (sandboxed) reads
// and encodes it. Photos and videos go to ~/Pictures/Camera.
//
// The window is always dark, like a viewfinder: the controls below the preview don't follow the
// light theme.
import {
  View, Text, Image, MediaView, Toggle, useTheme, useKeyboard, wm,
  t as tr, errorText, media, usePlayer, useThumb, userDirs, fs, mediaFormatTime, useSettings,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const APP = 'camera';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #3a3f47, #15171b)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden', alignItems: 'center', justifyContent: 'center' }}>
      {/* camera body with a large lens */}
      <View style={{ position: 'absolute', left: s * 0.16, top: s * 0.3, width: s * 0.68, height: s * 0.46, borderRadius: s * 0.08, backgroundColor: '#d9dde3' }} />
      <View style={{ position: 'absolute', left: s * 0.24, top: s * 0.23, width: s * 0.18, height: s * 0.09, borderRadius: s * 0.03, backgroundColor: '#d9dde3' }} />
      <View style={{ position: 'absolute', left: s * 0.32, top: s * 0.33, width: s * 0.36, height: s * 0.36, borderRadius: s * 0.18, backgroundColor: '#23262c' }} />
      <View style={{ position: 'absolute', left: s * 0.38, top: s * 0.39, width: s * 0.24, height: s * 0.24, borderRadius: s * 0.12, background: 'linear-gradient(135deg, #5b8cff, #1d2a66)' }} />
      <View style={{ position: 'absolute', left: s * 0.44, top: s * 0.43, width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: 'rgba(255,255,255,0.8)' }} />
      <View style={{ position: 'absolute', left: s * 0.72, top: s * 0.35, width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: '#30d158' }} />
    </View>
  );
}

const pad = (n) => String(n).padStart(2, '0');
function stamp() {
  const d = new Date();
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}.${pad(d.getMinutes())}.${pad(d.getSeconds())}`;
}
function outDir() {
  const dir = `${userDirs().pictures}/${tr('camera.folder')}`;
  try { fs.mkdir(userDirs().pictures); } catch (e) { /* already there */ }
  try { fs.mkdir(dir); } catch (e) { /* already there */ }
  return dir;
}
function freeName(base, ext) {
  const dir = outDir();
  for (let n = 1; n < 1000; n++) {
    const p = `${dir}/${base}${n > 1 ? ` (${n})` : ''}.${ext}`;
    try { fs.stat(p); } catch (e) { return p; }
  }
  return `${dir}/${base} ${Date.now()}.${ext}`;
}

function Shot({ path, onOpen }) {
  const thumb = useThumb(path, 128);
  return (
    <View onClick={onOpen} style={{ width: 52, height: 40, borderRadius: 6, overflow: 'hidden', backgroundColor: '#2a2a2e', border: '1px solid rgba(255,255,255,0.12)' }}>
      {thumb ? <Image src={thumb} style={{ width: 52, height: 40, objectFit: 'cover' }} /> : null}
    </View>
  );
}

// Photo / Video switch, as camera apps do it: two words, the current one in yellow.
function ModeSwitch({ value, onChange }) {
  return (
    <View style={{ flexDirection: 'row', gap: 4 }}>
      {[['photo', tr('camera.photo')], ['video', tr('camera.video')]].map(([v, label]) => (
        <View key={v} onClick={() => onChange(v)} style={{ height: 24, paddingHorizontal: 10, borderRadius: 12, justifyContent: 'center',
          backgroundColor: value === v ? 'rgba(255,214,10,0.14)' : 'transparent' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: value === v ? '#ffd60a' : 'rgba(255,255,255,0.7)', whiteSpace: 'nowrap' }}>{label}</Text>
        </View>
      ))}
    </View>
  );
}

// The empty state on the black viewfinder: a drawn camera, a title and one hint line.
function NoCamera({ title, hint }) {
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', gap: 8, paddingHorizontal: 32 }}>
      <View style={{ width: 52, height: 38, marginBottom: 8 }}>
        <View style={{ position: 'absolute', left: 12, top: 0, width: 16, height: 8, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.35)' }} />
        <View style={{ position: 'absolute', left: 0, top: 5, width: 52, height: 33, borderRadius: 7, backgroundColor: 'rgba(255,255,255,0.35)' }} />
        <View style={{ position: 'absolute', left: 15, top: 10, width: 22, height: 22, borderRadius: 11, backgroundColor: '#0b0b0d' }} />
        <View style={{ position: 'absolute', left: 20, top: 15, width: 12, height: 12, borderRadius: 6, backgroundColor: 'rgba(255,255,255,0.35)' }} />
      </View>
      <Text style={{ fontSize: 15, fontWeight: 600, color: '#f2f2f7', textAlign: 'center' }}>{title}</Text>
      {hint ? <Text style={{ fontSize: 12, color: 'rgba(235,235,245,0.6)', textAlign: 'center', maxWidth: 360, lineHeight: 1.4 }}>{hint}</Text> : null}
    </View>
  );
}

export default function Camera() {
  const th = useTheme();
  const [s, update] = useSettings();
  const mirror = (s.camera?.mirror ?? true);
  const [cams, setCams] = useState(() => media.cameras());
  const [dev, setDev] = useState(() => s.camera?.device || media.cameras()[0]?.device || null);
  const [id, setId] = useState(0);
  const [mode, setMode] = useState('photo');
  const [flash, setFlash] = useState(false);
  const [recStart, setRecStart] = useState(0);
  const [shots, setShots] = useState([]);
  const [denied, setDenied] = useState(false);
  const [now, setNow] = useState(Date.now());
  const p = usePlayer(id);
  const idRef = useRef(0);

  // open the chosen camera
  useEffect(() => {
    let alive = true;
    media.close(idRef.current);
    setId(0);
    if (!dev) return undefined;
    media.camera(APP, dev, { width: 1280, height: 720 }).then((nid) => {
      if (!alive) { media.close(nid); return; }
      if (!nid) { setDenied(true); return; }
      setDenied(false);
      idRef.current = nid;
      setId(nid);
    });
    return () => { alive = false; };
  }, [dev]);
  useEffect(() => () => media.close(idRef.current), []);
  useEffect(() => { if (id) media.view(id, { mirror: mirror ? 1 : 0, fit: 'contain', background: 0xff000000 }); }, [id, mirror]);
  // a camera that was just plugged in
  useEffect(() => { const tmr = setInterval(() => setCams(media.cameras()), 3000); return () => clearInterval(tmr); }, []);
  useEffect(() => { if (!dev && cams.length) setDev(cams[0].device); }, [cams]);
  useEffect(() => { if (!recStart) return undefined; const tmr = setInterval(() => setNow(Date.now()), 250); return () => clearInterval(tmr); }, [recStart]);

  useEffect(() => {
    if (p.photo?.ok) { setShots((l) => [p.photo.path, ...l].slice(0, 12)); }
  }, [p.photo]);
  useEffect(() => {
    if (p.video?.ok) setShots((l) => [p.video.path, ...l].slice(0, 12));
    setRecStart(0);
  }, [p.video]);

  const shoot = () => {
    if (!id) return;
    if (mode === 'photo') {
      media.photo(id, freeName(tr('camera.photoName', { date: stamp() }), 'jpg'), mirror);
      setFlash(true);
      setTimeout(() => setFlash(false), 160);
      return;
    }
    if (recStart) { media.cameraRecord(id, APP, null); return; }
    media.cameraRecord(id, APP, freeName(tr('camera.videoName', { date: stamp() }), 'mp4'), { mic: true, mirror, appName: tr('apps.camera') })
      .then((ok) => { if (ok) setRecStart(Date.now()); });
  };
  useKeyboard((e) => { if (e.key === ' ' || e.key === 'Enter') shoot(); });

  const cam = cams.find((c) => c.device === dev);
  const recording = mode === 'video' && recStart > 0;
  return (
    <View style={{ flex: 1, backgroundColor: '#0b0b0d' }}>
      <View style={{ flex: 1 }}>
        <MediaView media={id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
        {(!cams.length || denied || p.error) && (
          !cams.length ? <NoCamera title={tr('camera.none')} hint={tr('camera.noneHint')} />
            : <NoCamera title={denied ? tr('camera.deniedTitle') : tr('camera.errorTitle')} hint={denied ? tr('camera.denied') : tr('camera.error', { error: errorText(p.error) })} />
        )}
        {flash && <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(255,255,255,0.85)' }} />}
        {recStart > 0 && (
          <View style={{ position: 'absolute', top: 12, left: 0, right: 0, alignItems: 'center' }}>
            <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', paddingHorizontal: 12, height: 26, borderRadius: 13, backgroundColor: 'rgba(0,0,0,0.6)' }}>
              <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#ff3b30' }} />
              <Text style={{ fontSize: 12, fontWeight: 500, color: '#fff', direction: 'ltr' }}>{mediaFormatTime(now - recStart)}</Text>
            </View>
          </View>
        )}
      </View>
      <View style={{ height: 88, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 20, backgroundColor: '#161618' }}>
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: 'rgba(255,255,255,0.08)' }} />
        <View style={{ flex: 1, flexDirection: 'row', gap: 6 }}>
          {shots.slice(0, 3).map((x) => <Shot key={x} path={x} onOpen={() => wm.open(/\.mp4$/.test(x) ? 'player' : 'photos', { new: true, args: { path: x } })} />)}
        </View>
        <View onClick={shoot} style={{ width: 60, height: 60, borderRadius: 30, border: '3px solid #ffffff', alignItems: 'center', justifyContent: 'center', opacity: id ? 1 : 0.5 }}>
          <View style={{ width: recording ? 22 : 46, height: recording ? 22 : 46, borderRadius: recording ? 5 : 23,
            backgroundColor: mode === 'video' ? '#ff3b30' : '#ffffff' }} />
        </View>
        <View style={{ flex: 1, alignItems: 'flex-end', gap: 8 }}>
          <ModeSwitch value={mode} onChange={(v) => !recStart && setMode(v)} />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ fontSize: 12, color: 'rgba(235,235,245,0.7)' }}>{tr('camera.mirror')}</Text>
            <Toggle on={mirror} onChange={(v) => update({ camera: { ...(s.camera || {}), mirror: v } })} />
          </View>
        </View>
      </View>
      {cams.length > 1 && (
        <View style={{ position: 'absolute', right: 12, top: 12, flexDirection: 'row', gap: 4, padding: 3, borderRadius: 9, backgroundColor: 'rgba(0,0,0,0.55)' }}>
          {cams.map((c) => (
            <View key={c.device} onClick={() => { setDev(c.device); update({ camera: { ...(s.camera || {}), device: c.device } }); }}
              style={{ paddingHorizontal: 10, height: 24, borderRadius: 6, justifyContent: 'center', backgroundColor: c.device === dev ? th.accent : 'transparent' }}>
              <Text style={{ fontSize: 12, color: '#fff' }}>{c.name}</Text>
            </View>
          ))}
        </View>
      )}
      {cam && cams.length === 1 && (
        <View style={{ position: 'absolute', left: 12, top: 12, paddingHorizontal: 10, height: 24, justifyContent: 'center', borderRadius: 12, backgroundColor: 'rgba(0,0,0,0.5)' }}>
          <Text style={{ fontSize: 11, color: '#e5e5ea' }}>{cam.name}{p.info ? ` · ${p.info.width}×${p.info.height}` : ''}</Text>
        </View>
      )}
    </View>
  );
}
