// PersonalVoice.jsx — Settings → AI → Voice → "Create a Personal Voice" (docs/VOICE.md): the person reads a few
// phrases aloud and the model speaks with their voice from then on, in every language it has. Kyutai's model clones
// from a short sample, so six phrases (about a minute) are enough, and the voice is ready at once.
//
//   intro   what it is, a quiet room, it stays on this computer; the consent (Kyutai's terms) before anything else;
//           and how: reading six phrases, or speaking freely (for anyone reading aloud is hard for: dyslexia, ADHD…)
//   room    3 s of the room: too noisy says so (the voice would copy the noise)
//   phrase  each phrase in large type, the level as the player's colored bars; it stops by itself when the phrase
//           was read (voicerecmodel.js phraseDone) and goes on; any phrase can be read again
//   free    15 to 30 s about anything (a few ideas shown), a time bar; done when the person says so after 15 s, or at 30
//   name    the voice's name; the phrases joined into one WAV (silence trimmed, at most 30 s) → voices/<name>.wav
//   done    try it; it is the voice in use for the language
//
// Recordings go to ~/.cache/lordos/voice (lord-media, the microphone permission) and are read back here; only the
// joined WAV is kept, in ~/.local/share/lordos/voice/voices, never uploaded.
import {
  View, Text, Button, Toggle, TextInput, Layer, useTheme, useLocale, useAppWindow, wm, media, usePlayer, spacing, typeScale, fs,
  addOwnVoice, ownVoiceName, tryVoice, parseWav, levelDb, quietEnough, trimSilence, joinPhrases, phraseDone,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const PHRASES = 6;
const FREE_MIN_MS = 15000;
const FREE_MAX_MS = 30000;
const IDEAS = 3;
const ROOM_MS = 3000;
const PHRASE_MAX_MS = 15000;
const MIN_CLIP_MS = 800;
const CACHE = '.cache/lordos/voice';
const SILENT_MIC_MS = 4000;               // nothing from the microphone this long: it isn't working
// violet (low) to light blue (high), as the player's wave (sdk/voiceplayer.jsx)
const COLORS = ['#7c5cff', '#9159fa', '#a657f3', '#bb57ea', '#d058dd', '#e45ccc', '#f062b8', '#f46c9a', '#f5807c', '#f59a5f', '#e9b64a',
  '#bfcb4f', '#86d36a', '#4ccf9c', '#3cbcd0', '#4a96ff'];

function Meter({ levels }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 4, height: 44, direction: 'ltr' }}>
      {COLORS.map((c, i) => {
        const v = Math.min(1, (levels[levels.length - COLORS.length + i] || 0) * 2.5);
        return <View key={i} style={{ width: 6, height: Math.round(4 + v * 40), borderRadius: 3, backgroundColor: c }} />;
      })}
    </View>
  );
}

const Dots = ({ n, at, th }) => (
  <View style={{ flexDirection: 'row', gap: 6, justifyContent: 'center' }}>
    {Array.from({ length: n }, (_, i) => (
      <View key={i} style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: i < at ? th.accent : i === at ? th.text : th.separator }} />
    ))}
  </View>
);

export function PersonalVoiceSheet({ lang, onClose }) {
  const th = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const [step, setStep] = useState('intro');
  const [consent, setConsent] = useState(false);
  const [phrase, setPhrase] = useState(0);
  const [rec, setRec] = useState(0);
  const [levels, setLevels] = useState([]);
  const [warn, setWarn] = useState(null);
  const [name, setName] = useState('');
  const [made, setMade] = useState(null);
  const [how, setHow] = useState('read');         // 'read' (phrases) | 'free'
  const rp = usePlayer(rec);
  const clips = useRef([]);                 // Int16Array per phrase
  const rate = useRef(48000);
  const listen = useRef(null);              // phraseDone's state
  const what = useRef(null);                // 'room' | 'phrase' | 'free' while recording
  const watchdog = useRef(0);
  const heard = useRef(false);              // the recording has sent anything yet
  const home = fs.home();
  const path = (n) => `${home}/${CACHE}/personal-${n}.wav`;
  useEffect(() => { for (const d of ['.cache', '.cache/lordos', CACHE]) { try { fs.mkdir(d); } catch (e) { /* there */ } } }, []);
  useEffect(() => () => { if (rec) media.close(rec); }, [rec]);
  useEffect(() => () => clearTimeout(watchdog.current), []);

  const start = async (kind, file) => {
    setWarn(null);
    setLevels([]);
    what.current = { kind, file };
    listen.current = { start: Date.now() };
    const id = await media.audioRecord('settings', file, { appName: tr('settings.section.ai') });
    if (!id) { setWarn(tr('settings.ai.clone.denied')); what.current = null; return; }
    setRec(id);
    // a microphone that sends nothing (none plugged in, muted, taken by another app): say so instead of waiting
    clearTimeout(watchdog.current);
    watchdog.current = setTimeout(() => {
      if (what.current && !heard.current) { what.current = null; media.close(id); setRec(0); setWarn(tr('settings.ai.pv.noMic')); }
    }, SILENT_MIC_MS);
    heard.current = false;
  };

  // live: the level; the room stops after 3 s, a phrase when it was read (or after 15 s)
  useEffect(() => {
    if (!rec || rp.state !== 'recording') return;
    if (rp.ms > 0) heard.current = true;
    setLevels((l) => [...l.slice(-40), rp.level || 0]);
    const w = what.current;
    if (!w) return;
    const stop = w.kind === 'room' ? rp.ms >= ROOM_MS : w.kind === 'free' ? rp.ms >= FREE_MAX_MS
      : (rp.ms >= PHRASE_MAX_MS || phraseDone(listen.current, rp.level || 0, Date.now()));
    if (stop) media.stop(rec);
  }, [rp.ms, rp.level]);

  // the recording ended: read it back and judge it
  useEffect(() => {
    if (!rec || !(rp.state === 'done' || rp.closed)) return;
    media.close(rec);
    setRec(0);
    const w = what.current;
    what.current = null;
    if (!w) return;
    let wav = null;
    try { wav = parseWav(fs.read(w.file, { binary: true })); } catch (e) { wav = null; }
    if (!wav) { setWarn(tr('settings.ai.pv.unreadable')); return; }
    rate.current = wav.rate;
    if (w.kind === 'room') {
      if (quietEnough(wav.samples)) setStep(how === 'free' ? 'free' : 'phrase');
      else setWarn(tr('settings.ai.pv.noisy', { db: Math.round(levelDb(wav.samples)) }));
      return;
    }
    if (w.kind === 'free') {
      const all = trimSilence(wav.samples, wav.rate);
      if ((all.length * 1000) / wav.rate < FREE_MIN_MS * 0.6) { setWarn(tr('settings.ai.pv.freeShort')); return; }
      clips.current = [all];
      setStep('name');
      return;
    }
    const clip = trimSilence(wav.samples, wav.rate);
    if ((clip.length * 1000) / wav.rate < MIN_CLIP_MS) { setWarn(tr('settings.ai.pv.notHeard')); return; }
    clips.current[phrase] = clip;
    if (phrase + 1 < PHRASES) setPhrase(phrase + 1);
    else setStep('name');
  }, [rp.state, rp.closed]);

  const create = () => {
    const n = ownVoiceName(name);
    if (!n) return;
    try {
      const out = `${home}/${CACHE}/personal.wav`;
      fs.write(out, joinPhrases(clips.current.filter(Boolean), rate.current));
      addOwnVoice(n, out);
      setMade(n);
      setStep('done');
    } catch (e) { setWarn(e.message); }
  };
  const close = (n) => { if (rec) media.close(rec); onClose(n); };

  const g = wm.frame(wm.get(win.id) || {});
  const title = (k) => <Text style={{ fontSize: 17, fontWeight: 700, color: th.text, textAlign: 'center' }}>{tr(k)}</Text>;
  const body = (k, p) => <Text style={{ fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.45, textAlign: 'center' }}>{tr(k, p)}</Text>;
  const recording = !!rec;
  let content;
  if (step === 'intro') {
    content = (
      <>
        {title('settings.ai.pv.title')}
        {body('settings.ai.pv.intro')}
        <View style={{ gap: 8, paddingVertical: 4 }}>
          {['quiet', 'time', 'local'].map((k) => (
            <View key={k} style={{ flexDirection: 'row', gap: 10 }}>
              <Text style={{ color: th.accent, fontSize: typeScale.callout }}>●</Text>
              <Text style={{ flex: 1, fontSize: typeScale.callout, color: th.text, lineHeight: 1.4 }}>{tr(`settings.ai.pv.${k}`)}</Text>
            </View>
          ))}
        </View>
        <View style={{ gap: 6 }}>
          {['read', 'free'].map((k) => (
            <View key={k} onClick={() => setHow(k)} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 10, borderRadius: 8,
              border: `1.5px solid ${how === k ? th.accent : th.separator}` }}>
              <View style={{ width: 16, height: 16, borderRadius: 8, border: `2px solid ${how === k ? th.accent : th.textDim}`, alignItems: 'center', justifyContent: 'center' }}>
                {how === k ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: th.accent }} /> : null}
              </View>
              <View style={{ flex: 1, gap: 2 }}>
                <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: th.text }}>{tr(`settings.ai.pv.how.${k}`)}</Text>
                <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.35 }}>{tr(`settings.ai.pv.how.${k}Hint`)}</Text>
              </View>
            </View>
          ))}
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, padding: 10, borderRadius: 8, backgroundColor: th.hover }}>
          <Toggle small on={consent} onChange={setConsent} />
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: th.text, lineHeight: 1.4 }}>{tr('settings.ai.clone.consent')}</Text>
        </View>
        <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4 }}>{tr('settings.ai.clone.terms')}</Text>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('settings.ai.clone.cancel')} onClick={() => close(null)} />
          <Button primary label={tr('settings.ai.pv.begin')} disabled={!consent} onClick={() => { clips.current = []; setStep('room'); start('room', path('room')); }} />
        </View>
      </>
    );
  } else if (step === 'room') {
    content = (
      <>
        {title('settings.ai.pv.roomTitle')}
        {body(recording ? 'settings.ai.pv.roomListening' : 'settings.ai.pv.roomHint')}
        <Meter levels={levels} />
        {warn ? <Text style={{ fontSize: typeScale.callout, color: '#ff9f0a', textAlign: 'center', lineHeight: 1.4 }}>{warn}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('settings.ai.clone.cancel')} onClick={() => close(null)} />
          {warn && !recording ? <Button label={tr('settings.ai.pv.continueAnyway')} onClick={() => { setWarn(null); setStep(how === 'free' ? 'free' : 'phrase'); }} /> : null}
          {!recording ? <Button primary label={tr('settings.ai.pv.roomAgain')} onClick={() => start('room', path('room'))} /> : null}
        </View>
      </>
    );
  } else if (step === 'phrase') {
    content = (
      <>
        <Dots n={PHRASES} at={phrase} th={th} />
        <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'center' }}>{tr('settings.ai.pv.phraseOf', { n: phrase + 1, total: PHRASES })}</Text>
        <Text style={{ fontSize: 22, fontWeight: 600, color: th.text, textAlign: 'center', lineHeight: 1.35, paddingVertical: 10 }}>
          {tr(`settings.ai.pv.phrase.${phrase + 1}`)}
        </Text>
        <Meter levels={levels} />
        {body(recording ? 'settings.ai.pv.reading' : 'settings.ai.pv.pressRead')}
        {warn ? <Text style={{ fontSize: typeScale.callout, color: '#ff9f0a', textAlign: 'center' }}>{warn}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'space-between', gap: spacing.s }}>
          <Button label={tr('settings.ai.pv.back')} disabled={recording || phrase === 0} onClick={() => setPhrase(phrase - 1)} />
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <Button label={tr('settings.ai.clone.cancel')} onClick={() => close(null)} />
            <Button primary label={recording ? tr('settings.ai.clone.stop') : clips.current[phrase] ? tr('settings.ai.pv.readAgain') : tr('settings.ai.pv.read')}
              onClick={() => (recording ? media.stop(rec) : start('phrase', path(phrase + 1)))} />
          </View>
        </View>
      </>
    );
  } else if (step === 'free') {
    const ms = recording ? rp.ms || 0 : 0;
    const enough = ms >= FREE_MIN_MS;
    content = (
      <>
        {title('settings.ai.pv.freeTitle')}
        {body('settings.ai.pv.freeHint')}
        <View style={{ gap: 6, paddingVertical: 2 }}>
          {Array.from({ length: IDEAS }, (_, i) => (
            <Text key={i} style={{ fontSize: typeScale.callout, color: th.text, textAlign: 'center' }}>{tr(`settings.ai.pv.idea.${i + 1}`)}</Text>
          ))}
        </View>
        <Meter levels={levels} />
        <View style={{ height: 6, borderRadius: 3, backgroundColor: th.separator, direction: 'ltr', overflow: 'hidden' }}>
          <View style={{ width: `${Math.min(100, (ms * 100) / FREE_MAX_MS)}%`, height: 6, borderRadius: 3, backgroundColor: enough ? '#30d158' : th.accent }} />
        </View>
        {body(recording ? (enough ? 'settings.ai.pv.freeEnough' : 'settings.ai.pv.freeKeepGoing') : 'settings.ai.pv.freePress', { s: Math.floor(ms / 1000) })}
        {warn ? <Text style={{ fontSize: typeScale.callout, color: '#ff9f0a', textAlign: 'center' }}>{warn}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('settings.ai.clone.cancel')} onClick={() => close(null)} />
          <Button primary disabled={recording && !enough}
            label={recording ? tr('settings.ai.pv.freeDone') : tr('settings.ai.pv.freeStart')}
            onClick={() => (recording ? media.stop(rec) : start('free', path('free')))} />
        </View>
      </>
    );
  } else if (step === 'name') {
    content = (
      <>
        {title('settings.ai.pv.nameTitle')}
        {body('settings.ai.pv.nameHint')}
        <TextInput value={name} onChange={(x) => setName(x.slice(0, 60))} placeholder={tr('settings.ai.clone.name')} autoFocus onSubmit={create} />
        {warn ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a' }}>{warn}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('settings.ai.pv.back')} onClick={() => { if (how === 'free') setStep('free'); else { setPhrase(PHRASES - 1); setStep('phrase'); } }} />
          <Button primary label={tr('settings.ai.clone.save')} disabled={!ownVoiceName(name)} onClick={create} />
        </View>
      </>
    );
  } else {
    content = (
      <>
        <Text style={{ fontSize: 40, textAlign: 'center', color: '#30d158' }}>✓</Text>
        {title('settings.ai.pv.doneTitle')}
        {body('settings.ai.pv.doneHint', { name: made })}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('settings.ai.pv.tryMine')} onClick={() => tryVoice(lang, made)} />
          <Button primary label={tr('settings.ai.pv.finish')} onClick={() => close(made)} />
        </View>
      </>
    );
  }
  return (
    <Layer name="modal">
      <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 720, height: g.h || 560, alignItems: 'center',
        justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
        <View style={{ width: 540, maxWidth: '94%', padding: 24, gap: spacing.m, borderRadius: 14, backgroundColor: th.windowBg,
          border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
          {content}
        </View>
      </View>
    </Layer>
  );
}
