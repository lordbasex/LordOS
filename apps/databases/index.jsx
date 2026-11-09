// Databases — the MySQL and MariaDB client (docs/DATABASES.md). One window holds a tab per open connection and the
// Connect tab (favorites, Quick Connect, the Sequel Ace / Sequel Pro import); each connection is its own sandboxed
// lord-sql process. The same app opened with args.console is that connection's Console window.
import { View, Text, useAppWindow, useStore, wm, sqlPasswords, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Dot } from './ui.jsx';
import { store, listen, setActive, closeSession, openSession, sessionOf } from './conn.js';
import { colorOf } from './model.js';
import { dropView, patchView, viewOf } from './viewstate.js';

const viewTable = (s) => viewOf(s.key).table || '';
import { Connect, connectAsks } from './connect.jsx';
import { Workspace, openConsole } from './workspace.jsx';
import { ConsoleWindow } from './console.jsx';
import { errText } from './texts.js';

export function Icon({ size: s }) {
  const ring = (top, opacity) => (
    <View style={{ position: 'absolute', left: s * 0.22, top: s * top, width: s * 0.56, height: s * 0.18, borderRadius: s * 0.09, backgroundColor: '#ffffff', opacity }} />
  );
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #2f7cf6, #1858c9)' }}>
      {/* a stack of disks: the database */}
      <View style={{ position: 'absolute', left: s * 0.22, top: s * 0.24, width: s * 0.56, height: s * 0.54, borderRadius: s * 0.1, backgroundColor: 'rgba(255,255,255,0.28)' }} />
      {ring(0.2, 1)}
      {ring(0.42, 0.85)}
      {ring(0.64, 0.7)}
      <View style={{ position: 'absolute', left: s * 0.6, top: s * 0.6, width: s * 0.26, height: s * 0.26, borderRadius: s * 0.13, backgroundColor: '#ffd60a', alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: s * 0.16, fontWeight: 700, color: '#1858c9' }}>›</Text>
      </View>
    </View>
  );
}

export default function App() {
  const { args } = useAppWindow();
  if (args?.console) return <ConsoleWindow sessionKey={args.console} />;
  return <Main />;
}

function Main() {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const st = useStore(store);
  const [note, setNote] = useState('');
  useEffect(() => { listen(); }, []);
  // closing the window closes its connections
  useEffect(() => () => { for (const s of store.get().sessions) { closeSession(s.key); dropView(s.key); } }, []);

  const active = st.active ? sessionOf(st.active) : null;
  useEffect(() => {
    wm.setTitle(winId, active ? [active.fav.name, active.db, viewTable(active)].filter(Boolean).join(' — ') : null);
  });
  useEffect(() => {
    wm.setMenus(winId, [
      { label: tr('databases.menu.file'), items: [
        { label: tr('databases.menu.newConnection'), onClick: () => setActive(null) },
        { label: tr('databases.import.menu'), onClick: () => { setActive(null); setTimeout(() => connectAsks.importNow?.(), 60); } },
        { label: tr('databases.menu.closeConnection'), disabled: !active, onClick: () => active && close(active.key) },
      ] },
      { label: tr('databases.menu.view'), items: [
        ...['structure', 'content', 'info', 'query'].map((pane) => ({ label: tr(`databases.pane.${pane}`), disabled: !active, onClick: () => active && patchView(active.key, { pane }) })),
        { label: tr('databases.pane.console'), disabled: !active, onClick: () => active && openConsole(active) },
      ] },
    ]);
  }, [active?.key]);

  const close = (key) => { closeSession(key); dropView(key); };
  const reconnect = async (s) => {
    try {
      const fresh = await openSession(s.fav, await sqlPasswords.get(s.fav));
      close(s.key);
      setActive(fresh.key);
      setNote('');
    } catch (e) { setNote(errText(e)); }
  };

  const tabs = (
    <View style={{ height: 32, flexDirection: 'row', alignItems: 'stretch', backgroundColor: p.tabs }}>
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
      <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
      {!active ? <Tab on label={tr('databases.newConnection')} icon={<Text style={{ fontSize: 14, color: p.text }}>+</Text>} /> : null}
      {st.sessions.map((s) => (
        <Tab key={s.key} on={st.active === s.key} label={s.fav.name} icon={<Dot color={colorOf(s.fav) || p.dim} />} closed={!!s.closed}
          busy={s.busy} onClick={() => setActive(s.key)} onClose={() => close(s.key)} />
      ))}
      {active ? (
        <View onClick={() => setActive(null)} title={tr('databases.newConnection')} style={{ paddingHorizontal: 10, justifyContent: 'center' }}>
          <Text style={{ fontSize: 15, color: p.dim }}>+</Text>
        </View>
      ) : null}
    </View>
  );
  const banner = note ? <View style={{ paddingHorizontal: 12, paddingVertical: 6, backgroundColor: p.badBg }}><Text style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{note}</Text></View> : null;

  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      {active ? <Workspace key={active.key} s={active} tabs={<>{tabs}{banner}</>} onReconnect={() => reconnect(active)} /> : (
        <>
          {tabs}
          {banner}
          <Connect onConnected={() => setNote('')} />
        </>
      )}
    </View>
  );
}

function Tab({ on, label, icon, onClick, onClose, closed, busy }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 16, maxWidth: 220, backgroundColor: on ? p.bg : 'transparent' }}>
      <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: p.line }} />
      {icon}
      <Text style={{ fontSize: 12, color: on ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textDecoration: closed ? 'line-through' : 'none' }}>{label}</Text>
      {busy ? <Text style={{ fontSize: 10, color: p.dim }}>●</Text> : null}
      {onClose ? (
        <View onClick={(e) => { e.stopPropagation?.(); onClose(); }} title={tr('databases.menu.closeConnection')}
          style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center', opacity: hover || on ? 1 : 0 }}>
          <Text style={{ fontSize: 12, color: p.dim }}>×</Text>
        </View>
      ) : null}
    </View>
  );
}

