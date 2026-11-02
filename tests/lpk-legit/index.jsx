// Legitimate test app (tests/lpk-legit): asks for the microphone, the camera and files, and works
// after consent. Its texts use t() and its own catalogs (locales/<lang>.json in the package), so it
// also tests language and direction (RTL) live inside the isolated process.
// It also tests, inside the sandbox: several windows («New window» in its menu and in the Dock),
// the camera through axia-media's read-only frame ring, and saving a photo/video through the
// system's save dialog (powerbox). See docs/PRIVACY.md.
import { View, Text, Button, MediaView, useTheme, useLocale, useAppWindow, requestPermission, markDeviceUse, onPermissionRevoked, openFile, wm, micStart, micStop, micLevel, media, usePlayer } from '@lordos/sdk';
import { useState, useEffect } from 'react';

export function Icon({ size: s }) {
  return <View style={{ width: s, height: s, borderRadius: s * 0.22, backgroundColor: '#ff9f0a', alignItems: 'center', justifyContent: 'center' }}>
    <Text style={{ fontSize: s * 0.6, lineHeight: s }}>🎙</Text></View>;
}

let windowCount = 1;

export default function Recorder() {
  const th = useTheme();
  const { t } = useLocale();
  const { id } = useAppWindow();
  const appId = String(id || '').split('#')[0];
  const [rec, setRec] = useState(false);
  const [status, setStatus] = useState('');
  const [level, setLevel] = useState(0);
  const [cam, setCam] = useState(0);
  const [video, setVideo] = useState(false);
  const cs = usePlayer(cam);
  useEffect(() => {
    if (!rec) return undefined;
    const h = setInterval(() => setLevel(micLevel()), 100);
    return () => clearInterval(h);
  }, [rec]);
  useEffect(() => () => markDeviceUse(id, 'microphone', false), [id]);
  useEffect(() => onPermissionRevoked((perm) => {
    if (perm === 'microphone') { micStop(); setRec(false); setLevel(0); setStatus('stopped'); }
    if (perm === 'camera') { setCam((c) => { if (c) media.close(c); return 0; }); setVideo(false); setStatus('revoked'); }
  }), []);
  useEffect(() => () => { if (cam) media.close(cam); }, [cam]);
  // a second window gets its own title
  useEffect(() => { if (String(id).includes('#')) wm.setTitle(id, t('grabadora.window', { n: ++windowCount })); }, [id]);
  // photo / video saved through the powerbox (the app never sees the folder)
  useEffect(() => {
    if (cs.photo) setStatus(cs.photo.ok ? 'photoSaved' : 'saveFailed');
  }, [cs.photo]);
  useEffect(() => {
    if (cs.video) setStatus(cs.video.ok ? 'videoSaved' : 'saveFailed');
  }, [cs.video]);
  useEffect(() => {
    wm.setMenus(id, [{ label: t('grabadora.menu'), items: [
      { label: t('grabadora.newWindow'), hint: '❖N', onClick: () => wm.open(appId, { new: true }) },
      '-',
      { label: t('grabadora.record'), onClick: toggle },
      { label: cam ? t('grabadora.cameraOff') : t('grabadora.camera'), onClick: toggleCam },
      { label: t('grabadora.attach'), onClick: attach },
      '-',
      { label: t('grabadora.about'), onClick: () => setStatus('about') },
    ] }]);
  });

  const toggle = async () => {
    if (rec) { setRec(false); micStop(); setLevel(0); markDeviceUse(id, 'microphone', false); setStatus('stopped'); return; }
    const ok = await requestPermission(id, 'microphone');
    if (!ok) { setStatus('noMic'); return; }
    micStart();
    setRec(true);
    markDeviceUse(id, 'microphone', true);
    setStatus('recording');
  };
  const toggleCam = async () => {
    if (cam) { media.close(cam); setCam(0); setVideo(false); setStatus('camOff'); return; }
    if (!(await requestPermission(id, 'camera'))) { setStatus('noCam'); return; }
    const list = media.cameras();
    if (!list.length) { setStatus('noDevice'); return; }
    const c = await media.camera(appId, list[0].device, { width: 1280, height: 720 });
    if (!c) { setStatus('noCam'); return; }
    setCam(c);
    setStatus('camOn');
  };
  const photo = () => { if (cam) media.photo(cam, 'foto.jpg', false); };
  const toggleVideo = async () => {
    if (!cam) return;
    if (video) { await media.cameraRecord(cam, appId, null); setVideo(false); return; }
    if (await media.cameraRecord(cam, appId, 'video.mp4', { mic: false })) setVideo(true);
  };
  const [attached, setAttached] = useState('');
  const attach = async () => {
    const f = await openFile();                 // the system draws the dialog; the app only gets the fd
    if (!f) { setStatus('canceled'); return; }
    setAttached(`${f.name} · ${f.arrayBuffer().byteLength} B`);
    setStatus('attached');
  };
  const saved = (status === 'photoSaved' ? cs.photo?.path : cs.video?.path) || '';
  return (
    <View style={{ flex: 1, padding: 16, gap: 12, alignItems: 'center', justifyContent: 'center' }}>
      {cam ? <MediaView media={cam} style={{ alignSelf: 'stretch', flex: 1, borderRadius: 8 }} /> : <Text style={{ fontSize: 48 }}>{rec ? '🔴' : '🎙'}</Text>}
      {rec ? <View style={{ width: 240, height: 10, borderRadius: 5, backgroundColor: th.hover, overflow: 'hidden' }}>
        <View style={{ width: `${Math.min(100, Math.round(level * 400))}%`, height: '100%', backgroundColor: '#30d158' }} />
      </View> : null}
      <View style={{ flexDirection: 'row', gap: 8, flexWrap: 'wrap', justifyContent: 'center' }}>
        <Button primary label={rec ? t('grabadora.stop') : t('grabadora.record')} onClick={toggle} />
        <Button label={cam ? t('grabadora.cameraOff') : t('grabadora.camera')} onClick={toggleCam} />
        {cam ? <Button label={t('grabadora.photo')} onClick={photo} /> : null}
        {cam ? <Button label={video ? t('grabadora.videoStop') : t('grabadora.video')} onClick={toggleVideo} /> : null}
        <Button label={t('grabadora.newWindow')} onClick={() => wm.open(appId, { new: true })} />
      </View>
      <Text style={{ fontSize: 12, color: th.textDim, textAlign: 'center' }}>{status ? t(`grabadora.${status}`, { name: status === 'attached' ? attached : saved }) : ''}</Text>
    </View>
  );
}
