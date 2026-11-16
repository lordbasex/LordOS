// videowidget.jsx — the desktop's Video widget (0.8.8.10, Lord Workspaces): a camera, a stream or a TV channel in a card,
// played by lord-stream (sdk/streams.jsx). Small, medium, large and extra large; pause (a live stream closes: no
// bandwidth, nothing decoding) and mute; Reconnecting while a lost stream comes back. Small and medium ask a camera
// for its sub stream where the address says how (lowQuality). The address is kept without credentials in the
// widget's opts ({ url, user, name }); the password goes to the keyring (Passwords).
import { useEffect, useState } from 'react';
import { View, Text, MediaView, addListener } from '@lordos/runtime';
import { Layer } from './layers.js';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { TextInput, Button } from './components.jsx';
import { media } from './media.js';
import { askKeyringUnlock } from './vault.js';
import { useStream, saveStreamPassword } from './streams.jsx';
import { splitCredentials, streamUrlOk, protocolOf, lowQuality, parseM3U } from './streammodel.js';
import { read as readText } from './fs.js';
import { ScrollView } from './components.jsx';

// { widget, set(patch) } while a widget's source is being set up
export const videoSetup = createStore(null);

const SHADE = 'rgba(0,0,0,0.42)';
function Overlay({ children }) {
  return <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', gap: 6, backgroundColor: SHADE, paddingHorizontal: 10 }}>{children}</View>;
}
const Msg = ({ text, small }) => <Text style={{ fontSize: small ? 11 : 12.5, fontWeight: 600, color: '#fff', textAlign: 'center' }}>{text}</Text>;
const Chip = ({ label, onClick }) => (
  <View onClick={onClick} style={{ height: 24, paddingHorizontal: 10, borderRadius: 12, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.22)' }}>
    <Text style={{ fontSize: 11.5, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap' }}>{label}</Text>
  </View>
);
// round control buttons, drawn (no glyph font): pause bars / play triangle, speaker with or without waves
function CtlButton({ kind, onClick, title }) {
  const k = 30;
  return (
    <View onClick={onClick} title={title} style={{ width: k, height: k, borderRadius: k / 2, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.55)', direction: 'ltr' }}>
      {kind === 'pause' ? <View style={{ flexDirection: 'row', gap: 3 }}><View style={{ width: 3.5, height: 12, borderRadius: 1, backgroundColor: '#fff' }} /><View style={{ width: 3.5, height: 12, borderRadius: 1, backgroundColor: '#fff' }} /></View> : null}
      {kind === 'play' ? <View style={{ marginLeft: 3, width: 0, height: 0, borderTopWidth: 7, borderBottomWidth: 7, borderLeftWidth: 11, borderTopColor: 'transparent', borderBottomColor: 'transparent', borderLeftColor: '#fff' }} /> : null}
      {kind === 'mute' || kind === 'unmute' ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 2 }}>
          <View style={{ width: 5, height: 7, backgroundColor: '#fff', borderRadius: 1 }} />
          <View style={{ width: 0, height: 0, borderTopWidth: 6, borderBottomWidth: 6, borderRightWidth: 6, borderTopColor: 'transparent', borderBottomColor: 'transparent', borderRightColor: '#fff', marginLeft: -2 }} />
          {kind === 'mute' ? <View style={{ width: 4, height: 9, borderRadius: 2, border: '1.5px solid #fff', borderLeftWidth: 0, marginLeft: 1 }} /> : <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff', marginLeft: 1 }}>×</Text>}
        </View>
      ) : null}
    </View>
  );
}

export function VideoBody({ w, setOpts }) {
  const o = w.opts || {};
  const [hover, setHover] = useState(false);
  const paused = !!o.paused, muted = o.muted !== false;      // muted unless asked: a wall of cameras shouldn't speak at once
  const small = w.size === 's' || w.size === 'm';
  const url = o.url ? (small ? lowQuality(o.url) : o.url) : '';
  const s = useStream({ app: 'player', url, user: o.user || '', active: true, paused, muted, audio: true, name: o.name || '' });
  useEffect(() => { if (s.id) media.view(s.id, { fit: 'cover' }); }, [s.id]);
  const bleed = { position: 'absolute', left: -15, top: -13, right: -15, bottom: -14, backgroundColor: '#000' };
  if (!o.url) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 }}>
        <View style={bleed} />
        <Msg text={t('shell.widgets.video.empty')} small={w.size === 's'} />
        <Chip label={t('shell.widgets.video.setup')} onClick={() => videoSetup.set({ widget: w, set: setOpts })} />
      </View>
    );
  }
  const status = s.status;
  const errText = s.error === 'media.streamAuth' ? t('shell.widgets.video.failedAuth') : s.error === 'media.streamAddress' ? t('shell.widgets.video.failedAddress')
    : s.error === 'media.noStreamHelper' ? t('shell.widgets.video.noHelper') : t('shell.widgets.video.failed');
  return (
    <View style={{ flex: 1 }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}>
      <View style={bleed}>{s.id ? <MediaView media={s.id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} /> : null}</View>
      {status === 'connecting' ? <Overlay><Msg text={t('shell.widgets.video.connecting')} small={w.size === 's'} /></Overlay> : null}
      {status === 'reconnecting' ? <Overlay><Msg text={t('shell.widgets.video.reconnecting', { n: s.attempt })} small={w.size === 's'} /></Overlay> : null}
      {status === 'paused' ? <Overlay><CtlButton kind="play" title={t('shell.widgets.video.play')} onClick={() => setOpts({ paused: false })} /></Overlay> : null}
      {status === 'locked' ? <Overlay><Msg text={t('shell.widgets.video.locked')} small={w.size === 's'} /><Chip label={t('shell.widgets.video.unlock')} onClick={() => askKeyringUnlock()} /></Overlay> : null}
      {status === 'failed' ? <Overlay><Msg text={errText} small={w.size === 's'} /><Chip label={t('shell.widgets.video.setup')} onClick={() => videoSetup.set({ widget: w, set: setOpts })} /></Overlay> : null}
      {/* name and controls: on hover, out of the picture's way */}
      {hover && status !== 'paused' ? (
        <View style={{ position: 'absolute', left: -6, right: -6, bottom: -6, flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <View style={{ flex: 1, minWidth: 0 }}>
            {w.size !== 's' ? <Text style={{ fontSize: 12, fontWeight: 650, color: '#fff', textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{o.name || protocolOf(o.url)}</Text> : null}
          </View>
          <CtlButton kind={muted ? 'unmute' : 'mute'} title={t(muted ? 'shell.widgets.video.unmute' : 'shell.widgets.video.mute')} onClick={() => setOpts({ muted: !muted })} />
          <CtlButton kind="pause" title={t('shell.widgets.video.pause')} onClick={() => setOpts({ paused: true })} />
        </View>
      ) : null}
    </View>
  );
}

// The source of one widget: a name and an address (credentials allowed: they go to the keyring), or a channel of an
// M3U list (an http(s) list read by lord-stream, or a file in the home folder read here).
export function VideoSetup() {
  const v = useStore(videoSetup);
  const th = useTheme() || {};
  const o = v?.widget?.opts || {};
  const [name, setName] = useState(''), [addr, setAddr] = useState(''), [list, setList] = useState(''), [chans, setChans] = useState(null), [busy, setBusy] = useState(false), [err, setErr] = useState('');
  useEffect(() => { if (v) { setName(o.name || ''); setAddr(o.url ? (o.user ? o.url.replace(/^([a-z]+:\/\/)/, `$1${o.user}@`) : o.url) : ''); setList(''); setChans(null); setErr(''); } }, [v]);
  useEffect(() => (v ? addListener('keydown', (e) => { if (e.key === 'Escape') videoSetup.set(null); }) : undefined), [v]);
  if (!v) return null;
  const save = async (url0, name0) => {
    const sp = splitCredentials(url0.trim());
    if (!streamUrlOk(sp.url)) { setErr(t('shell.widgets.video.failedAddress')); return; }
    if (sp.pass) await saveStreamPassword(sp.url, sp.user, sp.pass);
    v.set({ url: sp.url, user: sp.user, name: (name0 || '').trim().slice(0, 60), paused: false });
    videoSetup.set(null);
  };
  const load = async () => {
    const src = list.trim();
    setErr('');
    setBusy(true);
    let r;
    if (/^https?:\/\//.test(src)) r = await media.m3u('player', src);
    else { try { r = { ok: true, items: parseM3U(readText(src.replace(/^~\//, ''))) }; } catch (e) { r = { ok: false, code: 'media.streamCannotOpen' }; } }
    setBusy(false);
    if (!r.ok || !r.items?.length) { setErr(t('shell.widgets.video.noChannels')); setChans(null); return; }
    setChans(r.items.slice(0, 400));
  };
  const field = { alignSelf: 'stretch' };
  const label = (k) => <Text style={{ fontSize: 11.5, fontWeight: 600, color: th.textDim, textAlign: 'start' }}>{t(k)}</Text>;
  return (
    <Layer name="modal">
      <View onMouseDown={(e) => { if (e.target === e.currentTarget) videoSetup.set(null); }} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.25)' }}>
        <View style={{ width: 460, maxHeight: '80%', padding: 18, gap: 8, borderRadius: 16, backgroundColor: th.windowBg || '#1f1f23', border: `1px solid ${th.separator}`, boxShadow: '0 18px 50px rgba(0,0,0,0.45)' }}>
          <Text style={{ fontSize: 15, fontWeight: 700, color: th.text, textAlign: 'start' }}>{t('shell.widgets.video.formTitle')}</Text>
          {label('shell.widgets.video.nameLabel')}
          <TextInput value={name} onChange={setName} placeholder={t('shell.widgets.video.namePlaceholder')} style={field} />
          {label('shell.widgets.video.urlLabel')}
          <TextInput value={addr} onChange={setAddr} placeholder="rtsp://user:password@192.168.1.10:554/stream" style={field} onSubmit={() => save(addr, name)} />
          <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'start' }}>{t('shell.widgets.video.urlHint')}</Text>
          {label('shell.widgets.video.listLabel')}
          <View style={{ flexDirection: 'row', gap: 6, alignItems: 'center' }}>
            <TextInput value={list} onChange={setList} placeholder="https://…/list.m3u" style={{ flex: 1 }} onSubmit={load} />
            <Button small label={busy ? t('shell.widgets.video.loading') : t('shell.widgets.video.load')} disabled={busy || !list.trim()} onClick={load} />
          </View>
          {chans ? (
            <ScrollView style={{ height: Math.min(200, chans.length * 28 + 8) }}>
              {chans.map((c) => (
                <View key={c.url} onClick={() => save(c.url, c.name)} style={{ flexDirection: 'row', gap: 8, paddingVertical: 4, paddingHorizontal: 6, borderRadius: 6, backgroundColor: th.hover }}>
                  <Text style={{ flex: 1, fontSize: 12.5, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{c.name}</Text>
                  <Text style={{ fontSize: 11, color: th.textDim }}>{c.group || protocolOf(c.url)}</Text>
                </View>
              ))}
            </ScrollView>
          ) : null}
          {err ? <Text style={{ fontSize: 12, color: '#E5484D', textAlign: 'start' }}>{err}</Text> : null}
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 6 }}>
            <Button label={t('clock.cancel')} onClick={() => videoSetup.set(null)} />
            <Button primary label={t('shell.widgets.video.save')} disabled={!addr.trim()} onClick={() => save(addr, name)} />
          </View>
        </View>
      </View>
    </Layer>
  );
}
