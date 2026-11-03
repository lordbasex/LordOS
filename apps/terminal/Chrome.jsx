// Chrome.jsx — everything around the panes: tabs, status bar, find bar, toolbelt, popups, dialogs
// and toasts.
import {
  View, Text, TextInput, Button, Slider, SearchField, ToolbarButton, Hairline, useStore, useTheme, term, clipboard, clipboardHistory, useClock,
  t as tr, formatTime,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import {
  meta, patchWin, getWin, selectTab, closeTab, addTab, tabTitle, sessionTitle, leaves, recentDirs, notes,
  focusPane, wins, moveTab, commandHistory, captured,
} from './state.js';
import { terminalSettings, getProfile, profileName } from './profiles.js';

const small = (t, extra) => ({ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', ...extra });

// ---- tabs ----

function Tab({ t, wid, tab, active, index, maxChars }) {
  const [hover, setHover] = useState(false);
  const m = useStore(meta, (x) => x[tab.focus]) || {};
  const anyBell = useStore(meta, (x) => leaves(tab.root).some((s) => x[s]?.bell));
  const anyActivity = useStore(meta, (x) => leaves(tab.root).some((s) => x[s]?.activity));
  const title = tabTitle(tab);
  const color = getProfile(m.profile).tabColor;
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onMouseDown={() => selectTab(wid, tab.id)}
      onMouseUp={(e) => { if (e.button === 1) closeTab(wid, tab.id); }}
      style={{ flex: 1, maxWidth: 260, minWidth: 90, height: 26, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 6,
        borderRadius: 6, backgroundColor: active ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : '#ffffff') : hover ? t.hover : 'transparent',
        boxShadow: active && t.mode !== 'dark' ? '0 1px 2px rgba(0,0,0,0.12)' : 'none' }}>
      <View onClick={(e) => { e.stopPropagation(); closeTab(wid, tab.id); }} style={{ width: 16, height: 16, borderRadius: 4,
        alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? t.hover : 'transparent' }}>
        <Text style={{ fontSize: 13, color: hover ? t.textDim : 'transparent' }}>✕</Text>
      </View>
      {anyBell && <Text style={{ fontSize: 11, color: '#ffd60a' }}>●</Text>}
      {!anyBell && anyActivity && !active && <Text style={{ fontSize: 9, color: t.accent }}>●</Text>}
      {m.lastExit > 0 && !anyBell && <Text style={{ fontSize: 10, color: '#ff453a' }}>●</Text>}
      <View style={{ flex: 1, overflow: 'hidden' }}>
        <Text style={{ fontSize: 12, fontWeight: active ? 600 : 500, color: active ? t.text : t.textDim, whiteSpace: 'nowrap',
          textAlign: 'center' }}>{title.length > maxChars ? `…${title.slice(title.length - maxChars + 1)}` : title}</Text>
      </View>
      <Text style={small(t, { fontSize: 11, opacity: active || hover ? 1 : 0.7 })}>{index < 9 ? `❖${index + 1}` : ''}</Text>
      {color ? <View style={{ position: 'absolute', left: 8, right: 8, bottom: 1, height: 3, borderRadius: 2, backgroundColor: color }} /> : null}
    </View>
  );
}

export function TabBar({ t, wid, w }) {
  const ref = useRef(null);
  const [width, setWidth] = useState(600);
  useEffect(() => { if (ref.current) { const lw = lord.getLayout(ref.current.id)[2]; if (lw && Math.abs(lw - width) > 4) setWidth(lw); } });
  const tabW = Math.min(260, (width - 50) / Math.max(1, w.tabs.length));
  const maxChars = Math.max(6, Math.floor((tabW - 64) / 6.6));
  return (
    <View ref={ref} style={{ height: 36, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 4, backgroundColor: t.sidebarBg }}>
      <Hairline edge="bottom" />
      {w.tabs.map((tab, i) => <Tab key={tab.id} t={t} wid={wid} tab={tab} index={i} active={tab.id === w.active} maxChars={maxChars} />)}
      <ToolbarButton icon="+" iconSize={17} small title={tr('terminal.action.newTab')} color={t.textDim} onClick={() => addTab(wid)} style={{ width: 26 }} />
      {w.broadcast && (
        <View style={{ paddingHorizontal: 8, height: 22, borderRadius: 5, justifyContent: 'center', backgroundColor: '#ff453a' }}>
          <Text style={{ fontSize: 11, fontWeight: 600, color: '#fff' }}>{tr('terminal.broadcasting')}</Text>
        </View>
      )}
    </View>
  );
}

// ---- status bar ----

export function StatusBar({ t, sid }) {
  const [info, setInfo] = useState(() => term.info(sid));
  const clock = useClock((d) => formatTime(d), 1000);
  const m = useStore(meta, (x) => x[sid]) || {};
  useEffect(() => {
    setInfo(term.info(sid));
    const id = setInterval(() => setInfo(term.info(sid)), 1000);
    return () => clearInterval(id);
  }, [sid]);
  if (!info) return null;
  const home = lord.fs.home();
  const cwd = (m.cwd || info.cwd || '').replace(home, '~');
  const Item = ({ label, color }) => <Text style={small(t, { color: color || t.textDim })}>{label}</Text>;
  return (
    <View style={{ height: 24, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 12, gap: 16, backgroundColor: t.sidebarBg }}>
      <Hairline edge="top" />
      <Item label={cwd || '~'} />
      <Item label={info.busy ? `▶ ${info.process}` : `${info.shell || 'shell'}`} color={info.busy ? t.accent : undefined} />
      {m.lastExit > 0 && <Item label={tr('terminal.status.lastExit', { code: m.lastExit })} color="#ff453a" />}
      <View style={{ flex: 1 }} />
      {info.view > 0 && <Item label={tr('terminal.status.history', { n: info.view })} />}
      {info.altScreen && <Item label={tr('terminal.status.altScreen')} />}
      <Item label={`${info.cols}×${info.rows}`} />
      <Item label={clock} />
    </View>
  );
}

// ---- find bar ----

// Regular expression search: the C engine gives the text by logical lines and takes the matches
// as (line, first character, count).
function regexSearch(sid, q, cs) {
  let re;
  try { re = new RegExp(q, cs ? 'gu' : 'giu'); } catch (e) { return { count: 0, index: -1, error: true }; }
  const lines = (term.logicalText(sid) || '').split('\n');
  const trip = [];
  for (let i = 0; i < lines.length && trip.length < 60000; i++) {
    const line = lines[i];
    if (!line) continue;
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(line)) && trip.length < 60000) {
      if (!m[0].length) { re.lastIndex++; continue; }
      const start = [...line.slice(0, m.index)].length;
      trip.push(i, start, [...m[0]].length);
    }
  }
  return term.setMatches(sid, trip);
}

export function FindBar({ t, wid, sid, find }) {
  const [res, setRes] = useState({ count: 0, index: -1 });
  const run = (q, cs, rx) => setRes(rx && q ? regexSearch(sid, q, cs) : term.search(sid, q, cs));
  useEffect(() => { run(find.query, find.cs, find.regex); return () => term.search(sid, '', false); }, [sid]);
  const set = (p) => {
    const next = { ...find, ...p };
    patchWin(wid, () => ({ find: next }));
    if ('query' in p || 'cs' in p || 'regex' in p) run(next.query, next.cs, next.regex);
  };
  const move = (d) => setRes(term.searchMove(sid, d));
  const close = () => { term.search(sid, '', false); patchWin(wid, () => ({ find: null })); };
  const Btn = ({ label, onClick, on }) => <ToolbarButton small label={label} active={on} onClick={onClick} style={{ minWidth: 26, paddingHorizontal: 4 }} />;
  return (
    <View style={{ position: 'absolute', right: 12, top: 8, flexDirection: 'row', alignItems: 'center', gap: 4, padding: 6, borderRadius: 10,
      backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 6px 18px rgba(0,0,0,0.35)' }}>
      <SearchField value={find.query} autoFocus placeholder={tr('terminal.find.placeholder')} width={200}
        onChange={(q) => set({ query: q })} onSubmit={() => move(-1)} onCancel={close}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && e.shift) { move(1); return true; }
          if (e.key === 'ArrowUp') { move(-1); return true; }
          if (e.key === 'ArrowDown') { move(1); return true; }
          return false;
        }} />
      <Text style={small(t, { width: 88, fontSize: 11, textAlign: 'center' })}>{res.error ? tr('terminal.find.badRegex') : res.count ? tr('terminal.find.position', { i: res.count - res.index, n: res.count }) : find.query ? tr('terminal.find.none') : ''}</Text>
      <Btn label="↑" onClick={() => move(-1)} />
      <Btn label="↓" onClick={() => move(1)} />
      <Btn label="Aa" on={find.cs} onClick={() => set({ cs: !find.cs })} />
      <Btn label=".*" on={find.regex} onClick={() => set({ regex: !find.regex })} />
      <ToolbarButton small icon="✕" iconSize={11} title={tr('terminal.find.close')} onClick={close} />
    </View>
  );
}

// ---- toolbelt ----

function Section({ t, title, children }) {
  return (
    <View style={{ marginBottom: 16 }}>
      <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginBottom: 4, paddingHorizontal: 6 }}>{title}</Text>
      {children}
    </View>
  );
}

function Row({ t, label, sub, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ paddingVertical: 4, paddingHorizontal: 6, borderRadius: 6, backgroundColor: hover ? t.hover : 'transparent' }}>
      <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{label.length > 34 ? `${label.slice(0, 33)}…` : label}</Text>
      {sub ? <Text style={small(t, { fontSize: 11 })}>{sub}</Text> : null}
    </View>
  );
}

export function Toolbelt({ t, wid, w, sid }) {
  const history = useStore(clipboardHistory).filter((x) => x.text != null);
  const dirs = useStore(recentDirs);
  const list = useStore(notes);
  const cmds = useStore(commandHistory);
  const caps = useStore(captured);
  const [note, setNote] = useState('');
  const home = lord.fs.home();
  const profiles = terminalSettings().profiles;
  const sessions = w.tabs.flatMap((tab) => leaves(tab.root));
  const quote = (s) => `'${s.replace(/'/g, "'\\''")}'`;
  return (
    <View style={{ width: 240, paddingHorizontal: 10, paddingVertical: 12, backgroundColor: t.sidebarBg, overflow: 'hidden' }}>
      <Hairline edge="start" />
      <Section t={t} title={tr('terminal.belt.jobs')}>
        {sessions.map((s) => {
          const info = term.info(s);
          return <Row key={s} t={t} label={sessionTitle(s)} sub={info?.busy ? `▶ ${info.process}` : info?.shell} onClick={() => focusPane(wid, s)} />;
        })}
      </Section>
      <Section t={t} title={tr('terminal.belt.commands')}>
        {cmds.length ? cmds.slice(0, 8).map((c, i) => (
          <Row key={i} t={t} label={c.cmd} sub={tr('terminal.belt.commandHint')} onClick={() => term.write(sid, c.cmd)} />
        )) : <Text style={small(t)}>{tr('terminal.belt.needsIntegration')}</Text>}
      </Section>
      {caps.length > 0 && (
        <Section t={t} title={tr('terminal.belt.captured')}>
          {caps.slice(0, 8).map((c, i) => <Row key={i} t={t} label={c.text} onClick={() => focusPane(wid, c.sid)} />)}
        </Section>
      )}
      <Section t={t} title={tr('terminal.belt.clipboard')}>
        {history.length ? history.slice(0, 8).map((h, i) => (
          <Row key={i} t={t} label={h.text.replace(/\n/g, '⏎ ')} onClick={() => term.paste(sid, h.text)} />
        )) : <Text style={small(t)}>{tr('terminal.belt.clipboardEmpty')}</Text>}
      </Section>
      <Section t={t} title={tr('terminal.belt.dirs')}>
        {dirs.length ? dirs.slice(0, 8).map((d) => (
          <Row key={d} t={t} label={d.replace(home, '~')} onClick={() => term.write(sid, `cd ${quote(d)}\r`)} />
        )) : <Text style={small(t)}>{tr('terminal.belt.needsIntegration')}</Text>}
      </Section>
      <Section t={t} title={tr('terminal.belt.profiles')}>
        {profiles.map((p) => <Row key={p.id} t={t} label={profileName(p)} onClick={() => addTab(wid, p.id)} />)}
      </Section>
      <Section t={t} title={tr('terminal.belt.notes')}>
        {list.map((n, i) => (
          <Row key={i} t={t} label={n} sub={tr('terminal.belt.noteHint')} onClick={() => notes.set((x) => x.filter((_, j) => j !== i))} />
        ))}
        <TextInput value={note} placeholder={tr('terminal.belt.addNote')} onChange={setNote} style={{ marginTop: 6 }}
          onSubmit={(v) => { if (v.trim()) notes.set((x) => [...x, v.trim()]); setNote(''); }} />
      </Section>
    </View>
  );
}

// ---- popups: autocomplete, paste history, quick open, title, composer ----

function Popup({ t, title, children, width = 380 }) {
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: 50, alignItems: 'center' }}>
      <View style={{ width, padding: 12, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
        boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginBottom: 8, paddingHorizontal: 2 }}>{title}</Text>
        {children}
      </View>
    </View>
  );
}

function ListPopup({ t, title, items, sel, empty }) {
  return (
    <Popup t={t} title={title}>
      {items.length ? items.slice(0, 12).map((it, i) => (
        <View key={i} style={{ paddingHorizontal: 8, height: 24, justifyContent: 'center', borderRadius: 5, backgroundColor: i === sel ? t.accent : 'transparent' }}>
          <Text style={{ fontSize: 12, color: i === sel ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{it.length > 60 ? `${it.slice(0, 59)}…` : it}</Text>
        </View>
      )) : <Text style={small(t)}>{empty}</Text>}
      <Text style={small(t, { fontSize: 11, marginTop: 8, paddingHorizontal: 2 })}>{tr('terminal.popup.keys')}</Text>
    </Popup>
  );
}

// Keys of the popup lists; returns true if it used them.
export function popupKey(wid, w, sid, e) {
  const p = w.popup;
  if (!p) return false;
  // these have a text field; with the focus out of it, Esc still closes them
  if (p.kind === 'quick' || p.kind === 'title' || p.kind === 'composer') {
    if (e.key !== 'Escape') return false;
    patchWin(wid, () => ({ popup: null }));
    return true;
  }
  const items = p.items || [];
  if (e.key === 'Escape') { patchWin(wid, () => ({ popup: null })); return true; }
  if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
    const d = e.key === 'ArrowUp' ? -1 : 1;
    patchWin(wid, () => ({ popup: { ...p, sel: (p.sel + d + items.length) % Math.max(1, items.length) } }));
    return true;
  }
  if (e.key === 'Enter') {
    const it = items[p.sel];
    if (it != null) {
      if (p.kind === 'complete') term.write(sid, it.slice(p.prefix.length));
      if (p.kind === 'paste') term.paste(sid, clipboardHistory.get().filter((x) => x.text != null)[p.sel].text);
      if (p.kind === 'history') term.write(sid, it);
    }
    patchWin(wid, () => ({ popup: null }));
    return true;
  }
  return true;
}

export function openAutocomplete(wid, sid) {
  const prefix = term.cursorWord(sid) || '';
  const items = (term.words(sid, prefix) || '').split('\n').filter(Boolean);
  patchWin(wid, () => ({ popup: { kind: 'complete', prefix, items, sel: 0 } }));
}

export function openCommandHistory(wid) {
  patchWin(wid, () => ({ popup: { kind: 'history', items: commandHistory.get().map((c) => c.cmd), sel: 0 } }));
}

export function openPasteHistory(wid) {
  patchWin(wid, () => ({ popup: { kind: 'paste', items: clipboardHistory.get().filter((x) => x.text != null).map((h) => h.text.replace(/\n/g, '⏎ ')), sel: 0 } }));
}

function QuickOpen({ t, wid }) {
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  const all = Object.entries(wins.get()).flatMap(([w, st]) => st.tabs.flatMap((tab) => leaves(tab.root).map((s) => ({ w, s, title: sessionTitle(s) }))));
  const items = all.filter((x) => x.title.toLowerCase().includes(q.toLowerCase()));
  const pick = (x) => { patchWin(wid, () => ({ popup: null })); if (x) focusPane(wid, x.s); };
  return (
    <Popup t={t} title={tr('terminal.popup.quick')}>
      <TextInput value={q} autoFocus placeholder={tr('terminal.popup.quickPlaceholder')} onChange={(v) => { setQ(v); setSel(0); }}
        onSubmit={() => pick(items[sel])} onCancel={() => pick(null)}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown') { setSel((s) => Math.min(items.length - 1, s + 1)); return true; }
          if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
          return false;
        }} />
      <View style={{ marginTop: 8 }}>
        {items.slice(0, 10).map((x, i) => (
          <View key={x.s} onClick={() => pick(x)} style={{ paddingHorizontal: 8, height: 24, justifyContent: 'center', borderRadius: 5,
            backgroundColor: i === sel ? t.accent : 'transparent' }}>
            <Text style={{ fontSize: 12, color: i === sel ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{x.title}</Text>
          </View>
        ))}
      </View>
    </Popup>
  );
}

function EditTitle({ t, wid, tab }) {
  const [v, setV] = useState(tab.title || '');
  const done = (val) => { patchWin(wid, (w) => ({ popup: null, tabs: w.tabs.map((x) => (x.id === tab.id ? { ...x, title: val || null } : x)) })); };
  return (
    <Popup t={t} title={tr('terminal.popup.title')}>
      <TextInput value={v} autoFocus onChange={setV} onSubmit={done} onCancel={() => patchWin(wid, () => ({ popup: null }))} />
    </Popup>
  );
}

// Composer: write a long command before sending it.
function Composer({ t, wid, sid, w }) {
  const [v, setV] = useState('');
  return (
    <View style={{ position: 'absolute', left: 12, right: 12, bottom: 12, padding: 10, borderRadius: 10, backgroundColor: t.windowBg,
      border: `1px solid ${t.accent}`, boxShadow: '0 10px 30px rgba(0,0,0,0.4)' }}>
      <Text style={small(t, { fontSize: 11, marginBottom: 6 })}>
        {w.broadcast ? tr('terminal.composer.broadcast') : tr('terminal.composer.hint')}
      </Text>
      <TextInput value={v} autoFocus onChange={setV}
        onSubmit={(val) => { term.paste(sid, `${val}\n`); setV(''); }}
        onCancel={() => patchWin(wid, () => ({ composer: false }))}
        onKeyDown={(e) => { if (e.key === 'Enter' && e.shift) { term.paste(sid, v); setV(''); return true; } return false; }} />
    </View>
  );
}

export function Overlays({ t, wid, w, sid, tab }) {
  const p = w.popup;
  return (
    <>
      {p?.kind === 'complete' && <ListPopup t={t} title={tr('terminal.popup.complete', { prefix: p.prefix })} items={p.items} sel={p.sel} empty={tr('terminal.popup.completeEmpty')} />}
      {p?.kind === 'paste' && <ListPopup t={t} title={tr('terminal.popup.paste')} items={p.items} sel={p.sel} empty={tr('terminal.popup.pasteEmpty')} />}
      {p?.kind === 'history' && <ListPopup t={t} title={tr('terminal.popup.history')} items={p.items} sel={p.sel} empty={tr('terminal.popup.historyEmpty')} />}
      {p?.kind === 'quick' && <QuickOpen t={t} wid={wid} />}
      {p?.kind === 'title' && tab && <EditTitle t={t} wid={wid} tab={tab} />}
      {w.composer && <Composer t={t} wid={wid} sid={sid} w={w} />}
      {w.dialog && (
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center',
          backgroundColor: 'rgba(0,0,0,0.35)' }}>
          <View style={{ width: 340, padding: 20, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
            boxShadow: '0 20px 50px rgba(0,0,0,0.5)' }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>{w.dialog.text}</Text>
            <Text style={{ fontSize: 13, color: t.textDim, marginTop: 6, lineHeight: 1.4 }}>{w.dialog.detail}</Text>
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 20 }}>
              <Button label={tr('terminal.dialog.cancel')} onClick={() => patchWin(wid, () => ({ dialog: null }))} />
              <Button label={tr('terminal.dialog.close')} primary onClick={() => { const fn = w.dialog.onConfirm; patchWin(wid, () => ({ dialog: null })); fn(); }} />
            </View>
          </View>
        </View>
      )}
      <View style={{ position: 'absolute', right: 14, bottom: 34, gap: 6, alignItems: 'flex-end', pointerEvents: 'none' }}>
        {w.toasts.map((n) => (
          <View key={n.id} style={{ maxWidth: 340, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 9, backgroundColor: t.windowBg,
            border: `1px solid ${t.separator}`, boxShadow: '0 8px 22px rgba(0,0,0,0.4)' }}>
            <Text style={{ fontSize: 12, color: t.text }}>{n.text}</Text>
          </View>
        ))}
      </View>
    </>
  );
}

// ---- Instant Replay ----

export function ReplayBar({ t, wid, replay }) {
  const go = (i) => patchWin(wid, () => ({ replay: { sid: replay.sid, ...term.replay(replay.sid, Math.round(i)) } }));
  const exit = () => { term.replay(replay.sid, 'exit'); patchWin(wid, () => ({ replay: null })); };
  const d = new Date(replay.time);
  const ago = Math.max(0, Math.round((Date.now() - replay.time) / 1000));
  return (
    <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 12, backgroundColor: '#3a1d5c' }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>Instant Replay</Text>
      <Button label="◀" onClick={() => go(Math.max(0, replay.index - 1))} />
      <Slider value={replay.index} min={0} max={Math.max(1, replay.count - 1)} width={300} onChange={go} />
      <Button label="▶" onClick={() => go(Math.min(replay.count - 1, replay.index + 1))} />
      <Text style={{ fontSize: 12, color: '#fff' }}>{tr('terminal.replay.when', { time: formatTime(d, { seconds: true }), s: ago })}</Text>
      <View style={{ flex: 1 }} />
      <Button label={tr('terminal.replay.exit')} primary onClick={exit} />
    </View>
  );
}

// ---- a pane's context menu ----

export function ContextMenu({ t, x, y, items, onClose }) {
  // width from the longest text (it changes with the language)
  const width = Math.ceil(Math.max(230, ...items.filter((it) => it !== '-').map((it) =>
    40 + lord.measureText(it.label, 13, 400) + (it.hint ? 16 + lord.measureText(it.hint, 11, 400) : 0))));
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} onMouseDown={onClose}>
      <View onMouseDown={(e) => e.stopPropagation()} style={{ position: 'absolute', left: x, top: y, width, paddingVertical: 5,
        borderRadius: 9, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 12px 32px rgba(0,0,0,0.45)' }}>
        {items.map((it, i) => (it === '-'
          ? <View key={i} style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />
          : <MenuRow key={i} t={t} item={it} onClose={onClose} />))}
      </View>
    </View>
  );
}

function MenuRow({ t, item, onClose }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onClick={() => { if (!item.disabled) { onClose(); item.onClick(); } }}
      style={{ height: 24, marginHorizontal: 5, paddingHorizontal: 10, borderRadius: 5, flexDirection: 'row', alignItems: 'center',
        backgroundColor: hover && !item.disabled ? t.accent : 'transparent' }}>
      <Text style={{ flex: 1, fontSize: 13, color: hover && !item.disabled ? '#fff' : item.disabled ? t.textDim : t.text }}>{item.label}</Text>
      {item.hint ? <Text style={{ fontSize: 11, color: hover ? '#fff' : t.textDim }}>{item.hint}</Text> : null}
    </View>
  );
}

export { getWin, moveTab, clipboard };
