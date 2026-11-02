// Voice Recorder: voice memos (our own design, in the style of desktop recorders).
//
// Records the microphone through PipeWire: the stream belongs to axia-server (after the
// permission) and axia-media writes the WAV. Recordings live in ~/Music/Recordings, with a
// waveform, play, rename, trim and export to Opus, MP3 or WAV (axia-media, with FFmpeg).
import {
  View, Text, Image, ScrollView, Button, TextInput, Segmented, Hairline, EmptyState, useTheme, useKeyboard, typeScale,
  t as tr, media, usePlayer, userDirs, fs, mediaFormatTime, formatDate, formatTime, addListener, useSound, microphones, defaultSource,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';

const APP = 'recorder';
const AUDIO = /\.(wav|opus|ogg|mp3|m4a|flac)$/i;

export function Icon({ size: s }) {
  const bars = [0.18, 0.34, 0.52, 0.3, 0.6, 0.42, 0.24, 0.12];
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #2b2b30, #0e0e10)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* red waveform on black */}
      {bars.map((h, i) => (
        <View key={i} style={{ position: 'absolute', left: s * (0.16 + i * 0.09), top: s * (0.5 - h / 2), width: s * 0.05, height: s * h,
          borderRadius: s * 0.025, backgroundColor: i === 4 ? '#fff' : '#ff453a' }} />
      ))}
    </View>
  );
}

function dir() {
  const d = `${userDirs().music}/${tr('recorder.folder')}`;
  try { fs.mkdir(userDirs().music); } catch (e) { /* already there */ }
  try { fs.mkdir(d); } catch (e) { /* already there */ }
  return d;
}
function freeName(base, ext) {
  for (let n = 1; n < 1000; n++) {
    const p = `${dir()}/${base}${n > 1 ? ` ${n}` : ''}.${ext}`;
    try { fs.stat(p); } catch (e) { return p; }
  }
  return `${dir()}/${base} ${Date.now()}.${ext}`;
}
const baseName = (p) => String(p).split('/').pop();
const stem = (p) => baseName(p).replace(/\.[^.]+$/, '');

// Waveform on an RGBA canvas (lighter than hundreds of boxes).
function Wave({ peaks, width, height, color, played = 0, sel }) {
  const px = useMemo(() => {
    const w = Math.max(1, Math.round(width)), h = Math.max(1, Math.round(height));
    const data = new Uint8Array(w * h * 4);
    const c = [parseInt(color.slice(1, 3), 16), parseInt(color.slice(3, 5), 16), parseInt(color.slice(5, 7), 16)];
    const n = peaks.length || 1;
    const top = Math.max(50, ...peaks);                 // drawn relative to the peak (as recorders do)
    for (let x = 0; x < w; x++) {
      const v = peaks.length ? peaks[Math.min(n - 1, Math.floor((x / w) * n))] / top : 0;
      const bh = Math.max(1, Math.round(v * (h - 2)));
      const y0 = Math.round((h - bh) / 2);
      const f = x / w;
      const inSel = !sel || (f >= sel[0] && f <= sel[1]);
      const a = (x % 3 === 2) ? 0 : f < played ? 255 : inSel ? 170 : 60;
      for (let y = y0; y < y0 + bh; y++) { const i = (y * w + x) * 4; data[i] = c[0]; data[i + 1] = c[1]; data[i + 2] = c[2]; data[i + 3] = a; }
    }
    return { width: w, height: h, data };
  }, [peaks, width, height, color, played, sel && sel[0], sel && sel[1]]);
  return <Image pixels={px} style={{ width, height }} />;
}

// Width of a box after layout (the waveform follows the window width).
function useBoxWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    if (!ref.current) return;
    const lw = Math.round(lord.getLayout(ref.current.id)[2]);
    if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
  });
  return w;
}

export default function Recorder() {
  const th = useTheme();
  const snd = useSound();
  const mics = microphones(snd);
  const [source, setSource] = useState('');
  const [rec, setRec] = useState(0);
  const [paused, setPaused] = useState(false);
  const [levels, setLevels] = useState([]);
  const [list, setList] = useState([]);
  const [sel, setSel] = useState(null);
  const [wave, setWave] = useState(null);
  const [playId, setPlayId] = useState(0);
  const [trim, setTrim] = useState(null);           // [from, to] 0..1
  const [renaming, setRenaming] = useState(null);
  const [format, setFormat] = useState('opus');
  const [msg, setMsg] = useState(null);
  const rp = usePlayer(rec);
  const pp = usePlayer(playId);
  const waveBox = useRef(null);
  const drag = useRef(null);

  const reload = () => {
    let l = [];
    try { l = fs.list(dir()).filter((e) => !e.dir && AUDIO.test(e.name)).map((e) => ({ path: `${dir()}/${e.name}`, name: e.name, mtime: e.mtime, size: e.size })); } catch (e) { /* no folder */ }
    setList(l.sort((a, b) => b.mtime - a.mtime));
  };
  useEffect(reload, []);
  useEffect(() => { if (rp.state === 'recording') setLevels((l) => [...l.slice(-180), rp.level]); }, [rp.ms]);
  useEffect(() => {
    if (rp.state === 'done' || rp.closed) { media.close(rec); setRec(0); setPaused(false); reload(); if (rp.done?.path) setSel(rp.done.path); }
  }, [rp.state, rp.closed]);
  useEffect(() => () => { media.close(rec); media.close(playId); }, []);

  // waveform of the selected one
  useEffect(() => {
    setWave(null); setTrim(null);
    media.close(playId); setPlayId(0);
    if (!sel) return;
    media.waveform(sel, 400).then((w) => setWave(w));
  }, [sel]);

  const start = async () => {
    if (rec) { media.stop(rec); return; }
    const path = freeName(tr('recorder.name', { date: formatDate(new Date()), time: formatTime(new Date()).replace(/:/g, '.') }), 'wav');
    const id = await media.audioRecord(APP, path, { source, appName: tr('apps.recorder') });
    if (!id) { setMsg(tr('recorder.denied')); setTimeout(() => setMsg(null), 3000); return; }
    setLevels([]);
    setRec(id);
  };
  const pause = () => { media.control(rec, paused ? 'play' : 'pause'); setPaused(!paused); };
  const play = () => {
    if (playId) { media.control(playId, pp.playing ? 'pause' : 'play'); return; }
    const id = media.play(APP, sel, { music: true, appName: tr('apps.recorder') });
    setPlayId(id || 0);
    if (trim && wave) setTimeout(() => media.control(id, 'seek', trim[0] * wave.duration), 200);
  };
  useEffect(() => { if (pp.state === 'ended') { media.close(playId); setPlayId(0); } }, [pp.state]);
  useEffect(() => { if (trim && wave && playId && pp.pos > trim[1] * wave.duration) media.control(playId, 'pause'); }, [pp.pos]);

  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (!drag.current || !waveBox.current) return;
      const [lx, , w] = lord.getLayout(waveBox.current.id);
      const f = Math.max(0, Math.min(1, (e.x - lx) / w));
      setTrim((t) => { const n = [...(t || [0, 1])]; n[drag.current.edge] = f; if (n[0] > n[1]) n.reverse(); return n; });
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  }, []);

  const exportAs = async (fmt, trimmed) => {
    const suffix = trimmed ? ` (${tr('recorder.trimmed')})` : '';
    const out = freeName(`${stem(sel)}${suffix}`, fmt === 'opus' ? 'opus' : fmt);
    const o = { format: fmt };
    if (trimmed && trim && wave) { o.from = Math.round(trim[0] * wave.duration); o.to = Math.round(trim[1] * wave.duration); }
    setMsg(tr('recorder.working'));
    const r = await media.transcode(APP, sel, out, o);
    setMsg(r.ok ? tr('recorder.exported', { name: baseName(r.path) }) : tr('recorder.exportError'));
    setTimeout(() => setMsg(null), 3500);
    if (r.ok) { reload(); if (trimmed) { setSel(r.path); } }
  };
  const doRename = () => {
    const name = (renaming || '').trim();
    if (name && sel) {
      const ext = baseName(sel).split('.').pop();
      const to = `${dir()}/${name.replace(/\//g, '-')}.${ext}`;
      try { fs.rename(sel, to); setSel(to); } catch (e) { setMsg(tr('recorder.renameError')); setTimeout(() => setMsg(null), 3000); }
      reload();
    }
    setRenaming(null);
  };
  useKeyboard((e) => {
    if (renaming !== null) return;
    if (e.key === ' ' && sel && !rec) play();
    if ((e.key === 'r' || e.key === 'R') && !e.ctrl && !e.meta) start();
  });

  const cur = list.find((x) => x.path === sel);
  const detail = useRef(null);
  const W = Math.max(200, Math.min(760, useBoxWidth(detail, 560) - 40));   // the box width minus its padding
  const WAVE_H = 120;
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 240, backgroundColor: th.sidebarBg }}>
        <Hairline edge="end" />
        <View style={{ height: 44, justifyContent: 'center', paddingHorizontal: 20 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: th.text }}>{tr('recorder.all')}</Text>
        </View>
        <ScrollView style={{ flex: 1, paddingHorizontal: 10 }}>
          {!list.length && <Text style={{ fontSize: typeScale.callout, color: th.textDim, paddingHorizontal: 10, paddingVertical: 8, lineHeight: 1.4 }}>{tr('recorder.empty')}</Text>}
          {list.map((x) => {
            const on = x.path === sel;
            return (
              <View key={x.path} onClick={() => setSel(x.path)} style={{ paddingHorizontal: 10, paddingVertical: 7, gap: 2, borderRadius: 7, marginBottom: 2,
                backgroundColor: on ? th.accent : 'transparent' }}>
                <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: on ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{stem(x.path)}</Text>
                <Text style={{ fontSize: typeScale.caption, color: on ? 'rgba(255,255,255,0.8)' : th.textDim, whiteSpace: 'nowrap' }}>
                  {`${formatDate(new Date(x.mtime))} · ${baseName(x.path).split('.').pop().toUpperCase()}`}
                </Text>
              </View>
            );
          })}
        </ScrollView>
        <View style={{ paddingHorizontal: 16, paddingTop: 12, paddingBottom: 16, gap: 10, alignItems: 'center' }}>
          <Hairline edge="top" />
          {rec ? (
            <View style={{ alignSelf: 'stretch', height: 48, flexDirection: 'row', alignItems: 'center', gap: 1, direction: 'ltr', overflow: 'hidden' }}>
              {levels.map((l, i) => <View key={i} style={{ width: 1, height: Math.max(1, Math.min(46, Math.sqrt(l) * 60)), backgroundColor: '#ff453a' }} />)}
            </View>
          ) : null}
          {rec ? <Text style={{ fontSize: 22, fontWeight: 300, color: th.text, direction: 'ltr' }}>{mediaFormatTime(rp.ms)}</Text> : null}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16 }}>
            {rec ? <View onClick={pause} style={{ width: 36, height: 36, borderRadius: 18, backgroundColor: th.control, alignItems: 'center', justifyContent: 'center' }}>
              {paused ? <Text style={{ fontSize: 13, color: th.text }}>▶</Text> : (
                <View style={{ flexDirection: 'row', gap: 4 }}>
                  <View style={{ width: 4, height: 13, borderRadius: 1, backgroundColor: th.text }} />
                  <View style={{ width: 4, height: 13, borderRadius: 1, backgroundColor: th.text }} />
                </View>
              )}
            </View> : null}
            <View onClick={start} style={{ width: 56, height: 56, borderRadius: 28, border: `3px solid ${th.mode === 'dark' ? 'rgba(255,255,255,0.28)' : 'rgba(0,0,0,0.18)'}`,
              alignItems: 'center', justifyContent: 'center' }}>
              <View style={{ width: rec ? 20 : 42, height: rec ? 20 : 42, borderRadius: rec ? 5 : 21, backgroundColor: '#ff3b30' }} />
            </View>
          </View>
          {mics.length > 1 && !rec && (
            <Segmented small value={source || defaultSource(snd)?.name || ''} onChange={setSource}
              options={mics.slice(0, 3).map((m) => [m.name, m.description.length > 14 ? `${m.description.slice(0, 13)}…` : m.description])} />
          )}
        </View>
      </View>
      <View ref={detail} style={{ flex: 1, minWidth: 0, padding: 20, gap: 12 }}>
        {cur ? (
          <>
            <View style={{ gap: 4 }}>
              {renaming !== null ? (
                <TextInput value={renaming} autoFocus selectAll onChange={setRenaming} onSubmit={doRename} onCancel={() => setRenaming(null)} size={17} />
              ) : (
                <Text onDoubleClick={() => setRenaming(stem(cur.path))} style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{stem(cur.path)}</Text>
              )}
              <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{`${formatDate(new Date(cur.mtime), 'long')} · ${wave ? mediaFormatTime(wave.duration) : '…'}`}</Text>
            </View>
            <View ref={waveBox} style={{ width: W, height: WAVE_H, borderRadius: 10, backgroundColor: th.hover, border: `1px solid ${th.separator}`, justifyContent: 'center', direction: 'ltr', overflow: 'hidden' }}
              onMouseDown={(e) => {
                const [lx, , w] = lord.getLayout(waveBox.current.id);
                const f = (e.x - lx) / w;
                if (!trim) { if (playId && wave) media.control(playId, 'seek', f * wave.duration); return; }
                drag.current = { edge: Math.abs(f - trim[0]) < Math.abs(f - trim[1]) ? 0 : 1 };
              }}>
              {wave ? <Wave peaks={wave.peaks} width={W - 2} height={WAVE_H - 16} color={th.mode === 'dark' ? '#ff6961' : '#e0352b'}
                played={playId && wave.duration ? pp.pos / wave.duration : 0} sel={trim} /> : <Text style={{ fontSize: typeScale.callout, color: th.textDim, textAlign: 'center' }}>…</Text>}
              {trim && [0, 1].map((k) => (
                <View key={k} style={{ position: 'absolute', left: trim[k] * W - 2, top: 0, width: 4, height: WAVE_H, borderRadius: 2, backgroundColor: '#ffcc00' }} />
              ))}
            </View>
            <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
              <Button primary label={pp.playing ? tr('recorder.pause') : tr('recorder.play')} onClick={play} />
              <Button label={tr('recorder.rename')} onClick={() => setRenaming(stem(cur.path))} />
              <Button label={trim ? tr('recorder.cancelTrim') : tr('recorder.trim')} onClick={() => setTrim(trim ? null : [0.1, 0.9])} />
              {trim && <Button primary label={tr('recorder.saveTrim')} onClick={() => exportAs(/\.wav$/i.test(sel) ? 'wav' : format, true)} />}
            </View>
            <View style={{ height: 1, backgroundColor: th.separator, marginVertical: 4 }} />
            <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
              <Text style={{ fontSize: typeScale.body, color: th.text }}>{tr('recorder.export')}</Text>
              <Segmented value={format} onChange={setFormat} options={[['opus', 'Opus'], ['mp3', 'MP3'], ['wav', 'WAV']]} />
              <Button label={tr('recorder.exportBtn')} onClick={() => exportAs(format, false)} />
            </View>
          </>
        ) : (
          <EmptyState icon={<Icon size={56} />} title={tr('recorder.emptyTitle')} hint={tr('recorder.hint')} />
        )}
        {msg && <Text style={{ fontSize: typeScale.callout, color: th.accent }}>{msg}</Text>}
      </View>
    </View>
  );
}
