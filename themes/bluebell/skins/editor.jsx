// skins/editor.jsx — Bluebell's skin for the Text Editor, in the manner of a notepad of 2001 with a toolbar: a row of
// command buttons (New, Open, Save, Cut, Copy, Paste, Find), property-sheet tabs, a status bar of sunken panels and the
// era's context menu (docs/THEMES-GUIDE.md, "App skins → Text Editor"). The text stays the Text Editor's; the skin only
// asks for its commands.
import { View, Text, useTheme, useThemeFont, PackIcon, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { ContextMenu } from './files.jsx';
import { Folder, Page } from '../glyphs.jsx';

const fontOf = (font) => (font ? { fontFamily: font } : null);

function Cmd({ t, font, label, icon, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} accessibilityLabel={label}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 5, borderRadius: 3,
        border: `1px solid ${hover ? t.barHoverEdge : 'transparent'}`, background: hover ? t.barHover : 'transparent' }}>
      {icon}
      <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap', ...fontOf(font) }}>{label}</Text>
    </View>
  );
}
const Sep = ({ t }) => <View style={{ width: 1, height: 20, marginHorizontal: 3, backgroundColor: t.separator }} />;
// a small floppy-like square for Save: a blue body with a white label, our own drawing
const SaveMark = () => (
  <View style={{ width: 16, height: 16, borderRadius: 2, backgroundColor: '#2f63d8', border: '1px solid #1d3f9a', alignItems: 'center' }}>
    <View style={{ width: 8, height: 5, marginTop: 1, backgroundColor: '#c9d8f5' }} />
    <View style={{ width: 10, height: 5, marginTop: 2, backgroundColor: '#ffffff' }} />
  </View>
);

export function EditorToolbar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const label = (id) => (model.commands.find((c) => c.id === id) || {}).label || id;
  return (
    <View style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 1, paddingHorizontal: 4, background: t.toolbarBg, borderBottom: `1px solid ${t.separator}` }}>
      <Cmd t={t} font={font} label={label('new')} icon={<Page size={18} />} onClick={() => a.command('new')} />
      <Cmd t={t} font={font} label={label('open')} icon={<Folder size={18} />} onClick={() => a.command('open')} />
      <Cmd t={t} font={font} label={label('save')} icon={<SaveMark />} onClick={() => a.command('save')} />
      <Sep t={t} />
      <Cmd t={t} font={font} label={label('cut')} onClick={() => a.command('cut')} />
      <Cmd t={t} font={font} label={label('copy')} onClick={() => a.command('copy')} />
      <Cmd t={t} font={font} label={label('paste')} onClick={() => a.command('paste')} />
      <Sep t={t} />
      <Cmd t={t} font={font} label={label('find')} icon={<PackIcon group="toolbar" name="search" size={18} />} onClick={() => a.command('find')} />
    </View>
  );
}

function SheetTab({ t, font, tab, a }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseDown={() => a.selectTab(tab.key)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ maxWidth: 200, minWidth: 80, height: tab.active ? 24 : 21, marginTop: tab.active ? 0 : 3, flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: 8,
        borderTopLeftRadius: 3, borderTopRightRadius: 3, borderWidth: 1, borderBottomWidth: 0, borderColor: t.separator,
        borderTopColor: tab.active ? '#e8a33c' : t.separator, borderTopWidth: tab.active ? 2 : 1, backgroundColor: tab.active ? t.windowBg : hover ? t.paneBody : t.sidebarBg }}>
      <Page size={14} />
      <Text style={{ flexShrink: 1, fontSize: 12, fontWeight: tab.active ? 700 : 400, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', ...fontOf(font) }}>
        {tab.dirty ? `${tab.name} *` : tab.name}</Text>
      <View onMouseDown={(e) => e.stopPropagation()} onClick={() => a.closeTab(tab.key)} style={{ width: 14, height: 14, borderRadius: 2, alignItems: 'center', justifyContent: 'center',
        background: hover || tab.active ? t.closeBg : 'transparent' }}>
        <Text style={{ fontSize: 9, fontWeight: 700, color: hover || tab.active ? '#ffffff' : 'transparent' }}>✕</Text>
      </View>
    </View>
  );
}

export function EditorTabs({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  return (
    <View style={{ height: 28, flexDirection: 'row', alignItems: 'flex-end', gap: 2, paddingHorizontal: 6, backgroundColor: t.sidebarBg, borderBottom: `1px solid ${t.separator}` }}>
      {model.tabs.map((tab) => <SheetTab key={tab.key} t={t} font={font} tab={tab} a={model.actions} />)}
    </View>
  );
}

function Panel({ t, font, children, flex, color, onClick }) {
  return (
    <View onClick={onClick} style={{ ...(flex ? { flex: 1, minWidth: 0 } : null), height: 18, justifyContent: 'center', paddingHorizontal: 6,
      borderWidth: 1, borderTopColor: t.separator, borderLeftColor: t.separator, borderBottomColor: t.paneEdge, borderRightColor: t.paneEdge }}>
      <Text style={{ fontSize: 11, color: color || t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{children}</Text>
    </View>
  );
}

export function EditorStatus({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const st = model.status;
  return (
    <View style={{ height: 24, flexDirection: 'row', alignItems: 'center', gap: 3, paddingHorizontal: 3, backgroundColor: t.statusBg, borderTop: `1px solid ${t.separator}` }}>
      <Panel t={t} font={font} flex color={st.message ? (st.message.ok ? '#2f8a25' : '#c8302a') : undefined}>{st.message ? st.message.text : ''}</Panel>
      <Panel t={t} font={font}>{tr('editor.status.position', { line: st.line, col: st.col })}</Panel>
      <Panel t={t} font={font} onClick={() => model.actions.command('language')}>{st.language}</Panel>
      <Panel t={t} font={font}>{st.indent}</Panel>
      <Panel t={t} font={font}>{st.eol}</Panel>
    </View>
  );
}

export const editorSkin = { toolbar: EditorToolbar, tabBar: EditorTabs, statusBar: EditorStatus, contextMenu: ContextMenu };
