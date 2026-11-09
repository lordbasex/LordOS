// Preferences.jsx — the profile editor, configurable shortcuts
// and the profile import from other terminal emulators. Laid out as a preferences pane (docs/DESIGN.md): a sidebar
// with the profiles and the sections, and a form with a right-aligned label column that stacks the
// labels above the controls when the window is narrow.
import {
  View, Text, TextInput, Toggle, Segmented, Stepper, Slider, Button, ScrollView, SidebarItem, SectionTitle, Hairline, GroupBox, ToolbarButton, ToolbarGroup,
  useStore, useTheme, fs, spacing, typeScale, direction, FilePicker, revealPath, errorText, t as T,
} from '@lordos/sdk';
import { createContext, useContext, useEffect, useRef, useState } from 'react';
import { PRESETS, DEFAULT_PROFILE, terminalSettings, saveTerminalSettings, schemeOf, allSchemes, newProfileId, profileName } from './profiles.js';
import { reconfigureAll, patchWin } from './state.js';
import { DEFAULT_BINDINGS, ACTION_IDS, actionLabel, bindings, comboLabel, keyCapture, hint } from './keys.js';
import { importIterm } from './iterm.js';
import { termConfig, chooseLocation, retryTermConfig, parseConfig, serializeConfig } from './config.js';

// Functions (not constants): the texts depend on the language in force when drawing.
const SECTIONS = () => [
  ['general', T('terminal.prefs.sec.general')], ['colors', T('terminal.prefs.sec.colors')], ['text', T('terminal.prefs.sec.text')], ['window', T('terminal.prefs.sec.window')],
  ['terminal', T('terminal.prefs.sec.terminal')], ['keys', T('terminal.prefs.sec.keys')], ['triggers', T('terminal.prefs.sec.triggers')],
  ['file', T('terminal.prefs.sec.file')],
];

// Monochrome glyphs of the sections in the sidebar.
const SECTION_ICON = { general: '≡', colors: '◐', text: 'A', window: '▭', terminal: '▤', keys: '⌨', triggers: '⟳', file: '▣' };

const ANSI_NAMES = () => ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white'].map((c) => T(`terminal.prefs.ansi.${c}`));

// Width of the form column: under 440 px the labels go above their controls.
const Narrow = createContext(false);
const LABEL_W = 170;

// A form row: the label right-aligned in its column, the control after it and an optional note
// (11 px, dim) under the control.
function Field({ t, label, children, note }) {
  const narrow = useContext(Narrow);
  return (
    <View style={{ flexDirection: narrow ? 'column' : 'row', alignItems: narrow ? 'stretch' : 'flex-start', gap: narrow ? 4 : spacing.m, paddingVertical: 4 }}>
      <Text style={narrow ? { fontSize: typeScale.callout, fontWeight: 500, color: t.text, textAlign: 'start' }
        : { width: LABEL_W, paddingTop: 6, lineHeight: 1.3, fontSize: typeScale.body, color: t.text, textAlign: 'end' }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
        <View style={{ minHeight: 28, flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s }}>{children}</View>
        {note ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{note}</Text> : null}
      </View>
    </View>
  );
}

// A group heading of the form (13/600, sentence case), 20 px above it except at the top.
const Heading = ({ t, children, top = 20 }) => (
  <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, marginTop: top, marginBottom: spacing.s, textAlign: 'start' }}>{children}</Text>
);

// Measures a box after layout (its width), to adapt the form (docs/DESIGN.md, "Media bar").
function useWidth(ref) {
  const [w, setW] = useState(0);
  useEffect(() => {
    const read = () => { try { if (ref.current) { const x = lord.getLayout(ref.current.id)[2]; setW((o) => (Math.abs(o - x) > 1 ? x : o)); } } catch (e) { /* not laid out yet */ } };
    read();
    const id = setInterval(read, 400);
    return () => clearInterval(id);
  }, []);
  return w;
}

const isHex = (v) => /^#[0-9a-fA-F]{6}$/.test(v);

function ColorWell({ t, label, value, onChange }) {
  const [edit, setEdit] = useState(null);
  return (
    <View style={{ width: 150, height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
      <View onClick={() => setEdit(value)} style={{ width: 28, height: 18, borderRadius: 5, backgroundColor: value, border: `1px solid ${t.separator}`,
        boxShadow: '0 1px 2px rgba(0,0,0,0.2)' }} />
      {edit != null
        ? <TextInput value={edit} autoFocus onChange={setEdit} style={{ width: 92, height: 22 }}
            onSubmit={(v) => { if (isHex(v)) onChange(v.toLowerCase()); setEdit(null); }} onCancel={() => setEdit(null)} />
        : <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>}
    </View>
  );
}

function SchemeCard({ t, id, scheme, selected, onClick }) {
  return (
    <View onClick={onClick} style={{ padding: 2, borderRadius: 10, border: `2px solid ${selected ? t.accent : 'transparent'}` }}>
      <View style={{ width: 128, height: 62, borderRadius: 7, padding: 8, backgroundColor: scheme.bg, border: `1px solid ${t.separator}`, overflow: 'hidden', direction: 'ltr' }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: scheme.fg, whiteSpace: 'nowrap', overflow: 'hidden' }}>{scheme.name}</Text>
        <Text style={{ fontSize: 10, color: scheme.ansi[2], marginTop: 2, fontFamily: 'monospace' }}>lord@lordos <Text style={{ color: scheme.ansi[4] }}>~</Text></Text>
        <View style={{ flexDirection: 'row', marginTop: 6, gap: 2 }}>
          {scheme.ansi.slice(0, 8).map((c, i) => <View key={`${id}${i}`} style={{ width: 12, height: 6, borderRadius: 2, backgroundColor: c }} />)}
        </View>
      </View>
    </View>
  );
}

// Inline drop-down list (it pushes the content down; Preferences has no floating layers).
function Picker({ t, value, options, onChange, width = 240, placeholder = '—' }) {
  const [open, setOpen] = useState(false);
  const cur = options.find(([v]) => v === value);
  return (
    <View style={{ width, maxWidth: '100%' }}>
      <View onClick={() => setOpen(!open)} style={{ height: 24, borderRadius: 6, paddingLeft: 9, paddingRight: 4, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: t.control, boxShadow: '0 1px 1px rgba(0,0,0,0.12)' }}>
        <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{cur ? cur[1] : placeholder}</Text>
        <View style={{ width: 16, height: 18, borderRadius: 4, backgroundColor: t.accent, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▲</Text>
          <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▼</Text>
        </View>
      </View>
      {open && (
        <View style={{ height: Math.min(200, options.length * 24 + 10), marginTop: 4, borderRadius: 8, border: `1px solid ${t.separator}`,
          backgroundColor: t.mode === 'dark' ? '#2c2c31' : '#ffffff', boxShadow: '0 6px 18px rgba(0,0,0,0.25)' }}>
          <ScrollView style={{ padding: 4 }}>
            {options.map(([v, label]) => (
              <View key={String(v)} onClick={() => { onChange(v); setOpen(false); }}
                style={{ height: 24, justifyContent: 'center', paddingHorizontal: spacing.s, borderRadius: 5, backgroundColor: v === value ? t.accent : 'transparent' }}>
                <Text style={{ fontSize: typeScale.callout, color: v === value ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>
              </View>
            ))}
          </ScrollView>
        </View>
      )}
    </View>
  );
}

// Button that waits for the next key combination.
function ComboButton({ t, combo, onChange, width = 120 }) {
  const waiting = useStore(keyCapture);
  const [mine, setMine] = useState(false);
  const active = mine && waiting;
  return (
    <View onClick={() => { setMine(true); keyCapture.set({ onCombo: (c) => { setMine(false); onChange(c); } }); }}
      style={{ width, height: 24, borderRadius: 6, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 6,
        border: `1px solid ${active ? t.accent : t.separator}`, backgroundColor: active ? 'rgba(10,132,255,0.15)' : t.control }}>
      <Text style={{ fontSize: typeScale.callout, color: active ? t.accent : t.text, whiteSpace: 'nowrap', direction: 'ltr' }}>{active ? T('terminal.prefs.combo.press') : combo ? comboLabel(combo) : T('terminal.prefs.combo.choose')}</Text>
    </View>
  );
}

const ACTION_OPTIONS = () => ACTION_IDS.map((a) => [a, actionLabel(a)]).sort((a, b) => a[1].localeCompare(b[1]));
const KIND = () => [['text', T('terminal.prefs.sec.text')], ['escape', 'Esc+'], ['hex', 'Hex'], ['ignore', T('terminal.prefs.kind.ignore')], ['action', T('terminal.prefs.kind.action')]];
const kindOf = (m) => (['text', 'escape', 'hex', 'ignore'].includes(m.action) ? m.action : 'action');

function KeyMapEditor({ t, list, onChange }) {
  const upd = (i, patch) => onChange(list.map((m, j) => (j === i ? { ...m, ...patch } : m)));
  return (
    <View style={{ gap: spacing.s }}>
      {list.length > 0 && (
      <GroupBox>
      {list.map((m, i) => (
        <View key={i} style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.s }}>
          <ComboButton t={t} combo={m.combo} width={96} onChange={(c) => upd(i, { combo: c })} />
          <Segmented small value={kindOf(m)} options={KIND()}
            onChange={(k) => upd(i, k === 'action' ? { action: 'newTab', param: '' } : { action: k, param: k === 'hex' ? '0x1b' : '' })} />
          {kindOf(m) === 'action'
            ? <Picker t={t} width={150} value={m.action} options={ACTION_OPTIONS()} onChange={(v) => upd(i, { action: v })} />
            : m.action !== 'ignore' && (
              <TextInput value={m.param || ''} style={{ width: 150 }} onChange={(v) => upd(i, { param: v })}
                placeholder={m.action === 'hex' ? '0x1b 0x5b 0x41' : m.action === 'escape' ? T('terminal.prefs.escPh') : 'git status\\n'} />
            )}
          <View style={{ flex: 1 }} />
          <Button small label="−" onClick={() => onChange(list.filter((_, j) => j !== i))} />
        </View>
      ))}
      </GroupBox>
      )}
      <Button small label={T('terminal.prefs.keys.add')} style={{ alignSelf: 'flex-start' }} onClick={() => onChange([...list, { combo: '', action: 'text', param: '' }])} />
    </View>
  );
}

// ---- import from other terminal emulators ----

function fontsApi() {
  const list = lord.fonts.list();
  const flat = (s) => s.replace(/[\s-]/g, '').toLowerCase();
  return {
    byPs(name) {
      const n = String(name || '');
      const exact = list.find((f) => f.ps && f.ps.toLowerCase() === n.toLowerCase());
      if (exact) return exact;
      const fam = flat(n.split('-')[0]);
      return list.find((f) => flat(f.family) === fam) || null;
    },
  };
}

const IMPORT_DIRS = ['Descargas', 'Downloads', 'Documentos', 'Escritorio', '.'];
const IMPORT_EXT = /\.(json|itermcolors|plist)$/i;

function candidates() {
  const out = [];
  for (const d of IMPORT_DIRS) {
    try {
      for (const e of fs.list(d, { hidden: false })) if (!e.dir && IMPORT_EXT.test(e.name)) out.push(d === '.' ? e.name : `${d}/${e.name}`);
    } catch (e) { /* the folder does not exist */ }
  }
  return out;
}

function readImport(path) {
  const rel = path.replace(/^~\/?/, '');
  const bytes = fs.read(rel, { binary: true });
  const head = String.fromCharCode(...new Uint8Array(bytes.slice(0, 8)));
  if (head === 'bplist00') return importIterm(bytes, path, fontsApi());
  const text = fs.read(rel);
  // a LordTerminal export (or a terminal.json): its profiles, schemes and key mappings as they are
  if (/^\s*\{/.test(text)) {
    let data = null;
    try { data = parseConfig(text); } catch (e) { data = null; }
    if (data && Array.isArray(data.profiles)) {
      return { kind: 'lordos', profiles: data.profiles.filter((x) => x && x.id).map((x) => ({ ...DEFAULT_PROFILE, ...x })),
        schemes: Object.entries(data.schemes || {}).map(([id, sc]) => ({ ...sc, id })), keyMap: Array.isArray(data.keyMap) ? data.keyMap : [],
        warnings: [], defaultGuid: null };
    }
  }
  return importIterm(text, path, fontsApi());
}

// What an imported profile would type into a shell by itself or on a key (security scan F1): its initial text, its
// 'send' triggers and its key mappings that send text. Each is said in the import list, and such a profile never
// replaces the default one or becomes it.
const TYPING_KEYS = new Set(['text', 'escape', 'hex']);
const typingKey = (m) => !!m && TYPING_KEYS.has(m.action);
function typesInto(p) {
  const out = [];
  const init = String(p.initialText || '').trim();
  if (init) out.push(T('terminal.prefs.import.types', { text: init.slice(0, 60) }));
  const sends = (p.triggers || []).filter((x) => x && x.action === 'send');
  if (sends.length) out.push(T('terminal.prefs.import.sends', { count: sends.length, text: String(sends[0].param || '').slice(0, 40) }));
  const keys = (p.keyMap || []).filter(typingKey);
  if (keys.length) out.push(T('terminal.prefs.import.typingKeys', { count: keys.length }));
  return out;
}

function ImportPanel({ t, onClose, onDone }) {
  const [path, setPath] = useState('');
  const [result, setResult] = useState(null);
  const [error, setError] = useState(null);
  const [pick, setPick] = useState({});
  const [withKeys, setWithKeys] = useState(false);   // global key mappings that type text: only when asked
  const [files] = useState(candidates);
  const load = (p) => {
    setPath(p);
    try {
      const r = readImport(p);
      setResult(r);
      setError(null);
      setPick(Object.fromEntries(r.profiles.map((x) => [x.id, true])));
    } catch (e) { setResult(null); setError(e.message || String(e)); }
  };
  const apply = (makeDefault) => {
    const cfg = terminalSettings();
    let profiles = [...cfg.profiles];
    let defaultProfile = cfg.defaultProfile;
    let added = 0;
    for (const np of result.profiles) {
      if (!pick[np.id]) continue;
      // the same iTerm2 profile, or (a LordTerminal export) the same profile id: replaced, not duplicated
      let i = np.itermGuid ? profiles.findIndex((x) => x.itermGuid === np.itermGuid) : -1;
      if (i < 0 && result.kind === 'lordos') i = profiles.findIndex((x) => x.id === np.id);
      // the default profile is never replaced by one that types into the shell by itself (every new tab would run
      // it): that one comes in as a profile of its own, a menu away
      const types = typesInto(np).length > 0;
      if (i >= 0 && types && profiles[i].id === defaultProfile) i = -1;
      const taken = i < 0 && profiles.some((x) => x.id === np.id);
      const prof = i >= 0 ? { ...np, id: profiles[i].id } : taken ? { ...np, id: newProfileId() + added } : np;
      if (i >= 0) profiles[i] = prof; else profiles.push(prof);
      // a profile that types a command when it opens never becomes the default: every new tab would run it
      if (makeDefault && np.itermGuid && np.itermGuid === result.defaultGuid && !types) defaultProfile = prof.id;
      added++;
    }
    const schemes = { ...(cfg.schemes || {}) };
    for (const s of result.schemes) {
      if (result.kind === 'lordos') { const { id, ...sc } = s; schemes[id] = sc; } else schemes[`it_${s.name.toLowerCase().replace(/[^a-z0-9]+/g, '_')}`] = s;
    }
    const incoming = result.keyMap.filter((m) => withKeys || !typingKey(m));
    const keyMap = [...(cfg.keyMap || []).filter((m) => !incoming.some((k) => k.combo === m.combo)), ...incoming];
    saveTerminalSettings({ profiles, defaultProfile, schemes, keyMap });
    reconfigureAll();
    onDone(T('terminal.prefs.import.done', { p: added, s: result.schemes.length, k: incoming.length }),
      result.profiles.find((x) => pick[x.id])?.id);
  };
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: t.windowBg, padding: spacing.xl, gap: spacing.m }}>
      <View style={{ gap: spacing.xs }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{T('terminal.prefs.import.title')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{T('terminal.prefs.import.intro')}</Text>
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center' }}>
        <TextInput value={path} placeholder={T('terminal.prefs.import.placeholder')} onChange={setPath} onSubmit={load} style={{ flex: 1 }} />
        <Button label={T('terminal.prefs.cancel')} onClick={onClose} />
        <Button label={T('terminal.prefs.import.open')} primary onClick={() => load(path)} />
      </View>
      <View style={{ flex: 1, flexDirection: 'row', gap: spacing.l }}>
        <View style={{ width: 220 }}>
          <Heading t={t} top={0}>{T('terminal.prefs.import.found')}</Heading>
          {files.length ? files.map((f) => (
            <SidebarItem key={f} label={f} selected={path === `~/${f}`} onClick={() => load(`~/${f}`)} />
          )) : <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{T('terminal.prefs.import.none')}</Text>}
        </View>
        <View style={{ flex: 1 }}>
          <ScrollView>
            {error && <Text style={{ fontSize: 13, color: '#ff453a' }}>{T('terminal.prefs.import.failed', { error })}</Text>}
            {result && (
              <>
                {result.profiles.length > 0 && <Heading t={t} top={0}>{T('terminal.prefs.import.profiles', { count: result.profiles.length })}</Heading>}
                {result.profiles.map((p) => {
                  const s = schemeOf(p);
                  return (
                    <View key={p.id} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 32 }}>
                      <Toggle small on={!!pick[p.id]} onChange={(v) => setPick({ ...pick, [p.id]: v })} />
                      <View style={{ width: 54, height: 20, borderRadius: 4, backgroundColor: s.bg, flexDirection: 'row', gap: 1, padding: 3 }}>
                        {s.ansi.slice(1, 7).map((c, i) => <View key={i} style={{ flex: 1, backgroundColor: c, borderRadius: 1 }} />)}
                      </View>
                      <Text style={{ fontSize: 13, color: t.text }}>{profileName(p)}{p.itermGuid && p.itermGuid === result.defaultGuid ? '  ★' : ''}</Text>
                      {typesInto(p).map((w, i) => <Text key={i} style={{ fontSize: 11, color: '#ff9f0a' }}>{w}</Text>)}
                      <Text style={{ fontSize: 11, color: t.textDim }}>{[`${p.font || 'JetBrains Mono'} ${p.fontSize} pt`, p.ligatures && T('terminal.prefs.import.ligatures'), p.triggers.length && T('terminal.prefs.import.triggers', { count: p.triggers.length }), p.keyMap.length && T('terminal.prefs.import.keys', { count: p.keyMap.length })].filter(Boolean).join(' · ')}</Text>
                    </View>
                  );
                })}
                {result.schemes.length > 0 && <Heading t={t}>{T('terminal.prefs.import.schemes', { count: result.schemes.length })}</Heading>}
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s }}>
                  {result.schemes.slice(0, 12).map((s) => <SchemeCard key={s.name} t={t} id={s.name} scheme={s} selected={false} onClick={() => {}} />)}
                </View>
                {result.keyMap.some(typingKey) && (
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: spacing.m }}>
                    <Toggle small on={withKeys} onChange={setWithKeys} />
                    <Text style={{ fontSize: 12, color: '#ff9f0a', flex: 1, textAlign: 'start' }}>{T('terminal.prefs.import.withKeys', { count: result.keyMap.filter(typingKey).length, list: result.keyMap.filter(typingKey).slice(0, 4).map((m) => m.combo).join(', ') })}</Text>
                  </View>
                )}
                {result.warnings.length > 0 && <Heading t={t}>{T('terminal.prefs.import.warnings')}</Heading>}
                {result.warnings.map((w, i) => <Text key={i} style={{ fontSize: 12, color: t.textDim, lineHeight: 1.5 }}>• {w}</Text>)}
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, marginTop: spacing.m, justifyContent: 'flex-end' }}>
                  {result.defaultGuid && <Button label={T('terminal.prefs.import.doDefault')} onClick={() => apply(true)} />}
                  <Button label={T('terminal.prefs.import.do')} primary onClick={() => apply(false)} />
                </View>
              </>
            )}
          </ScrollView>
        </View>
      </View>
    </View>
  );
}

// ---- the configuration file (config.js) ----

const homePath = (p) => (String(p).startsWith('~/') ? `${fs.home()}/${p.slice(2)}` : String(p));
const shortPath = (p) => { const h = fs.home(); return p && p.startsWith(`${h}/`) ? `~/${p.slice(h.length + 1)}` : p || ''; };

// Where the file is, a different place, back to the default one, import and export; and what happens when the
// chosen place isn't there (read-only from the last good copy, Retry, Choose Another).
function ConfigFileSection({ t, onImport, onNotice }) {
  const st = useStore(termConfig);
  const [picking, setPicking] = useState(null);      // 'location' | 'export'
  const [ask, setAsk] = useState(null);              // { path, exists }
  const [error, setError] = useState(null);
  const move = (path, how) => {
    setAsk(null);
    try { chooseLocation(path, how); setError(null); reconfigureAll(); onNotice(T(path ? 'terminal.prefs.file.moved' : 'terminal.prefs.file.backDefault', { path: shortPath(termConfig.get().path) })); }
    catch (e) { setError(errorText(e)); }
  };
  const picked = (raw) => {
    const path = homePath(raw);
    setPicking(null);
    if (!/\.json$/i.test(path)) { setError(T('terminal.prefs.file.mustJson')); return; }
    if (path === st.path) return;
    let exists = false;
    try { exists = !!fs.stat(path); } catch (e) { exists = false; }
    setAsk({ path, exists });
  };
  const exportTo = (raw) => {
    setPicking(null);
    try { lord.termconf.exportTo(homePath(raw), serializeConfig(st.data || {})); setError(null); onNotice(T('terminal.prefs.file.exported', { path: shortPath(homePath(raw)) })); }
    catch (e) { setError(errorText(e)); }
  };
  const Note = ({ children, bad }) => (
    <View style={{ paddingHorizontal: spacing.m, paddingVertical: spacing.s, borderRadius: 8, marginBottom: spacing.m, backgroundColor: bad ? 'rgba(255,159,10,0.16)' : t.control }}>
      <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.45, textAlign: 'start' }}>{children}</Text>
    </View>
  );
  return (
    <>
      <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginBottom: spacing.m, lineHeight: 1.45, textAlign: 'start' }}>{T('terminal.prefs.file.intro')}</Text>
      {st.readOnly ? (
        <Note bad>{T('terminal.prefs.file.away', { path: shortPath(st.path), why: errorText(st.problem || 'termconf.unavailable') })}</Note>
      ) : st.problem ? <Note bad>{errorText(st.problem)}</Note> : null}
      {error ? <Note bad>{error}</Note> : null}
      <Field t={t} label={T('terminal.prefs.file.where')} note={st.custom ? T('terminal.prefs.file.cacheNote', { path: shortPath(st.cache) }) : T('terminal.prefs.file.defaultNote')}>
        <TextInput value={shortPath(st.path)} onChange={() => {}} readOnly style={{ flex: 1, minWidth: 220 }} />
        <Button small label={T('terminal.prefs.file.show')} onClick={() => revealPath(st.readOnly ? st.cache : st.path)} />
      </Field>
      <Field t={t} label="">
        <Button small label={T('terminal.prefs.file.choose')} onClick={() => setPicking('location')} />
        {st.custom && <Button small label={T('terminal.prefs.file.reset')} onClick={() => move('', 'move')} />}
        {st.readOnly && <Button small label={T('terminal.prefs.file.retry')} onClick={() => { if (retryTermConfig()) { setError(null); reconfigureAll(); onNotice(T('terminal.prefs.file.back')); } }} />}
      </Field>
      <Field t={t} label={T('terminal.prefs.file.profiles')} note={T('terminal.prefs.file.backupsNote')}>
        <Button small label={T('terminal.prefs.file.import')} onClick={onImport} />
        <Button small label={T('terminal.prefs.file.export')} onClick={() => setPicking('export')} />
      </Field>
      {ask && (
        <View style={{ marginTop: spacing.m, padding: spacing.m, borderRadius: 10, gap: spacing.s, borderWidth: 1, borderColor: t.separator }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{shortPath(ask.path)}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45, textAlign: 'start' }}>{T(ask.exists ? 'terminal.prefs.file.askExists' : 'terminal.prefs.file.askMove')}</Text>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, justifyContent: 'flex-end' }}>
            <Button small label={T('terminal.prefs.cancel')} onClick={() => setAsk(null)} />
            {ask.exists && <Button small label={T('terminal.prefs.file.replace')} onClick={() => move(ask.path, 'move')} />}
            <Button small primary label={T(ask.exists ? 'terminal.prefs.file.useThere' : 'terminal.prefs.file.moveThere')} onClick={() => move(ask.path, ask.exists ? 'use' : 'move')} />
          </View>
        </View>
      )}
      {picking && <FilePicker mode="save" modal askReplace={picking === 'export'} name={picking === 'export' ? 'terminal-profiles.json' : 'terminal.json'}
        title={T(picking === 'export' ? 'terminal.prefs.file.exportTitle' : 'terminal.prefs.file.chooseTitle')}
        onClose={() => setPicking(null)} onPick={(p) => (picking === 'export' ? exportTo(p) : picked(p))} />}
    </>
  );
}

// ---- preferences ----

export function Preferences({ wid, onClose }) {
  const t = useTheme();
  useStore(termConfig);        // re-render on save, and when the file is read again
  const cfg = terminalSettings();
  const [sel, setSel] = useState(cfg.defaultProfile);
  const [section, setSection] = useState('general');
  const [importing, setImporting] = useState(false);
  const [notice, setNotice] = useState(null);
  const pane = useRef(null);
  const paneW = useWidth(pane);
  const narrow = paneW > 0 && paneW < 480;          // the form stacks its labels
  const narrowWin = paneW > 0 && paneW < 420;       // and the sidebar gets a bit narrower
  const p = cfg.profiles.find((x) => x.id === sel) || cfg.profiles[0];

  const save = (profiles, extra = {}) => { saveTerminalSettings({ profiles, ...extra }); reconfigureAll(); };
  const set = (patch) => save(cfg.profiles.map((x) => (x.id === p.id ? { ...x, ...patch } : x)));
  const setColor = (key, value, idx) => {
    const base = { ...schemeOf(p), name: T('terminal.prefs.custom') };
    const custom = { ...base, ansi: [...base.ansi] };
    if (key === 'ansi') custom.ansi[idx] = value; else custom[key] = value;
    set({ colors: 'custom', custom });
  };
  const addProfile = (from) => {
    const id = newProfileId();
    const np = { ...(from || DEFAULT_PROFILE), id, itermGuid: '', name: from ? T('terminal.prefs.copyOf', { name: profileName(from) }) : T('terminal.prefs.newProfile') };
    save([...cfg.profiles, np]);
    setSel(id);
  };
  const removeProfile = () => {
    if (cfg.profiles.length < 2) return;
    const rest = cfg.profiles.filter((x) => x.id !== p.id);
    save(rest, { defaultProfile: cfg.defaultProfile === p.id ? rest[0].id : cfg.defaultProfile });
    setSel(rest[0].id);
  };
  const saveShortcut = (action, combos) => {
    const cur = bindings();
    const custom = { ...(cfg.shortcuts || {}) };
    // a combination belongs to one action only: it is removed from the others
    for (const [a, list] of Object.entries(cur)) {
      if (a === action || !combos.some((c) => list.includes(c))) continue;
      custom[a] = list.filter((c) => !combos.includes(c));
    }
    custom[action] = combos;
    save(cfg.profiles, { shortcuts: custom });
  };
  const resetShortcut = (action) => {
    const custom = { ...(cfg.shortcuts || {}) };
    delete custom[action];
    save(cfg.profiles, { shortcuts: custom });
  };

  const fontList = (() => { try { return lord.fonts.list(); } catch (e) { return []; } })();
  const families = [...new Set(fontList.filter((f) => !f.color).map((f) => f.family))];
  const monoFamilies = [...new Set(fontList.filter((f) => f.mono).map((f) => f.family))];
  const schemes = allSchemes();
  const imported = Object.entries(schemes).filter(([id]) => !PRESETS[id]);
  const scheme = schemeOf(p);

  const body = {
    general: (
      <>
        <Field t={t} label={T('terminal.prefs.name')}><TextInput value={p.name} onChange={(v) => set({ name: v })} style={{ width: 240 }} /></Field>
        <Field t={t} label={T('terminal.prefs.defaultProfile')}>
          {cfg.defaultProfile === p.id ? <Text style={{ fontSize: 12, color: t.textDim }}>{T('terminal.prefs.isDefault')}</Text>
            : <Button label={T('terminal.prefs.useDefault')} onClick={() => save(cfg.profiles, { defaultProfile: p.id })} />}
        </Field>
        <Field t={t} label={T('terminal.prefs.workingDir')}>
          <Segmented small value={p.workingDir} onChange={(v) => set({ workingDir: v })}
            options={[['home', T('terminal.prefs.dir.home')], ['reuse', T('terminal.prefs.dir.reuse')], ['custom', T('terminal.prefs.dir.custom')]]} />
        </Field>
        {p.workingDir === 'custom' && (
          <Field t={t} label={T('terminal.prefs.folder')}><TextInput value={p.customDir} placeholder="~/proyectos" onChange={(v) => set({ customDir: v })} style={{ width: 240 }} /></Field>
        )}
        <Field t={t} label={T('terminal.prefs.initialText')} note="\n = Enter">
          <TextInput value={p.initialText || ''} placeholder={T('terminal.prefs.initialTextPh')} onChange={(v) => set({ initialText: v })} style={{ width: 240 }} />
        </Field>
        <Field t={t} label={T('terminal.prefs.badge')} note={T('terminal.prefs.badgeNote')}>
          <TextInput value={p.badge} placeholder={T('terminal.prefs.badgePh')} onChange={(v) => set({ badge: v })} style={{ width: 200 }} />
        </Field>
        <Field t={t} label={T('terminal.prefs.tabColor')}>
          {['', '#ff453a', '#ff9f0a', '#ffd60a', '#32d74b', '#0a84ff', '#bf5af2'].map((c) => (
            <View key={c || 'none'} onClick={() => set({ tabColor: c })} style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
              backgroundColor: c || 'transparent', border: c ? '0 solid transparent' : `1.5px solid ${t.textDim}` }}>
              {p.tabColor === c ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: c ? '#fff' : t.textDim }} /> : null}
            </View>
          ))}
          {p.tabColor && !['#ff453a', '#ff9f0a', '#ffd60a', '#32d74b', '#0a84ff', '#bf5af2'].includes(p.tabColor) && (
            <View style={{ width: 20, height: 20, borderRadius: 10, backgroundColor: p.tabColor, alignItems: 'center', justifyContent: 'center' }}>
              <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' }} />
            </View>
          )}
        </Field>
        <Field t={t} label={T('terminal.prefs.rules')} note={`${T('terminal.prefs.rulesNote')}. ${T('terminal.prefs.rulesHint')}`}>
          <TextInput value={(p.rules || '').replace(/\n/g, ' ; ')} placeholder="~/prod/** ; root@* ; *.prod.com" style={{ width: 260, maxWidth: '100%' }}
            onChange={(v) => set({ rules: v.split(/\s*;\s*/).join('\n') })} />
        </Field>
        <Field t={t} label={T('terminal.prefs.confirmClose')}><Toggle small on={p.confirmClose} onChange={(v) => set({ confirmClose: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.hotkey')}>
          <Segmented small value={cfg.hotkey} onChange={(v) => save(cfg.profiles, { hotkey: v })} options={[['F12', 'F12'], ['Super+`', '❖`'], ['off', T('terminal.prefs.off')]]} />
        </Field>
      </>
    ),
    colors: (
      <>
        <Heading t={t} top={0}>{T('terminal.prefs.schemes')}</Heading>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.xs }}>
          {Object.entries(PRESETS).map(([id, s]) => (
            <SchemeCard key={id} t={t} id={id} scheme={s} selected={p.colors === id} onClick={() => set({ colors: id })} />
          ))}
        </View>
        {imported.length > 0 && <Heading t={t}>{T('terminal.prefs.imported')}</Heading>}
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.xs }}>
          {imported.map(([id, s]) => (
            <SchemeCard key={id} t={t} id={id} scheme={s} selected={p.colors === id} onClick={() => set({ colors: id })} />
          ))}
        </View>
        <Heading t={t}>{p.colors === 'custom' ? T('terminal.prefs.customColors') : T('terminal.prefs.editColors')}</Heading>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.l }}>
          <View>
            {[['fg', T('terminal.prefs.sec.text')], ['bg', T('terminal.prefs.c.bg')], ['bold', T('terminal.prefs.c.bold')], ['link', T('terminal.prefs.c.link')]].map(([k, l]) => (
              <ColorWell key={k} t={t} label={l} value={scheme[k] || scheme.fg} onChange={(v) => setColor(k, v)} />
            ))}
          </View>
          <View>
            {[['cursor', T('terminal.prefs.cursor')], ['cursorText', T('terminal.prefs.c.cursorText')], ['selection', T('terminal.prefs.c.selection')], ['selectionText', T('terminal.prefs.c.selectionText')]].map(([k, l]) => (
              <ColorWell key={k} t={t} label={l} value={scheme[k] || scheme.fg} onChange={(v) => setColor(k, v)} />
            ))}
          </View>
          <View>
            {ANSI_NAMES().map((n, i) => <ColorWell key={n} t={t} label={n} value={scheme.ansi[i]} onChange={(v) => setColor('ansi', v, i)} />)}
          </View>
          <View>
            {ANSI_NAMES().map((n, i) => <ColorWell key={n} t={t} label={T('terminal.prefs.bright', { color: n })} value={scheme.ansi[i + 8]} onChange={(v) => setColor('ansi', v, i + 8)} />)}
          </View>
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, marginTop: spacing.s, lineHeight: 1.4, textAlign: 'start' }}>{T('terminal.prefs.colorHint')}</Text>
      </>
    ),
    text: (
      <>
        <Field t={t} label={T('terminal.prefs.font')} note={T('terminal.prefs.monoCount', { count: monoFamilies.length })}>
          <Picker t={t} value={p.font || ''} onChange={(v) => set({ font: v })}
            options={[['', T('terminal.prefs.fontBuiltin')], ...monoFamilies.filter((f) => f !== 'JetBrains Mono').map((f) => [f, f])]} />
        </Field>
        <Field t={t} label={T('terminal.prefs.ligatures')} note="-> => != === (JetBrains Mono, Fira Code…)"><Toggle small on={p.ligatures} onChange={(v) => set({ ligatures: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.bidi')} note={T('terminal.prefs.bidiNote')}><Toggle small on={p.bidi !== false} onChange={(v) => set({ bidi: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.nonAscii')} note={T('terminal.prefs.nonAsciiNote')}>
          <Picker t={t} value={p.nonAsciiFont || ''} onChange={(v) => set({ nonAsciiFont: v })}
            options={[['', T('terminal.prefs.same')], ...families.map((f) => [f, f])]} />
        </Field>
        <Field t={t} label={T('terminal.prefs.size')}><Stepper value={p.fontSize} min={7} max={40} onChange={(v) => set({ fontSize: v })} format={(v) => `${v} pt`} /></Field>
        <Field t={t} label={T('terminal.prefs.lineSpacing')}>
          <Stepper value={p.lineSpacing} min={0.8} max={2} step={0.05} onChange={(v) => set({ lineSpacing: Math.round(v * 100) / 100 })} format={(v) => `${Math.round(v * 100)}%`} />
        </Field>
        <Field t={t} label={T('terminal.prefs.padding')}><Stepper value={p.padding} min={0} max={40} onChange={(v) => set({ padding: v })} format={(v) => `${v} px`} /></Field>
        <Field t={t} label={T('terminal.prefs.cursor')}>
          <Segmented small value={p.cursorStyle} onChange={(v) => set({ cursorStyle: v })} options={[['block', T('terminal.prefs.cur.block')], ['underline', T('terminal.prefs.cur.underline')], ['beam', T('terminal.prefs.cur.beam')]]} />
        </Field>
        <Field t={t} label={T('terminal.prefs.cursorBlink')}><Toggle small on={p.cursorBlink} onChange={(v) => set({ cursorBlink: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.cursorGuide')} note={T('terminal.prefs.cursorGuideNote')}><Toggle small on={p.cursorGuide} onChange={(v) => set({ cursorGuide: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.boldBright')}><Toggle small on={p.boldBright} onChange={(v) => set({ boldBright: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.useBold')} note={T('terminal.prefs.useBoldNote')}><Toggle small on={p.useBoldFont} onChange={(v) => set({ useBoldFont: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.useItalic')}><Toggle small on={p.useItalic} onChange={(v) => set({ useItalic: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.blinkText')}><Toggle small on={p.blinkText} onChange={(v) => set({ blinkText: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.minContrast')} note={`${Math.round(p.minimumContrast * 100)}%`}>
          <Slider value={p.minimumContrast} onChange={(v) => set({ minimumContrast: Math.round(v * 100) / 100 })} />
        </Field>
      </>
    ),
    window: (
      <>
        <Field t={t} label={T('terminal.prefs.transparency')} note={`${Math.round(p.transparency * 100)}%`}>
          <Slider value={p.transparency} max={0.9} onChange={(v) => set({ transparency: Math.round(v * 100) / 100 })} />
        </Field>
        <Field t={t} label={T('terminal.prefs.blur')}><Toggle small on={p.blur} onChange={(v) => set({ blur: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.dimInactive')}><Toggle small on={p.dimInactive} onChange={(v) => set({ dimInactive: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.statusBar')}><Toggle small on={p.statusBar} onChange={(v) => set({ statusBar: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.columns')} note={T('terminal.prefs.columnsNote')}>
          <Stepper value={p.columns || 0} min={0} max={400} step={10} onChange={(v) => set({ columns: v })} format={(v) => (v ? String(v) : '—')} />
        </Field>
        <Field t={t} label={T('terminal.prefs.rows')}>
          <Stepper value={p.rows || 0} min={0} max={200} step={5} onChange={(v) => set({ rows: v })} format={(v) => (v ? String(v) : '—')} />
        </Field>
      </>
    ),
    terminal: (
      <>
        <Field t={t} label={T('terminal.prefs.scrollback')} note={T('terminal.prefs.scrollbackNote')}>
          <Segmented small value={p.scrollback} onChange={(v) => set({ scrollback: v })}
            options={[[1000, '1.000'], [10000, '10.000'], [100000, '100.000'], [1000000, T('terminal.prefs.million')]]} />
        </Field>
        <Field t={t} label={T('terminal.prefs.mouseReporting')} note={T('terminal.prefs.mouseNote')}><Toggle small on={p.mouseReporting} onChange={(v) => set({ mouseReporting: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.copyOnSelect')}><Toggle small on={p.copyOnSelect} onChange={(v) => set({ copyOnSelect: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.altScroll')}><Toggle small on={p.alternateScroll} onChange={(v) => set({ alternateScroll: v })} /></Field>
        <Field t={t} label={T('terminal.prefs.bell')}>
          <Segmented small value={p.bell} onChange={(v) => set({ bell: v })} options={[['visual', T('terminal.prefs.bellVisual')], ['none', T('terminal.prefs.bellNone')]]} />
        </Field>
        <Field t={t} label={T('terminal.prefs.wordChars')} note={T('terminal.prefs.wordNote')}>
          <TextInput value={p.wordChars} onChange={(v) => set({ wordChars: v.slice(0, 60) })} style={{ width: 160 }} />
        </Field>
      </>
    ),
    keys: (
      <>
        <Field t={t} label={T('terminal.prefs.altEsc')}><Toggle small on={p.altSendsEscape} onChange={(v) => set({ altSendsEscape: v })} /></Field>
        <Heading t={t}>{T('terminal.prefs.keys.profile')}</Heading>
        <KeyMapEditor t={t} list={p.keyMap || []} onChange={(keyMap) => set({ keyMap })} />
        <Heading t={t}>{T('terminal.prefs.keys.global')}</Heading>
        <KeyMapEditor t={t} list={cfg.keyMap || []} onChange={(keyMap) => save(cfg.profiles, { keyMap })} />
        <Heading t={t}>{T('terminal.prefs.keys.shortcuts')}</Heading>
        <GroupBox>
        {Object.keys(DEFAULT_BINDINGS).map((a) => {
          const combos = bindings()[a] || [];
          const changed = !!cfg.shortcuts?.[a];
          return (
            <View key={a} style={{ flexDirection: 'row', alignItems: 'center', minHeight: 34, gap: spacing.s, paddingVertical: 4 }}>
              <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{actionLabel(a)}</Text>
              {!narrow && combos.length > 1 ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', direction: 'ltr' }}>{combos.slice(1).map(comboLabel).join('  ')}</Text> : null}
              {changed && <Button small plain label={T('terminal.prefs.reset')} onClick={() => resetShortcut(a)} />}
              <ComboButton t={t} combo={combos[0]} onChange={(c) => saveShortcut(a, [c, ...combos.slice(1).filter((x) => x !== c)])} />
            </View>
          );
        })}
        </GroupBox>
      </>
    ),
    file: <ConfigFileSection t={t} onImport={() => setImporting(true)} onNotice={setNotice} />,
    triggers: (
      <>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginBottom: spacing.m, lineHeight: 1.45, textAlign: 'start' }}>{T('terminal.prefs.triggersHint')}</Text>
        {(p.triggers || []).length > 0 && (
        <GroupBox style={{ marginBottom: spacing.s }}>
        {(p.triggers || []).map((tr, i) => {
          const upd = (patch) => set({ triggers: p.triggers.map((x, j) => (j === i ? { ...x, ...patch } : x)) });
          return (
            <View key={i} style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.s }}>
              <Toggle small on={tr.enabled !== false} onChange={(v) => upd({ enabled: v })} />
              <TextInput value={tr.regex} placeholder="regex" onChange={(v) => upd({ regex: v })} style={{ width: 150 }} />
              <Picker t={t} width={130} value={tr.action} onChange={(v) => upd({ action: v })}
                options={[['notify', T('terminal.prefs.tr.notify')], ['bell', T('terminal.prefs.bell')], ['attention', T('terminal.prefs.tr.attention')], ['send', T('terminal.prefs.tr.send')], ['capture', T('terminal.prefs.tr.capture')],
                  ['highlight', T('terminal.prefs.tr.highlight')], ['mark', T('terminal.prefs.tr.mark')], ['title', T('terminal.prefs.tr.title')], ['stop', T('terminal.prefs.tr.stop')]]} />
              <TextInput value={tr.param || ''} placeholder={T('terminal.prefs.param')} onChange={(v) => upd({ param: v })} style={{ width: 130 }} />
              <View style={{ flex: 1 }} />
              <Button small label="−" onClick={() => set({ triggers: p.triggers.filter((_, j) => j !== i) })} />
            </View>
          );
        })}
        </GroupBox>
        )}
        <Button small label={T('terminal.prefs.tr.add')} onClick={() => set({ triggers: [...(p.triggers || []), { regex: 'error', action: 'notify', param: '', enabled: true }] })} style={{ alignSelf: 'flex-start' }} />
      </>
    ),
  };

  const sectionLabel = SECTIONS().find(([id]) => id === section)?.[1] || '';
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: t.windowBg, flexDirection: 'row' }}>
      <View style={{ width: narrowWin ? 168 : 196, backgroundColor: t.sidebarBg, paddingTop: spacing.s }}>
        <Hairline edge="end" />
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 10, paddingBottom: spacing.m }}>
            <SectionTitle>{T('terminal.prefs.profiles')}</SectionTitle>
            {cfg.profiles.map((x) => (
              <SidebarItem key={x.id} label={profileName(x)} detail={x.id === cfg.defaultProfile ? '★' : null} selected={x.id === p.id} onClick={() => setSel(x.id)} />
            ))}
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs, marginTop: spacing.s, paddingHorizontal: spacing.xs }}>
              <ToolbarGroup>
                <ToolbarButton small icon="+" title={T('terminal.prefs.newProfile')} onClick={() => addProfile(null)} />
                <ToolbarButton small icon="−" title={T('terminal.prefs.remove')} disabled={cfg.profiles.length < 2} onClick={removeProfile} />
              </ToolbarGroup>
              <ToolbarButton small icon="⧉" title={T('terminal.prefs.duplicate')} onClick={() => addProfile(p)} />
            </View>
            <View style={{ height: 1, backgroundColor: t.separator, marginVertical: spacing.m, marginHorizontal: spacing.xs }} />
            {SECTIONS().map(([id, label]) => (
              <SidebarItem key={id} label={label} selected={id === section} onClick={() => setSection(id)} icon={SECTION_ICON[id]} />
            ))}
          </View>
        </ScrollView>
        <View style={{ padding: 10, gap: spacing.s }}>
          <Hairline edge="top" />
          <Button small plain label={T('terminal.prefs.importBtn')} onClick={() => setImporting(true)} />
        </View>
      </View>
      <View ref={pane} style={{ flex: 1, minWidth: 0 }}>
        <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: spacing.xl }}>
          <Hairline edge="bottom" />
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{sectionLabel}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{profileName(p)}</Text>
          </View>
          <Button primary label={T('terminal.prefs.done')} onClick={onClose} />
        </View>
        {notice && (
          <View style={{ marginHorizontal: spacing.xl, marginTop: spacing.m, paddingHorizontal: spacing.m, paddingVertical: spacing.s, borderRadius: 8, backgroundColor: 'rgba(50,215,75,0.16)' }}>
            <Text style={{ fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{notice}</Text>
          </View>
        )}
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: spacing.xl, paddingTop: spacing.l, paddingBottom: spacing.xl }}>
            <Narrow.Provider value={narrow}>{body[section]}</Narrow.Provider>
          </View>
        </ScrollView>
      </View>
      {importing && (
        <ImportPanel t={t} onClose={() => setImporting(false)}
          onDone={(msg, first) => { setImporting(false); setNotice(msg); if (first) setSel(cfg.profiles.find((x) => x.id === first) ? first : terminalSettings().profiles.find((x) => x.id === first)?.id || sel); }} />
      )}
    </View>
  );
}

export const closePrefs = (wid) => patchWin(wid, () => ({ prefs: false }));
export { hint };
