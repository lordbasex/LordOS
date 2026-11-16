// deskwidgets.jsx — the desktop widget catalog (0.8.8.10; Lord Workspaces' "Add widget"). widgets.js draws the layer;
// this file is what can be put on it and where it is kept.
//
//   registerWidgetKind({ type, nameKey, descKey, Icon, add(x, y) })   an entry of the catalog (an app may add its own:
//                                     the Notes app adds the sticky note, whose widgets it keeps itself)
//   widgetKinds()                     the catalog, in order
//   addDeskWidget(type, x, y)         a built-in widget at (x, y): settings.deskWidgets, kept across sessions
//   removeDeskWidget(id) / moveDeskWidget(id, x, y)
//   startDeskWidgets()                the theme that offers the catalog starts publishing its widgets (Lord Workspaces
//                                     only: the other themes never show them)
//
// The built-in widgets: Clock (the minute, no timer per second), System (CPU every 1.5 s and memory every 5 s, only while
// shown), Calendar (this month, days with events marked), Now playing, Read aloud (the clipboard, with lord-say), Network
// (the rate every 2 s from /proc/net/dev, the address and Wi-Fi signal every 8 s), Disks (every 30 s), Temperatures
// (lord-fand every 2 s, while shown), Battery (pushed by lord-server), Reminders and Calendar (on lord-maild's changes),
// Clipboard (the history store) and Claude Code (its session files every 3 s). Every timer lives only while its widget
// is on screen.
// Each one sits in a WidgetShell: a title bar shown on hover, dragged to move (one write when released), × to remove.
import { useEffect, useRef, useState } from 'react';
import { View, Text, addListener, screen } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { registerWidgetType, setWidgets } from './widgets.js';
import { useTheme } from './theme.js';
import { t, formatTime, formatDate, monthName, weekdayName } from './i18n.js';
import { useCpuUsage, useSystemInfo, formatBytes } from './system.js';
import { memUsedKB } from './memfigure.js';
import { NowPlayingTile } from './media.js';
import { speak, stopSpeaking, speakAvailable } from './voice.js';
import { events, reminders, usePimQuery } from './calendar.js';
import { useFan } from './cooling.jsx';
import { battery } from './hardware.jsx';
import { clipboard, clipboardHistory } from './clipboard.js';
import { wifiBars } from './menubarmodel.js';

const kinds = [];
export const widgetCatalog = createStore(0);
export function registerWidgetKind(k) {
  if (!k || !k.type || kinds.some((x) => x.type === k.type)) return;
  kinds.push(k);
  widgetCatalog.set(widgetCatalog.get() + 1);
}
export const widgetKinds = () => kinds.filter((k) => !k.available || k.available());

const MAX_WIDGETS = 24;
const list = () => (Array.isArray(settings.get().deskWidgets) ? settings.get().deskWidgets : []);
const SIZES = { clock: [240, 112], system: [250, 136], calendar: [256, 248], nowplaying: [320, 116], readaloud: [250, 118],
  network: [260, 140], disks: [260, 150], temps: [250, 150], battery: [230, 110], reminders: [270, 200], clipboard: [280, 210], claude: [270, 150] };
export const deskWidgetSize = (type) => SIZES[type] || [240, 120];

export function addDeskWidget(type, x, y) {
  if (!SIZES[type]) return null;
  const [w, h] = SIZES[type];
  const id = `${type}-${Date.now().toString(36)}`;
  const nx = Math.round(Math.max(0, Math.min(screen.width - w, x))), ny = Math.round(Math.max(0, Math.min(screen.height - h, y)));
  settings.set({ deskWidgets: [...list(), { id, type, x: nx, y: ny }].slice(-MAX_WIDGETS) });
  return id;
}
export const removeDeskWidget = (id) => settings.set({ deskWidgets: list().filter((w) => w.id !== id) });
export const moveDeskWidget = (id, x, y) => settings.set({ deskWidgets: list().map((w) => (w.id === id ? { ...w, x: Math.round(x), y: Math.round(y) } : w)) });

let started = false;
export function startDeskWidgets() {
  if (started) return;
  started = true;
  const publish = () => {
    const all = list().filter((w) => w && SIZES[w.type] && Number.isFinite(w.x) && Number.isFinite(w.y));
    for (const type of Object.keys(SIZES)) {
      const [ww, hh] = SIZES[type];
      setWidgets(`ws-${type}`, all.filter((w) => w.type === type).map((w) => ({ id: w.id, x: w.x, y: w.y, w: ww, h: hh })));
    }
  };
  for (const type of Object.keys(SIZES)) registerWidgetType(`ws-${type}`, BODIES[type]);
  let last = '';
  settings.subscribe(() => { const k = JSON.stringify(settings.get().deskWidgets || []); if (k !== last) { last = k; publish(); } });
  last = JSON.stringify(settings.get().deskWidgets || []);
  publish();
}

// ---- the shell every built-in widget sits in ----
function WidgetShell({ widget, title, children }) {
  const th = useTheme() || {};
  const [hover, setHover] = useState(false);
  const [live, setLive] = useState(null);
  const drag = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      setLive({ x: Math.max(0, Math.min(screen.width - 60, d.x + e.x - d.mx)), y: Math.max(0, Math.min(screen.height - 40, d.y + e.y - d.my)) });
    });
    const offUp = addListener('pointerup', () => {
      const d = drag.current;
      drag.current = null;
      setLive((l) => { if (d && l) moveDeskWidget(widget.id, l.x, l.y); return null; });
    });
    return () => { offMove(); offUp(); };
  }, [widget.id]);
  const g = live || widget;
  const dark = th.mode !== 'light';
  const mat = th.mat || {};
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ position: 'absolute', left: g.x, top: g.y, width: widget.w, height: widget.h, borderRadius: 16, overflow: 'hidden',
        backgroundColor: mat.bgStill || (dark ? 'rgba(26,22,50,0.86)' : 'rgba(255,255,255,0.9)'), border: `1px solid ${mat.border || 'rgba(255,255,255,0.12)'}`,
        boxShadow: dark ? '0 8px 22px rgba(0,0,0,0.35)' : '0 8px 22px rgba(40,30,110,0.16)' }}>
      {mat.sheen ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: '50%', background: mat.sheen, pointerEvents: 'none' }} /> : null}
      <View onMouseDown={(e) => { if (!e.button) drag.current = { mx: e.x, my: e.y, x: g.x, y: g.y }; }}
        style={{ height: 24, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 10, gap: 6, cursor: 'grab' }}>
        <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: th.textDim || '#9C97B8', textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{title}</Text>
        {hover ? (
          <View title={t('shell.widgets.remove')} onClick={() => removeDeskWidget(widget.id)}
            style={{ width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: dark ? 'rgba(255,255,255,0.10)' : 'rgba(0,0,0,0.06)' }}>
            <Text style={{ fontSize: 10, color: th.text || '#ECEAF6' }}>✕</Text>
          </View>
        ) : null}
      </View>
      <View style={{ flex: 1, paddingHorizontal: 12, paddingBottom: 10 }}>{children}</View>
    </View>
  );
}

// ---- the built-in widgets ----
function useMinute() {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    let iv = 0;
    const first = setTimeout(() => { setNow(new Date()); iv = setInterval(() => setNow(new Date()), 60000); }, 60000 - (Date.now() % 60000) + 50);
    return () => { clearTimeout(first); clearInterval(iv); };
  }, []);
  return now;
}

function ClockWidget({ widget }) {
  const th = useTheme() || {};
  const now = useMinute();
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.clock.name')}>
      <Text style={{ fontSize: 40, fontWeight: 700, color: th.text || '#ECEAF6', textAlign: 'start', fontVariantNumeric: 'tabular-nums' }}>{formatTime(now)}</Text>
      <Text style={{ fontSize: 12.5, color: th.textDim || '#9C97B8', textAlign: 'start' }}>{formatDate(now, 'long')}</Text>
    </WidgetShell>
  );
}

function Meter({ th, label, value, detail }) {
  return (
    <View style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row' }}>
        <Text style={{ flex: 1, fontSize: 12, color: th.text || '#ECEAF6', textAlign: 'start' }}>{label}</Text>
        <Text style={{ fontSize: 11.5, color: th.textDim || '#9C97B8', fontVariantNumeric: 'tabular-nums' }}>{detail}</Text>
      </View>
      <View style={{ height: 6, borderRadius: 3, backgroundColor: th.mode === 'light' ? 'rgba(0,0,0,0.08)' : 'rgba(255,255,255,0.10)', direction: 'ltr' }}>
        <View style={{ width: `${Math.round(Math.max(0.02, Math.min(1, value)) * 100)}%`, height: 6, borderRadius: 3, backgroundColor: th.accent || '#7C5CFF' }} />
      </View>
    </View>
  );
}

function SystemWidget({ widget }) {
  const th = useTheme() || {};
  const cpu = useCpuUsage(1500);
  const info = useSystemInfo(5000) || {};
  const used = memUsedKB(info), total = info.memTotalKB || 0;
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.system.name')}>
      <View style={{ gap: 10, paddingTop: 4 }}>
        <Meter th={th} label="CPU" value={cpu} detail={`${Math.round(cpu * 100)} %`} />
        <Meter th={th} label={t('shell.cc.memory')} value={total ? used / total : 0} detail={total ? `${formatBytes(used * 1024)} / ${formatBytes(total * 1024)}` : ''} />
      </View>
    </WidgetShell>
  );
}

const ymd = (d) => `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
function CalendarWidget({ widget }) {
  const th = useTheme() || {};
  const now = useMinute();
  const y = now.getFullYear(), m = now.getMonth();
  const first = new Date(y, m, 1), days = new Date(y, m + 1, 0).getDate();
  const busy = usePimQuery(async () => {
    try {
      const occ = await events.range(first.getTime(), new Date(y, m + 1, 1).getTime());
      return new Set((occ || []).map((o) => ymd(new Date(o.start))));
    } catch (e) { return new Set(); }
  }, [y, m]) || new Set();
  const cells = [];
  for (let i = 0; i < first.getDay(); i++) cells.push(null);
  for (let d = 1; d <= days; d++) cells.push(d);
  const W = 32;
  return (
    <WidgetShell widget={widget} title={`${monthName(m)} ${y}`}>
      <View style={{ flexDirection: 'row', direction: 'ltr' }}>
        {[0, 1, 2, 3, 4, 5, 6].map((wd) => <Text key={wd} style={{ width: W, fontSize: 10, textAlign: 'center', color: th.textDim || '#9C97B8' }}>{weekdayName(wd).slice(0, 2)}</Text>)}
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', direction: 'ltr' }}>
        {cells.map((d, i) => {
          const today = d === now.getDate();
          const has = d && busy.has(ymd(new Date(y, m, d)));
          return (
            <View key={i} style={{ width: W, height: 28, alignItems: 'center', justifyContent: 'center' }}>
              {d ? (
                <View style={{ width: 24, height: 24, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: today ? th.accent || '#7C5CFF' : 'transparent' }}>
                  <Text style={{ fontSize: 11.5, color: today ? '#ffffff' : th.text || '#ECEAF6', fontWeight: today ? 700 : 400 }}>{String(d)}</Text>
                  {has && !today ? <View style={{ position: 'absolute', bottom: 1, width: 4, height: 4, borderRadius: 2, backgroundColor: th.accent || '#7C5CFF' }} /> : null}
                </View>
              ) : null}
            </View>
          );
        })}
      </View>
    </WidgetShell>
  );
}

function NowPlayingWidget({ widget }) {
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.nowplaying.name')}>
      <NowPlayingTile style={{ backgroundColor: 'transparent', padding: 0 }} />
    </WidgetShell>
  );
}

function Pill({ th, label, onClick, primary, disabled }) {
  return (
    <View onClick={disabled ? undefined : onClick}
      style={{ height: 30, paddingHorizontal: 12, borderRadius: 9, justifyContent: 'center', opacity: disabled ? 0.45 : 1,
        backgroundColor: primary ? th.accent || '#7C5CFF' : th.mode === 'light' ? 'rgba(0,0,0,0.06)' : 'rgba(255,255,255,0.10)' }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: primary ? '#ffffff' : th.text || '#ECEAF6' }}>{label}</Text>
    </View>
  );
}

function ReadAloudWidget({ widget }) {
  const th = useTheme() || {};
  const [note, setNote] = useState('');
  const can = speakAvailable();
  const read = () => {
    let text = '';
    try { text = typeof lord !== 'undefined' && lord.clipboard ? lord.clipboard.get() || '' : ''; } catch (e) { text = ''; }
    if (!text.trim()) { setNote(t('shell.widgets.readaloud.empty')); return; }
    setNote('');
    speak(text.slice(0, 20000), undefined, t('shell.widgets.readaloud.name'));
  };
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.readaloud.name')}>
      <View style={{ gap: 8, paddingTop: 2 }}>
        <Text style={{ fontSize: 11.5, color: th.textDim || '#9C97B8', textAlign: 'start' }}>{can ? note || t('shell.widgets.readaloud.hint') : t('shell.widgets.readaloud.noVoice')}</Text>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Pill th={th} primary disabled={!can} label={t('shell.widgets.readaloud.read')} onClick={read} />
          <Pill th={th} disabled={!can} label={t('shell.widgets.readaloud.stop')} onClick={() => stopSpeaking()} />
        </View>
      </View>
    </WidgetShell>
  );
}


// ---- stage 2: what the system already knows ----
const dimOf = (th) => th.textDim || '#9C97B8';
const textOf = (th) => th.text || '#ECEAF6';
function Row({ th, label, value, accent }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 20 }}>
      <Text style={{ flex: 1, fontSize: 12, color: dimOf(th), textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{label}</Text>
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: accent ? th.accent || '#7C5CFF' : textOf(th), fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>{value}</Text>
    </View>
  );
}
const Quiet = ({ th, text }) => <Text style={{ fontSize: 12, color: dimOf(th), textAlign: 'start', paddingTop: 6 }}>{text}</Text>;
// a timer only while mounted, at `ms`
function useEvery(fn, ms, deps = []) {
  const [v, setV] = useState(() => { try { return fn(); } catch (e) { return null; } });
  useEffect(() => {
    const id = setInterval(() => { try { setV(fn()); } catch (e) { setV(null); } }, ms);
    return () => clearInterval(id);
  }, deps);
  return v;
}
const rate = (n) => `${formatBytes(Math.max(0, n))}/s`;

function NetworkWidget({ widget }) {
  const th = useTheme() || {};
  const prev = useRef(null);
  const speed = useEvery(() => {
    const s = typeof lord !== 'undefined' && lord.monitor?.netTotals ? lord.monitor.netTotals() : null;
    const p = prev.current;
    prev.current = s;
    if (!s || !p || s.t <= p.t) return null;
    const dt = (s.t - p.t) / 1000;
    return { rx: (s.rx - p.rx) / dt, tx: (s.tx - p.tx) / dt };
  }, 2000);
  const ifs = useEvery(() => (typeof lord !== 'undefined' && lord.net?.interfaces ? lord.net.interfaces() : []), 8000) || [];
  const up = ifs.filter((i) => i.up && i.carrier !== false && i.type !== 'loopback');
  const main = up.find((i) => i.type === 'wifi' || i.type === 'ethernet') || up[0];
  const ip = main?.addresses?.find((a) => a.scope === 'global' && a.family === 4)?.address || '';
  const kind = !main ? t('shell.widgets.network.offline') : main.type === 'wifi'
    ? `${t('shell.widgets.network.wifi')} · ${'▮'.repeat(wifiBars(main.signal))}${'▯'.repeat(4 - wifiBars(main.signal))}` : t('shell.widgets.network.wired');
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.network.name')}>
      <Row th={th} label={kind} value={ip} />
      <Row th={th} label={`↓ ${t('shell.widgets.network.down')}`} value={speed ? rate(speed.rx) : '…'} accent />
      <Row th={th} label={`↑ ${t('shell.widgets.network.up')}`} value={speed ? rate(speed.tx) : '…'} />
    </WidgetShell>
  );
}

function DisksWidget({ widget }) {
  const th = useTheme() || {};
  const list = useEvery(() => {
    const out = [];
    const info = lord.system.info();
    if (info && info.diskTotal) out.push({ name: t('shell.widgets.disks.system'), total: info.diskTotal, free: info.diskFree });
    try { for (const v of (lord.fs.volumes ? lord.fs.volumes() : []).slice(0, 3)) if (v.total) out.push({ name: v.name || v.path, total: v.total, free: v.free }); } catch (e) { /* none */ }
    return out;
  }, 30000) || [];
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.disks.name')}>
      <View style={{ gap: 8, paddingTop: 2 }}>
        {list.length ? list.map((d) => (
          <Meter key={d.name} th={th} label={d.name} value={d.total ? 1 - d.free / d.total : 0} detail={t('shell.widgets.disks.free', { free: formatBytes(d.free) })} />
        )) : <Quiet th={th} text="…" />}
      </View>
    </WidgetShell>
  );
}

function TempsWidget({ widget }) {
  const th = useTheme() || {};
  const f = useFan();
  const fans = f ? [...(f.fans || []), ...(f.firmwareFans || []).map((x, i) => ({ id: `fw${i}`, rpm: x.rpm }))].filter((x) => x.rpm >= 0).slice(0, 3) : [];
  const gpu = f ? (f.gpuFans || []).find((g) => g.temp != null) : null;
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.temps.name')}>
      {f === null ? <Quiet th={th} text={t('shell.widgets.temps.none')} /> : (
        <View style={{ gap: 2 }}>
          <Row th={th} label="CPU" value={f?.temp != null ? `${Math.round(f.temp)} °C` : '…'} accent />
          {gpu ? <Row th={th} label="GPU" value={`${Math.round(gpu.temp)} °C · ${gpu.rpm >= 0 ? `${gpu.rpm} rpm` : ''}`} /> : null}
          {fans.map((x, i) => <Row key={x.id || i} th={th} label={t('shell.widgets.temps.fan', { n: i + 1 })} value={`${x.rpm} rpm`} />)}
        </View>
      )}
    </WidgetShell>
  );
}

function BatteryWidget({ widget }) {
  const th = useTheme() || {};
  const b = useStore(battery);
  const pct = b && b.percent >= 0 ? b.percent : null;
  const left = b && b.minutesLeft > 0 ? t('shell.widgets.battery.left', { h: Math.floor(b.minutesLeft / 60), m: b.minutesLeft % 60 }) : '';
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.battery.name')}>
      {!b || !b.hasBattery ? <Quiet th={th} text={t('shell.widgets.battery.none')} /> : (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: 30, fontWeight: 700, color: textOf(th), textAlign: 'start' }}>{pct != null ? `${pct} %` : '…'}</Text>
          <Text style={{ fontSize: 12, color: dimOf(th), textAlign: 'start' }}>{b.charging ? t('shell.widgets.battery.charging') : b.onAC ? t('shell.widgets.battery.plugged') : left}</Text>
        </View>
      )}
    </WidgetShell>
  );
}

function RemindersWidget({ widget }) {
  const th = useTheme() || {};
  const items = usePimQuery(async () => {
    const all = await reminders.items();
    return (all || []).filter((it) => !it.done).sort((a, b) => (a.due ?? Infinity) - (b.due ?? Infinity)).slice(0, 5);
  }, []) || [];
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.reminders.name')}>
      {!items.length ? <Quiet th={th} text={t('shell.widgets.reminders.none')} /> : (
        <View style={{ gap: 4 }}>
          {items.map((it) => (
            <View key={it.uid} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 24 }}>
              <View onClick={() => reminders.toggle(it)} title={t('shell.widgets.reminders.done')}
                style={{ width: 16, height: 16, borderRadius: 8, border: `1.5px solid ${it.color || th.accent || '#7C5CFF'}` }} />
              <Text style={{ flex: 1, fontSize: 12.5, color: textOf(th), textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{it.title}</Text>
              {it.due ? <Text style={{ fontSize: 11, color: it.due < Date.now() ? '#E5484D' : dimOf(th) }}>{formatDate(new Date(it.due))}</Text> : null}
            </View>
          ))}
        </View>
      )}
    </WidgetShell>
  );
}

function ClipboardWidget({ widget }) {
  const th = useTheme() || {};
  const hist = useStore(clipboardHistory) || [];
  const [copied, setCopied] = useState(null);
  const shown = hist.filter((x) => x.text != null || x.image).slice(0, 5);
  return (
    <WidgetShell widget={widget} title={t('shell.widgets.clipboard.name')}>
      {!shown.length ? <Quiet th={th} text={t('shell.widgets.clipboard.none')} /> : (
        <View style={{ gap: 3 }}>
          {shown.map((x) => (
            <View key={x.id} onClick={() => { clipboard.use(x); setCopied(x.id); setTimeout(() => setCopied(null), 1200); }} title={t('shell.widgets.clipboard.copy')}
              style={{ height: 28, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, borderRadius: 8,
                backgroundColor: copied === x.id ? th.accent || '#7C5CFF' : th.mode === 'light' ? 'rgba(0,0,0,0.04)' : 'rgba(255,255,255,0.06)' }}>
              <Text style={{ flex: 1, fontSize: 12, color: copied === x.id ? '#ffffff' : textOf(th), textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>
                {copied === x.id ? t('shell.widgets.clipboard.copied') : x.image ? t('shell.widgets.clipboard.image') : String(x.text).replace(/\s+/g, ' ').slice(0, 80)}
              </Text>
            </View>
          ))}
        </View>
      )}
    </WidgetShell>
  );
}

// Claude Code sessions running on this computer: ~/.claude/sessions/<pid>.json (working or waiting) and the status
// line lord-claude-status keeps (~/.cache/lordos/claude/<session>.json: model and context), the same files the
// Terminal's footer reads; every 3 s while shown.
const readJson = (path) => { try { const v = JSON.parse(lord.fs.read(path)); return v && typeof v === 'object' ? v : null; } catch (e) { return null; } };
function claudeSessions() {
  if (typeof lord === 'undefined' || !lord.fs) return [];
  const home = lord.fs.home();
  let names = [];
  try { names = lord.fs.list(`${home}/.claude/sessions`).map((e) => e.name).filter((n) => /^\d{1,9}\.json$/.test(n)).slice(0, 16); } catch (e) { return []; }
  return names.map((n) => {
    const d = readJson(`${home}/.claude/sessions/${n}`);
    const id = d && /^[A-Za-z0-9-]{1,64}$/.test(String(d.sessionId || '')) ? d.sessionId : null;
    if (!id) return null;
    const st = readJson(`${home}/.cache/lordos/claude/${id}.json`);
    const ctx = st && st.session_id === id ? Math.round(+st.context_window?.used_percentage) : NaN;
    const dir = st && typeof st.workspace?.current_dir === 'string' ? st.workspace.current_dir : typeof d.cwd === 'string' ? d.cwd : '';
    return { id, state: d.status === 'busy' ? 'working' : d.status === 'idle' ? 'waiting' : '', ctx: Number.isFinite(ctx) ? ctx : null,
      model: typeof st?.model?.display_name === 'string' ? st.model.display_name.slice(0, 24) : '', dir: dir.split('/').filter(Boolean).pop() || '' };
  }).filter(Boolean).slice(0, 3);
}
function ClaudeWidget({ widget }) {
  const th = useTheme() || {};
  const list = useEvery(claudeSessions, 3000) || [];
  return (
    <WidgetShell widget={widget} title="Claude Code">
      {!list.length ? <Quiet th={th} text={t('shell.widgets.claude.none')} /> : (
        <View style={{ gap: 6 }}>
          {list.map((c) => (
            <View key={c.id} style={{ gap: 2 }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
                <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: c.state === 'working' ? '#F5A524' : c.state === 'waiting' ? '#22A06B' : dimOf(th) }} />
                <Text style={{ flex: 1, fontSize: 12.5, fontWeight: 600, color: textOf(th), textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{c.dir || c.model || 'Claude'}</Text>
                <Text style={{ fontSize: 11.5, color: dimOf(th) }}>{t(c.state === 'working' ? 'shell.widgets.claude.working' : c.state === 'waiting' ? 'shell.widgets.claude.waiting' : 'shell.widgets.claude.idle')}</Text>
              </View>
              {c.ctx != null ? <Meter th={th} label={c.model || 'ctx'} value={c.ctx / 100} detail={`ctx ${c.ctx} %`} /> : null}
            </View>
          ))}
        </View>
      )}
    </WidgetShell>
  );
}

const BODIES = { clock: ClockWidget, system: SystemWidget, calendar: CalendarWidget, nowplaying: NowPlayingWidget, readaloud: ReadAloudWidget,
  network: NetworkWidget, disks: DisksWidget, temps: TempsWidget, battery: BatteryWidget, reminders: RemindersWidget, clipboard: ClipboardWidget, claude: ClaudeWidget };

// the built-in entries of the catalog (the Notes app adds the sticky note)
for (const type of ['clock', 'system', 'calendar', 'nowplaying', 'readaloud', 'network', 'disks', 'temps', 'battery', 'reminders', 'clipboard', 'claude']) {
  registerWidgetKind({ type, nameKey: `shell.widgets.${type}.name`, descKey: `shell.widgets.${type}.desc`, add: (x, y) => addDeskWidget(type, x, y),
    // offered only where it means something: a battery to show
    ...(type === 'battery' ? { available: () => !!battery.get()?.hasBattery } : null) });
}
