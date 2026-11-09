// ProfilesWindow.jsx — the Profiles window (❖O, terminal#profiles), as iTerm2's: a search by name or tag, the profiles
// grouped by tag, and ONE CLICK OPENS the profile (no Enter, no double click) in a new tab, a new window or a split of
// the last terminal window used. The arrows move the selection and Enter opens it. "Close this window after opening a
// profile" and where it opens are remembered (terminal.json: profilesWindow).
import { View, Text, SearchField, Segmented, ScrollView, Button, Hairline, useTheme, useAppWindow, useKeyboard, wm, spacing, typeScale, direction, t as T } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { terminalSettings, saveTerminalSettings, profileGroups, profileName } from './profiles.js';
import { addTab, split, openWindowWithProfile, lastTermWin } from './state.js';

const TARGETS = () => [['tab', T('terminal.profilesWin.tab')], ['window', T('terminal.profilesWin.window')],
  ['right', T('terminal.profilesWin.splitRight')], ['down', T('terminal.profilesWin.splitDown')]];

// what a profile runs, in one line: its text sent at start (an ssh line, most of the time) or the login shell
export const commandLine = (p) => {
  const s = String(p.initialText || '').replace(/\\n|\n/g, ' ').trim();
  return s || T('terminal.profilesWin.loginShell');
};

// opens the profile where chosen; with no terminal window left, always in a new one
export function openProfile(id, target) {
  const wid = lastTermWin();
  if (!wid || target === 'window') { openWindowWithProfile(id); return; }
  if (target === 'right' || target === 'down') split(wid, target === 'right' ? 'row' : 'column', id);
  else addTab(wid, id);
  wm.focus(wid);
}

export function ProfilesWindow({ onEdit }) {
  const t = useTheme();
  const { id: wid } = useAppWindow();
  const cfg = terminalSettings();
  const prefs = { closeAfter: true, target: 'tab', ...(cfg.profilesWindow || {}) };
  const [query, setQuery] = useState('');
  const [closed, setClosed] = useState({});
  const [sel, setSel] = useState(0);
  useEffect(() => { wm.setTitle(wid, T('terminal.profilesWin.title')); }, []);
  const setPrefs = (patch) => saveTerminalSettings({ profilesWindow: { ...prefs, ...patch } });

  const groups = profileGroups(cfg.profiles, query);
  // the rows as shown (folded groups left out), for the arrows
  const flat = groups.flatMap((g) => (g.tag !== null && !query && closed[g.tag] ? [] : g.items.map((p) => ({ p, tag: g.tag }))));
  const cur = Math.min(sel, Math.max(0, flat.length - 1));
  const open = (p) => {
    openProfile(p.id, prefs.target);
    if (prefs.closeAfter) wm.close(wid);
  };
  useKeyboard((e) => {
    if (e.key === 'Escape') wm.close(wid);
    else if (e.key === 'Enter' && flat[cur]) open(flat[cur].p);
    else if (e.key === 'ArrowDown') setSel(Math.min(flat.length - 1, cur + 1));
    else if (e.key === 'ArrowUp') setSel(Math.max(0, cur - 1));
  });

  const rows = [];
  let i = 0;
  for (const g of groups) {
    const shut = g.tag !== null && !query && !!closed[g.tag];
    if (g.tag !== null) {
      rows.push(
        <View key={`g:${g.tag}`} onClick={() => setClosed((c) => ({ ...c, [g.tag]: !shut }))} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 26, paddingHorizontal: 8, marginTop: 4 }}>
          <Text style={{ width: 10, fontSize: 9, color: t.textDim }}>{shut ? (direction() === 'rtl' ? '◀' : '▶') : '▼'}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: 11, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{g.tag.toUpperCase()}</Text>
          <Text style={{ fontSize: 11, color: t.textDim }}>{g.items.length}</Text>
        </View>,
      );
    }
    if (shut) continue;
    for (const p of g.items) {
      const k = i++;
      const on = k === cur;
      rows.push(
        <View key={`${g.tag ?? ''}:${p.id}`} onClick={() => open(p)} onMouseEnter={() => setSel(k)}
          style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 34, paddingHorizontal: 8, paddingLeft: g.tag !== null ? 24 : 8, borderRadius: 6, backgroundColor: on ? t.accent : 'transparent' }}>
          <Text style={{ width: 14, fontSize: 12, color: on ? '#fff' : '#ffd60a', textAlign: 'center' }}>{p.id === cfg.defaultProfile ? '★' : ''}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: on ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{profileName(p)}</Text>
          <Text style={{ width: '40%', fontFamily: 'monospace', fontSize: 12, color: on ? 'rgba(255,255,255,0.85)' : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', direction: 'ltr' }}>{commandLine(p)}</Text>
        </View>,
      );
    }
  }

  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: t.windowBg }}>
      <View style={{ padding: spacing.m }}>
        <SearchField value={query} autoFocus onChange={(v) => { setQuery(v); setSel(0); }} placeholder={T('terminal.prefs.search')}
          onSubmit={() => flat[cur] && open(flat[cur].p)} onCancel={() => { if (!query) wm.close(wid); }}
          onKeyDown={(e) => {
            if (e.key === 'ArrowDown') { setSel(Math.min(flat.length - 1, cur + 1)); return true; }
            if (e.key === 'ArrowUp') { setSel(Math.max(0, cur - 1)); return true; }
            return false;
          }} />
      </View>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: spacing.s, paddingBottom: spacing.s }}>
          {rows.length ? rows : <Text style={{ padding: spacing.m, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{T('terminal.prefs.noMatch')}</Text>}
        </View>
      </ScrollView>
      <View style={{ gap: spacing.s, padding: spacing.m }}>
        <Hairline edge="top" />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap' }}>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{T('terminal.profilesWin.opensIn')}</Text>
          <Segmented small options={TARGETS()} value={prefs.target} onChange={(v) => setPrefs({ target: v })} />
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <View onClick={() => setPrefs({ closeAfter: !prefs.closeAfter })} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, flex: 1, minWidth: 0 }}>
            <View style={{ width: 14, height: 14, borderRadius: 3, alignItems: 'center', justifyContent: 'center', backgroundColor: prefs.closeAfter ? t.accent : t.control }}>
              {prefs.closeAfter ? <Text style={{ fontSize: 10, color: '#fff', fontWeight: 700 }}>✓</Text> : null}
            </View>
            <Text style={{ fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{T('terminal.profilesWin.closeAfter')}</Text>
          </View>
          <Button small label={T('terminal.profilesWin.edit')} onClick={onEdit} />
        </View>
      </View>
    </View>
  );
}
