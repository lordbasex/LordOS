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
// shown), Calendar (this month, days with events marked), Now playing and Read aloud (the clipboard, with lord-say).
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
import { events, usePimQuery } from './calendar.js';

const kinds = [];
export const widgetCatalog = createStore(0);
export function registerWidgetKind(k) {
  if (!k || !k.type || kinds.some((x) => x.type === k.type)) return;
  kinds.push(k);
  widgetCatalog.set(widgetCatalog.get() + 1);
}
export const widgetKinds = () => [...kinds];

const MAX_WIDGETS = 24;
const list = () => (Array.isArray(settings.get().deskWidgets) ? settings.get().deskWidgets : []);
const SIZES = { clock: [240, 112], system: [250, 136], calendar: [256, 248], nowplaying: [320, 116], readaloud: [250, 118] };
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

const BODIES = { clock: ClockWidget, system: SystemWidget, calendar: CalendarWidget, nowplaying: NowPlayingWidget, readaloud: ReadAloudWidget };

// the built-in entries of the catalog (the Notes app adds the sticky note)
for (const type of ['clock', 'system', 'calendar', 'nowplaying', 'readaloud']) {
  registerWidgetKind({ type, nameKey: `shell.widgets.${type}.name`, descKey: `shell.widgets.${type}.desc`, add: (x, y) => addDeskWidget(type, x, y) });
}
