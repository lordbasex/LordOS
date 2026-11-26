// Settings → Artificial intelligence (0.8.9.6.2): a store around Lumo (docs/AI.md). At the top Lumo with the chain of
// its pieces (Ear → Brain → Voice → Eyes → Hands, each ✓ when installed) and "Talk to Lumo", disabled until the Ear and
// the agent exist; then a search with the filters All, Installed and one per capability (each with its count); "What
// Lumo can do": the functions of the one brain with their level (Sentinela works today and opens its own page, the
// others are "coming soon"); the capabilities as cards (Brain, Ear, Voice, Vision, Image) and one row of each; a
// capability's own page ("‹ Artificial intelligence › Voice", Esc and the mouse's Back go back: subpage.js); a sheet
// per model (its size, memory, license, source, what uses it, "try it" where it really works, install, remove, use);
// and at the bottom the space the models take, where they run and the privacy note.
//
// Everything the page had before is here: the Voice capability page holds reading aloud (on/off, ❖Esc, the player, live
// speech, where speech is made, quality), the languages' models and voices with their recorded samples, downloaded and
// removed as signed lordos-voice-* packages (sdk/voicemanager.js), the voice used, and the person's own voices
// (PersonalVoice.jsx, CloneSheet below). The Brain holds the lordos-llm-* models and the engine (sdk/llmcatalog.js),
// chosen for Sentinela, whose page (Sentinela.jsx) opens from its function card or with { section: 'ai', page:
// 'sentinela' }. Vision is Tesseract (the confined lord-ocr helper) with Debian's language data. The Ear (Whisper) and
// Image (Real-ESRGAN) are not inside LordOS yet: their cards show their facts and "coming soon". The pure parts are
// sdk/aistore.js.
import {
  View, Text, Image, Button, Toggle, Segmented, Dropdown, TextInput, SearchField, Layer, FilePicker, ScrollView, Lumo, useTheme, useLocale,
  tOptional, useSettings, useStore, useAppWindow, useKeyboard, direction, wm, media, admin, formatBytes, spacing, typeScale, settings,
  VOICE_DEFAULTS, speakAvailable, previewVoice, tryVoice, player as voicePlayer, VOICE_LANGS, voiceLabel, voicePackage, modelPackage, hasVoice,
  PREVIEWS_PACKAGE, scanVoices, installVoices, removeVoices, packagesFor, voiceConf, setVoiceConf, addOwnVoice, removeOwnVoice, ownVoiceName,
  takeName, errorText, fs, LLM_MODELS, packagesForModel, sentinelaEngine, sentinelaPrefs, setSentinelaPrefs, refreshSentinelaEngine, installLlm,
  removeLlm, lumoReady, translateWithLumo, ENGINE_PACKAGE, writingChoice, writingEnabled, setWritingOn,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import {
  CAPS, CAP_COLOR, CAP_ICON, FUNCTIONS, matchFunctions, llmFunctions, llmTryReady, LEVELS, AI_FILTERS, TESSDATA, ocrPackageOk, tessCodes, ocrLangs, brainEntries, voiceEntries,
  eyeEntries, earEntries, imageEntries, lumoState, filterEntries, filterCounts, groupByCap, capCard, modelBytes, sortForPage, voicePage,
} from '../../sdk/aistore.js';
import { Stack, Section, SettingRow, Note, Actions, tint } from './ui.jsx';
import { PersonalVoiceSheet } from './PersonalVoice.jsx';
import { SentinelaSettings } from './Sentinela.jsx';
import { subPage } from './subpage.js';

const BACKENDS = ['auto', 'cpu', 'gpu'];
const QUALITIES = ['auto', 'standard', 'hd'];
const CARD = 200, GAP = 12, FN_CARD = 190;
const SYS_VOICE = '/usr/share/lordos/voice', LLM_DIR = '/usr/share/lordos/llm';
const userVoice = () => `${fs.home()}/.local/share/lordos/voice`;
// the recorded samples (lordos-voice-previews), where say looks for them: the person's voice folder, then the system's
const hasPreviews = () => [userVoice(), SYS_VOICE].some((root) => { try { return !!fs.stat(`${root}/previews/es/lola.wav`); } catch (e) { return false; } });
// Tesseract's language data on this computer; ready: lord-ocr's English is there
function readTess() {
  let names = [];
  try { names = fs.list(TESSDATA, { hidden: false }).filter((e) => !e.dir).map((e) => e.name); } catch (e) { names = []; }
  const codes = tessCodes(names);
  return { codes, ready: codes.includes('eng') };
}
// as many columns of at least min px as fit, each stretched to share the width
const columns = (avail, min) => { const cols = Math.max(1, Math.floor((avail + GAP) / (min + GAP))); return { cols, w: Math.floor((avail - (cols - 1) * GAP) / cols) }; };
// "progress download <done> <total>" from the package engine (docs/PACKAGES.md)
const progressOf = (line) => { const m = /^progress download (\d+) (\d+)/.exec(String(line || '')); return m ? { done: Number(m[1]), total: Number(m[2]) } : null; };
const ocrNames = (list) => { if (!list.length || !list.every(ocrPackageOk)) throw new Error('not an OCR package'); return list.join(' '); };
const installOcr = (list, onP) => admin.run('pkg-install', ocrNames(list), (l) => { const p = progressOf(l); if (p && onP) onP(p); });
const removeOcr = (list) => admin.run('pkg-remove', ocrNames(list));

// The width of a box after layout (the page column narrows with the window), as the wallpaper gallery does.
function useWidth(ref, fallback) {
  const [w, setW] = useState(fallback);
  useEffect(() => {
    const measure = () => {
      const n = ref.current;
      if (!n) return;
      const x = Math.round(lord.getLayout(n.id)[2]);
      if (x > 0) setW((old) => (Math.abs(x - old) > 1 ? x : old));
    };
    measure();
    const id = setTimeout(measure, 60);
    return () => clearTimeout(id);
  });
  return w;
}

// a capability's icon, white (assets/ai/icons: <name>-96.png made from <name>-symbolic.svg by rsvg-convert, docs/AI.md)
function Icon({ name, size = 24 }) {
  return <Image src={`asset:ai/icons/${name}-96.png`} style={{ width: size, height: size }} />;
}
function IconTile({ cap, size = 44 }) {
  return (
    <View style={{ width: size, height: size, borderRadius: Math.round(size * 0.27), alignItems: 'center', justifyContent: 'center', backgroundColor: CAP_COLOR[cap] || '#5e5ce6' }}>
      <Icon name={CAP_ICON[cap] || 'brain'} size={Math.round(size * 0.55)} />
    </View>
  );
}

// NEW, COMING SOON, RECOMMENDED, IN USE and the functions' levels
function Tag({ label, tone }) {
  const t = useTheme();
  const bg = tone === 'accent' ? t.accent : tone === 'ok' ? 'rgba(48,209,88,0.22)' : tone === 'warn' ? 'rgba(255,159,10,0.22)' : tone === 'dark' ? 'rgba(0,0,0,0.55)' : 'rgba(142,142,147,0.35)';
  const fg = tone === 'ok' ? tint.ok : tone === 'warn' ? tint.warn : tone === 'accent' || tone === 'dark' ? '#fff' : t.text;
  return (
    <View style={{ paddingHorizontal: 5, height: 15, borderRadius: 4, justifyContent: 'center', backgroundColor: bg }}>
      <Text style={{ fontSize: 9, fontWeight: 700, color: fg, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

function Link({ label, onClick }) {
  const t = useTheme();
  return <Text onClick={onClick} style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.accent, whiteSpace: 'nowrap' }}>{label}</Text>;
}

// A heading over cards (the cards sit on the page, not in a group box): its title, a note and an accessory at the end.
function Head({ title, sub, accessory }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap', paddingHorizontal: spacing.xs }}>
      <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
      {sub ? <Text style={{ flex: 1, minWidth: 120, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{sub}</Text> : <View style={{ flex: 1 }} />}
      {accessory || null}
    </View>
  );
}

// The filters as wrapping pills, each with its count.
function Chips({ value, options, onChange }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
      {options.map(([id, label, count]) => {
        const on = id === value;
        return (
          <View key={id} focusable onClick={() => onChange(id)} onKeyDown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') onChange(id); }}
            style={{ flexDirection: 'row', alignItems: 'center', gap: 5, height: 28, paddingHorizontal: 11, borderRadius: 14,
              backgroundColor: on ? 'rgba(224,79,134,0.22)' : t.control, border: `1px solid ${on ? t.accent : 'transparent'}` }}>
            <Text style={{ fontSize: typeScale.callout, color: on ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{label}</Text>
            <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, fontVariantNumeric: 'tabular-nums' }}>{String(count)}</Text>
          </View>
        );
      })}
    </View>
  );
}

// One model: a band in its capability's colour with its icon, the state at the bottom (✓ installed, ☁ downloads), its
// tags at the top, the download's progress along the band's edge; its name, a short description and its facts.
function ModelCard({ e, w, ctx }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const percent = ctx.percentOf(e.id);
  const facts = [e.bytes ? formatBytes(e.bytes) : null, e.ramMb ? tr('settings.aiStore.ram', { size: formatBytes(e.ramMb * 1048576) }) : null, e.license || null].filter(Boolean);
  const dim = e.state === 'soon';
  return (
    <View focusable onClick={() => ctx.open(e)} onKeyDown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') ctx.open(e); }}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: w, borderRadius: 12, overflow: 'hidden', border: `1px solid ${hover ? t.accent : t.separator}`, backgroundColor: hover ? t.hover : t.control }}>
      <View style={{ height: 78, alignItems: 'center', justifyContent: 'center', backgroundColor: CAP_COLOR[e.cap], opacity: dim ? 0.6 : 1 }}>
        <Icon name={CAP_ICON[e.cap]} size={32} />
        <View style={{ position: 'absolute', top: 7, insetInlineEnd: 7, flexDirection: 'row', gap: 4 }}>
          {e.inUse ? <Tag tone="dark" label={tr('settings.aiStore.tag.inUse')} /> : null}
          {e.recommended && !e.inUse ? <Tag tone="dark" label={tr('settings.aiStore.tag.recommended')} /> : null}
          {e.state === 'soon' ? <Tag tone="dark" label={tr('settings.aiStore.tag.soon')} /> : null}
        </View>
        {e.state === 'soon' || percent != null ? null : (
          <View style={{ position: 'absolute', bottom: 7, insetInlineStart: 7, width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
            backgroundColor: e.state === 'installed' ? tint.ok : 'rgba(0,0,0,0.45)' }}>
            <Text style={{ fontSize: 11, fontWeight: 700, color: e.state === 'installed' ? '#08240f' : '#fff' }}>{e.state === 'installed' ? '✓' : '☁'}</Text>
          </View>
        )}
        {percent != null ? (
          <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 4, backgroundColor: 'rgba(0,0,0,0.4)', direction: 'ltr' }}>
            <View style={{ width: `${Math.max(2, percent)}%`, height: 4, backgroundColor: '#fff' }} />
          </View>
        ) : null}
      </View>
      <View style={{ padding: 10, paddingTop: 9, gap: 3 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{ctx.label(e)}</Text>
        <Text style={{ height: 30, overflow: 'hidden', fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{ctx.desc(e)}</Text>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 5, marginTop: 4, height: 18, overflow: 'hidden' }}>
          {facts.map((f, i) => (
            <View key={i} style={{ paddingHorizontal: 6, height: 18, borderRadius: 5, justifyContent: 'center', backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)' }}>
              <Text style={{ fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>{f}</Text>
            </View>
          ))}
        </View>
      </View>
    </View>
  );
}

function Grid({ list, avail, ctx }) {
  const { w } = columns(avail, CARD);
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP }}>
      {list.map((e) => <ModelCard key={e.id} e={e} w={w} ctx={ctx} />)}
    </View>
  );
}

// ---- the top: Lumo and its pieces ----

function LumoHero({ st, width }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const narrow = width < 560;
  const arrow = direction() === 'rtl' ? '←' : '→';
  return (
    <View style={{ flexDirection: narrow ? 'column' : 'row', alignItems: narrow ? 'flex-start' : 'center', gap: 20, padding: 18, borderRadius: 14,
      border: `1px solid ${t.separator}`, background: t.mode === 'dark' ? 'linear-gradient(135deg, #2c2148, #241d33 55%, #2d1b2b)' : 'linear-gradient(135deg, #ece6ff, #f6f2fb 55%, #fbe9f1)' }}>
      <Lumo pose={st.pose} size={140} />
      <View style={{ flex: 1, minWidth: 0, gap: 8 }}>
        <Text style={{ fontSize: typeScale.title, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr('settings.aiStore.lumo.title')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('settings.aiStore.lumo.sub')}</Text>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6, marginTop: 4 }}>
          {st.pieces.map((p, i) => (
            <View key={p.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              {i ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{arrow}</Text> : null}
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 28, paddingHorizontal: 10, borderRadius: 14, backgroundColor: t.control, border: `1px solid ${t.separator}` }}>
                <View style={{ width: 17, height: 17, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: p.ready ? tint.ok : 'transparent',
                  border: `1.5px solid ${p.ready ? tint.ok : t.textDim}` }}>
                  {p.ready ? <Text style={{ fontSize: 10, fontWeight: 700, color: '#062214' }}>✓</Text> : null}
                </View>
                <Text style={{ fontSize: typeScale.callout, color: t.text, whiteSpace: 'nowrap' }}>{tr(`settings.aiStore.piece.${p.id}`)}</Text>
                {p.soon ? <Text style={{ fontSize: 10, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('settings.aiStore.soonShort')}</Text> : null}
              </View>
            </View>
          ))}
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('settings.aiStore.lumo.ready', { ready: st.ready, total: st.total })}</Text>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, marginTop: 2 }}>
          <Button primary small disabled={!st.canTalk} label={tr('settings.aiStore.lumo.talk')} onClick={() => {}} />
          {!st.canTalk ? <Text style={{ flex: 1, minWidth: 200, fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{tr('settings.aiStore.lumo.talkSoon')}</Text> : null}
        </View>
      </View>
    </View>
  );
}

// ---- what Lumo can do: the one brain's functions ----

const fnName = (f, tr) => (f.id === 'sentinela' ? tr('sentinela.name') : f.id === 'writing' ? tr('writing.name') : tr(`settings.aiStore.fn.${f.id}`));
const fnExample = (f, tr) => (f.id === 'writing' ? tr('writing.example') : tr(`settings.aiStore.fnExample.${f.id}`));

function FunctionCard({ f, w, on, onOpen }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const level = LEVELS[f.level];
  const name = fnName(f, tr);
  return (
    <View focusable={!!onOpen} onClick={onOpen} onKeyDown={(ev) => { if (onOpen && (ev.key === 'Enter' || ev.key === ' ')) onOpen(); }}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: w, minHeight: 116, padding: 12, gap: 6, borderRadius: 12, opacity: f.ready ? 1 : 0.72, justifyContent: 'space-between',
        border: `1px solid ${f.ready && on ? t.accent : hover && onOpen ? t.accent : t.separator}`, backgroundColor: hover && onOpen ? t.hover : t.control }}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{name}</Text>
        <Tag tone={level === 'read' ? 'ok' : level === 'admin' ? 'warn' : undefined} label={tr(`settings.aiStore.level.${level}`)} />
      </View>
      <Text style={{ height: 30, overflow: 'hidden', fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{fnExample(f, tr)}</Text>
      {f.ready ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <Text style={{ flex: 1, fontSize: typeScale.caption, fontWeight: 600, color: on ? tint.ok : t.textDim, textAlign: 'start' }}>{tr(on ? 'settings.aiStore.fnOn' : 'settings.aiStore.fnOff')}</Text>
          <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.accent }}>{tr('settings.aiStore.fnSettings')}</Text>
        </View>
      ) : <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('settings.aiStore.soon')}</Text>}
    </View>
  );
}

// ---- a capability's card ----

function CapCard({ card, w, onOpen }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  return (
    <View focusable onClick={onOpen} onKeyDown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') onOpen(); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: w, padding: 12, gap: 8, borderRadius: 12, border: `1px solid ${hover ? t.accent : t.separator}`, backgroundColor: hover ? t.hover : t.control }}>
      <IconTile cap={card.cap} />
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(`settings.aiStore.cap.${card.cap}`)}</Text>
        {card.soon ? <Tag label={tr('settings.aiStore.tag.soon')} /> : null}
      </View>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`settings.aiStore.tech.${card.cap}`)}</Text>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('settings.aiStore.capMeta', { count: card.count, installed: card.installed })}</Text>
    </View>
  );
}

// "‹ Artificial intelligence › Voice": the first part goes back (so do Esc and the mouse's Back)
function Crumb({ here, onBack }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <View focusable onClick={onBack} onKeyDown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') onBack(); }}
        style={{ flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8, paddingVertical: 4, marginInlineStart: -8, borderRadius: 6 }}>
        <Text style={{ fontSize: 17, color: t.accent, transform: direction() === 'rtl' ? 'scaleX(-1)' : undefined }}>‹</Text>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.accent }}>{tr('settings.section.ai')}</Text>
      </View>
      <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>›</Text>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{here}</Text>
    </View>
  );
}

function CapHeader({ cap }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, padding: 16, borderRadius: 12, border: `1px solid ${t.separator}`, backgroundColor: t.control }}>
      <IconTile cap={cap} size={52} />
      <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
          <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr(`settings.aiStore.cap.${cap}`)}</Text>
          {cap === 'ear' || cap === 'img' ? <Tag label={tr('settings.aiStore.tag.soon')} /> : null}
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`settings.aiStore.tech.${cap}`)}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, textAlign: 'start' }}>{tr(`settings.aiStore.capDesc.${cap}`)}</Text>
      </View>
    </View>
  );
}

// ---- a model's sheet ----

function Fact({ k, v }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', gap: spacing.m }}>
      <Text style={{ width: 92, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{k}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.caption, color: t.text, textAlign: 'start' }}>{v}</Text>
    </View>
  );
}
function Box({ title, children }) {
  const t = useTheme();
  return (
    <View style={{ padding: 14, gap: 8, borderRadius: 12, border: `1px solid ${t.separator}`, backgroundColor: t.control }}>
      {title ? <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text> : null}
      {children}
    </View>
  );
}

// "Try it" where it really works: listen to a voice, ask the brain to translate, read a picture's text, compare a
// picture before and after; nothing for what is coming soon.
function TryBox({ e, ctx }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const [text, setText] = useState('');
  const [out, setOut] = useState('');
  const [busy, setBusy] = useState(false);
  const [picking, setPicking] = useState(false);
  useEffect(() => { setOut(''); setText(''); setBusy(false); }, [e.id]);
  const outBox = (s, placeholder) => (
    <View style={{ minHeight: 36, padding: 9, borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.22)' }}>
      <Text style={{ fontSize: typeScale.caption, color: s ? t.text : t.textDim, lineHeight: 1.4, textAlign: 'start', userSelect: 'text' }}>{s || placeholder}</Text>
    </View>
  );
  if (e.kind === 'voiceLang' || e.kind === 'voiceVoice' || e.kind === 'voiceModel' || e.kind === 'ownVoice') {
    const L = VOICE_LANGS.find((x) => x.id === e.lang);
    const voice = e.kind === 'ownVoice' ? e.voice : e.voice || (L && L.own);
    return (
      <Box title={tr('settings.aiStore.try.listen')}>
        {L ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{`«${L.sample}»`}</Text> : null}
        <Actions start>
          {e.kind !== 'ownVoice' ? <Button small label={tr('settings.aiStore.try.sample')} disabled={!ctx.previews} onClick={() => previewVoice(e.lang, voice)} /> : null}
          <Button small label={tr('settings.ai.try')} disabled={!ctx.voiceReady(e)} onClick={() => tryVoice(e.lang, voice)} />
        </Actions>
        {!ctx.previews && e.kind !== 'ownVoice' ? <Note>{tr('settings.ai.previewsHint')}</Note> : null}
      </Box>
    );
  }
  if (e.kind === 'llm') {
    const eng = sentinelaEngine.get() || {};
    const can = llmTryReady({ installed: (eng.models || []).some((m) => m.id === e.model.id), engine: eng.available,
      sentinelaOn: sentinelaPrefs().on, writingChoice: writingChoice() });
    const go = async () => {
      if (!text.trim() || busy) return;
      setBusy(true);
      setOut('');
      const r = await translateWithLumo(text.trim(), { model: e.model.id });
      setBusy(false);
      setOut(r.ok ? r.text : errorText(r.error));
    };
    return (
      <Box title={tr('settings.aiStore.try.title')}>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('settings.aiStore.try.translate')}</Text>
        <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center' }}>
          <TextInput value={text} onChange={(v) => setText(v.slice(0, 400))} onSubmit={go} placeholder={tr('settings.aiStore.try.translateHint')} style={{ flex: 1, minWidth: 0 }} />
          <Button small primary disabled={!can || busy || !text.trim()} label={tr('settings.aiStore.try.translateDo')} onClick={go} />
        </View>
        {can ? outBox(busy ? tr('settings.aiStore.try.thinking') : out, tr('settings.aiStore.try.out')) : <Note>{tr('writing.brain.tryNeeds')}</Note>}
      </Box>
    );
  }
  if (e.kind === 'ocr' || e.kind === 'ocrLang') {
    const read = async (p) => {
      setPicking(false);
      const path = Array.isArray(p) ? p[0] : p;
      if (!path) return;
      setBusy(true);
      setOut('');
      try { setOut((await readPictureText(path.startsWith('/') ? path : `${fs.home()}/${path}`, ocrLangs(ctx.uiLang, ctx.tess.codes))).trim() || tr('settings.aiStore.try.ocrNone')); }
      catch (err) { setOut(errorText(err)); }
      setBusy(false);
    };
    const g = picking ? wm.frame(wm.get(win.id) || {}) : null;
    return (
      <Box title={tr('settings.aiStore.try.title')}>
        <Actions start><Button small disabled={!ctx.tess.ready || busy} label={tr('settings.aiStore.try.ocr')} onClick={() => setPicking(true)} /></Actions>
        {outBox(busy ? tr('settings.aiStore.try.reading') : out, tr('settings.aiStore.try.ocrOut'))}
        {picking ? (
          <Layer name="modal">
            <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 640, height: g.h || 480 }}>
              <FilePicker computer title={tr('settings.aiStore.try.ocr')} filter={/\.(png|jpe?g|webp|bmp|tiff?)$/i} onClose={() => setPicking(false)} onPick={read} />
            </View>
          </Layer>
        ) : null}
      </Box>
    );
  }
  if (e.compare) {
    return (
      <Box title={tr('settings.aiStore.try.compare')}>
        <View style={{ borderRadius: 8, overflow: 'hidden', direction: 'ltr' }}>
          <Image src="asset:ai/esrgan-compare.jpg" style={{ width: '100%', height: 150, objectFit: 'cover' }} />
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('settings.aiStore.try.compareHint')}</Text>
      </Box>
    );
  }
  return null;
}

// a picture's text: decoded by lord-media, read by the confined lord-ocr helper (as Screenshots' "copy the text")
function readPictureText(path, langs) {
  return new Promise((resolve, reject) => {
    const img = media.image('settings', path);
    if (!img) { reject(new Error(tOptional('errors.media.imageUnreadable') || 'media.imageUnreadable')); return; }
    const off = media.on(img, (type, d) => {
      if (type !== 'ready' && type !== 'error') return;
      off();
      if (type === 'error') { media.close(img); reject(new Error(d.message || 'media.imageUnreadable')); return; }
      const id = media.crop(img, 0, 0, d.width, d.height);
      media.close(img);
      if (!id) { reject(new Error('media.imageUnreadable')); return; }
      media.ocr(id, langs || 'eng').then((s) => { media.close(id); resolve(String(s || '').replace(/\f/g, '').replace(/\n{3,}/g, '\n\n')); },
        (err) => { media.close(id); reject(err); });
    });
  });
}

function Sheet({ e, ctx, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  useKeyboard((ev) => { if (ev.key === 'Escape') onClose(); });
  const g = wm.frame(wm.get(win.id) || {}) || {};
  const W = g.w || 720, H = g.h || 560;
  const wide = W >= 760;
  const sw = Math.min(820, W - 32);
  const percent = ctx.percentOf(e.id);
  const busy = !!ctx.job;
  const installed = e.state === 'installed', soon = e.state === 'soon';
  const sub = [tr(`settings.aiStore.cap.${e.cap}`), tr(`settings.aiStore.tech.${e.cap}`), e.recommended ? tr('settings.aiStore.recommended') : null].filter(Boolean).join(' · ');
  // the brain's models: the functions that really use the model, from the list of functions, and Monitor's Investigate
  const used = e.cap === 'brain' && e.kind !== 'engine'
    ? tr('writing.brain.usedBy', { list: llmFunctions().map((f) => fnName(f, tr)).join(', ') })
    : tr(`settings.aiStore.used.${e.kind === 'engine' ? 'engine' : e.cap}`);
  const stateText = tr(soon ? 'settings.aiStore.state.soon' : installed ? 'settings.aiStore.state.installed' : 'settings.aiStore.state.cloud');
  const hint = tr(soon ? 'settings.aiStore.hint.soon' : installed ? 'settings.aiStore.hint.installed' : e.kind === 'ocrLang' ? 'settings.aiStore.hint.debian' : 'settings.aiStore.hint.cloud');
  const acts = ctx.actions(e);
  const facts = (
    <Box>
      <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.45, textAlign: 'start' }}>{ctx.desc(e)}</Text>
      <View style={{ gap: 5, marginTop: 4 }}>
        {e.bytes ? <Fact k={tr('settings.aiStore.fact.size')} v={formatBytes(e.bytes)} /> : null}
        {e.ramMb ? <Fact k={tr('settings.aiStore.fact.memory')} v={formatBytes(e.ramMb * 1048576)} /> : null}
        {e.license ? <Fact k={tr('settings.aiStore.fact.license')} v={e.license} /> : null}
        {e.source ? <Fact k={tr('settings.aiStore.fact.source')} v={e.source} /> : null}
        <Fact k={tr('settings.aiStore.fact.usedBy')} v={used} />
      </View>
    </Box>
  );
  const state = (
    <Box>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: installed ? tint.ok : soon ? tint.off : tint.warn }} />
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{e.inUse ? `${stateText} · ${tr('settings.ai.inUse')}` : stateText}</Text>
      </View>
      {percent != null ? (
        <View style={{ gap: 4 }}>
          <View style={{ height: 6, borderRadius: 3, backgroundColor: 'rgba(0,0,0,0.25)', overflow: 'hidden', direction: 'ltr' }}>
            <View style={{ width: `${Math.max(2, percent)}%`, height: 6, backgroundColor: t.accent }} />
          </View>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('settings.ai.downloading', { pct: percent })}</Text>
        </View>
      ) : null}
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{hint}</Text>
      {acts.length ? (
        <Actions start>
          {acts.map((a) => <Button key={a.id} small primary={a.primary} destructive={a.destructive} disabled={(a.needsIdle && busy) || a.disabled} label={a.label} onClick={a.run} />)}
        </Actions>
      ) : null}
      {ctx.msg ? <Note tone={ctx.msg.ok ? 'ok' : 'error'}>{ctx.msg.text}</Note> : null}
    </Box>
  );
  return (
    <Layer name="modal">
      <View onClick={onClose} style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: W, height: H, alignItems: 'center', justifyContent: 'center',
        backgroundColor: 'rgba(0,0,0,0.5)' }}>
        <View onClick={(ev) => ev?.stopPropagation?.()} style={{ width: sw, maxHeight: H - 32, borderRadius: 14, overflow: 'hidden',
          backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
          <ScrollView style={{ maxHeight: H - 34 }}>
            <View style={{ padding: 20, gap: 16 }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16 }}>
                <IconTile cap={e.cap} size={64} />
                <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
                  <Text style={{ fontSize: typeScale.title, fontWeight: 700, color: t.text, textAlign: 'start' }}>{ctx.label(e)}</Text>
                  <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{sub}</Text>
                </View>
                <Link label={tr('settings.aiStore.close')} onClick={onClose} />
              </View>
              <View style={{ flexDirection: wide ? 'row' : 'column', gap: 16, alignItems: 'flex-start' }}>
                <View style={{ flex: wide ? 1.3 : undefined, width: wide ? undefined : '100%', minWidth: 0, gap: 16 }}>
                  {facts}
                  <TryBox e={e} ctx={ctx} />
                </View>
                <View style={{ flex: wide ? 1 : undefined, width: wide ? undefined : '100%', minWidth: 0 }}>{state}</View>
              </View>
            </View>
          </ScrollView>
        </View>
      </View>
    </Layer>
  );
}

// ---- the Voice capability page: reading aloud, a language's models and voices, your own voices ----

function VoicePage({ ctx, avail }) {
  const { t: tr } = useLocale();
  const th = useTheme();
  const [s, set] = useSettings();
  const v = { ...VOICE_DEFAULTS, ...(s.voice || {}) };
  const put = (patch) => set({ voice: { ...(s.voice || {}), ...patch } });
  const [clone, setClone] = useState(null);        // 'record' | 'file' while the clone sheet is open
  useStore(voicePlayer);
  const { conf, lang, setLang, voiceView: vp } = ctx;
  const quality = QUALITIES.includes(conf.quality) ? conf.quality : 'auto';
  const own = vp.own;
  return (
    <View style={{ gap: 20 }}>
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
          <Segmented small value={BACKENDS.includes(conf.backend) ? conf.backend : 'auto'} onChange={(b) => ctx.saveConf({ backend: b })}
            options={BACKENDS.map((b) => [b, tr(`settings.ai.backend.${b}`)])} />
        </SettingRow>
        <SettingRow label={tr('settings.ai.quality')} hint={tr('settings.ai.qualityHint')}>
          <Segmented small value={quality} onChange={(q) => ctx.saveConf({ quality: q })} options={QUALITIES.map((q) => [q, tr(`settings.ai.quality.${q}`)])} />
        </SettingRow>
      </Section>

      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, flexWrap: 'wrap' }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: th.text }}>{tr('settings.aiStore.language')}</Text>
        {/* seven languages don't fit a segmented control in every interface language: a pop-up */}
        <Dropdown value={lang} onChange={setLang} width={220} options={VOICE_LANGS.map((l) => [l.id, tr(`settings.ai.lang.${l.id}`)])} />
      </View>

      <Head title={tr('settings.ai.models')} sub={tr('settings.ai.modelsFooter')} />
      <Grid list={vp.models} avail={avail} ctx={ctx} />

      <Head title={tr(vp.hd ? 'settings.ai.voicesHd' : 'settings.ai.voicesStandard')} sub={tr('settings.ai.voicesFooter')} />
      {!ctx.previews ? (
        <Section>
          <SettingRow label={tr('settings.ai.previews')} hint={tr('settings.ai.previewsHint')}>
            {ctx.percentOf('previews') != null ? <Text style={{ fontSize: typeScale.caption, color: th.textDim }}>{tr('settings.ai.downloading', { pct: ctx.percentOf('previews') })}</Text>
              : <Button small label={tr('settings.ai.download')} disabled={!!ctx.job} onClick={() => ctx.run('previews', (onP) => installVoices([PREVIEWS_PACKAGE], onP))} />}
          </SettingRow>
        </Section>
      ) : null}
      <Grid list={sortForPage(vp.voices)} avail={avail} ctx={ctx} />

      <Head title={tr('settings.ai.own')} sub={tr('settings.ai.ownFooter')}
        accessory={(
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <Button small primary label={tr('settings.ai.record')} onClick={() => setClone('record')} />
            <Button small label={tr('settings.ai.upload')} onClick={() => setClone('file')} />
          </View>
        )} />
      {own.length ? <Grid list={own} avail={avail} ctx={ctx} /> : <Note>{tr('settings.ai.ownEmpty')}</Note>}
      <Note>{speakAvailable() ? tr('settings.ai.privacy') : tr('settings.ai.notInstalled')}</Note>
      {clone ? (() => {
        const done = (name) => {
          setClone(null);
          if (name) { ctx.saveConf({ voices: { [lang]: name } }); ctx.setMsg({ ok: true, text: tr('settings.ai.clone.saved', { name }) }); }
          ctx.rescan();
        };
        return clone === 'record' ? <PersonalVoiceSheet lang={lang} onClose={done} /> : <CloneSheet lang={lang} onClose={done} />;
      })() : null}
    </View>
  );
}

// ---- the bottom: space, where the models run, privacy ----

function Footer({ ctx, width }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { w } = columns(width, 240);
  const used = modelBytes(ctx.sizes);
  const box = (title, children) => (
    <View style={{ width: w, padding: 14, gap: 8, borderRadius: 12, border: `1px solid ${t.separator}`, backgroundColor: t.control }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
      {children}
    </View>
  );
  const line = (s) => <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{s}</Text>;
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP }}>
      {box(tr('settings.aiStore.foot.space'), (
        <>
          <View style={{ height: 7, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.22)', overflow: 'hidden', direction: 'ltr' }}>
            <View style={{ width: `${Math.min(100, Math.max(used ? 2 : 0, (used / (8 * 1024 * 1024 * 1024)) * 100))}%`, height: 7, backgroundColor: t.accent }} />
          </View>
          {line(ctx.sizes ? tr('settings.aiStore.foot.spaceUsed', { size: formatBytes(used) }) : tr('settings.aiStore.foot.measuring'))}
        </>
      ))}
      {box(tr('settings.aiStore.foot.where'), (
        <>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap' }}>
            <Text style={{ fontSize: typeScale.caption, color: t.text }}>{tr('settings.aiStore.cap.voice')}</Text>
            <Segmented small value={BACKENDS.includes(ctx.conf.backend) ? ctx.conf.backend : 'auto'} onChange={(b) => ctx.saveConf({ backend: b })}
              options={BACKENDS.map((b) => [b, tr(`settings.ai.backend.${b}`)])} />
          </View>
          {line(tr('settings.aiStore.foot.brainCpu'))}
          {line(tr('settings.aiStore.foot.eyeCpu'))}
        </>
      ))}
      {box(tr('settings.aiStore.foot.privacy'), line(tr('settings.aiStore.foot.privacyText')))}
    </View>
  );
}

// ---- Writing: ✨ AI on selected text (docs/AI.md "Writing"), off by default ----

function WritingPage({ ready, onBrain }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(settings);
  // on with a model unless turned off; the switch writes the person's choice, kept for good
  const on = writingEnabled(writingChoice(), ready);
  return (
    <View style={{ gap: 14 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 14, borderRadius: 12, border: `1px solid ${t.separator}`, backgroundColor: t.control }}>
        <Text style={{ fontSize: 24 }}>✨</Text>
        <View style={{ flex: 1, gap: 3 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('writing.settings.toggle')}</Text>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('writing.settings.what')}</Text>
        </View>
        <Toggle on={on} onChange={(v) => setWritingOn(v)} />
      </View>
      {!ready ? <Note tone="warn">{tr('writing.settings.noModel')}</Note> : null}
      {!ready ? <View style={{ flexDirection: 'row' }}><Link label={tr('writing.settings.openBrain')} onClick={onBrain} /></View> : null}
      <Note>{tr('writing.settings.privacy')}</Note>
      <Note>{tr('writing.settings.how')}</Note>
    </View>
  );
}

// ---- the page ----

// page: index.jsx turns { section: 'ai', page } into the sub-page; alert: the Sentinela alert to open
export function AISettings({ alert }) {
  const { t: tr, lang: uiLang } = useLocale();
  const t = useTheme();
  useStore(settings);
  const view = useStore(subPage);                      // a capability's page, 'sentinela', or null: the store
  const setView = (v) => subPage.set(v || null);
  const eng = useStore(sentinelaEngine);
  const [conf, setConf] = useState(voiceConf);
  const [scan, setScan] = useState(scanVoices);
  const [previews, setPreviews] = useState(hasPreviews);
  const [tess, setTess] = useState(readTess);
  const [sizes, setSizes] = useState(null);
  const [lang, setLang] = useState(() => (VOICE_LANGS.some((l) => l.id === uiLang) ? uiLang : 'es'));
  const [job, setJob] = useState(null);            // { key, done, total } while a package downloads
  const [msg, setMsg] = useState(null);            // { ok, text }
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState('all');
  const [sheet, setSheet] = useState(null);        // the id of the entry whose sheet is open
  const pageRef = useRef(null);
  const width = useWidth(pageRef, 680);
  useKeyboard((ev) => { if (ev.key === 'Escape' && view && !sheet) setView(null); });
  const measure = () => {
    const dirs = { voiceSys: SYS_VOICE, voiceUser: userVoice(), llm: LLM_DIR, ocr: TESSDATA };
    Promise.all(Object.entries(dirs).map(([k, p]) => fs.duAsync(p, 4000).then((r) => [k, r && typeof r.size === 'number' ? r.size : 0], () => [k, 0])))
      .then((pairs) => setSizes(Object.fromEntries(pairs)));
  };
  useEffect(() => { refreshSentinelaEngine(); measure(); }, []);

  const p = sentinelaPrefs();
  const installedLlm = new Set((eng.models || []).map((m) => m.id));
  const chosen = p.model && installedLlm.has(p.model) ? p.model : null;
  const rescan = () => { setScan(scanVoices()); setPreviews(hasPreviews()); setTess(readTess()); refreshSentinelaEngine(); measure(); };
  const saveConf = (patch) => { try { setConf(setVoiceConf(patch)); } catch (e) { setMsg({ ok: false, text: e.message }); } };
  const run = async (key, fn) => {
    if (job) return;
    setMsg(null);
    setJob({ key, done: 0, total: 0 });
    try { await fn((pr) => setJob({ key, ...pr })); } catch (e) { setMsg({ ok: false, text: e.message || errorText(e) }); }
    setJob(null);
    rescan();
  };
  const percentOf = (key) => (job && job.key === key ? (job.total ? Math.floor((job.done * 100) / job.total) : 0) : null);
  const langName = (code) => tOptional(`languages.${code}`) || code;
  const ocrName = (e) => {
    const base = langName(e.locale.split('-')[0]);
    return e.locale === 'zh-Hans' || e.locale === 'zh-Hant' ? tr('settings.aiStore.scriptName', { language: base, script: tr(`settings.aiStore.script.${e.locale === 'zh-Hans' ? 'hans' : 'hant'}`) }) : base;
  };
  const label = (e) => {
    if (e.kind === 'voiceLang') return tr('settings.aiStore.voiceLangName', { language: tr(`settings.ai.lang.${e.lang}`), voice: voiceLabel(e.voice) });
    if (e.kind === 'voiceModel') return tr(`settings.ai.${e.q}`);
    if (e.kind === 'voiceVoice') return voiceLabel(e.voice);
    if (e.kind === 'ocrLang') return ocrName(e);
    return e.name;
  };
  const desc = (e) => {
    if (e.kind === 'voiceLang') return tr('settings.aiStore.desc.voiceLang', { voice: voiceLabel(e.voice) });
    if (e.kind === 'voiceModel') return tr(`settings.ai.${e.q}Hint`, { size: formatBytes(e.bytes) });
    if (e.kind === 'voiceVoice') return [tr(e.gender === 'f' ? 'settings.ai.female' : 'settings.ai.male'), e.inUse ? tr('settings.ai.inUse') : null].filter(Boolean).join(' · ');
    if (e.kind === 'ownVoice') return tr('settings.aiStore.desc.ownVoice');
    if (e.kind === 'ocrLang') return tr('settings.aiStore.desc.ocrLang', { language: ocrName(e) });
    return tr(`settings.aiStore.desc.${e.id.replace(/^[a-z]+:/, '').replace(/\./g, '_')}`);
  };

  const entries = [
    ...brainEntries(LLM_MODELS, eng, chosen),
    ...earEntries(),
    ...voiceEntries(VOICE_LANGS, scan, conf.voices || {}),
    ...eyeEntries(tess.ready, tess.codes),
    ...imageEntries(),
  ];
  const lumo = lumoState(entries);

  // what each sheet offers; a voice of the page's language, model or own voice as the old Voice page did
  const voiceReady = (e) => {
    const st = scan[e.lang] || {};
    if (e.kind === 'voiceVoice') return e.state === 'installed';
    return !!(st.standard || st.hd);
  };
  const removeLang = (lang_) => {
    const st = scan[lang_];
    const out = [];
    for (const hd of [false, true]) {
      if (!(hd ? st.hd : st.standard)) continue;
      out.push(modelPackage(lang_, hd), ...[...st.voices[hd ? 'hd' : 'standard']].filter((x) => hasVoice(x, hd, lang_)).map((x) => voicePackage(lang_, x, hd)));
    }
    return out.filter(Boolean);
  };
  const actions = (e) => {
    const A = [];
    const add = (id, lbl, fn, o = {}) => A.push({ id, label: lbl, run: fn, needsIdle: true, ...o });
    if (e.state === 'soon') return A;
    if (e.kind === 'llm') {
      if (e.state === 'installed') {
        if (!e.inUse) add('use', tr('settings.ai.use'), () => setSentinelaPrefs({ model: e.model.id }), { primary: true, needsIdle: false });
        add('remove', tr('settings.ai.remove'), () => run(e.id, async () => { await removeLlm([e.model.package]); if (p.model === e.model.id) setSentinelaPrefs({ model: null }); }), { destructive: true });
      } else {
        add('get', tr('settings.aiStore.install', { size: formatBytes(e.bytes) }), () => run(e.id, async (onP) => {
          await installLlm(packagesForModel(e.model.id, eng.available), onP);
          const v = refreshSentinelaEngine();
          if (!chosen && v.models.some((x) => x.id === e.model.id)) setSentinelaPrefs({ model: e.model.id });
        }), { primary: true });
      }
    } else if (e.kind === 'engine') {
      if (e.state !== 'installed') add('get', tr('settings.ai.download'), () => run(e.id, (onP) => installLlm([ENGINE_PACKAGE], onP)), { primary: true });
    } else if (e.kind === 'voiceLang') {
      const st = scan[e.lang] || {};
      const own = VOICE_LANGS.find((l) => l.id === e.lang).own;
      if (!st.standard) add('get', tr('settings.aiStore.install', { size: formatBytes(e.bytes) }), () => run(e.id, (onP) => installVoices(packagesFor(e.lang, own, false, scan), onP)), { primary: true });
      if (!st.hd) add('getHd', tr('settings.aiStore.installHd', { size: formatBytes(e.bytesHd) }), () => run(e.id, (onP) => installVoices(packagesFor(e.lang, own, true, scan), onP)));
      add('open', tr('settings.aiStore.openVoices'), () => { setSheet(null); setLang(e.lang); setView('voice'); }, { needsIdle: false });
      if (e.state === 'installed') add('remove', tr('settings.ai.remove'), () => run(e.id, () => removeVoices(removeLang(e.lang))), { destructive: true });
    } else if (e.kind === 'voiceModel') {
      const L = VOICE_LANGS.find((l) => l.id === e.lang);
      const hd = e.q === 'hd', st = scan[e.lang];
      if (e.state === 'installed') {
        add('remove', tr('settings.ai.remove'), () => run(e.id, () => removeVoices([modelPackage(e.lang, hd),
          ...[...st.voices[hd ? 'hd' : 'standard']].filter((x) => hasVoice(x, hd, e.lang)).map((x) => voicePackage(e.lang, x, hd))].filter(Boolean))), { destructive: true });
      } else add('get', tr('settings.aiStore.install', { size: formatBytes(e.bytes) }), () => run(e.id, (onP) => installVoices(packagesFor(e.lang, L.own, hd, scan), onP)), { primary: true });
    } else if (e.kind === 'voiceVoice') {
      if (e.state === 'installed') {
        if (!e.inUse) add('use', tr('settings.ai.use'), () => saveConf({ voices: { [e.lang]: e.voice } }), { primary: true, needsIdle: false });
        add('remove', tr('settings.ai.remove'), () => run(e.id, () => removeVoices([voicePackage(e.lang, e.voice, e.hd)])), { destructive: true });
      } else {
        const need = packagesFor(e.lang, e.voice, e.hd, scan);
        const L = VOICE_LANGS.find((l) => l.id === e.lang);
        add('get', tr('settings.ai.downloadSize', { size: formatBytes(e.bytes) }), () => run(e.id, (onP) => installVoices(need, onP)), { primary: true });
        if (need.length > 1) A.push({ id: 'needs', label: tr('settings.ai.needsModel', { size: formatBytes(L.mb[e.hd ? 'hd' : 'standard'] * 1e6) }), run: () => {}, disabled: true });
      }
    } else if (e.kind === 'ownVoice') {
      if (!e.inUse) add('use', tr('settings.ai.use'), () => saveConf({ voices: { [e.lang]: e.voice } }), { primary: true, needsIdle: false });
      add('remove', tr('settings.ai.remove'), () => {
        try {
          removeOwnVoice(e.voice);
          if (e.inUse) saveConf({ voices: { [e.lang]: VOICE_LANGS.find((l) => l.id === e.lang).own } });
          setSheet(null);
          rescan();
        } catch (err) { setMsg({ ok: false, text: err.message }); }
      }, { destructive: true, needsIdle: false });
    } else if (e.kind === 'ocrLang') {
      if (e.state === 'installed') add('remove', tr('settings.ai.remove'), () => run(e.id, () => removeOcr([e.package])), { destructive: true });
      else add('get', tr('settings.aiStore.install', { size: formatBytes(e.bytes) }), () => run(e.id, (onP) => installOcr([e.package], onP)), { primary: true });
    }
    return A;
  };

  // the Voice page's cards for the language chosen there: its two models, its voices (of the quality reading uses), your own voices
  const voiceView = (() => {
    const L = VOICE_LANGS.find((l) => l.id === lang);
    const vp = voicePage(L, scan[lang], QUALITIES.includes(conf.quality) ? conf.quality : 'auto', (conf.voices && conf.voices[lang]) || L.own, scan.own);
    return { ...vp, own: vp.own.map((e) => ({ ...e, license: tr('settings.aiStore.yours') })) };
  })();
  const voiceList = [...voiceView.models, ...voiceView.voices, ...voiceView.own];
  const ctx = {
    job, msg, setMsg, run, percentOf, label, desc, actions, previews, voiceReady, tess, uiLang, conf, saveConf, scan, lang, setLang, rescan, sizes, voiceView,
    open: (e) => { setMsg(null); setSheet(e.id); },
  };
  const sheetEntry = sheet ? entries.find((e) => e.id === sheet) || voiceList.find((e) => e.id === sheet) : null;
  const capName = (c) => tr(`settings.aiStore.cap.${c}`);
  const avail = width;

  let body;
  if (view === 'writing') {
    body = (
      <View style={{ gap: 20 }}>
        <Crumb here={tr('writing.name')} onBack={() => setView(null)} />
        <WritingPage ready={!!chosen || installedLlm.size > 0} onBrain={() => setView('brain')} />
      </View>
    );
  } else if (view === 'sentinela') {
    body = (
      <View style={{ gap: 20 }}>
        <Crumb here={tr('sentinela.name')} onBack={() => setView(null)} />
        <SentinelaSettings alertId={alert} />
      </View>
    );
  } else if (view && CAPS.includes(view)) {
    const list = sortForPage(entries.filter((e) => e.cap === view));
    body = (
      <View style={{ gap: 20 }}>
        <Crumb here={capName(view)} onBack={() => setView(null)} />
        <CapHeader cap={view} />
        {view === 'brain' ? <Note>{tr('settings.aiStore.brainNote')}</Note> : null}
        {view === 'voice' ? <VoicePage ctx={ctx} avail={avail} /> : <Grid list={list} avail={avail} ctx={ctx} />}
        {view === 'voice' ? (
          <>
            <Head title={tr('settings.aiStore.languages')} />
            <Grid list={list} avail={avail} ctx={ctx} />
          </>
        ) : null}
        {msg && !sheetEntry ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
      </View>
    );
  } else {
    const counts = filterCounts(entries, query, label, capName);
    const shown = filterEntries(entries, { filter, query, label, capName });
    const groups = groupByCap(shown);
    const perRow = columns(avail, CARD).cols;
    const fn = columns(avail, FN_CARD);
    const caps = columns(avail, 170);
    const plain = !query.trim() && filter === 'all';
    // a search finds functions too (Sentinela, Writing…), by their name and description in the person's language
    const fnFound = filter === 'all' ? matchFunctions(FUNCTIONS, query, (f) => fnName(f, tr), (f) => fnExample(f, tr)) : [];
    body = (
      <View style={{ gap: 20 }}>
        <LumoHero st={lumo} width={width} />
        <View style={{ gap: 10 }}>
          <SearchField value={query} onChange={setQuery} placeholder={tr('settings.aiStore.search')} style={{ width: '100%' }} />
          <Chips value={filter} onChange={setFilter}
            options={AI_FILTERS.map((f) => [f, f === 'all' || f === 'installed' ? tr(`settings.aiStore.filter.${f}`) : capName(f), counts[f]])} />
        </View>
        {plain || fnFound.length ? (
          <View style={{ gap: 10 }}>
            <Head title={tr('settings.aiStore.functions')} sub={tr('settings.aiStore.functionsSub')} />
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP }}>
              {(plain ? FUNCTIONS : fnFound).map((f) => <FunctionCard key={f.id} f={f} w={fn.w} on={f.id === 'sentinela' ? p.on : f.id === 'writing' ? writingEnabled(writingChoice(), installedLlm.size > 0) : false} onOpen={f.ready ? () => setView(f.id) : null} />)}
            </View>
          </View>
        ) : null}
        {plain ? (
          <View style={{ gap: 10 }}>
            <Head title={tr('settings.aiStore.capabilities')} sub={tr('settings.aiStore.capabilitiesSub', { count: entries.length })} />
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAP }}>
              {CAPS.map((c) => <CapCard key={c} card={capCard(c, entries)} w={caps.w} onOpen={() => setView(c)} />)}
            </View>
          </View>
        ) : null}
        {!groups.length && !fnFound.length ? (
          <Text style={{ paddingVertical: spacing.xl, fontSize: typeScale.callout, color: t.textDim, textAlign: 'center' }}>{tr('settings.aiStore.noMatch', { query: query.trim() })}</Text>
        ) : groups.map(([c, es]) => {
          const cut = query.trim() || filter !== 'all' ? es.length : perRow;
          const list = sortForPage(es);
          return (
            <View key={c} style={{ gap: 10 }}>
              <Head title={capName(c)} sub={tr(`settings.aiStore.tech.${c}`)}
                accessory={<Link label={es.length > cut ? tr('settings.aiStore.seeAll', { count: es.length }) : tr('settings.aiStore.open')} onClick={() => setView(c)} />} />
              <Grid list={list.slice(0, cut)} avail={avail} ctx={ctx} />
            </View>
          );
        })}
        {msg && !sheetEntry ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
        <Footer ctx={ctx} width={avail} />
      </View>
    );
  }
  return (
    <Stack>
      <View ref={pageRef} style={{ gap: 20 }}>{body}</View>
      {sheetEntry ? <Sheet e={sheetEntry} ctx={ctx} onClose={() => setSheet(null)} /> : null}
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
