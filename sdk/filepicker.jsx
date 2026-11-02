// filepicker.jsx — a small "choose a file" sheet for apps that run in the theme's context
// (Mail attachments, a contact's photo, importing .ics/.vcf). Isolated .lpk apps use the
// system powerbox (fs.openFile) instead. Only the home folder and the user's mounts are listed
// (lord.fs enforces that anyway).
//
//   <FilePicker onPick={(path) => …} onClose={() => …} filter={/\.(png|jpe?g)$/i} title={t('…')} multiple />
import { useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { Button, ScrollView, TextInput } from './components.jsx';
import { createStore, useStore } from './store.js';
import * as fs from './fs.js';
import { formatBytes } from './system.js';

// mode: 'open' (the default), 'folder' (the folder in view) or 'save' (a name, with a question before replacing a file).
export function FilePicker({ onPick, onClose, filter = null, title = '', multiple = false, start = '', mode = 'open', name = '', accept = '' }) {
  const th = useTheme();
  const [dir, setDir] = useState(start);
  const [chosen, setChosen] = useState([]);
  const [saveName, setSaveName] = useState(name);
  const [replacing, setReplacing] = useState(false);
  let entries = [];
  try { entries = fs.list(dir || '.').filter((e) => !e.name.startsWith('.') && (e.dir || (mode !== 'folder' && (!filter || filter.test(e.name))))); } catch (e) { entries = []; }
  entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
  const abs = (name) => `${fs.home()}/${dir ? `${dir}/` : ''}${name}`;
  const pick = (e) => {
    if (e.dir) { setDir(dir ? `${dir}/${e.name}` : e.name); setReplacing(false); return; }
    if (mode === 'save') { setSaveName(e.name); setReplacing(false); return; }
    if (!multiple) { onPick(abs(e.name)); return; }
    const p = abs(e.name);
    setChosen((c) => (c.includes(p) ? c.filter((x) => x !== p) : [...c, p]));
  };
  const validName = saveName.trim() && !saveName.includes('/') && saveName !== '.' && saveName !== '..';
  const save = () => {
    if (!validName) return;
    if (!replacing && entries.some((e) => !e.dir && e.name === saveName)) { setReplacing(true); return; }
    onPick(abs(saveName));
  };
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 30, top: 40, right: 30, bottom: 30, padding: 12, gap: 8, borderRadius: 12,
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Button label="‹" onClick={() => setDir(dir.split('/').slice(0, -1).join('/'))} />
        <View style={{ flex: 1 }}>
          {title ? <Text style={{ fontSize: 13, fontWeight: 600, color: th.text }}>{title}</Text> : null}
          <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{`~/${dir}`}</Text>
        </View>
        <Button label={t('shell.picker.cancel')} onClick={onClose} />
        {multiple && mode === 'open' && <Button primary label={t('shell.picker.choose', { n: chosen.length })} onClick={() => chosen.length && onPick(chosen)} />}
        {mode === 'folder' && <Button primary label={accept || t('shell.picker.chooseFolder')} onClick={() => onPick(dir ? `${fs.home()}/${dir}` : fs.home())} />}
        {mode === 'save' && <Button primary label={replacing ? t('shell.picker.replace') : accept || t('shell.picker.save')} onClick={save} />}
      </View>
      <ScrollView style={{ flex: 1 }}>
        {entries.map((e) => {
          const on = chosen.includes(abs(e.name));
          return (
            <View key={e.name} onClick={() => pick(e)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 5, paddingHorizontal: 6, borderRadius: 6,
              backgroundColor: on ? th.accent : 'transparent' }}>
              <Text style={{ fontSize: 15 }}>{e.dir ? '📁' : '📄'}</Text>
              <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{e.name}</Text>
              {!e.dir && <Text style={{ fontSize: 11, color: on ? '#fff' : th.textDim }}>{formatBytes(e.size)}</Text>}
            </View>
          );
        })}
        {!entries.length && <Text style={{ fontSize: 12, color: th.textDim, margin: 8 }}>{t('shell.picker.empty')}</Text>}
      </ScrollView>
      {mode === 'save' && <TextInput value={saveName} placeholder={t('shell.picker.fileName')} autoFocus selectAll
        onChange={(v) => { setSaveName(v); setReplacing(false); }} onSubmit={save} />}
      {replacing && <Text style={{ fontSize: 12, color: th.textDim }}>{t('shell.picker.exists')}</Text>}
    </View>
  );
}

// ---- A Linux app asks the desktop for a file (org.freedesktop.portal.FileChooser, server/src/ui/portal.c) ----
// Inside the home folder, like every other picker here. The app gets only what the person picks.
export const portalFile = createStore(null);

const globPart = (c) => (c === '*' ? '.*' : c === '?' ? '.' : c.replace(/[.+^${}()|[\]\\]/g, '\\$&'));
const portalFilter = (patterns) => {
  if (!patterns?.length || patterns.includes('*')) return null;
  return new RegExp(`^(${patterns.map((p) => [...p].map(globPart).join('')).join('|')})$`, 'i');
};
const insideHome = (folder) => {
  const home = fs.home();
  return folder.startsWith(`${home}/`) ? folder.slice(home.length + 1).replace(/\/+$/, '') : '';
};

export function PortalFilePrompt() {
  const p = useStore(portalFile);
  if (!p) return null;
  const answer = (paths) => lord.portal.chosen(p.id, paths);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
      <View style={{ position: 'relative', width: 640, height: 500 }}>
        <FilePicker key={p.id} mode={p.mode} multiple={p.multiple} title={p.title} name={p.name} accept={p.accept} start={insideHome(p.folder)}
          filter={portalFilter(p.patterns)} onClose={() => answer(null)} onPick={(x) => answer(Array.isArray(x) ? x : [x])} />
      </View>
    </View>
  );
}
