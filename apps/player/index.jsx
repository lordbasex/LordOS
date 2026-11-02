// Player: videos and music (in the style of desktop players, with our own design).
//
// Decoding belongs to axia-media (sandboxed); here are the library (artist and album from the
// tags, covers), the queue, shuffle and repeat, the video controls (time bar, volume, .srt and
// in-stream subtitles, full screen, picture in picture) and what is published for Control Center
// and the media keys (nowPlaying).
import {
  View, Text, Image, MediaView, ScrollView, SidebarItem, SectionTitle, Hairline, EmptyState, ToolbarButton, useTheme, useAppWindow, useKeyboard,
  useStore, useWindowState, wm, typeScale, t as tr, errorText, media, usePlayer, nowPlaying, mediaFormatTime, useThumb, fs, addListener, direction,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { library, scan, albumsOf, artistsOf, parseSrt, AUDIO, VIDEO } from './library.js';

const APP = 'player';
const useWindowNonce = new Map();      // last "Open with" handled, per window

// Triangle made of bars (the engine doesn't rotate boxes): for the play icons. left: pointing left.
function Tri({ x, y, w, h, color, steps = 12, left = false }) {
  const rows = [];
  for (let i = 0; i < steps; i++) {
    const f = 1 - Math.abs((i + 0.5) / steps - 0.5) * 2;
    const bw = Math.max(1, w * f);
    rows.push(<View key={i} style={{ position: 'absolute', left: left ? x + w - bw : x, top: y + (h * i) / steps, width: bw, height: h / steps + 0.6, backgroundColor: color }} />);
  }
  return rows;
}

// Monochrome transport glyphs drawn with boxes: the media symbols of the fonts are color emoji.
// They always point left to right (time runs that way, also in RTL).
function PlayGlyph({ size, color }) {
  return <View style={{ width: size, height: size, direction: 'ltr' }}><Tri x={size * 0.2} y={0} w={size * 0.72} h={size} color={color} steps={Math.max(8, Math.round(size))} /></View>;
}
function PauseGlyph({ size, color }) {
  const bw = Math.max(2, Math.round(size * 0.28));
  return (
    <View style={{ width: size, height: size, flexDirection: 'row', justifyContent: 'center', gap: Math.round(size * 0.2), direction: 'ltr' }}>
      <View style={{ width: bw, height: size, borderRadius: 1, backgroundColor: color }} />
      <View style={{ width: bw, height: size, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}
function SkipGlyph({ size, color, back }) {
  const h = size * 0.7, w = size * 0.42, y = (size - h) / 2, steps = Math.max(8, Math.round(h));
  return (
    <View style={{ width: size, height: size, direction: 'ltr' }}>
      {back ? <View style={{ position: 'absolute', left: 0, top: y, width: 2, height: h, backgroundColor: color }} /> : null}
      <Tri x={back ? 2 : size - 2 - w * 2} y={y} w={w} h={h} color={color} steps={steps} left={back} />
      <Tri x={back ? 2 + w : size - 2 - w} y={y} w={w} h={h} color={color} steps={steps} left={back} />
      {back ? null : <View style={{ position: 'absolute', left: size - 2, top: y, width: 2, height: h, backgroundColor: color }} />}
    </View>
  );
}
function SpeakerGlyph({ size, color, level }) {
  const h = size * 0.75, y = (size - h) / 2;
  return (
    <View style={{ width: size + 6, height: size, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: 0, top: size * 0.36, width: size * 0.22, height: size * 0.28, backgroundColor: color }} />
      <Tri x={0} y={y} w={size * 0.5} h={h} color={color} steps={Math.max(8, Math.round(h))} left />
      {level > 0 ? <View style={{ position: 'absolute', left: size * 0.62, top: size * 0.36, width: 2, height: size * 0.28, borderRadius: 1, backgroundColor: color }} /> : null}
      {level > 0.5 ? <View style={{ position: 'absolute', left: size * 0.62 + 4, top: size * 0.22, width: 2, height: size * 0.56, borderRadius: 1, backgroundColor: color }} /> : null}
      {level > 1 ? <View style={{ position: 'absolute', left: size * 0.62 + 8, top: size * 0.08, width: 2, height: size * 0.84, borderRadius: 1, backgroundColor: color }} /> : null}
    </View>
  );
}
// Subtitles: "CC" in a rounded box.
function CaptionsGlyph({ color }) {
  return (
    <View style={{ height: 14, paddingHorizontal: 3, borderRadius: 3, border: `1.5px solid ${color}`, justifyContent: 'center' }}>
      <Text style={{ fontSize: 8, fontWeight: 700, color, lineHeight: 10 }}>CC</Text>
    </View>
  );
}
// Repeat (one): the circular arrow with a small "1".
function RepeatGlyph({ color, one }) {
  return (
    <View style={{ width: 18, height: 18, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 16, color }}>↻</Text>
      {one ? <Text style={{ position: 'absolute', right: -2, bottom: -2, fontSize: 8, fontWeight: 700, color }}>1</Text> : null}
    </View>
  );
}

// Width of a box after layout (to adapt the bar and the columns to the window).
function useBoxWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    if (!ref.current) return;
    const lw = Math.round(lord.getLayout(ref.current.id)[2]);
    if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
  });
  return w;
}

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #ff7a59, #d6246e)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* a screen with film perforations */}
      <View style={{ position: 'absolute', left: s * 0.14, top: s * 0.22, width: s * 0.72, height: s * 0.56, borderRadius: s * 0.08, backgroundColor: 'rgba(20,10,30,0.82)' }} />
      {[0, 1, 2, 3].map((i) => (
        <View key={i} style={{ position: 'absolute', left: s * (0.2 + i * 0.17), top: s * 0.25, width: s * 0.08, height: s * 0.05, borderRadius: s * 0.015, backgroundColor: 'rgba(255,255,255,0.35)' }} />
      ))}
      {[0, 1, 2, 3].map((i) => (
        <View key={`b${i}`} style={{ position: 'absolute', left: s * (0.2 + i * 0.17), top: s * 0.70, width: s * 0.08, height: s * 0.05, borderRadius: s * 0.015, backgroundColor: 'rgba(255,255,255,0.35)' }} />
      ))}
      <Tri x={s * 0.42} y={s * 0.36} w={s * 0.24} h={s * 0.28} color="#fff" steps={Math.max(6, Math.round(s / 5))} />
    </View>
  );
}

const isVideo = (p) => VIDEO.test(p || '');
const isAudio = (p) => AUDIO.test(p || '');
const baseName = (p) => String(p || '').split('/').pop();

// Time (and volume) bar: draggable; fixed LTR direction (it uses the pointer's x).
function Bar({ value, onChange, onCommit, height = 4, color, track, style }) {
  const ref = useRef(null), drag = useRef(false), cb = useRef({});
  cb.current = { onChange, onCommit };
  const at = (x) => {
    const [lx, , w] = lord.getLayout(ref.current.id);
    return Math.max(0, Math.min(1, (x - lx) / (w || 1)));
  };
  useEffect(() => {
    const a = addListener('pointermove', (e) => { if (drag.current) cb.current.onChange?.(at(e.x)); });
    const b = addListener('pointerup', (e) => { if (drag.current) { drag.current = false; cb.current.onCommit?.(at(e.x)); } });
    return () => { a(); b(); };
  }, []);
  const f = Math.max(0, Math.min(1, value || 0));
  return (
    <View ref={ref} onMouseDown={(e) => { drag.current = true; cb.current.onChange?.(at(e.x)); }}
      style={[{ height: 16, justifyContent: 'center', direction: 'ltr' }, style]}>
      <View style={{ height, borderRadius: height / 2, backgroundColor: track }}>
        <View style={{ width: `${f * 100}%`, height, borderRadius: height / 2, backgroundColor: color }} />
      </View>
    </View>
  );
}

// Round borderless button for a glyph (text) or a drawn icon (children).
function IconBtn({ glyph, onClick, size = 16, color, active, children, box = 30 }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minWidth: box, height: box, paddingHorizontal: 4, borderRadius: box / 2, alignItems: 'center', justifyContent: 'center',
        backgroundColor: hover ? 'rgba(127,127,127,0.18)' : 'transparent' }}>
      {children || <Text style={{ fontSize: size, color: active ? th.accent : color || th.text }}>{glyph}</Text>}
    </View>
  );
}

function Cover({ path, size, radius = 6 }) {
  const th = useTheme();
  const thumb = useThumb(path, 256);
  return (
    <View style={{ width: size, height: size, borderRadius: radius, overflow: 'hidden', backgroundColor: th.control, alignItems: 'center', justifyContent: 'center' }}>
      {thumb ? <Image src={thumb} style={{ width: size, height: size, objectFit: 'cover' }} />
        : <Text style={{ fontSize: size * 0.42, color: th.textDim }}>♪</Text>}
    </View>
  );
}

// Simple menu that opens upwards (subtitles, audio).
function Popup({ items, onClose, right = 12, bottom = 64 }) {
  const th = useTheme();
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', right, bottom, minWidth: 200, padding: 6, borderRadius: 10, gap: 2,
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 12px 30px rgba(0,0,0,0.45)', zIndex: 20 }}>
      {items.map((it, i) => (it === '-' ? <View key={i} style={{ height: 1, backgroundColor: th.separator, marginVertical: 3 }} /> : (
        <View key={i} onClick={() => { it.onClick(); onClose(); }} style={{ paddingHorizontal: 8, height: 26, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 6,
          backgroundColor: it.checked ? th.hover : 'transparent' }}>
          <Text style={{ width: 12, fontSize: 11, color: th.text }}>{it.checked ? '✓' : ''}</Text>
          <Text style={{ fontSize: 13, color: th.text, whiteSpace: 'nowrap' }}>{it.label}</Text>
        </View>
      )))}
    </View>
  );
}

export default function Player() {
  const th = useTheme();
  const win = useAppWindow();
  const lib = useStore(library);
  const [view, setView] = useWindowState('view', 'songs');           // songs | artists | albums | videos | queue
  const [sel, setSel] = useWindowState('sel', null);                 // chosen artist or album
  const [queue, setQueue] = useWindowState('queue', []);               // paths
  const [index, setIndex] = useWindowState('index', -1);
  const [shuffle, setShuffle] = useWindowState('shuffle', false);
  const [repeat, setRepeat] = useWindowState('repeat', 'off');          // off | all | one
  const [id, setId] = useWindowState('id', 0);
  const [volume, setVolume] = useWindowState('volume', 1);
  const [seekPreview, setSeekPreview] = useState(null);
  const [extSubs, setExtSubs] = useWindowState('extSubs', null);         // .srt next to the video
  const [subTrack, setSubTrack] = useWindowState('subTrack', -1);         // subtitle track of the movie
  const [menu, setMenu] = useState(null);
  const [controls, setControls] = useState(true);
  const [showVideo, setShowVideo] = useWindowState('showVideo', false);
  const [openFor, setOpenFor] = useWindowState('openFor', null);   // path of the open player
  const root = useRef(null);
  const width = useBoxWidth(root, 960);
  const p = usePlayer(id);
  const current = queue[index] || null;
  const video = !!current && isVideo(current) && p.info && p.info.video >= 0;
  const idRef = useRef(0);
  idRef.current = id;
  const state = useRef({});
  state.current = { queue, index, shuffle, repeat };

  useEffect(() => { scan(); }, []);
  // when the window closes (not when it remounts for full screen or picture in picture)
  useEffect(() => () => setTimeout(() => {
    if (wm.get(win.id)) return;
    media.close(idRef.current);
    if (nowPlaying.get()?.window === win.id) nowPlaying.set(null);
  }, 0), []);

  // open what Files sent ("Open with")
  const lastNonce = useRef(null);
  useEffect(() => {
    const path = win.args?.path;
    if (path && win.args.nonce !== lastNonce.current && win.args.nonce !== useWindowNonce.get(win.id)) {
      useWindowNonce.set(win.id, win.args.nonce);
      setQueue([path]); setIndex(0); setShowVideo(isVideo(path));
    }
    lastNonce.current = win.args?.nonce;
  }, [win.args?.nonce]);

  // one player per track
  useEffect(() => {
    if (current && current === openFor && idRef.current) return;     // already open (the app remounted)
    media.close(idRef.current);
    setId(0);
    setExtSubs(null);
    setSubTrack(-1);
    setOpenFor(current);
    if (!current) return;
    const nid = media.play(APP, current, { appName: tr('apps.player'), music: isAudio(current), volume });
    setId(nid || 0);
    if (isVideo(current)) {
      const srt = current.replace(/\.[^./]+$/, '.srt');
      try { setExtSubs(parseSrt(fs.read(srt))); } catch (e) { /* no .srt */ }
    }
    wm.setTitle(win.id, baseName(current));
  }, [current, index]);

  const next = (auto) => {
    const { queue: q, index: i, shuffle: sh, repeat: rp } = state.current;
    if (!q.length) return;
    if (auto && rp === 'one') { media.control(idRef.current, 'seek', 0); media.control(idRef.current, 'play'); return; }
    let n = sh ? Math.floor(Math.random() * q.length) : i + 1;
    if (n >= q.length) { if (rp !== 'all' && auto) return; n = 0; }
    setIndex(n);
  };
  const prev = () => {
    if (p.pos > 3000) { media.control(id, 'seek', 0); return; }
    setIndex((i) => Math.max(0, i - 1));
  };
  const toggle = () => media.control(id, p.playing ? 'pause' : 'play');

  useEffect(() => { if (p.state === 'ended') next(true); }, [p.state]);

  // for Control Center and the media keys
  const tracks = lib.tracks;
  const meta = useMemo(() => tracks.find((x) => x.path === current) || null, [tracks, current]);
  useEffect(() => {
    if (!current || !id) { if (nowPlaying.get()?.window === win.id) nowPlaying.set(null); return; }
    const tags = p.info?.tags || {};
    nowPlaying.set({
      app: APP, window: win.id, title: meta?.title || tags.title || baseName(current), artist: meta?.artist || tags.artist || '',
      album: meta?.album || tags.album || '', cover: media.thumb(current, 256), playing: p.playing, pos: p.pos, duration: p.duration,
      toggle: () => media.control(idRef.current, nowPlaying.get()?.playing ? 'pause' : 'play'), next: () => next(false), prev, stop: () => media.control(idRef.current, 'pause'),
    });
  }, [current, id, p.playing, Math.floor(p.pos / 1000), p.duration, meta, p.info]);

  // hide the video controls
  const lastMove = useRef(0);
  useEffect(() => {
    if (!video || !p.playing) { setControls(true); return undefined; }
    const tmr = setInterval(() => { if (Date.now() - lastMove.current > 2800 && !menu) setControls(false); }, 500);
    return () => clearInterval(tmr);
  }, [video, p.playing, menu]);
  const poke = () => { lastMove.current = Date.now(); if (!controls) setControls(true); };

  useKeyboard((e) => {
    if (e.key === ' ' ) { toggle(); return; }
    if (e.key === 'ArrowRight') media.control(id, 'seek', p.pos + (e.shift ? 30000 : 5000));
    else if (e.key === 'ArrowLeft') media.control(id, 'seek', Math.max(0, p.pos - (e.shift ? 30000 : 5000)));
    else if (e.key === 'ArrowUp') changeVolume(Math.min(1.5, volume + 0.1));
    else if (e.key === 'ArrowDown') changeVolume(Math.max(0, volume - 0.1));
    else if (String(e.base || e.key).toLowerCase() === 'f' && video) fullscreen();
  });

  const changeVolume = (v) => { setVolume(v); media.control(id, 'volume', v); };
  const fullscreen = () => wm.setFullscreen(win.id, win.mode !== 'fullscreen');
  const pip = () => {
    if (win.mode === 'pip') { wm.setPip(win.id, null); return; }
    const sw = lord.screen.width, sh = lord.screen.height;
    const w = 400, hh = Math.round(w * ((p.info?.streams?.find((s) => s.type === 'video')?.height || 9) / (p.info?.streams?.find((s) => s.type === 'video')?.width || 16)));
    wm.setPip(win.id, { x: sw - w - 24, y: sh - hh - 96, w, h: hh });
  };

  const playList = (paths, i) => { setQueue(paths); setIndex(i); setShowVideo(isVideo(paths[i])); };

  // subtitles visible now
  const pos = seekPreview != null ? seekPreview : p.pos;
  const subs = subTrack >= 0 ? p.subs : extSubs;
  const cue = subs ? subs.filter((c) => c.start <= p.pos && p.pos < c.end).map((c) => c.text).join('\n') : '';
  const subStreams = (p.info?.streams || []).filter((s) => s.type === 'subtitle' && s.text);
  const audioStreams = (p.info?.streams || []).filter((s) => s.type === 'audio');
  const label = (s, i) => [s.title, s.lang && s.lang !== 'und' ? s.lang : null].filter(Boolean).join(' · ') || tr('player.track', { n: i + 1 });

  // ---- transport pieces (the library bar and the video overlay arrange them differently) ----
  const buttons = (col) => (
    <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 6, direction: 'ltr' }}>
      {!video && <IconBtn glyph="⤮" size={15} color={col} active={shuffle} onClick={() => setShuffle(!shuffle)} />}
      <IconBtn color={col} onClick={prev}><SkipGlyph size={14} color={col} back /></IconBtn>
      <IconBtn color={col} box={36} onClick={toggle}>{p.playing ? <PauseGlyph size={16} color={col} /> : <PlayGlyph size={16} color={col} />}</IconBtn>
      <IconBtn color={col} onClick={() => next(false)}><SkipGlyph size={14} color={col} /></IconBtn>
      {!video && <IconBtn onClick={() => setRepeat(repeat === 'off' ? 'all' : repeat === 'all' ? 'one' : 'off')}>
        <RepeatGlyph color={repeat !== 'off' ? th.accent : col} one={repeat === 'one'} />
      </IconBtn>}
    </View>
  );
  const progress = (dark) => {
    const dim = dark ? 'rgba(255,255,255,0.7)' : th.textDim;
    return (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
        <Text style={{ fontSize: typeScale.caption, color: dim, width: 40, textAlign: 'end' }}>{mediaFormatTime(pos)}</Text>
        <Bar style={{ flex: 1 }} value={p.duration ? pos / p.duration : 0} color={dark ? '#fff' : th.accent} track={dark ? 'rgba(255,255,255,0.25)' : th.control}
          onChange={(f) => setSeekPreview(f * p.duration)} onCommit={(f) => { media.control(id, 'seek', Math.round(f * p.duration)); setSeekPreview(null); }} />
        <Text style={{ fontSize: typeScale.caption, color: dim, width: 40 }}>{mediaFormatTime(p.duration)}</Text>
      </View>
    );
  };
  const volumeCtl = (dark, col) => (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, direction: 'ltr' }}>
      <SpeakerGlyph size={13} color={dark ? '#fff' : th.textDim} level={volume} />
      <Bar style={{ width: 84 }} value={volume / 1.5} color={dark ? '#fff' : th.textDim} track={dark ? 'rgba(255,255,255,0.25)' : th.control}
        onChange={(f) => changeVolume(Math.round(f * 150) / 100)} />
      {video && <IconBtn color={col} active={subTrack >= 0 || !!extSubs} onClick={() => setMenu(menu === 'subs' ? null : 'subs')}>
        <CaptionsGlyph color={subTrack >= 0 || extSubs ? th.accent : col} />
      </IconBtn>}
      {video && audioStreams.length > 1 && <IconBtn glyph="♫" color={col} onClick={() => setMenu(menu === 'audio' ? null : 'audio')} />}
      {video && <IconBtn glyph="⧉" color={col} active={win.mode === 'pip'} onClick={pip} />}
      {video && <IconBtn glyph={win.mode === 'fullscreen' ? '⤡' : '⤢'} color={col} onClick={fullscreen} />}
    </View>
  );
  // the video overlay: progress on top, the controls and the volume below
  const transport = (dark) => {
    const col = dark ? '#fff' : th.text;
    return (
      <View style={{ gap: 6 }}>
        {progress(dark)}
        <View style={{ flexDirection: 'row', alignItems: 'center', direction: 'ltr' }}>
          <View style={{ flex: 1 }} />
          {buttons(col)}
          <View style={{ flex: 1, alignItems: 'flex-end' }}>{volumeCtl(dark, col)}</View>
        </View>
      </View>
    );
  };

  // ---- video ----
  if (current && (showVideo || win.mode !== 'window') && isVideo(current)) {
    const small = win.mode === 'pip';
    return (
      <View style={{ flex: 1, backgroundColor: '#000' }} onMouseMove={poke} onMouseDown={() => setMenu(null)}>
        <MediaView media={id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
        {p.error && <View style={{ position: 'absolute', left: 0, right: 0, top: '45%', alignItems: 'center' }}>
          <Text style={{ fontSize: 13, color: '#ff9f9f' }}>{p.error === 'crash' ? tr('player.crashed') : tr('player.error', { error: errorText(p.error) })}</Text>
        </View>}
        {cue ? (
          <View style={{ position: 'absolute', left: 24, right: 24, bottom: controls && !small ? 96 : 28, alignItems: 'center' }}>
            <View style={{ paddingHorizontal: 10, paddingVertical: 4, borderRadius: 6, backgroundColor: 'rgba(0,0,0,0.55)' }}>
              <Text style={{ fontSize: small ? 12 : 20, color: '#fff', textAlign: 'center', lineHeight: 1.3 }}>{cue}</Text>
            </View>
          </View>
        ) : null}
        {small ? (
          <View style={{ position: 'absolute', right: 6, top: 6, flexDirection: 'row', gap: 2 }}>
            <IconBtn onClick={toggle}>{p.playing ? <PauseGlyph size={13} color="#fff" /> : <PlayGlyph size={13} color="#fff" />}</IconBtn>
            <IconBtn glyph="⧉" color="#fff" onClick={pip} />
          </View>
        ) : controls && (
          <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, paddingHorizontal: 20, paddingTop: 28, paddingBottom: 12,
            background: 'linear-gradient(180deg, rgba(0,0,0,0), rgba(0,0,0,0.75))' }}>
            {transport(true)}
          </View>
        )}
        {controls && !small && win.mode === 'window' && (
          <View style={{ position: 'absolute', left: 12, top: 12 }}>
            <IconBtn glyph={direction() === 'rtl' ? '›' : '‹'} size={20} color="#fff" onClick={() => setShowVideo(false)} />
          </View>
        )}
        {menu === 'subs' && <Popup onClose={() => setMenu(null)} items={[
          { label: tr('player.subs.off'), checked: subTrack < 0 && !extSubs, onClick: () => { setSubTrack(-1); setExtSubs(null); media.control(id, 'subTrack', -1); } },
          ...(extSubs ? [{ label: tr('player.subs.file'), checked: subTrack < 0, onClick: () => { setSubTrack(-1); media.control(id, 'subTrack', -1); } }] : []),
          ...subStreams.map((s, i) => ({ label: label(s, i), checked: subTrack === s.index, onClick: () => { setSubTrack(s.index); media.control(id, 'subTrack', s.index); } })),
        ]} />}
        {menu === 'audio' && <Popup onClose={() => setMenu(null)} items={audioStreams.map((s, i) => ({
          label: `${label(s, i)} (${s.codec}, ${s.channels} ch)`, checked: p.info?.audio === s.index, onClick: () => media.control(id, 'audioTrack', s.index) }))} />}
      </View>
    );
  }

  // ---- library ----
  const albums = albumsOf(tracks), artists = artistsOf(tracks);
  const contentW = width - 200;
  const showArtist = contentW >= 380, showAlbum = contentW >= 560;
  const narrow = width < 720;
  const col = (flex, extra) => ({ flex, minWidth: 0, fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', ...extra });
  const header = (
    <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10 }}>
      <Hairline edge="bottom" />
      <Text style={{ width: 22, fontSize: typeScale.caption, fontWeight: 600, color: th.textDim, textAlign: 'end' }}>#</Text>
      <Text style={col(2, { fontSize: typeScale.caption, fontWeight: 600 })}>{tr('player.col.title')}</Text>
      {showArtist && <Text style={col(1, { fontSize: typeScale.caption, fontWeight: 600 })}>{tr('player.col.artist')}</Text>}
      {showAlbum && <Text style={col(1, { fontSize: typeScale.caption, fontWeight: 600 })}>{tr('player.col.album')}</Text>}
      <Text style={{ width: 56, fontSize: typeScale.caption, fontWeight: 600, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap' }}>{tr('player.col.time')}</Text>
    </View>
  );
  const row = (x, i, list, extra) => {
    const on = x.path === current;
    return (
      <View key={x.path} onDoubleClick={() => playList(list.map((y) => y.path), i)} onClick={() => playList(list.map((y) => y.path), i)}
        style={{ height: 30, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 6,
          backgroundColor: on ? th.hover : i % 2 ? (th.mode === 'dark' ? 'rgba(255,255,255,0.025)' : 'rgba(0,0,0,0.025)') : 'transparent' }}>
        <Text style={{ width: 22, fontSize: typeScale.caption, color: on ? th.accent : th.textDim, textAlign: 'end' }}>{on ? '♪' : extra ?? i + 1}</Text>
        <Text style={col(2, { fontSize: 13, color: on ? th.accent : th.text, fontWeight: on ? 500 : 400 })}>{x.title}</Text>
        {showArtist && <Text style={col(1)}>{x.artist}</Text>}
        {showAlbum && <Text style={col(1)}>{x.album}</Text>}
        <Text style={{ width: 56, fontSize: typeScale.caption, color: th.textDim, textAlign: 'end', direction: 'ltr' }}>{x.duration ? mediaFormatTime(x.duration) : ''}</Text>
      </View>
    );
  };
  const songList = (list, mk) => <View>{header}<View style={{ height: 4 }} />{list.map(mk)}</View>;
  let content;
  if (view === 'albums' || (view === 'artists' && sel)) {
    const list = view === 'artists' ? albums.filter((a) => a.artist === sel || a.tracks.some((x) => x.artist === sel)) : albums;
    if (view === 'albums' && sel) {
      const a = albums.find((x) => x.key === sel);
      content = a ? (
        <View style={{ gap: 20, paddingHorizontal: 10 }}>
          <View style={{ flexDirection: 'row', gap: 20, alignItems: 'flex-end' }}>
            <Cover path={a.tracks[0].path} size={140} radius={10} />
            <View style={{ gap: 4, flex: 1, minWidth: 0 }}>
              <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text }}>{a.album || tr('player.unknownAlbum')}</Text>
              <Text style={{ fontSize: typeScale.headline, color: th.accent }}>{a.artist || tr('player.unknownArtist')}</Text>
              <Text style={{ fontSize: typeScale.callout, color: th.textDim }}>{[a.year, tr('player.songs', { count: a.tracks.length })].filter(Boolean).join(' · ')}</Text>
            </View>
          </View>
          <View style={{ marginHorizontal: -10 }}>{songList(a.tracks, (x, i) => row(x, i, a.tracks, x.track || i + 1))}</View>
        </View>
      ) : null;
    } else {
      content = (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 20, paddingHorizontal: 10 }}>
          {list.map((a) => (
            <View key={a.key} onClick={() => { setView('albums'); setSel(a.key); }} style={{ width: 150, gap: 2 }}>
              <Cover path={a.tracks[0].path} size={150} radius={8} />
              <Text style={{ fontSize: 13, fontWeight: 500, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginTop: 6 }}>{a.album || tr('player.unknownAlbum')}</Text>
              <Text style={{ fontSize: typeScale.callout, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{a.artist || tr('player.unknownArtist')}</Text>
            </View>
          ))}
        </View>
      );
    }
  } else if (view === 'artists') {
    content = (
      <View>{artists.map((a) => (
        <View key={a.name} onClick={() => setSel(a.name)} style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 8 }}>
          <Cover path={a.tracks[0].path} size={36} radius={18} />
          <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{a.name || tr('player.unknownArtist')}</Text>
          <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('player.songs', { count: a.tracks.length })}</Text>
        </View>
      ))}</View>
    );
  } else if (view === 'videos') {
    content = (
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 20, paddingHorizontal: 10 }}>
        {lib.videos.map((v, i) => <VideoCard key={v.path} v={v} onOpen={() => playList(lib.videos.map((x) => x.path), i)} />)}
      </View>
    );
  } else if (view === 'queue') {
    content = songList(queue, (q, i) => row(tracks.find((x) => x.path === q) || { path: q, title: baseName(q), artist: '', album: '' }, i, queue.map((x) => ({ path: x }))));
  } else {
    content = songList(tracks.slice().sort((a, b) => a.title.localeCompare(b.title)), (x, i, list) => row(x, i, list));
  }
  const titles = { songs: tr('player.songsTitle'), artists: sel || tr('player.artists'), albums: tr('player.albums'), videos: tr('player.videos'), queue: tr('player.queue') };
  const empty = !((view === 'videos' ? lib.videos.length : tracks.length) || view === 'queue');

  return (
    <View ref={root} style={{ flex: 1 }} onMouseDown={() => setMenu(null)}>
      <View style={{ flex: 1, flexDirection: 'row' }}>
        <View style={{ width: 200, backgroundColor: th.sidebarBg, paddingHorizontal: 10, paddingTop: 4, gap: 2 }}>
          <Hairline edge="end" />
          <SectionTitle>{tr('player.library')}</SectionTitle>
          {[['songs', '♪'], ['artists', '☺'], ['albums', '◫'], ['videos', '▶']].map(([k, g]) => (
            <SidebarItem key={k} label={tr(`player.nav.${k}`)} icon={g} selected={view === k} onClick={() => { setView(k); setSel(null); }} />
          ))}
          <SectionTitle>{tr('player.playing')}</SectionTitle>
          <SidebarItem label={tr('player.nav.queue')} icon="≡" selected={view === 'queue'} onClick={() => setView('queue')} />
          {current && isVideo(current) && <SidebarItem label={tr('player.nav.video')} icon="▣" selected={false} onClick={() => setShowVideo(true)} />}
          <View style={{ flex: 1 }} />
          {lib.scanning && <Text style={{ fontSize: typeScale.caption, color: th.textDim, paddingHorizontal: 10, paddingBottom: 10 }}>{tr('player.scanning', { done: lib.done, total: lib.total })}</Text>}
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>
          <View style={{ height: 52, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 20, gap: 6 }}>
            {sel && <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={20} title={tr('player.back')} onClick={() => setSel(null)} style={{ marginHorizontal: -6 }} />}
            <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{titles[view]}</Text>
          </View>
          {empty ? (
            <EmptyState icon={view === 'videos' ? '▶' : '♪'} title={tr(view === 'videos' ? 'player.emptyVideosTitle' : 'player.emptyMusicTitle')}
              hint={tr(view === 'videos' ? 'player.emptyVideos' : 'player.emptyMusic')} />
          ) : (
            <ScrollView style={{ flex: 1, paddingHorizontal: 10 }}>
              {content}
              <View style={{ height: 16 }} />
            </ScrollView>
          )}
        </View>
      </View>
      {current && (
        <View style={{ height: 72, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 16, backgroundColor: th.sidebarBg }}>
          <Hairline edge="top" />
          <View style={[{ minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 12 }, narrow ? null : { flex: 1 }]}>
            <View onClick={() => isVideo(current) && setShowVideo(true)}><Cover path={current} size={48} /></View>
            {!narrow && (
              <View style={{ flex: 1, minWidth: 0, gap: 2, overflow: 'hidden' }}>
                <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{meta?.title || p.info?.tags?.title || baseName(current)}</Text>
                <Text style={{ fontSize: typeScale.callout, color: th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{meta?.artist || p.info?.tags?.artist || ''}</Text>
              </View>
            )}
          </View>
          <View style={[{ minWidth: 0, gap: 2 }, narrow ? { flex: 1 } : { width: 380 }]}>
            {buttons(th.text)}
            {progress(false)}
          </View>
          {!narrow && <View style={{ flex: 1, minWidth: 0, alignItems: 'flex-end' }}>{volumeCtl(false, th.text)}</View>}
        </View>
      )}
    </View>
  );
}

function VideoCard({ v, onOpen }) {
  const th = useTheme();
  const thumb = useThumb(v.path, 320);
  return (
    <View onClick={onOpen} style={{ width: 200, gap: 8 }}>
      <View style={{ width: 200, height: 112, borderRadius: 8, overflow: 'hidden', backgroundColor: '#111', alignItems: 'center', justifyContent: 'center' }}>
        {thumb ? <Image src={thumb} style={{ width: 200, height: 112, objectFit: 'cover' }} /> : <Text style={{ fontSize: 26, color: '#666' }}>▶</Text>}
      </View>
      <Text style={{ fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{v.title}</Text>
    </View>
  );
}
