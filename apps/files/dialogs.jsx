// dialogs.jsx — Go to Folder, Connect to Server and Files → Settings: sheets inside the window.
import { View, Text, Button, TextInput, ScrollView, Toggle, Segmented, useTheme, fs, addListener, spacing, typeScale, t as tr } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { TAG_COLORS } from './glyphs.jsx';
import { SIDEBAR_ITEMS, setPrefs, useFilesPrefs, toggleFavorite } from './prefs.js';
import { SCHEMES, SUGGESTIONS } from './connect.js';
import { userDirs } from './locations.js';

function Sheet({ t, width = 460, children }) {
  return (
    <View onMouseDown={(e) => e.stopPropagation()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width, maxHeight: '92%', padding: spacing.xl, gap: spacing.s, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        {children}
      </View>
    </View>
  );
}
const Title = ({ t, children }) => <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{children}</Text>;
const Dim = ({ t, children }) => <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{children}</Text>;
const Actions = ({ children }) => <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>{children}</View>;

// ---- Go to Folder: Tab completes, the arrows pick ----

const expand = (v) => { const h = fs.home(); const p = v.trim().replace(/^~(?=\/|$)/, h); return p.startsWith('/') ? p : `${h}/${p}`; };
function completions(value) {
  const p = expand(value);
  const cut = p.lastIndexOf('/');
  const dir = p.slice(0, cut) || '/', head = p.slice(cut + 1).toLowerCase();
  try {
    return lord.fs.list(dir).filter((e) => e.dir && (head.startsWith('.') || !e.name.startsWith('.')) && e.name.toLowerCase().startsWith(head)).map((e) => `${dir === '/' ? '' : dir}/${e.name}`).sort((a, b) => a.localeCompare(b)).slice(0, 8);
  } catch (e) { return []; }
}

export function GoToFolder({ start, onGo, onClose }) {
  const t = useTheme();
  const [value, setValue] = useState(start || '');
  const [sel, setSel] = useState(0);
  const [error, setError] = useState('');
  const list = useMemo(() => (value.trim() ? completions(value) : []), [value]);
  const go = () => {
    const path = expand(list[sel] && !value.endsWith('/') ? list[sel] : value).replace(/\/+$/, '') || '/';
    try { if (fs.stat(path).dir) { onGo(path); return; } } catch (e) { /* below */ }
    setError(tr('files.goto.notFound', { path }));
  };
  return (
    <Sheet t={t}>
      <Title t={t}>{tr('files.menu.goToFolder')}</Title>
      <TextInput autoFocus selectAll value={value} onChange={(v) => { setValue(v); setSel(0); setError(''); }} placeholder="~/Documents" onSubmit={go} onCancel={onClose}
        onKeyDown={(e) => {
          if (e.key === 'Tab' && list[sel]) { setValue(`${list[sel]}/`); setSel(0); return true; }
          if (e.key === 'ArrowDown') { setSel((s) => Math.min(list.length - 1, s + 1)); return true; }
          if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
          return false;
        }} />
      {list.map((p, i) => (
        <View key={p} onClick={() => setValue(`${p}/`)} style={{ height: 24, paddingHorizontal: 8, borderRadius: 5, justifyContent: 'center', backgroundColor: i === sel ? t.accent : 'transparent' }}>
          <Text style={{ fontSize: typeScale.callout, color: i === sel ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{p}</Text>
        </View>
      ))}
      {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
      <Actions><Button label={tr('files.cancel')} onClick={onClose} /><Button primary label={tr('files.goto.go')} onClick={go} disabled={!value.trim()} /></Actions>
    </Sheet>
  );
}

// ---- Connect to Server ----

export function ConnectServer({ onConnect, onBrowse, onClose }) {
  const t = useTheme();
  const p = useFilesPrefs();
  const [value, setValue] = useState(p.servers.history[0] || '');
  const [menu, setMenu] = useState(false);
  const [help, setHelp] = useState(false);
  const [result, setResult] = useState(null);             // { error, store }
  const [sel, setSel] = useState('');
  const go = (v = value) => { const r = onConnect(v.trim()); if (r) setResult(r); };
  const favs = p.servers.favorites;
  const row = (text, on, click, dbl) => (
    <View key={text} onClick={click} onDoubleClick={dbl} style={{ minHeight: 24, paddingHorizontal: 8, borderRadius: 5, justifyContent: 'center', backgroundColor: on ? t.accent : 'transparent' }}>
      <Text style={{ fontSize: typeScale.callout, color: on ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{text}</Text>
    </View>
  );
  return (
    <Sheet t={t} width={500}>
      <Title t={t}>{tr('files.menu.connect')}</Title>
      <View style={{ flexDirection: 'row', gap: spacing.xs, alignItems: 'center' }}>
        <View style={{ flex: 1 }}><TextInput autoFocus selectAll value={value} onChange={(v) => { setValue(v); setResult(null); }} placeholder="smb://192.168.1.10" onSubmit={() => go()} onCancel={onClose} /></View>
        <Button small label="⌄" onClick={() => setMenu((m) => !m)} />
      </View>
      {menu ? (
        <View style={{ gap: 2, padding: 4, borderRadius: 8, border: `1px solid ${t.separator}` }}>
          {p.servers.history.length ? <Dim t={t}>{tr('files.connect.recent')}</Dim> : null}
          {p.servers.history.map((h) => row(h, false, () => { setValue(h); setMenu(false); }))}
          <Dim t={t}>{tr('files.connect.services')}</Dim>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
            {SUGGESTIONS.map((s) => <Button key={s} small label={s} onClick={() => { setValue(s); setMenu(false); }} />)}
          </View>
        </View>
      ) : null}
      <Dim t={t}>{tr('files.connect.favorites')}</Dim>
      <View style={{ minHeight: 70, maxHeight: 130, padding: 4, borderRadius: 8, border: `1px solid ${t.separator}`, overflow: 'hidden' }}>
        <ScrollView>{favs.map((f) => row(f, sel === f, () => { setSel(f); setValue(f); }, () => go(f)))}</ScrollView>
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.xs, alignItems: 'center' }}>
        <Button small label="+" onClick={() => { if (value.trim() && !favs.includes(value.trim())) toggleFavorite(value.trim()); }} disabled={!value.trim()} />
        <Button small label="−" onClick={() => { toggleFavorite(sel); setSel(''); }} disabled={!sel} />
        <Button small label="?" onClick={() => setHelp((h) => !h)} />
      </View>
      {help ? (
        <ScrollView style={{ maxHeight: 180 }}>
          <Dim t={t}>{tr('files.connect.helpIntro')}</Dim>
          {SCHEMES.map(([id, form]) => (
            <View key={form} style={{ flexDirection: 'row', gap: spacing.s, paddingVertical: 2 }}>
              <Text style={{ width: 190, fontFamily: 'monospace', fontSize: typeScale.caption, color: t.text, textAlign: 'start' }}>{form}</Text>
              <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`files.connect.help.${id}`)}</Text>
            </View>
          ))}
        </ScrollView>
      ) : null}
      {result ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{result.error}</Text> : null}
      <Actions>
        <Button label={tr('files.connect.browse')} onClick={onBrowse} />
        {result?.store ? <Button label={tr('files.connect.openStore')} onClick={result.store} /> : null}
        <Button label={tr('files.cancel')} onClick={onClose} />
        <Button primary label={tr('files.net.connect')} onClick={() => go()} disabled={!value.trim()} />
      </Actions>
    </Sheet>
  );
}

// ---- Files → Settings ----

function Line({ t, label, on, onChange, hint }) {
  return (
    <View style={{ gap: 2, paddingVertical: 4 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
        <Toggle small on={on} onChange={onChange} />
      </View>
      {hint ? <Dim t={t}>{hint}</Dim> : null}
    </View>
  );
}

export function FilesSettings({ onClose }) {
  const t = useTheme();
  const p = useFilesPrefs();
  const [tab, setTab] = useState('general');
  const dirs = useMemo(() => userDirs(), []);
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); }), []);
  const flip = (key, id) => setPrefs({ [key]: p[key].includes(id) ? p[key].filter((x) => x !== id) : [...p[key], id] });
  const placeName = (id) => tr(id === 'volumes' ? 'files.settings.volumes' : id === 'tags' ? 'files.tags' : `files.place.${id}`);
  const starts = [['home', tr('files.place.home')], ['recent', tr('files.place.recent')], ['computer', tr('files.place.computer')], ...dirs.map((d) => [d.label, tr(`files.place.${d.label}`)])];
  return (
    <Sheet t={t} width={480}>
      <Title t={t}>{tr('files.settings.title')}</Title>
      <Segmented small value={tab} onChange={setTab} options={['general', 'tags', 'sidebar', 'advanced'].map((k) => [k, tr(`files.settings.${k}`)])} />
      <ScrollView style={{ height: 330 }}>
        {tab === 'general' && (
          <View style={{ gap: spacing.xs, paddingVertical: spacing.s }}>
            <Dim t={t}>{tr('files.settings.newWindow')}</Dim>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
              {starts.map(([k, l]) => <Button key={k} small primary={p.newWindow === k} label={l} onClick={() => setPrefs({ newWindow: k })} />)}
            </View>
            <Line t={t} label={tr('files.settings.tabs')} on={p.foldersInTabs} onChange={(v) => setPrefs({ foldersInTabs: v })} />
            <Dim t={t}>{tr('files.settings.defaultView')}</Dim>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
              {['icons', 'list', 'columns', 'gallery'].map((v) => <Button key={v} small primary={(p.defaultView?.view || 'icons') === v} label={tr(`files.view.${v}`)} onClick={() => setPrefs({ defaultView: { view: v, sort: p.defaultView?.sort || { by: 'name', desc: false } } })} />)}
            </View>
          </View>
        )}
        {tab === 'tags' && (
          <View style={{ gap: spacing.xs, paddingVertical: spacing.s }}>
            <Dim t={t}>{tr('files.settings.tagsHint')}</Dim>
            {Object.entries(TAG_COLORS).map(([color, hex]) => (
              <View key={color} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
                <View style={{ width: 12, height: 12, borderRadius: 6, backgroundColor: hex }} />
                <View style={{ flex: 1 }}><TextInput value={p.tagNames[color] ?? ''} placeholder={tr(`files.tag.${color}`)} onChange={(v) => setPrefs({ tagNames: { ...p.tagNames, [color]: v.slice(0, 40) } })} /></View>
                <Toggle small on={!p.hiddenTags.includes(color)} onChange={() => flip('hiddenTags', color)} />
              </View>
            ))}
          </View>
        )}
        {tab === 'sidebar' && (
          <View style={{ paddingVertical: spacing.s }}>
            <Dim t={t}>{tr('files.settings.sidebarHint')}</Dim>
            {SIDEBAR_ITEMS.map((id) => <Line key={id} t={t} label={placeName(id)} on={!p.hiddenSide.includes(id)} onChange={() => flip('hiddenSide', id)} />)}
          </View>
        )}
        {tab === 'advanced' && (
          <View style={{ paddingVertical: spacing.s }}>
            <Line t={t} label={tr('files.settings.extensions')} on={p.showExtensions} onChange={(v) => setPrefs({ showExtensions: v })} />
            <Line t={t} label={tr('files.settings.warnExt')} on={p.warnExtension} onChange={(v) => setPrefs({ warnExtension: v })} />
            <Line t={t} label={tr('files.settings.warnTrash')} on={p.warnEmptyTrash} onChange={(v) => setPrefs({ warnEmptyTrash: v })} />
            <Line t={t} label={tr('files.settings.trash30')} hint={tr('files.settings.trash30Hint')} on={p.trash30} onChange={(v) => setPrefs({ trash30: v })} />
            <Line t={t} label={tr('files.settings.foldersFirst')} on={p.foldersFirst} onChange={(v) => setPrefs({ foldersFirst: v })} />
            <Dim t={t}>{tr('files.settings.searchIn')}</Dim>
            <Segmented small value={p.searchScope} onChange={(v) => setPrefs({ searchScope: v })} options={[['home', tr('files.place.home')], ['here', tr('files.settings.scopeHere')], ['last', tr('files.settings.scopeLast')]]} />
          </View>
        )}
      </ScrollView>
      <Actions><Button primary label={tr('files.close')} onClick={onClose} /></Actions>
    </Sheet>
  );
}
