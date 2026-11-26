// skins/terminal.jsx — Luna's skin for the Terminal: a row of command buttons and property-sheet tabs in the
// manner of 2001, a status bar of sunken panels, and the era's context menu (docs/THEMES-GUIDE.md, "App skins →
// Terminal"). The panes stay the Terminal's; the skin only asks for the Terminal's own commands.
import { View, Text, useTheme, useThemeFont, PackIcon, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { ContextMenu } from './files.jsx';

const fontOf = (font) => (font ? { fontFamily: font } : null);

function Cmd({ t, font, label, icon, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 6, borderRadius: 3,
        border: `1px solid ${hover ? t.barHoverEdge : 'transparent'}`, background: hover ? t.barHover : 'transparent' }}>
      {icon}
      <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap', ...fontOf(font) }}>{label}</Text>
    </View>
  );
}

function SheetTab({ t, font, tab, a }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseDown={() => a.selectTab(tab.key)} onContextMenu={(ev) => a.tabMenu(tab.key, ev)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ maxWidth: 200, minWidth: 80, height: tab.active ? 24 : 21, marginTop: tab.active ? 0 : 3, flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: 8,
        borderTopLeftRadius: 3, borderTopRightRadius: 3, borderWidth: 1, borderBottomWidth: 0, borderColor: t.separator,
        borderTopColor: tab.active ? '#e8a33c' : t.separator, borderTopWidth: tab.active ? 2 : 1,
        backgroundColor: tab.active ? t.windowBg : hover ? t.paneBody : t.sidebarBg }}>
      {tab.color ? <View style={{ width: 7, height: 7, borderRadius: 2, backgroundColor: tab.color }} /> : null}
      {tab.bell ? <Text style={{ fontSize: 10, color: '#e0a020' }}>●</Text> : null}
      <Text style={{ flexShrink: 1, fontSize: 12, fontWeight: tab.active ? 700 : 400, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', ...fontOf(font) }}>{tab.title}</Text>
      <View onMouseDown={(e) => e.stopPropagation()} onClick={() => a.closeTab(tab.key)} style={{ width: 14, height: 14, borderRadius: 2, alignItems: 'center', justifyContent: 'center',
        background: hover || tab.active ? t.closeBg : 'transparent' }}>
        <Text style={{ fontSize: 9, fontWeight: 700, color: hover || tab.active ? '#ffffff' : 'transparent' }}>✕</Text>
      </View>
    </View>
  );
}

export function TerminalTabs({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const label = (id) => (model.commands.find((c) => c.id === id) || {}).label || id;
  return (
    <View style={{ background: t.toolbarBg }}>
      <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 4 }}>
        <Cmd t={t} font={font} label={label('newTab')} icon={<Text style={{ fontSize: 14, fontWeight: 700, color: '#2f8a25' }}>+</Text>} onClick={() => a.command('newTab')} />
        <View style={{ width: 1, height: 20, marginHorizontal: 3, backgroundColor: t.separator }} />
        <Cmd t={t} font={font} label={label('copy')} onClick={() => a.command('copy')} />
        <Cmd t={t} font={font} label={label('paste')} onClick={() => a.command('paste')} />
        <View style={{ width: 1, height: 20, marginHorizontal: 3, backgroundColor: t.separator }} />
        <Cmd t={t} font={font} label={label('find')} icon={<PackIcon group="toolbar" name="search" size={18} />} onClick={() => a.command('find')} />
        <View style={{ flex: 1 }} />
        {model.broadcast ? <Text style={{ fontSize: 11, fontWeight: 700, color: '#c8302a', ...fontOf(font) }}>{tr('terminal.broadcasting')}</Text> : null}
      </View>
      <View style={{ height: 26, flexDirection: 'row', alignItems: 'flex-end', gap: 2, paddingHorizontal: 6, borderBottom: `1px solid ${t.separator}` }}>
        {model.tabs.map((tab) => <SheetTab key={tab.key} t={t} font={font} tab={tab} a={a} />)}
      </View>
    </View>
  );
}

function Panel({ t, font, children, flex }) {
  return (
    <View style={{ ...(flex ? { flex: 1, minWidth: 0 } : null), height: 18, justifyContent: 'center', paddingHorizontal: 6,
      borderWidth: 1, borderTopColor: t.separator, borderLeftColor: t.separator, borderBottomColor: t.paneEdge, borderRightColor: t.paneEdge }}>
      <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{children}</Text>
    </View>
  );
}

export function TerminalStatus({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const st = model.status;
  if (!st) return null;
  return (
    <View style={{ height: 24, flexDirection: 'row', alignItems: 'center', gap: 3, paddingHorizontal: 3, backgroundColor: t.statusBg, borderTop: `1px solid ${t.separator}` }}>
      <Panel t={t} font={font} flex>{st.cwd || '~'}</Panel>
      <Panel t={t} font={font}>{st.busy ? st.process : st.shell || 'shell'}</Panel>
      <Panel t={t} font={font}>{`${st.cols}×${st.rows}`}</Panel>
    </View>
  );
}

export const terminalSkin = { tabBar: TerminalTabs, statusBar: TerminalStatus, contextMenu: ContextMenu };
