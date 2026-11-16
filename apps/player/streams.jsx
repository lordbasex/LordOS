// streams.jsx — the Player's Streams (0.8.8.10; docs/MEDIA.md "Network streams"): cameras, streams and TV channels by
// their address, played by lord-stream through useStream (reconnecting by itself), a list of saved ones and the channels
// of an M3U list (an http(s) address read by lord-stream, or a file). A saved stream keeps its name, address and user;
// its password is in the keyring (Passwords), never in settings.json.
import {
  View, Text, MediaView, ScrollView, Button, TextInput, ToolbarButton, useTheme, useStore, settings, media, t as tr,
  useStream, saveStreamPassword, splitCredentials, streamUrlOk, protocolOf, parseM3U, askKeyringUnlock, fs, typeScale,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';

const saved = () => (Array.isArray(settings.get().playerStreams) ? settings.get().playerStreams : []);

function Pane({ s, item, paused, setPaused, muted, setMuted, onSave, onClose, isSaved }) {
  const th = useTheme();
  useEffect(() => { if (s.id) media.view(s.id, { fit: 'contain' }); }, [s.id]);
  const msg = s.status === 'connecting' ? tr('shell.widgets.video.connecting')
    : s.status === 'reconnecting' ? tr('shell.widgets.video.reconnecting', { n: s.attempt })
    : s.status === 'locked' ? tr('shell.widgets.video.locked')
    : s.status === 'failed' ? (s.error === 'media.streamAuth' ? tr('shell.widgets.video.failedAuth') : s.error === 'media.streamAddress' ? tr('shell.widgets.video.failedAddress') : tr('shell.widgets.video.failed'))
    : s.status === 'paused' ? tr('shell.widgets.video.pause') : '';
  return (
    <View style={{ gap: 8, paddingHorizontal: 10, paddingBottom: 12 }}>
      <View style={{ aspectRatio: 16 / 9, maxHeight: 420, borderRadius: 10, overflow: 'hidden', backgroundColor: '#000' }}>
        {s.id ? <MediaView media={s.id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} /> : null}
        {msg ? (
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', gap: 8, backgroundColor: 'rgba(0,0,0,0.45)' }}>
            <Text style={{ fontSize: 14, fontWeight: 600, color: '#fff', textAlign: 'center' }}>{msg}</Text>
            {s.status === 'locked' ? <Button small label={tr('shell.widgets.video.unlock')} onClick={() => askKeyringUnlock()} /> : null}
          </View>
        ) : null}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
        <View style={{ flex: 1, minWidth: 120 }}>
          <Text style={{ fontSize: 14, fontWeight: 600, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{item.name || protocolOf(item.url)}</Text>
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{`${protocolOf(item.url)} · ${item.url}`}</Text>
        </View>
        <Button small label={tr(paused ? 'shell.widgets.video.play' : 'shell.widgets.video.pause')} onClick={() => setPaused(!paused)} />
        <Button small label={tr(muted ? 'shell.widgets.video.unmute' : 'shell.widgets.video.mute')} onClick={() => setMuted(!muted)} />
        {!isSaved ? <Button small label={tr('player.streams.save')} onClick={onSave} /> : null}
        <Button small label={tr('player.streams.close')} onClick={onClose} />
      </View>
    </View>
  );
}

export function StreamsView({ onStart }) {
  const th = useTheme();
  const list = useStore(settings, (x) => x.playerStreams) || [];
  const [item, setItem] = useState(null);           // { name, url, user }
  const [paused, setPaused] = useState(false);
  const [muted, setMuted] = useState(false);
  const [form, setForm] = useState(null);           // { name, addr } | { list }
  const [chans, setChans] = useState(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const s = useStream({ app: 'player', url: item?.url || '', user: item?.user || '', active: !!item, paused, muted, name: item?.name || '' });
  const play = (it) => { setItem(it); setPaused(false); onStart?.(); };
  const openAddr = async () => {
    const sp = splitCredentials((form.addr || '').trim());
    if (!streamUrlOk(sp.url)) { setErr(tr('shell.widgets.video.failedAddress')); return; }
    if (sp.pass) await saveStreamPassword(sp.url, sp.user, sp.pass);
    setErr('');
    setForm(null);
    play({ name: (form.name || '').trim().slice(0, 60), url: sp.url, user: sp.user });
  };
  const loadList = async () => {
    const src = (form.list || '').trim();
    setBusy(true);
    setErr('');
    let r;
    if (/^https?:\/\//.test(src)) r = await media.m3u('player', src);
    else { try { r = { ok: true, items: parseM3U(fs.read(src.replace(/^~\//, ''))) }; } catch (e) { r = { ok: false }; } }
    setBusy(false);
    if (!r.ok || !r.items?.length) { setErr(tr('shell.widgets.video.noChannels')); return; }
    setChans(r.items);
    setForm(null);
  };
  const isSaved = !!item && list.some((x) => x.url === item.url && (x.user || '') === (item.user || ''));
  const save = () => { if (item && !isSaved) settings.set({ playerStreams: [...list, { name: item.name || protocolOf(item.url), url: item.url, user: item.user || '' }].slice(-200) }); };
  const remove = (x) => settings.set({ playerStreams: list.filter((y) => y !== x) });
  const row = (x, i, extra) => (
    <View key={`${x.url}#${i}`} onClick={() => play({ name: x.name, url: x.url, user: x.user || '' })}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 10, height: 36, paddingHorizontal: 10, borderRadius: 6, backgroundColor: item?.url === x.url ? th.hover : 'transparent' }}>
      <Text style={{ width: 52, fontSize: 10.5, fontWeight: 700, color: th.textDim }}>{protocolOf(x.url)}</Text>
      <Text style={{ flex: 1, fontSize: 13, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{x.name || x.url}</Text>
      {x.group ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{x.group}</Text> : null}
      {extra}
    </View>
  );
  return (
    <View style={{ gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8, paddingHorizontal: 10, flexWrap: 'wrap' }}>
        <Button small label={tr('player.streams.open')} onClick={() => { setErr(''); setForm({ name: '', addr: '' }); }} />
        <Button small label={tr('player.streams.list')} onClick={() => { setErr(''); setForm({ list: '' }); }} />
      </View>
      {form && form.list === undefined ? (
        <View style={{ gap: 6, paddingHorizontal: 10 }}>
          <TextInput value={form.name} onChange={(v) => setForm({ ...form, name: v })} placeholder={tr('shell.widgets.video.namePlaceholder')} />
          <TextInput value={form.addr} onChange={(v) => setForm({ ...form, addr: v })} placeholder="rtsp://user:password@192.168.1.10:554/stream" onSubmit={openAddr} autoFocus />
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' }}>{tr('shell.widgets.video.urlHint')}</Text>
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
            <Button small label={tr('clock.cancel')} onClick={() => setForm(null)} />
            <Button small primary label={tr('shell.widgets.video.play')} disabled={!form.addr?.trim()} onClick={openAddr} />
          </View>
        </View>
      ) : null}
      {form && form.list !== undefined ? (
        <View style={{ gap: 6, paddingHorizontal: 10 }}>
          <TextInput value={form.list} onChange={(v) => setForm({ list: v })} placeholder="https://…/list.m3u · ~/Downloads/list.m3u" onSubmit={loadList} autoFocus />
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
            <Button small label={tr('clock.cancel')} onClick={() => setForm(null)} />
            <Button small primary label={busy ? tr('shell.widgets.video.loading') : tr('shell.widgets.video.load')} disabled={busy || !form.list?.trim()} onClick={loadList} />
          </View>
        </View>
      ) : null}
      {err ? <Text style={{ fontSize: 12, color: '#E5484D', textAlign: 'start', paddingHorizontal: 10 }}>{err}</Text> : null}
      {item ? <Pane s={s} item={item} paused={paused} setPaused={setPaused} muted={muted} setMuted={setMuted} onSave={save} onClose={() => setItem(null)} isSaved={isSaved} /> : null}
      {list.length ? <Text style={{ fontSize: 12, fontWeight: 600, color: th.textDim, textAlign: 'start', paddingHorizontal: 10 }}>{tr('player.streams.saved')}</Text> : null}
      {list.map((x, i) => row(x, i, <ToolbarButton small icon="×" title={tr('player.streams.remove')} onClick={() => remove(x)} />))}
      {chans ? <Text style={{ fontSize: 12, fontWeight: 600, color: th.textDim, textAlign: 'start', paddingHorizontal: 10, marginTop: 6 }}>{tr('player.streams.channels', { n: chans.length })}</Text> : null}
      {chans ? chans.slice(0, 500).map((x, i) => row(x, i)) : null}
      {!item && !list.length && !chans && !form ? <Text style={{ fontSize: 13, color: th.textDim, textAlign: 'start', paddingHorizontal: 10, paddingTop: 20 }}>{tr('player.streams.empty')}</Text> : null}
    </View>
  );
}
