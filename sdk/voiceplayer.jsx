// voiceplayer.jsx — the small player while LordOS reads aloud (docs/VOICE.md), and the dialog that asks where to save
// a text as a sound file. sdk/index.js mounts both in their layers; the player shows when a reading starts (unless
// Settings → AI → Voice turns it off) and goes a few seconds after it ends, or at once with ✕.
//
//   ⠿ drag it anywhere, to any display: it comes back there next time (settings.voice.pos; voiceplayermodel.js)
//   ↻ read it again from the start · ⏸/▶ pause, go on · ■ stop (it stays, to read again) · ✕ stop and close
//
// The wave is lord.voice.levels(): how loud each band of what is heard now is, so it moves with the voice and stands
// still in a pause. Its bars' heights are set on their nodes (lord.setStyle), not by a React render per frame.
import { useEffect, useRef, useState } from 'react';
import { View, Text, addListener, displays, displayAt } from '@lordos/runtime';
import { useStore } from './store.js';
import { ThemeContext, DEFAULT_TOKENS, shellTokens } from './theme.js';
import { t } from './i18n.js';
import { settings } from './settings.js';
import { wm } from './windows.js';
import { FilePicker } from './filepicker.jsx';
import { player, voiceSave, saveSpeechTo, musicFolder, pauseSpeaking, replaySpeaking, stopSpeaking, closePlayer } from './voice.js';
import { placePlayer, rememberPlace, smoothBands, PLAYER_BARS } from './voiceplayermodel.js';

const SIZE = { w: 480, h: 60 };
const BAR_W = 4, BAR_GAP = 3, BAR_MAX = 28, BAR_MIN = 3;
// violet (low) to light blue (high), one per band
const COLORS = ['#7c5cff', '#9159fa', '#a657f3', '#bb57ea', '#d058dd', '#e45ccc', '#f062b8', '#f46c9a', '#f5807c', '#f59a5f', '#e9b64a',
  '#bfcb4f', '#86d36a', '#4ccf9c', '#3cbcd0', '#4a96ff'];

function readingDisplay() {
  const w = wm.get(wm.focused());
  return w && Number.isFinite(w.x) && Number.isFinite(w.w) ? displayAt(w.x + w.w / 2, w.y + (w.h || 0) / 2) : null;
}

function RoundButton({ label, onClick, main = false, children, th }) {
  const s = main ? 44 : 34;
  return (
    <View onClick={onClick} accessibilityLabel={label} title={label}
      style={{ width: s, height: s, borderRadius: s / 2, alignItems: 'center', justifyContent: 'center', backgroundColor: main ? th.text : 'rgba(255,255,255,0.09)' }}>
      {children}
    </View>
  );
}

const Glyph = ({ c, color, size = 15 }) => <Text style={{ fontSize: size, color, lineHeight: size + 2 }}>{c}</Text>;

// pause and play drawn with views: a font may draw ⏸ and ▶ as color emoji
const PauseIcon = ({ color }) => (
  <View style={{ flexDirection: 'row', gap: 4 }}>
    <View style={{ width: 4, height: 14, borderRadius: 1, backgroundColor: color }} />
    <View style={{ width: 4, height: 14, borderRadius: 1, backgroundColor: color }} />
  </View>
);
const PlayIcon = ({ color }) => (
  <View style={{ width: 11, height: 16, marginLeft: 4, overflow: 'hidden' }}>
    <View style={{ position: 'absolute', left: -6, top: 2.3, width: 11.3, height: 11.3, borderRadius: 1.5, backgroundColor: color, transform: 'rotate(45deg)' }} />
  </View>
);

function Grip({ color, onMouseDown, label }) {
  const dot = (k) => <View key={k} style={{ width: 3, height: 3, borderRadius: 2, backgroundColor: color }} />;
  return (
    <View onMouseDown={onMouseDown} accessibilityLabel={label} title={label}
      style={{ width: 16, height: 40, alignItems: 'center', justifyContent: 'center', cursor: 'grab' }}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', width: 9, gap: 3 }}>{[0, 1, 2, 3, 4, 5].map(dot)}</View>
    </View>
  );
}

export function VoicePlayer() {
  const p = useStore(player);
  const th = useStore(shellTokens) || DEFAULT_TOKENS;
  const [pos, setPos] = useState(null);
  const bars = useRef([]);
  const drag = useRef(null);
  const shown = !!p;

  // where it opens: where it was left, else under the window that reads
  useEffect(() => {
    if (!shown) { setPos(null); return; }
    setPos(placePlayer(displays(), settings.get().voice?.pos || null, readingDisplay(), SIZE));
  }, [shown]);

  // the wave
  useEffect(() => {
    if (!shown) return undefined;
    let prev = null, last = Date.now();
    const id = setInterval(() => {
      const now = Date.now();
      let lv = null;
      try { lv = lord.voice.levels(); } catch (e) { /* no lord.voice */ }
      prev = smoothBands(prev, lv, now - last);
      last = now;
      prev.forEach((v, i) => {
        const n = bars.current[i];
        if (n) try { lord.setStyle(n, 'height', Math.round(BAR_MIN + v * (BAR_MAX - BAR_MIN))); } catch (e) { /* gone meanwhile */ }
      });
    }, 33);
    return () => clearInterval(id);
  }, [shown]);

  // dragging, also to another display
  useEffect(() => {
    if (!shown) return undefined;
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (d) setPos({ x: d.ox + e.x - d.x, y: d.oy + e.y - d.y });
    });
    const offUp = addListener('pointerup', (e) => {
      const d = drag.current;
      drag.current = null;
      if (!d) return;
      const place = rememberPlace(displays(), d.ox + e.x - d.x, d.oy + e.y - d.y, SIZE);
      settings.set({ voice: { ...(settings.get().voice || {}), pos: place } });
      setPos(placePlayer(displays(), place, null, SIZE));
    });
    return () => { offMove(); offUp(); };
  }, [shown]);

  if (!p || !pos) return null;
  const playing = p.state === 'playing', paused = p.state === 'paused';
  const title = t(playing ? 'shell.voice.playing' : paused ? 'shell.voice.paused' : 'shell.voice.done');
  const main = playing ? { label: t('shell.voice.pause'), run: () => pauseSpeaking(true) }
    : paused ? { label: t('shell.voice.resume'), run: () => pauseSpeaking(false) }
      : { label: t('shell.voice.replay'), run: replaySpeaking };
  return (
    <ThemeContext.Provider value={th}>
      <View style={{ position: 'absolute', left: pos.x, top: pos.y, width: SIZE.w, height: SIZE.h, flexDirection: 'row', alignItems: 'center', gap: 10,
        paddingLeft: 6, paddingRight: 8, borderRadius: SIZE.h / 2, direction: 'ltr', backgroundColor: 'rgba(28,26,40,0.9)', border: '1px solid rgba(255,255,255,0.1)',
        boxShadow: '0 16px 44px rgba(0,0,0,0.5)', backdropFilter: 'blur(20px)' }}>
        <Grip color="rgba(236,236,241,0.5)" label={t('shell.voice.move')}
          onMouseDown={(e) => { e.stopPropagation?.(); drag.current = { x: e.x, y: e.y, ox: pos.x, oy: pos.y }; }} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: BAR_GAP, height: 32 }}>
          {Array.from({ length: PLAYER_BARS }, (_, i) => (
            <View key={i} ref={(n) => { bars.current[i] = n && n.id ? n.id : null; }}
              style={{ width: BAR_W, height: BAR_MIN, borderRadius: 2, backgroundColor: COLORS[i] }} />
          ))}
        </View>
        <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
          <Text style={{ fontSize: 13, fontWeight: 600, color: '#ececf1', whiteSpace: 'nowrap' }}>{title}</Text>
          {p.source ? <Text style={{ fontSize: 11, color: 'rgba(236,236,241,0.62)', whiteSpace: 'nowrap', overflow: 'hidden' }}>{p.source}</Text> : null}
        </View>
        <RoundButton th={th} label={t('shell.voice.replay')} onClick={replaySpeaking}><Glyph c="↻" color="#ececf1" /></RoundButton>
        <RoundButton th={{ text: '#ececf1' }} main label={main.label} onClick={main.run}>{playing ? <PauseIcon color="#16141f" /> : <PlayIcon color="#16141f" />}</RoundButton>
        <RoundButton th={th} label={t('shell.voice.stop')} onClick={stopSpeaking}><Glyph c="■" color="#ececf1" size={13} /></RoundButton>
        <View onClick={closePlayer} accessibilityLabel={t('shell.voice.close')} title={t('shell.voice.close')}
          style={{ width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center' }}>
          <Glyph c="✕" color="rgba(236,236,241,0.7)" size={12} />
        </View>
      </View>
    </ThemeContext.Provider>
  );
}

// Edit → Speech → Save as Audio…: where the WAV goes (the Music folder first)
export function VoiceSavePrompt() {
  const req = useStore(voiceSave);
  const tokens = useStore(shellTokens);
  if (!req) return null;
  return (
    <ThemeContext.Provider value={tokens || DEFAULT_TOKENS}>
      <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
        <View style={{ position: 'relative', width: 920, maxWidth: '96%', height: 620, maxHeight: '94%' }}>
          <FilePicker modal mode="save" title={t('shell.voice.saveTitle')} name={req.name} start={musicFolder()} accept={t('shell.voice.saveButton')}
            filter={/\.wav$/i} onClose={() => saveSpeechTo(null)} onPick={(path) => saveSpeechTo(Array.isArray(path) ? path[0] : path)} />
        </View>
      </View>
    </ThemeContext.Provider>
  );
}
