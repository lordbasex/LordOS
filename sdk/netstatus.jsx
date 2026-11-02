// netstatus.jsx — the network item of the menu bar and the blocklists' daily update (0.8.5,
// docs/MONITOR.md "The menu bar network item", docs/FIREWALL.md "Blocklists").
//
// The item is optional and off by default (settings.monitor.menuItem; Settings → Privacy → Monitor
// or the Monitor's view menu). While it is shown, once per second: the machine's byte counters
// (lord.monitor.netTotals(), one small file read) and lord-netperm's notices (firewall.refresh()).
// Hidden, it costs nothing. Its panel shows the sent/received graph of the last five minutes, the
// apps moving bytes now, what was denied in the last ten minutes, the firewall mode, the
// blocklists and a link to the Monitor.
//
// startNetStatus() (startShell) also asks lord-server, once an hour, to download the enabled
// blocklists that are a day old (lord.fw.updateLists; lord-server does nothing when they are fresh).
import { memo, useEffect, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme, popupBg } from './theme.js';
import { t, formatNumber } from './i18n.js';
import { formatBytes } from './system.js';
import { registerStatusItem } from './statusitems.js';
import { Overlay } from './layers.js';
import { Segmented } from './components.jsx';
import { firewall, firewallState } from './monitor.js';
import { wm } from './windows.js';
import { useLauncherApps } from './apps.js';
import { COMPONENTS } from './netmodel.js';

const SAMPLES = 300;                    // one per second: the last five minutes
// What the bar shows ({ rx, tx } as short text): it changes only when the rounded rates do, so an
// idle network redraws nothing. The rates and the five minutes behind them stay out of any store;
// the panel, while open, reads them once per second.
export const netStatus = createStore({ rx: '0', tx: '0' });
const live = { rx: 0, tx: 0, hist: [], n: 0 };   // hist: [[rxRate, txRate], …]; n: samples so far
let timer = null, prev = null, panelOpen = false, ticks = 0;

const shown = () => settings.get()?.monitor?.menuItem === true;

function tick() {
  let s = null;
  try { s = typeof lord !== 'undefined' && lord.monitor?.netTotals ? lord.monitor.netTotals() : null; } catch (e) { s = null; }
  if (s && prev) {
    const dt = Math.max(0.2, (s.t - prev.t) / 1000);
    live.rx = Math.max(0, (s.rx - prev.rx) / dt);
    live.tx = Math.max(0, (s.tx - prev.tx) / dt);
    live.hist.push([live.rx, live.tx]);
    live.n++;
    if (live.hist.length > SAMPLES) live.hist.shift();
    // the bar's text every other second (the panel's numbers every second)
    if (++ticks % 2 === 0) {
      const o = netStatus.get(), n = live.hist.slice(-2), rx = short(n.reduce((a, p) => a + p[0], 0) / n.length),
        tx = short(n.reduce((a, p) => a + p[1], 0) / n.length);
      if (o.rx !== rx || o.tx !== tx) netStatus.set({ rx, tx });
    }
  }
  if (s) prev = s;
  if (panelOpen) firewall.refresh();      // lord-netperm's notices only while the panel shows them
}

function sync() {
  const on = shown();
  if (on && !timer) { prev = null; tick(); timer = setInterval(tick, 1000); }
  else if (!on && timer) { clearInterval(timer); timer = null; prev = null; live.hist = []; netStatus.set({ rx: '0', tx: '0' }); }
}

const rate = (n) => t('monitor.rate', { v: formatBytes(Math.max(0, n || 0)) });

// a compact rate for the bar: "1.2M", "340K", "0" (under 1 KB/s: background chatter); changing the
// text relays out the bar, so it changes only when this rounded value does
function short(n) {
  const v = Math.max(0, n || 0);
  if (v >= 1e9) return `${(v / 1e9).toFixed(1)}G`;
  if (v >= 1e6) return `${(v / 1e6).toFixed(v >= 1e7 ? 0 : 1)}M`;
  if (v >= 1e3) return `${Math.round(v / 1e3)}K`;
  return '0';
}

// ---- the panel ----

// redrawn when a new 5-second bar is complete, not every second (60 bars are 180 nodes)
const Graph = memo(function Graph({ hist }) {
  const th = useTheme();
  // 60 bars of 5 s each (the newest on the right), sent above the line and received below
  const bars = Array.from({ length: 60 }, (_, i) => {
    const end = hist.length - (59 - i) * 5, part = hist.slice(Math.max(0, end - 5), Math.max(0, end));
    const n = part.length || 1;
    return [part.reduce((a, p) => a + p[0], 0) / n, part.reduce((a, p) => a + p[1], 0) / n];
  });
  const peak = Math.max(1, ...bars.map((b) => Math.max(b[0], b[1])));
  const H = 28;
  return (
    <View style={{ height: H * 2 + 1, flexDirection: 'row', alignItems: 'center', gap: 1, direction: 'ltr' }}>
      {bars.map((b, i) => (
        <View key={i} style={{ flex: 1, height: H * 2 + 1 }}>
          <View style={{ height: H, justifyContent: 'flex-end' }}>
            <View style={{ height: b[1] ? Math.max(1, (b[1] / peak) * H) : 0, backgroundColor: '#f97316', borderRadius: 1 }} />
          </View>
          <View style={{ height: 1, backgroundColor: th.separator }} />
          <View style={{ height: H }}>
            <View style={{ height: b[0] ? Math.max(1, (b[0] / peak) * H) : 0, backgroundColor: '#22c55e', borderRadius: 1 }} />
          </View>
        </View>
      ))}
    </View>
  );
}, (a, b) => a.bucket === b.bucket);

function appLabel(id, launcher) {
  if (id === 'lordos') return t('monitor.lordos');
  if (COMPONENTS[id]) return t(COMPONENTS[id].label);
  const a = launcher.find((x) => x.id === id || `lpk:${x.id}` === id || `desktop:${x.id}` === id);
  return a?.name || String(id).replace(/^(desktop|lpk):/, '');
}

function Panel({ onClose }) {
  const th = useTheme();
  const [, setTick] = useState(0);
  useEffect(() => {
    panelOpen = true;
    firewall.refresh();
    const id = setInterval(() => setTick((x) => x + 1), 1000);
    return () => { panelOpen = false; clearInterval(id); };
  }, []);
  const st = live;
  const fw = useStore(firewallState);
  const launcher = useLauncherApps();
  const mode = firewall.state().mode || 'silent';
  const lists = firewall.lists();
  const sum = st.hist.reduce((a, p) => [a[0] + p[0], a[1] + p[1]], [0, 0]);
  const active = Object.entries(fw.counts || {}).filter(([, v]) => Array.isArray(v) && (v[4] || v[5]))
    .sort((a, b) => b[1][4] + b[1][5] - (a[1][4] + a[1][5])).slice(0, 5);
  const now = Date.now();
  const denied = (fw.events || []).filter((e) => e.v === 'deny' && e.at && now - e.at < 10 * 60 * 1000);
  const seenDst = new Map();                  // the latest of each (app, destination), with how many
  for (const e of denied) {
    const k = `${e.app}|${(e.hosts && e.hosts[0]) || e.addr}`;
    seenDst.set(k, { e, n: (seenDst.get(k)?.n || 0) + 1 });
  }
  const recent = [...seenDst.values()].slice(-3).reverse();
  const label = (k) => <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start' }}>{k}</Text>;
  const row = (k, v, color) => (
    <View style={{ flexDirection: 'row', gap: 8 }}>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{k}</Text>
      <Text style={{ fontSize: 12, color: color || th.textDim, whiteSpace: 'nowrap' }}>{v}</Text>
    </View>
  );
  return (
    <View style={{ width: 320, padding: 14, gap: 10 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start' }}>{t('monitor.status.title')}</Text>
        <Text style={{ fontSize: 11, color: '#22c55e' }}>{`↓ ${rate(st.rx)}`}</Text>
        <Text style={{ fontSize: 11, color: '#f97316' }}>{`↑ ${rate(st.tx)}`}</Text>
      </View>
      <Graph hist={st.hist} bucket={Math.floor(st.n / 5)} />
      <Text style={{ fontSize: 10, color: th.textDim, textAlign: 'start' }}>
        {t('monitor.status.lastMinutes', { down: formatBytes(sum[0]), up: formatBytes(sum[1]) })}
      </Text>
      <View style={{ height: 1, backgroundColor: th.separator }} />
      {label(t('monitor.status.activity'))}
      {active.length ? active.map(([id, v]) => row(appLabel(id, launcher), `↓ ${rate(v[4])} · ↑ ${rate(v[5])}`))
        : <Text style={{ fontSize: 12, color: th.textDim, textAlign: 'start' }}>{t('monitor.status.quiet')}</Text>}
      <View style={{ height: 1, backgroundColor: th.separator }} />
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <View style={{ flex: 1 }}>{label(t('monitor.status.denied'))}</View>
        <View style={{ minWidth: 18, height: 16, borderRadius: 8, paddingHorizontal: 5, alignItems: 'center', justifyContent: 'center',
          backgroundColor: denied.length ? 'rgba(255,69,58,0.2)' : th.control }}>
          <Text style={{ fontSize: 10, fontWeight: 700, color: denied.length ? '#ff453a' : th.textDim }}>{formatNumber(denied.length, 0)}</Text>
        </View>
      </View>
      {recent.map(({ e, n }, i) => (
        <View key={i}>{row(`${(e.hosts && e.hosts[0]) || e.addr || '?'}${n > 1 ? ` ×${n}` : ''}`, appLabel(e.app, launcher))}</View>
      ))}
      <View style={{ height: 1, backgroundColor: th.separator }} />
      {label(t('monitor.status.mode'))}
      <Segmented small fill value={mode === 'ask' ? 'silent' : mode} onChange={(m) => firewall.setMode(m)}
        options={[['silent', t('firewall.mode.silent')], ['deny', t('firewall.mode.deny')], ['off', t('firewall.mode.off')]]} />
      <Text style={{ fontSize: 10, color: th.textDim, lineHeight: 1.35, textAlign: 'start' }}>
        {t(mode === 'deny' ? 'firewall.mode.denyHint' : mode === 'off' ? 'firewall.mode.offHint' : 'firewall.mode.silentHint')}
      </Text>
      {lists ? (
        <View onClick={() => { onClose(); wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } }); }}>
          {row(t('monitor.status.lists'), t('monitor.status.listsValue', { lists: formatNumber(lists.lists.filter((l) => l.on).length, 0),
            entries: formatNumber((lists.domains || 0) + (lists.nets || 0), 0) }))}
        </View>
      ) : null}
      <View style={{ height: 1, backgroundColor: th.separator }} />
      <View onClick={() => { onClose(); wm.open('monitor', { args: { tab: 'network', nonce: Date.now() } }); }}>
        <Text style={{ fontSize: 12, color: th.accent, textAlign: 'start' }}>{t('monitor.status.open')}</Text>
      </View>
    </View>
  );
}

function NetStatusItem({ color, height }) {
  const th = useTheme();
  const s = useStore(settings);
  const st = useStore(netStatus);
  const [open, setOpen] = useState(false);
  const btn = useRef(null), panel = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, hh] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + hh; };
      if (!inside(panel.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  if (s?.monitor?.menuItem !== true) return null;
  return (
    <View ref={btn} onClick={() => setOpen((o) => !o)} style={{ height, paddingHorizontal: 6, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ width: 44 }}>
        <Text style={{ fontSize: 9, lineHeight: 1.05, color, fontFamily: 'monospace', textAlign: 'end' }}>{`↑${st.tx}`}</Text>
        <Text style={{ fontSize: 9, lineHeight: 1.05, color, fontFamily: 'monospace', textAlign: 'end' }}>{`↓${st.rx}`}</Text>
      </View>
      {open && (
        <Overlay anchor={btn}>
          <View ref={panel} style={{ position: 'absolute', top: height + 4, right: 0, borderRadius: th.menuRadius || 10, backgroundColor: popupBg(th),
            border: `1px solid ${th.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            <Panel onClose={() => setOpen(false)} />
          </View>
        </Overlay>
      )}
    </View>
  );
}

// ---- start ----

let started = false;

function updateLists() {
  if (!firewall.available()) return;
  firewall.updateLists(false);
  // lord-server swaps the new set in and tells lord-netperm when the job ends; reading the state
  // is what lets it notice, so keep reading while it works
  let n = 0;
  const poll = () => {
    const st = firewall.lists();
    if (st?.busy && n++ < 400) setTimeout(poll, 3000);
  };
  setTimeout(poll, 3000);
}

export function startNetStatus() {
  if (started) return;
  started = true;
  registerStatusItem('network', NetStatusItem, 3);
  sync();
  settings.subscribe(sync);
  setTimeout(updateLists, 20 * 1000);
  setInterval(updateLists, 60 * 60 * 1000);
}

