// ClaudeBar.jsx — connecting Claude Code to the Terminal's footer (claude.js): it puts lord-claude-status in
// front of the user's status line command in ~/.claude/settings.json, after asking, so the footer gets the
// context and the plan's limits; what Claude Code's own status line shows doesn't change. The file is changed
// only when it reads as JSON, everything else in it is kept, and the previous one is kept as settings.json.lordos-backup.
// When it is already connected, the same bar offers to disconnect (the original command back).
import { View, Text, useTheme, t as T } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { patchMeta, toast } from './state.js';
import { settingsFile, parseSettings, isConnected, connectSettings, disconnectSettings } from './claude.js';

export function ClaudeBar({ sid, wid }) {
  const th = useTheme();
  const [s, setS] = useState(undefined);              // the settings, null when they can't be read as JSON
  const home = lord.fs.home();
  const file = settingsFile(home);
  useEffect(() => {
    let text = '';
    try { text = lord.fs.read(file); } catch (e) { text = ''; }
    setS({ text, obj: parseSettings(text) });
  }, []);
  const close = () => patchMeta(sid, { claudeConnect: false });
  if (s === undefined) return null;
  const connected = !!s.obj && isConnected(s.obj);
  const apply = () => {
    const next = connected ? disconnectSettings(s.obj) : connectSettings(s.obj);
    try {
      lord.fs.mkdir(`${home}/.claude`);
    } catch (e) { /* already there */ }
    try {
      if (s.text) lord.fs.write(`${file}.lordos-backup`, s.text);
      lord.fs.write(file, `${JSON.stringify(next, null, 2)}\n`);
      toast(wid, T(connected ? 'terminal.claude.disconnected' : 'terminal.claude.connected'));
    } catch (e) { toast(wid, T('terminal.claude.writeFailed')); }
    close();
  };
  const btn = (label, onClick, primary) => (
    <View onClick={onClick} style={{ paddingHorizontal: 10, height: 24, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
      backgroundColor: primary ? th.accent : th.control }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: primary ? '#fff' : th.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
  const ask = !s.obj ? 'terminal.claude.unreadable' : connected ? 'terminal.claude.askDisconnect' : 'terminal.claude.ask';
  return (
    <View style={{ position: 'absolute', left: 10, right: 10, bottom: 10, padding: 8, gap: 6, borderRadius: 10, backgroundColor: th.windowBg,
      border: `1px solid ${th.separator}`, boxShadow: '0 8px 24px rgba(0,0,0,0.45)' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ fontSize: 14, color: '#d97757' }}>✳</Text>
        <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.text, textAlign: 'start' }}>{T(ask)}</Text>
        {s.obj && btn(T(connected ? 'terminal.claude.disconnect' : 'terminal.claude.connectButton'), apply, true)}
        {btn(T(s.obj ? 'terminal.save.notNow' : 'terminal.claude.close'), close)}
      </View>
      {s.obj && <Text style={{ fontSize: 12, color: th.textDim, textAlign: 'start' }}>{T(connected ? 'terminal.claude.detailDisconnect' : 'terminal.claude.detail', { file: '~/.claude/settings.json' })}</Text>}
    </View>
  );
}
