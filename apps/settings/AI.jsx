// Settings → AI: reading aloud (docs/VOICE.md). Voice: on or off, the ❖Esc shortcut, the small player (settings.voice,
// sdk/voice.js) and where speech is made (voice.json's backend). Per language: the standard and HD models and the
// voices (sdk/voicecatalog.js), each heard from its recorded sample before downloading it, downloaded and removed as
// signed lordos-voice-* packages (sdk/voicemanager.js), the one used chosen; and the person's own voices, recorded
// with the microphone or taken from a file, cloned by the model (CloneSheet).
import {
  View, Text, Button, Toggle, Segmented, TextInput, Layer, FilePicker, useTheme, useLocale, useSettings, useStore, useAppWindow,
  wm, media, formatBytes, spacing, typeScale, VOICE_DEFAULTS, speakAvailable, previewVoice, tryVoice, player as voicePlayer,
  VOICE_LANGS, VOICES, voiceLabel, voicePackage, modelPackage, hasVoice, PREVIEWS_PACKAGE, scanVoices, installVoices, removeVoices,
  packagesFor, voiceConf, setVoiceConf, addOwnVoice, removeOwnVoice, ownVoiceName, takeName, errorText, fs,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, Note } from './ui.jsx';
import { PersonalVoiceSheet } from './PersonalVoice.jsx';

const BACKENDS = ['auto', 'cpu', 'gpu'];
const QUALITIES = ['auto', 'standard', 'hd'];
const size = (mb) => formatBytes(mb * 1e6);
// the recorded samples (lordos-voice-previews), where say looks for them: the person's voice folder, then the system's
const hasPreviews = () => [`${fs.home()}/.local/share/lordos/voice`, '/usr/share/lordos/voice']
  .some((root) => { try { return !!fs.stat(`${root}/previews/es/lola.wav`); } catch (e) { return false; } });

export function AISettings() {
  const { t: tr, lang: uiLang } = useLocale();
  const th = useTheme();
  const [s, set] = useSettings();
  const v = { ...VOICE_DEFAULTS, ...(s.voice || {}) };
  const put = (patch) => set({ voice: { ...(s.voice || {}), ...patch } });
  const [conf, setConf] = useState(voiceConf);
  const [scan, setScan] = useState(scanVoices);
  const [lang, setLang] = useState(() => (VOICE_LANGS.some((l) => l.id === uiLang) ? uiLang : 'es'));
  const [job, setJob] = useState(null);            // { key, done, total } while a package downloads
  const [msg, setMsg] = useState(null);            // { ok, text }
  const [clone, setClone] = useState(null);        // 'record' | 'file' while the clone sheet is open
  const [previews, setPreviews] = useState(hasPreviews);
  useStore(voicePlayer);

  const L = VOICE_LANGS.find((l) => l.id === lang);
  const st = scan[lang];
  const quality = QUALITIES.includes(conf.quality) ? conf.quality : 'auto';
  // the voices shown: of the model reading uses (HD unless standard is asked for or only standard is installed)
  const hd = quality === 'hd' || (quality === 'auto' && (st.hd || !st.standard));
  const chosen = (conf.voices && conf.voices[lang]) || L.own;
  const rescan = () => { setScan(scanVoices()); setPreviews(hasPreviews()); };
  const saveConf = (patch) => { try { setConf(setVoiceConf(patch)); } catch (e) { setMsg({ ok: false, text: e.message }); } };

  const run = async (key, fn) => {
    if (job) return;
    setMsg(null);
    setJob({ key, done: 0, total: 0 });
    try { await fn((p) => setJob({ key, ...p })); } catch (e) { setMsg({ ok: false, text: e.message }); }
    setJob(null);
    rescan();
  };
  const install = (key, names) => run(key, (onP) => installVoices(names, onP));
  const remove = (key, names) => run(key, () => removeVoices(names));
  const pct = job && job.total ? Math.floor((job.done * 100) / job.total) : 0;
  const busyLabel = (key) => (job && job.key === key ? tr('settings.ai.downloading', { pct }) : null);

  const modelRow = (q) => {
    const on = q === 'hd' ? st.hd : st.standard;
    const key = `model-${lang}-${q}`;
    const mb = L.mb[q];
    return (
      <SettingRow key={q} label={tr(`settings.ai.${q}`)} hint={tr(`settings.ai.${q}Hint`, { size: size(mb) })}>
        {busyLabel(key) ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{busyLabel(key)}</Text>
          : on ? <Button small destructive label={tr('settings.ai.remove')} disabled={!!job}
            onClick={() => remove(key, [modelPackage(lang, q === 'hd'),
              ...VOICES.filter((x) => hasVoice(x.id, q === 'hd') && st.voices[q].has(x.id)).map((x) => voicePackage(lang, x.id, q === 'hd'))])} />
            : <Button small label={tr('settings.ai.download')} disabled={!!job} onClick={() => install(key, packagesFor(lang, L.own, q === 'hd', scan))} />}
      </SettingRow>
    );
  };

  const voiceRow = (x) => {
    const installed = st.voices[hd ? 'hd' : 'standard'].has(x.id);
    const key = `voice-${lang}-${x.id}-${hd}`;
    const inUse = chosen === x.id;
    const need = packagesFor(lang, x.id, hd, scan);
    const extra = need.length > 1 ? tr('settings.ai.needsModel', { size: size(L.mb[hd ? 'hd' : 'standard']) }) : null;
    return (
      <SettingRow key={x.id} label={voiceLabel(x.id)}
        hint={[tr(x.gender === 'f' ? 'settings.ai.female' : 'settings.ai.male'), inUse ? tr('settings.ai.inUse') : null, extra].filter(Boolean).join(' · ')}>
        <Button small label="▶" onClick={() => previewVoice(lang, x.id)} disabled={!previews} />
        {busyLabel(key) ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{busyLabel(key)}</Text> : installed ? (
          <>
            <Button small label={tr('settings.ai.try')} onClick={() => tryVoice(lang, x.id)} />
            {!inUse ? <Button small label={tr('settings.ai.use')} onClick={() => saveConf({ voices: { [lang]: x.id } })} /> : null}
            <Button small destructive label={tr('settings.ai.remove')} disabled={!!job} onClick={() => remove(key, [voicePackage(lang, x.id, hd)])} />
          </>
        ) : <Button small label={tr('settings.ai.downloadSize', { size: size(L.mb[hd ? 'voiceHd' : 'voice']) })} disabled={!!job}
          onClick={() => install(key, need)} />}
      </SettingRow>
    );
  };

  const pickBackend = (b) => saveConf({ backend: b });
  return (
    <Stack>
      <Section title={tr('settings.ai.voice')} footer={tr('settings.ai.voiceFooter')}>
        <SettingRow label={tr('settings.ai.readAloud')} hint={tr('settings.ai.readAloudHint')}>
          <Toggle small on={v.enabled} onChange={(enabled) => put({ enabled })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.shortcut')} hint={tr('settings.ai.shortcutHint')} disabled={!v.enabled}>
          <Toggle small on={v.shortcut} onChange={(shortcut) => v.enabled && put({ shortcut })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.player')} hint={tr('settings.ai.playerHint')} disabled={!v.enabled}>
          <Toggle small on={v.player} onChange={(player) => v.enabled && put({ player })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.liveSpeech')} hint={tr('settings.ai.liveSpeechHint')} disabled={!v.enabled}>
          <Toggle small on={v.liveSpeech} onChange={(liveSpeech) => v.enabled && put({ liveSpeech })} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.backend')} hint={tr('settings.ai.backendHint')}>
          <Segmented small value={BACKENDS.includes(conf.backend) ? conf.backend : 'auto'} onChange={pickBackend}
            options={BACKENDS.map((b) => [b, tr(`settings.ai.backend.${b}`)])} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.quality')} hint={tr('settings.ai.qualityHint')}>
          <Segmented small value={quality} onChange={(q) => saveConf({ quality: q })} options={QUALITIES.map((q) => [q, tr(`settings.ai.quality.${q}`)])} />
        </SettingRow>
      </Section>

      <View style={{ alignItems: 'flex-start' }}>
        <Segmented value={lang} onChange={setLang} options={VOICE_LANGS.map((l) => [l.id, tr(`settings.ai.lang.${l.id}`)])} />
      </View>

      <Section title={tr('settings.ai.models')} footer={tr('settings.ai.modelsFooter')}>
        {modelRow('standard')}
        {modelRow('hd')}
      </Section>

      <Section title={tr(hd ? 'settings.ai.voicesHd' : 'settings.ai.voicesStandard')} footer={tr('settings.ai.voicesFooter')}>
        {!previews ? (
          <SettingRow label={tr('settings.ai.previews')} hint={tr('settings.ai.previewsHint')}>
            {busyLabel('previews') ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{busyLabel('previews')}</Text>
              : <Button small label={tr('settings.ai.download')} disabled={!!job} onClick={() => install('previews', [PREVIEWS_PACKAGE])} />}
          </SettingRow>
        ) : null}
        {VOICES.filter((x) => hasVoice(x.id, hd)).map(voiceRow)}
      </Section>

      <Section title={tr('settings.ai.own')} footer={tr('settings.ai.ownFooter')}>
        {scan.own.length ? scan.own.map((name) => (
          <SettingRow key={name} label={name} hint={chosen === name ? tr('settings.ai.inUse') : null}>
            <Button small label={tr('settings.ai.try')} onClick={() => tryVoice(lang, name)} disabled={!(st.hd || st.standard)} />
            {chosen !== name ? <Button small label={tr('settings.ai.use')} onClick={() => saveConf({ voices: { [lang]: name } })} /> : null}
            <Button small destructive label={tr('settings.ai.remove')} onClick={() => {
              try { removeOwnVoice(name); if (chosen === name) saveConf({ voices: { [lang]: L.own } }); rescan(); } catch (e) { setMsg({ ok: false, text: e.message }); }
            }} />
          </SettingRow>
        )) : <SettingRow label={tr('settings.ai.ownEmpty')} />}
        <View style={{ flexDirection: 'row', gap: spacing.s, paddingVertical: spacing.s }}>
          <Button small primary label={tr('settings.ai.record')} onClick={() => setClone('record')} />
          <Button small label={tr('settings.ai.upload')} onClick={() => setClone('file')} />
        </View>
      </Section>

      {msg ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
      <Note>{speakAvailable() ? tr('settings.ai.privacy') : tr('settings.ai.notInstalled')}</Note>
      {clone ? (() => {
        const done = (name) => {
          setClone(null);
          if (name) { saveConf({ voices: { [lang]: name } }); setMsg({ ok: true, text: tr('settings.ai.clone.saved', { name }) }); }
          rescan();
        };
        return clone === 'record' ? <PersonalVoiceSheet lang={lang} onClose={done} /> : <CloneSheet lang={lang} onClose={done} />;
      })() : null}
    </Stack>
  );
}

// ---- a voice from a recording the person already has (the guided recording is PersonalVoice.jsx) ----

const CACHE = '.cache/lordos/voice';
const MAX_MS = 30000;
const MIN_MS = 3000;

function CloneSheet({ onClose }) {
  const th = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const [name, setName] = useState('');
  const [consent, setConsent] = useState(false);
  const [wav, setWav] = useState(null);           // the clip, a WAV in ~/.cache/lordos/voice
  const [picking, setPicking] = useState(true);
  const [err, setErr] = useState(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => { for (const d of ['.cache', '.cache/lordos', CACHE]) { try { fs.mkdir(d); } catch (e) { /* there */ } } }, []);

  // any audio file → a WAV of its first 30 s (lord-media, sandboxed), checked for length
  const convert = async (path) => {
    setBusy(true);
    setErr(null);
    try {
      const info = await media.probe(path).catch(() => null);
      const ms = info && Number.isFinite(info.duration) && info.duration > 0 ? info.duration : null;   // milliseconds (lm_image.c)
      if (ms !== null && ms < MIN_MS) throw new Error(tr('settings.ai.clone.tooShort'));
      // a new name each time: lord-media never writes over a file, and a fixed one made the second try fail (and keep
      // the first clip)
      const out = `${fs.home()}/${CACHE}/${takeName('clone', Date.now(), 0)}`;
      const r = await media.transcode('settings', path, out, { format: 'wav', from: 0, to: MAX_MS });
      if (!r || !r.ok) throw new Error(r && r.error ? errorText(r.error) : tr('settings.ai.clone.failed'));
      setWav(out);
    } catch (e) { setErr(e.message || tr('settings.ai.clone.failed')); }
    setBusy(false);
  };
  const save = () => {
    const n = ownVoiceName(name);
    if (!n || !wav || !consent) return;
    try { addOwnVoice(n, wav); onClose(n); } catch (e) { setErr(e.message); }
  };

  const g = wm.frame(wm.get(win.id) || {});
  return (
    <Layer name="modal">
      <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 720, height: g.h || 560, alignItems: 'center',
        justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
        {picking ? (
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }}>
            <FilePicker title={tr('settings.ai.clone.pick')} filter={/\.(wav|mp3|m4a|ogg|oga|opus|flac|aac|webm)$/i}
              onClose={() => { setPicking(false); if (!wav) onClose(null); }} onPick={(p) => { setPicking(false); convert(Array.isArray(p) ? p[0] : p); }} />
          </View>
        ) : (
          <View style={{ width: 520, maxWidth: '94%', padding: 20, gap: spacing.m, borderRadius: 12, backgroundColor: th.windowBg,
            border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: th.text }}>{tr('settings.ai.clone.titleFile')}</Text>
            <Text style={{ fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4 }}>{tr('settings.ai.clone.tips')}</Text>
            <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4 }}>{tr('settings.ai.clone.format')}</Text>
            <View style={{ flexDirection: 'row', gap: spacing.s }}>
              <Button label={tr('settings.ai.clone.choose')} onClick={() => setPicking(true)} disabled={busy} />
            </View>
            {busy ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('settings.ai.clone.converting')}</Text> : null}
            {wav ? <Text style={{ fontSize: typeScale.caption, color: '#30d158' }}>{tr('settings.ai.clone.ready')}</Text> : null}
            <TextInput value={name} onChange={(x) => setName(x.slice(0, 60))} placeholder={tr('settings.ai.clone.name')} />
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
              <Toggle small on={consent} onChange={setConsent} />
              <Text style={{ flex: 1, fontSize: typeScale.callout, color: th.text, lineHeight: 1.4 }}>{tr('settings.ai.clone.consent')}</Text>
            </View>
            <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4 }}>{tr('settings.ai.clone.terms')}</Text>
            {err ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a' }}>{err}</Text> : null}
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
              <Button label={tr('settings.ai.clone.cancel')} onClick={() => onClose(null)} />
              <Button primary label={tr('settings.ai.clone.save')} disabled={!wav || !consent || !ownVoiceName(name) || busy} onClick={save} />
            </View>
          </View>
        )}
      </View>
    </Layer>
  );
}
