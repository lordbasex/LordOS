// The contents of Celeste's drawers (components/Drawer.jsx slides them out of the frame):
//   top     Dashboard: the user and the date, a month calendar, CPU, memory and battery, what is playing; Media;
//           Performance
//   bottom  Launcher: the apps, "=" a calculator, ">" the theme's actions
//   end     Session: log out (after a confirmation), shut down and restart (the system's 60 s confirmation,
//           PowerDialog), lock
//   end     Sidebar: Wi-Fi, Bluetooth, Do Not Disturb, Night Light, volume, brightness, and the notifications with
//           their buttons
// Each one is mounted only while its drawer is out, so their polls (system information, CPU, media) run only then.
import {
  Image, ScrollView, Text, TextInput, View, addListener, appName, clearNotifyHistory, clipboard, defaultSink,
  dndActive, formatBytes, formatTime, getApp, launchApp, locale, mediaFormatTime, memUsedKB, monthName, monthWeeks,
  nightActive, notifyHistory, notifyPrefs, openCalendarApp, refreshApps, session, setBluetoothPower, setNightManual,
  settings, sound, t as tr, useBacklight, useBattery, useBluetooth, useCpuUsage, useLauncherApps, useSound, useStore,
  useSystemInfo, weekStartFor, weekdayName, wifiTileView, wm, BluetoothGlyph, MoonGlyph, SignalGlyph, SunGlyph,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { calc } from '../model.js';
import { clampN } from '../cae.js';
import { Glyph } from './Glyph.jsx';
import { useWifi } from './Bar.jsx';
import { useMediaEntry } from '../mediaHook.js';

const Card = ({ cx, style, children }) => (
  <View style={{ backgroundColor: cx.surface, borderRadius: 20, padding: 14, ...style }}>{children}</View>
);

function Meter({ cx, label, value, text }) {
  return (
    <View style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row' }}>
        <Text style={{ flex: 1, fontSize: 11, color: cx.sub, textAlign: 'start' }}>{label}</Text>
        <Text style={{ fontSize: 11, fontWeight: 700, color: cx.text }}>{text}</Text>
      </View>
      <View style={{ height: 8, borderRadius: 4, backgroundColor: cx.surfaceHi, direction: 'ltr' }}>
        <View style={{ width: `${clampN(value, 0, 1) * 100}%`, height: 8, borderRadius: 4, backgroundColor: cx.primary }} />
      </View>
    </View>
  );
}

const pct = (v) => `${Math.round(clampN(v, 0, 1) * 100)}%`;

// A horizontal slider in the scheme's colours, laid out left to right in every language (a mouse-driven control)
function HSlider({ cx, value, width, onChange }) {
  const ref = useRef(null);
  const drag = useRef(false);
  const cb = useRef(onChange);
  cb.current = onChange;
  const at = (x) => {
    try {
      const [lx, , w] = lord.getLayout(ref.current.id);
      cb.current(clampN((x - lx) / Math.max(1, w), 0, 1));
    } catch (e) { /* not laid out yet */ }
  };
  useEffect(() => {
    const a = addListener('pointermove', (e) => { if (drag.current) at(e.x); });
    const b = addListener('pointerup', () => { drag.current = false; });
    return () => { a(); b(); };
  }, []);
  const f = clampN(value, 0, 1);
  return (
    <View ref={ref} onMouseDown={(e) => { drag.current = true; at(e.x); }} style={{ width, height: 22, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ height: 14, borderRadius: 7, backgroundColor: cx.surfaceHi }}>
        <View style={{ width: Math.max(14, f * width), height: 14, borderRadius: 7, backgroundColor: cx.primary }} />
      </View>
    </View>
  );
}

// ---------- Dashboard ----------
function Tab({ cx, label, on, onClick }) {
  return (
    <View onClick={onClick} style={{ paddingHorizontal: 16, height: 32, borderRadius: 16, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? cx.container : 'transparent' }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: on ? cx.onContainer : cx.sub }}>{label}</Text>
    </View>
  );
}

function MonthCard({ cx, onClose }) {
  const today = new Date();
  const first = weekStartFor(locale.get().lang);
  const y = today.getFullYear(), m = today.getMonth();
  return (
    <Card cx={cx} style={{ width: 236, gap: 2 }}>
      <Text style={{ fontSize: 13, fontWeight: 700, color: cx.text, marginBottom: 4, textAlign: 'start' }}>{`${monthName(m)} ${y}`}</Text>
      <View style={{ flexDirection: 'row' }}>
        {Array.from({ length: 7 }, (_, i) => (
          <Text key={i} style={{ flex: 1, fontSize: 9.5, color: cx.dim, textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden' }}>{weekdayName((i + first) % 7)}</Text>
        ))}
      </View>
      {monthWeeks(y, m, first).map((wk, r) => (
        <View key={r} style={{ flexDirection: 'row' }}>
          {wk.map((d, i) => {
            const on = d === today.getDate();
            return (
              <View key={i} onClick={d ? () => { onClose(); openCalendarApp(new Date(y, m, d, 9).getTime()); } : undefined}
                style={{ flex: 1, height: 24, alignItems: 'center', justifyContent: 'center' }}>
                {d ? (
                  <View style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? cx.primary : 'transparent' }}>
                    <Text style={{ fontSize: 10.5, fontWeight: on ? 700 : 400, color: on ? cx.onPrimary : cx.text }}>{String(d)}</Text>
                  </View>
                ) : null}
              </View>
            );
          })}
        </View>
      ))}
    </Card>
  );
}

function DashTab({ cx, info, onClose }) {
  const cpu = useCpuUsage(1500);
  const bat = useBattery();
  const media = useMediaEntry();
  const today = new Date();
  const mem = info && info.memTotalKB > 0 ? memUsedKB(info) / info.memTotalKB : 0;
  const user = (info && info.user) || '';
  return (
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <View style={{ gap: 12, width: 170 }}>
        <Card cx={cx} style={{ alignItems: 'center', gap: 6 }}>
          <View style={{ width: 50, height: 50, borderRadius: 25, backgroundColor: cx.container, alignItems: 'center', justifyContent: 'center' }}>
            {user ? <Text style={{ fontSize: 22, fontWeight: 700, color: cx.onContainer }}>{Array.from(user)[0].toUpperCase()}</Text> : <Glyph name="user" size={24} color={cx.onContainer} />}
          </View>
          <Text style={{ fontSize: 14, fontWeight: 700, color: cx.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{user}</Text>
          <Text style={{ fontSize: 10.5, color: cx.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{(info && info.hostname) || ''}</Text>
        </Card>
        <Card cx={cx} style={{ alignItems: 'center', flex: 1, justifyContent: 'center', gap: 2 }}>
          <Text style={{ fontSize: 28, fontWeight: 700, color: cx.primary }}>{formatTime(today)}</Text>
          <Text style={{ fontSize: 11, color: cx.sub }}>{`${weekdayName(today.getDay(), false)} ${today.getDate()} ${monthName(today.getMonth())}`}</Text>
        </Card>
      </View>
      <MonthCard cx={cx} onClose={onClose} />
      <View style={{ gap: 12, flex: 1 }}>
        <Card cx={cx} style={{ gap: 10 }}>
          <Meter cx={cx} label={tr('shell.celeste.dash.cpu')} value={cpu || 0} text={pct(cpu || 0)} />
          <Meter cx={cx} label={tr('shell.celeste.dash.memory')} value={mem} text={pct(mem)} />
          {bat && bat.hasBattery && bat.percent >= 0 ? <Meter cx={cx} label={tr('shell.celeste.dash.battery')} value={bat.percent / 100} text={pct(bat.percent / 100)} /> : null}
        </Card>
        <Card cx={cx} style={{ flex: 1, justifyContent: 'center', gap: 2 }}>
          <Text style={{ fontSize: 10, fontWeight: 700, color: cx.dim, textAlign: 'start' }}>{tr(media ? (media.playing ? 'shell.celeste.media.playing' : 'shell.celeste.media.paused') : 'shell.celeste.dash.media')}</Text>
          <Text style={{ fontSize: 12.5, fontWeight: 700, color: cx.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{media ? media.title : tr('shell.celeste.media.none')}</Text>
          {media && media.artist ? <Text style={{ fontSize: 11, color: cx.sub, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{media.artist}</Text> : null}
        </Card>
      </View>
    </View>
  );
}

function MediaTab({ cx }) {
  const m = useMediaEntry();
  const done = m && m.duration ? clampN(m.pos / m.duration, 0, 1) : 0;
  const Ctl = ({ icon, onClick, big, label }) => (
    <View title={tr(label)} onClick={onClick || undefined}
      style={{ width: big ? 56 : 44, height: big ? 56 : 44, borderRadius: big ? 28 : 22, alignItems: 'center', justifyContent: 'center', backgroundColor: big ? cx.primary : cx.surfaceHi, opacity: onClick ? 1 : 0.4 }}>
      <Glyph name={icon} size={big ? 22 : 18} color={big ? cx.onPrimary : cx.text} />
    </View>
  );
  return (
    <Card cx={cx} style={{ flexDirection: 'row', gap: 22, alignItems: 'center', height: 222, paddingHorizontal: 24 }}>
      <View style={{ width: 170, height: 170, borderRadius: 24, backgroundColor: cx.container, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
        {m && m.image ? <Image src={m.image} style={{ width: 170, height: 170, objectFit: 'cover' }} /> : <Glyph name="play" size={54} color={cx.onContainer} />}
      </View>
      <View style={{ flex: 1, gap: 8 }}>
        <Text style={{ fontSize: 20, fontWeight: 700, color: cx.text, textAlign: 'start', numberOfLines: 2 }}>{m ? m.title : tr('shell.celeste.media.none')}</Text>
        <Text style={{ fontSize: 14, color: cx.sub, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{m ? m.artist || '' : tr('shell.celeste.media.hint')}</Text>
        <View style={{ height: 8, borderRadius: 4, backgroundColor: cx.surfaceHi, marginTop: 6, direction: 'ltr' }}>
          <View style={{ width: `${done * 100}%`, height: 8, borderRadius: 4, backgroundColor: cx.primary }} />
        </View>
        <View style={{ flexDirection: 'row', direction: 'ltr' }}>
          <Text style={{ flex: 1, fontSize: 10.5, color: cx.dim }}>{mediaFormatTime(m ? m.pos : 0)}</Text>
          <Text style={{ fontSize: 10.5, color: cx.dim }}>{mediaFormatTime(m ? m.duration : 0)}</Text>
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, justifyContent: 'center', direction: 'ltr' }}>
          <Ctl icon="prev" label="shell.celeste.media.prev" onClick={m && m.prev} />
          <Ctl icon={m && m.playing ? 'pause' : 'play'} label={m && m.playing ? 'shell.celeste.media.pause' : 'shell.celeste.media.play'} onClick={m && m.toggle} big />
          <Ctl icon="next" label="shell.celeste.media.next" onClick={m && m.next} />
        </View>
      </View>
    </Card>
  );
}

function PerfTab({ cx, info }) {
  const cpu = useCpuUsage(1000);
  const bat = useBattery();
  const mem = info && info.memTotalKB > 0 ? memUsedKB(info) / info.memTotalKB : 0;
  const Big = ({ label, v, sub }) => (
    <Card cx={cx} style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 }}>
      <Text style={{ fontSize: 11, fontWeight: 700, color: cx.dim }}>{label}</Text>
      <Text style={{ fontSize: 36, fontWeight: 700, color: cx.primary }}>{pct(v)}</Text>
      <View style={{ width: '80%', height: 10, borderRadius: 5, backgroundColor: cx.surfaceHi, direction: 'ltr' }}>
        <View style={{ width: `${clampN(v, 0, 1) * 100}%`, height: 10, borderRadius: 5, backgroundColor: cx.primary }} />
      </View>
      <Text style={{ fontSize: 10.5, color: cx.sub, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{sub}</Text>
    </Card>
  );
  return (
    <View style={{ flexDirection: 'row', gap: 12, height: 222 }}>
      <Big label={tr('shell.celeste.dash.cpu')} v={cpu || 0} sub={info && info.cpus ? tr('shell.celeste.dash.cores', { count: info.cpus }) : ''} />
      <Big label={tr('shell.celeste.dash.memory')} v={mem} sub={info && info.memTotalKB ? `${formatBytes(memUsedKB(info) * 1024)} / ${formatBytes(info.memTotalKB * 1024)}` : ''} />
      {bat && bat.hasBattery ? <Big label={tr('shell.celeste.dash.battery')} v={(bat.percent || 0) / 100} sub={tr(bat.charging ? 'shell.celeste.dash.charging' : 'shell.celeste.dash.onBattery')} /> : null}
    </View>
  );
}

export function Dashboard({ cx, onClose }) {
  const info = useSystemInfo(3000);
  const [tab, setTab] = useState(0);
  return (
    <View style={{ padding: 16, paddingTop: 14, gap: 12 }}>
      <View style={{ flexDirection: 'row', gap: 6, justifyContent: 'center' }}>
        <Tab cx={cx} label={tr('shell.celeste.bar.dashboard')} on={tab === 0} onClick={() => setTab(0)} />
        <Tab cx={cx} label={tr('shell.celeste.dash.media')} on={tab === 1} onClick={() => setTab(1)} />
        <Tab cx={cx} label={tr('shell.celeste.dash.performance')} on={tab === 2} onClick={() => setTab(2)} />
      </View>
      {tab === 0 ? <DashTab cx={cx} info={info} onClose={onClose} /> : tab === 1 ? <MediaTab cx={cx} /> : <PerfTab cx={cx} info={info} />}
    </View>
  );
}

// ---------- Launcher ----------
// The theme's actions after ">", by their translated names
export function launcherActions() {
  return [
    { id: 'wallpaper', key: 'shell.celeste.action.wallpaper', icon: 'image' },
    { id: 'lock', key: 'shell.menu.lock', icon: 'lock' },
    { id: 'settings', key: 'shell.menu.settings', icon: 'gear' },
    { id: 'options', key: 'shell.celeste.action.options', icon: 'contrast' },
    { id: 'terminal', key: 'shell.celeste.action.terminal', icon: 'grid' },
    { id: 'dnd', key: 'shell.celeste.action.dnd', icon: 'dnd' },
    { id: 'night', key: 'shell.celeste.action.night', icon: 'moon' },
    { id: 'scheme', key: 'shell.celeste.action.scheme', icon: 'contrast' },
  ].map((a) => ({ ...a, name: tr(a.key) }));
}

export const LAUNCHER_ROW = 46;
// rows: the result count shown (the drawer's height follows it, launcherHeight)
export const launcherHeight = (rows) => 70 + (rows ? rows * LAUNCHER_ROW + 12 : 0);

export function Launcher({ cx, onClose, onAction, onRows }) {
  const apps = useLauncherApps();
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  useEffect(refreshApps, []);
  const raw = q.trim();
  let rows = [];
  let mode = 'apps';
  if (raw.startsWith('>')) {
    mode = 'actions';
    const k = raw.slice(1).trim().toLocaleLowerCase();
    rows = launcherActions().filter((a) => !k || a.name.toLocaleLowerCase().includes(k));
  } else {
    const expr = raw.startsWith('=') ? raw.slice(1) : raw;
    const v = /[+\-*/%^]/.test(expr) || raw.startsWith('=') ? calc(expr) : null;
    if (v !== null) {
      mode = 'calc';
      rows = [{ id: 'calc', name: String(v), sub: tr('shell.celeste.launcher.copy'), value: v }];
    } else {
      const k = raw.toLocaleLowerCase();
      rows = apps.filter((a) => !k || [appName(a), a.name, a.genericName, a.keywords, a.comment].some((x) => x && String(x).toLocaleLowerCase().includes(k))).slice(0, 40);
    }
  }
  const idx = clampN(sel, 0, Math.max(0, rows.length - 1));
  const shown = Math.min(rows.length, 7);
  useEffect(() => { onRows(shown); }, [shown]);
  const run = (r) => {
    if (!r) return;
    onClose();
    if (mode === 'apps') launchApp(r.id);
    else if (mode === 'actions') onAction(r.id);
    else clipboard.set(String(r.value));
  };
  const keyRef = useRef({});
  keyRef.current = { idx, len: rows.length, run: () => run(rows[idx]) };
  useEffect(() => addListener('keydown', (e) => {
    if (e.key === 'ArrowDown') setSel(Math.min(keyRef.current.len - 1, keyRef.current.idx + 1));
    else if (e.key === 'ArrowUp') setSel(Math.max(0, keyRef.current.idx - 1));
  }), []);
  const listH = shown ? shown * LAUNCHER_ROW : 0;
  return (
    <View style={{ padding: 14, gap: 10, flex: 1 }}>
      {shown ? (
        <ScrollView style={{ height: listH }}>
          {rows.map((r, i) => {
            const on = i === idx;
            return (
              <View key={r.id} onClick={() => run(r)} onMouseEnter={() => setSel(i)}
                style={{ height: LAUNCHER_ROW, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 16, backgroundColor: on ? cx.container : 'transparent' }}>
                <View style={{ width: 34, height: 34, alignItems: 'center', justifyContent: 'center' }}>
                  {mode === 'apps' ? (r.Icon ? <r.Icon size={30} /> : <Glyph name="grid" size={22} color={cx.sub} />)
                    : <Glyph name={mode === 'calc' ? 'calc' : r.icon} size={22} color={on ? cx.onContainer : cx.primary} />}
                </View>
                <View style={{ flex: 1 }}>
                  <Text style={{ fontSize: 14, fontWeight: 600, color: on ? cx.onContainer : cx.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{mode === 'apps' ? appName(r) : r.name}</Text>
                  {r.sub ? <Text style={{ fontSize: 10.5, color: on ? cx.onContainer : cx.dim, textAlign: 'start' }}>{r.sub}</Text> : null}
                </View>
              </View>
            );
          })}
        </ScrollView>
      ) : null}
      <View style={{ height: 46, borderRadius: 23, backgroundColor: cx.surfaceHi, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 16 }}>
        <Glyph name="search" size={17} color={cx.sub} />
        <TextInput value={q} onChange={(v) => { setQ(v); setSel(0); }} placeholder={tr('shell.celeste.launcher.placeholder')} autoFocus
          onSubmit={() => keyRef.current.run()} onCancel={onClose} style={{ flex: 1, fontSize: 14, color: cx.text, backgroundColor: 'transparent' }} />
      </View>
    </View>
  );
}

// ---------- Session ----------
// Lock at once; log out after a confirmation (onLogout opens it); restart and shut down through the system's own
// confirmation (session.restart / session.shutdown, PowerDialog), or Settings when this session can't power off.
export const SESSION_ITEMS = ['lock', 'logout', 'restart', 'shutdown'];
export function SessionPanel({ cx, onClose, onLogout }) {
  const [hot, setHot] = useState(null);
  const items = [
    { id: 'lock', icon: 'lock', key: 'shell.menu.lock', run: () => session.lock(), show: session.managed() },
    { id: 'logout', icon: 'logout', key: 'shell.menu.logout', run: onLogout, show: true },
    { id: 'restart', icon: 'restart', key: 'shell.menu.restart', run: () => session.restart(), show: session.canPower() },
    { id: 'shutdown', icon: 'power', key: 'shell.menu.shutdown', run: () => session.shutdown(), show: session.canPower() },
  ].filter((it) => it.show);
  return (
    <View style={{ padding: 16, gap: 12 }}>
      {items.map((it) => (
        <View key={it.id} title={tr(it.key)} onMouseEnter={() => setHot(it.id)} onMouseLeave={() => setHot(null)}
          onClick={() => { onClose(); it.run(); }}
          style={{ width: 66, height: 66, borderRadius: hot === it.id ? 28 : 18, alignItems: 'center', justifyContent: 'center', backgroundColor: hot === it.id ? cx.container : cx.surfaceHi }}>
          <Glyph name={it.icon} size={26} color={hot === it.id ? cx.onContainer : it.id === 'shutdown' ? cx.danger : cx.text} />
        </View>
      ))}
    </View>
  );
}

// ---------- Sidebar ----------
const openSettings = (onClose, section, page) => { onClose(); wm.open('settings', { args: { section, ...(page ? { page } : null) } }); };

function Tile({ cx, label, detail, on, onClick, children }) {
  return (
    <View onClick={onClick} title={detail || label}
      style={{ flex: 1, minWidth: 0, height: 64, borderRadius: 18, alignItems: 'center', justifyContent: 'center', gap: 4, paddingHorizontal: 4, backgroundColor: on ? cx.primary : cx.surfaceHi }}>
      {children(on ? cx.onPrimary : cx.text)}
      <Text style={{ fontSize: 10, fontWeight: 600, color: on ? cx.onPrimary : cx.sub, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: '100%' }}>{label}</Text>
    </View>
  );
}

// One notification of the history, with its buttons (they run the notification's own actions, as in Flow's
// clock panel) and a ✕ that removes it from the history.
function Note({ cx, n, onRemove }) {
  const [hover, setHover] = useState(false);
  const app = n.app ? getApp(n.app) : null;
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ backgroundColor: cx.surface, borderRadius: 16, padding: 12, paddingEnd: 30, marginBottom: 8, gap: 2 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Text style={{ flex: 1, fontSize: 10, fontWeight: 700, color: cx.primary, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{app ? appName(app) : n.label || n.app || ''}</Text>
        <Text style={{ fontSize: 10, color: cx.dim }}>{formatTime(new Date(n.time))}</Text>
      </View>
      <Text style={{ fontSize: 13, fontWeight: 700, color: cx.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{n.title}</Text>
      {n.body ? <Text style={{ fontSize: 11.5, color: cx.sub, textAlign: 'start', numberOfLines: 3 }}>{n.body}</Text> : null}
      {n.actions?.length ? (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 6 }}>
          {n.actions.map((a) => (
            <View key={a.id} onClick={() => { onRemove(); try { a.run?.(); } catch (e) { console.error('notify:', e); } }}
              style={{ height: 26, paddingHorizontal: 12, borderRadius: 13, justifyContent: 'center', backgroundColor: cx.surfaceHi }}>
              <Text style={{ fontSize: 11.5, fontWeight: 600, color: cx.text }}>{a.label}</Text>
            </View>
          ))}
        </View>
      ) : null}
      {hover ? (
        <View title={tr('shell.celeste.side.remove')} onClick={onRemove}
          style={{ position: 'absolute', top: 10, insetInlineEnd: 8, width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', backgroundColor: cx.surfaceHi }}>
          <Text style={{ fontSize: 10, color: cx.sub }}>✕</Text>
        </View>
      ) : null}
    </View>
  );
}

export function Sidebar({ cx, width, onClose }) {
  const hist = useStore(notifyHistory);
  const bt = useBluetooth();
  const snd = useSound();
  const bl = useBacklight();
  const wifi = useWifi();
  const dnd = dndActive();
  const night = nightActive();
  const out = defaultSink(snd);
  const btOn = !!bt?.adapter?.powered;
  const sw = width - 32 - 24 - 28 - 44;
  const wv = wifi ? wifiTileView(wifi) : null;
  const remove = (id) => notifyHistory.set((l) => l.filter((x) => x.id !== id));
  return (
    <View style={{ padding: 16, gap: 12, flex: 1 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        {wifi ? (
          <Tile cx={cx} label={tr('shell.cc.wifi')} detail={wv.ssid || tr(wv.key)} on={wv.on} onClick={() => openSettings(onClose, 'network', 'wifi')}>
            {(c) => <SignalGlyph size={18} color={c} level={wv.bars} off={wv.dim} />}
          </Tile>
        ) : null}
        {bt?.adapter ? (
          <Tile cx={cx} label={tr('shell.cc.bluetooth')} on={btOn} onClick={() => setBluetoothPower(!btOn)}>
            {(c) => <BluetoothGlyph size={18} color={c} />}
          </Tile>
        ) : null}
        <Tile cx={cx} label={tr('shell.celeste.side.dnd')} on={dnd} onClick={() => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !dnd } }))}>
          {(c) => <Glyph name="dnd" size={19} color={c} />}
        </Tile>
        <Tile cx={cx} label={tr('shell.celeste.side.night')} on={night} onClick={() => setNightManual(!night)}>
          {(c) => <MoonGlyph size={18} color={c} bg={night ? cx.primary : cx.surfaceHi} />}
        </Tile>
      </View>
      {out || bl ? (
        <Card cx={cx} style={{ gap: 12, padding: 12 }}>
          {out ? (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }} title={tr('shell.celeste.side.volume')}>
              <View onClick={() => sound.setMute('sink', out.index, !out.mute)} style={{ width: 24, alignItems: 'center' }}>
                <Glyph name={out.mute ? 'volume-muted' : 'volume'} size={18} color={cx.primary} />
              </View>
              <HSlider cx={cx} value={out.level || 0} width={sw} onChange={(v) => sound.setVolume('sink', out.index, v)} />
              <Text style={{ width: 40, fontSize: 11, color: cx.sub, textAlign: 'end' }}>{pct(out.level || 0)}</Text>
            </View>
          ) : null}
          {bl ? (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }} title={tr('shell.celeste.side.brightness')}>
              <View style={{ width: 24, alignItems: 'center' }}><SunGlyph size={17} color={cx.primary} /></View>
              <HSlider cx={cx} value={bl.fraction} width={sw} onChange={bl.set} />
              <Text style={{ width: 40, fontSize: 11, color: cx.sub, textAlign: 'end' }}>{pct(bl.fraction)}</Text>
            </View>
          ) : null}
        </Card>
      ) : null}
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 14, fontWeight: 700, color: cx.text, textAlign: 'start' }}>{tr('shell.celeste.side.notifications')}</Text>
        {hist.length ? (
          <View onClick={() => clearNotifyHistory()} style={{ paddingHorizontal: 12, height: 26, borderRadius: 13, backgroundColor: cx.surfaceHi, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 11, color: cx.sub }}>{tr('shell.celeste.side.clearAll')}</Text>
          </View>
        ) : null}
      </View>
      <ScrollView style={{ flex: 1 }}>
        {hist.length === 0 ? (
          <View style={{ height: 120, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 12, color: cx.dim }}>{tr('shell.celeste.side.empty')}</Text>
          </View>
        ) : hist.slice(0, 50).map((n) => <Note key={n.id} cx={cx} n={n} onRemove={() => remove(n.id)} />)}
      </ScrollView>
    </View>
  );
}
