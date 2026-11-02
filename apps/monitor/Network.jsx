// Network.jsx — the Monitor's Network tab, laid out like a per-app network monitor, with TangoOS colors:
//   izquierda: apps con conexiones (búsqueda, barra de tráfico, columna de regla reservada para
//              la fase 2), cada una desplegable con sus destinos; abajo, totales y barras de
//              subida/bajada por segundo;
//   centro:    mapa con un arco desde este equipo a cada país y un punto por país;
//   derecha:   resumen (sin selección) o el detalle de la app elegida.
// Todo es de solo lectura. El país sale de la base local (DB-IP Lite); ninguna IP sale del
// equipo, por eso no hay DNS inverso (consultarlo mandaría las IPs al servidor DNS).
//
// Modelo para la fase 2 (firewall): cada conexión trae `verdict` ('allow' hoy) y se agrupa por
// app con una clave estable (`appKey`: 'lordos', 'desktop:<id>' o la ruta del ejecutable). Las
// reglas futuras son { appKey, host | país, puerto, protocolo, dirección, verdict }: la columna
// de regla de la lista y los contadores de permitidas/denegadas ya están en su lugar.
import {
  View, Text, ScrollView, useTheme, useLocale, useStore, monitor, monitorConnections, useMonitor,
  formatBytes, formatNumber, tOptional, useSystemInfo, wm, direction, firewall, firewallState, useLauncherApps, Lumo, Button, useAppWindow, useSettings,
  netModel, ConfirmDialog,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { C, fmt, AppGlyph, useCardStyle } from './index.jsx';
import { Glyph } from './icons.jsx';
import { MapView } from './MapView.jsx';
import { TrafficChart, systemBuckets } from './TrafficChart.jsx';
import { RuleSheet } from './RuleSheet.jsx';
import { HistoryControls, HistoryChart, useHistory } from './History.jsx';
import { REMOTE_KEY, RemoteDetail, remoteGroup, useRemoteDesktop } from './RemoteDesktop.jsx';
import { TokenSearch, ViewButton, useNetView, TreeChildren, Child } from './NetTree.jsx';

const { COMPONENTS, isSystemId, filterFlows, groupFlows, sortTree } = netModel;

// (0.8.5) a socket made in an app's scope is that app's (conn.c "cgApp"), whoever holds it: the ones
// Mail's connector made are Mail's, a program in the Terminal is the Terminal's
export const appKeyOf = (c) => c.cgApp || (c.self ? 'lordos' : c.app || c.exe || c.name || '?');
// The row's name: LordOS's own components by their label, LordOS itself, or the app's name.
export const labelOf = (tr, g) => (COMPONENTS[g.key] ? tr(COMPONENTS[g.key].label) : g.key === 'lordos' ? tr('monitor.lordos') : g.name);
const countryName = (cc) => (cc ? tOptional(`countries.${cc}`) || cc : '');

// Where an address is: its country, LAN / this computer, or, for a destination known only from the
// firewall's recent notices, "recent" (or "recently denied").
const placeOf = (tr, h) => (h.cc ? countryName(h.cc) : h.scope ? tr(`monitor.net.scope.${h.scope}`) : tr(h.denied ? 'fwrules.recentDenied' : 'fwrules.recent'));

// Names the system resolver gave for each address, if lord-netperm's notices carry them ("host" or
// "hosts"). The names belong to the system, not to one app (docs/FIREWALL.md, "Where the names come
// from"), so one map serves every app. Without them a row offers a rule for its address instead.
// (0.8.5) An address the resolver reached through a CNAME chain has several names (deb.debian.org →
// debian.map.fastlydns.net): the row shows the shortest, usually the one the app asked for, and
// allNamesOf keeps them all for the search chips and the blocklists.
const allNames = new Map();
function namesOf(events) {
  const out = new Map();
  for (const e of events || []) {
    const list = (Array.isArray(e.hosts) ? e.hosts : e.host ? [e.host] : []).filter((n) => typeof n === 'string' && n);
    if (!e.addr || !list.length) continue;
    const best = [...list].sort((a, b) => a.split('.').length - b.split('.').length || a.length - b.length)[0];
    out.set(e.addr, best);
    if (allNames.size > 4096 && !allNames.has(e.addr)) allNames.delete(allNames.keys().next().value);
    allNames.set(e.addr, list);
  }
  return out;
}
const allNamesOf = (addr, name) => allNames.get(addr) || (name ? [name] : []);

// The sheet's choices for a name: the exact name and, a level up, its parent with every subdomain.
function nameChoices(name) {
  const labels = name.split('.');
  const parent = labels.length > 2 ? labels.slice(1).join('.') : name;
  return [...new Set([name, `*.${parent}`])];
}

// Settings → Privacy, with this app's rules open.
export function openAppRules(key) {
  firewall.focus.set(() => key);
  wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } });
}

// Country and scope of every address seen in an open connection, so an address that only appears
// in lord-netperm's byte counters (its socket already closed) still lands on the map.
const geoSeen = new Map();
function rememberGeo(c) {
  if (!c.remote || c.dir === 'listen') return;
  if (geoSeen.size > 4096 && !geoSeen.has(c.remote)) geoSeen.delete(geoSeen.keys().next().value);
  geoSeen.set(c.remote, { cc: c.cc, scope: c.scope, port: `${c.proto.toUpperCase()} ${c.rport}` });
}

// Raw IP and packet sockets (conn.c): the protocol or ethertype by name where it's a common one.
const IP_PROTOS = { 1: 'ICMP', 2: 'IGMP', 6: 'TCP', 17: 'UDP', 41: 'IPv6', 47: 'GRE', 50: 'ESP', 58: 'ICMPv6', 89: 'OSPF', 112: 'VRRP', 132: 'SCTP', 255: 'IP' };
export const ipProtoName = (n) => IP_PROTOS[n] || `IP ${n}`;
const ETHERTYPES = { 0x0003: 'ALL', 0x0800: 'IPv4', 0x0806: 'ARP', 0x86dd: 'IPv6', 0x888e: 'EAPOL', 0x88cc: 'LLDP', 0x8100: 'VLAN', 0x88f7: 'PTP' };
export const etherName = (n) => ETHERTYPES[n] || `0x${(n || 0).toString(16).padStart(4, '0')}`;

// Groups the connections by app. Without lord-netperm's byte counters (withFirewall), bytes and
// rates come from the open TCP sockets (tcp_info) and the difference with the previous reading.
function useGroups(conns) {
  const prev = useRef({ t: 0, bytes: new Map() });
  return useMemo(() => {
    if (!conns) return { apps: [], list: [] };
    const dt = prev.current.t ? Math.max(0.2, (conns.t - prev.current.t) / 1000) : 0;
    const before = prev.current.bytes, now = new Map();
    const apps = new Map();
    for (const c of conns.list) {
      const key = appKeyOf(c);
      let g = apps.get(key);
      if (!g) {
        const comp = !!COMPONENTS[key] || key === 'lordos';
        g = { key, item: comp ? { app: key } : c, name: comp || (c.self && !c.cgApp) ? null : c.appName || c.name || '?', exe: c.exe, pids: new Set(), conns: [], hosts: new Map(),
          sent: 0, recv: 0, rateTx: 0, rateRx: 0, listen: [], icmp: [], raw: [], packet: [], incoming: 0, verdict: 'allow', programs: new Set() };
        apps.set(key, g);
      }
      if (c.name && c.cgApp && !c.self) g.programs.add(c.name);
      if (!g.exe && c.exe) g.exe = c.exe;
      if (c.pid > 0) g.pids.add(c.pid);
      g.conns.push(c);
      const s = c.sent || 0, r = c.recv || 0;
      now.set(c.id, [s, r]);
      g.sent += s;
      g.recv += r;
      const old = before.get(c.id);
      const rtx = old && dt ? Math.max(0, s - old[0]) / dt : 0, rrx = old && dt ? Math.max(0, r - old[1]) / dt : 0;
      g.rateTx += rtx;
      g.rateRx += rrx;
      if (c.dir === 'listen') { g.listen.push(c); continue; }
      if (c.dir === 'icmp') { g.icmp.push(c); continue; }     // a ping socket: the kernel keeps no destination
      if (c.dir === 'raw') { g.raw.push(c); continue; }       // raw IP (CAP_NET_RAW): lport holds the IP protocol
      if (c.dir === 'packet') { g.packet.push(c); continue; } // a packet socket (CAP_NET_RAW): ethertype and interface
      if (c.dir === 'in') g.incoming++;
      rememberGeo(c);
      const h = g.hosts.get(c.remote) || { remote: c.remote, cc: c.cc, scope: c.scope, ports: new Set(), sent: 0, recv: 0, rateTx: 0, rateRx: 0, count: 0 };
      h.ports.add(`${c.proto.toUpperCase()} ${c.rport}`);
      h.sent += s;
      h.recv += r;
      h.rateTx += rtx;
      h.rateRx += rrx;
      h.count++;
      g.hosts.set(c.remote, h);
    }
    prev.current = { t: conns.t, bytes: now };
    const list = [...apps.values()].sort((a, b) => b.sent + b.recv - (a.sent + a.recv) || b.conns.length - a.conns.length);
    return { apps: list, list: conns.list };
  }, [conns]);
}

// Agrega a cada app su regla y sus contadores del firewall (lord.fw), y las apps que no tienen
// conexiones abiertas pero sí intentos (una app denegada no llega a tener sockets).
// lord-netperm's byte counters for one app ([allowed, denied, rx, tx, rxRate, txRate, flows,
// blocked]): the kernel counts every packet of the app's sockets (TCP and UDP, so QUIC too, and the
// connections that already closed), which replaces the open-socket estimate. flows: [[addr, rx, tx,
// rxRate, txRate, blocked], ...], its busiest addresses. blocked: bytes of open connections the
// kernel dropped because a rule now denies them.
function applyBytes(g, v) {
  g.recv = v[2] || 0;
  g.sent = v[3] || 0;
  g.rateRx = v[4] || 0;
  g.rateTx = v[5] || 0;
  g.bytesFrom = 'kernel';
  g.blocked = v[7] || 0;
  for (const f of Array.isArray(v[6]) ? v[6] : []) {
    if (!Array.isArray(f) || typeof f[0] !== 'string') continue;
    const known = g.hosts.get(f[0]), geo = geoSeen.get(f[0]);
    const h = known ? { ...known } : { remote: f[0], cc: geo?.cc || '', scope: geo?.scope || '', ports: new Set(geo ? [geo.port] : []), count: 0, recent: true };
    h.recv = f[1] || 0;
    h.sent = f[2] || 0;
    h.rateRx = f[3] || 0;
    h.rateTx = f[4] || 0;
    h.blocked = f[5] || 0;
    h.portStats = Array.isArray(f[6]) ? f[6] : null;       // (0.8.5) [[port, proto, dir, rx, tx, rxRate, txRate], ...]
    g.hosts.set(f[0], h);
  }
}

const hasBytes = (fw, v) => !!fw?.status?.bytes && Array.isArray(v) && v.length >= 6;

function withFirewall(groups, fw, launcher) {
  const counts = fw?.counts || {};
  const out = groups.map((g) => {
    const v = counts[g.key];
    const n = { ...g, hosts: new Map(g.hosts), rule: firewall.appRule(g.key), allowed: v?.[0] || 0, denied: v?.[1] || 0 };
    if (hasBytes(fw, v)) applyBytes(n, v);
    return n;
  });
  const have = new Set(out.map((g) => g.key));
  for (const [key, v] of Object.entries(counts)) {
    if (have.has(key) || key === 'lordos' || !Array.isArray(v) || !(v[0] || v[1] || (hasBytes(fw, v) && (v[2] || v[3])))) continue;
    const la = launcher.find((a) => a.id === key || `lpk:${a.id}` === key);
    const comp = !!COMPONENTS[key];
    const name = comp ? null : la?.name || key.replace(/^(desktop|lpk):/, '');
    const g = { key, item: comp ? { app: key } : { app: key, appName: name, name, appIcon: la?.icon || '' }, name, exe: '', pids: new Set(), conns: [], hosts: new Map(), sent: 0, recv: 0,
      rateTx: 0, rateRx: 0, listen: [], incoming: 0, verdict: v[1] && !v[0] ? 'deny' : 'allow', rule: firewall.appRule(key), allowed: v[0] || 0, denied: v[1] || 0,
      programs: new Set(), system: isSystemId(key) };
    if (hasBytes(fw, v)) applyBytes(g, v);
    out.push(g);
  }
  out.sort((a, b) => b.sent + b.recv - (a.sent + a.recv) || b.conns.length - a.conns.length);
  // Short connections (a curl, a DNS query) close before the next sample: the destinations the
  // firewall reported recently are listed too, so they can be given a rule ("recent"; "denied" if
  // the last one was refused).
  const byKey = new Map(out.map((g) => [g.key, g]));
  for (const e of fw?.events || []) {
    const g = byKey.get(e.app);
    if (!g || e.dir !== 'out' || !e.addr) continue;
    const geo = geoSeen.get(e.addr);
    const h = g.hosts.get(e.addr) || { remote: e.addr, cc: geo?.cc || '', scope: geo?.scope || '', ports: new Set(), sent: 0, recv: 0, rateTx: 0, rateRx: 0, count: 0, recent: true };
    h.ports = new Set(h.ports).add(`${String(e.proto).toUpperCase()} ${e.port}`);
    if (h.recent) { h.denied = e.v === 'deny'; h.count++; }
    g.hosts.set(e.addr, h);
  }
  return out;
}

// ---- columna izquierda ----

// Interruptor de la columna Regla: prendido = la app puede usar la red. Escribe la regla
// { app, out, any, allow|deny } (lord.fw); ir y volver no acumula reglas. Las apps sin scope propia
// (un programa suelto, identificado por su ruta) no se pueden filtrar: el interruptor queda fijo.
function RuleSwitch({ g, selected, onConfirm }) {
  const t = useTheme();
  // the essential system services and the VPN can't take a rule: a dash instead of a switch
  if (g.system) return <Text style={{ width: 26, fontSize: 11, color: selected ? '#fff' : t.textDim, textAlign: 'center' }}>—</Text>;
  const on = g.rule !== 'deny';
  const can = firewall.available() && firewall.enforceable(g.key);
  const color = on ? '#30d158' : '#ff453a';
  // blocking a component with a warning (Store and updates: no security updates) asks first
  const flip = () => {
    if (!can) return;
    if (on && COMPONENTS[g.key]?.warn && onConfirm) onConfirm(g);
    else firewall.setAppRule(g.key, on ? 'deny' : 'allow');
  };
  return (
    <View onClick={(e) => { e.stopPropagation(); flip(); }}
      style={{ width: 26, height: 14, borderRadius: 7, backgroundColor: selected ? 'rgba(255,255,255,0.35)' : on ? 'rgba(48,209,88,0.35)' : 'rgba(255,69,58,0.35)',
        opacity: can ? 1 : 0.45, justifyContent: 'center', alignItems: on ? 'flex-end' : 'flex-start', paddingHorizontal: 2, direction: 'ltr' }}>
      <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: selected ? '#fff' : color }} />
    </View>
  );
}

// A tiny live indicator: the app's traffic over the last 12 s, one bar per second.
function Activity({ hist, selected }) {
  const t = useTheme();
  const last = (hist || []).slice(-12);
  while (last.length < 12) last.unshift([0, 0]);
  const peak = Math.max(1, ...last.map((p) => p[0] + p[1]));
  return (
    <View style={{ width: 25, height: 12, flexDirection: 'row', alignItems: 'flex-end', gap: 1, direction: 'ltr' }}>
      {last.map((p, i) => {
        const v = p[0] + p[1];
        return <View key={i} style={{ width: 1, height: v ? Math.max(2, Math.round((v / peak) * 12)) : 1, borderRadius: 1,
          backgroundColor: selected ? 'rgba(255,255,255,0.85)' : v ? (p[1] > p[0] ? C.tx : C.rx) : t.separator }} />;
      })}
    </View>
  );
}

function AppRow({ g, node, ctx, open, selected, onToggle, onSelect, maxBytes, tr, hist, onConfirm, mapColor }) {
  const t = useTheme();
  const label = labelOf(tr, g);
  const total = g.sent + g.recv;
  const active = g.conns.length - g.listen.length;
  const kids = node?.children?.length || 0;
  return (
    <View>
      <View onClick={onSelect} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 40, paddingVertical: 4, paddingHorizontal: 8, borderRadius: 8,
        backgroundColor: selected ? t.accent : 'transparent' }}>
        <View onClick={(e) => { e.stopPropagation(); onToggle(); }} style={{ width: 14, alignItems: 'center' }}>
          <Text style={{ fontSize: 10, color: selected ? '#fff' : t.textDim }}>{kids || g.listen.length ? (open ? '▾' : direction() === 'rtl' ? '◂' : '▸') : ''}</Text>
        </View>
        <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: mapColor || 'transparent' }} />
        <AppGlyph item={g.item} size={20} />
        <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
          {/* the name gets the whole width (two lines at most); the counts go next to the traffic bar */}
          <Text style={{ fontSize: 12, fontWeight: 600, color: selected ? '#fff' : t.text, lineHeight: 1.25, textAlign: 'start' }} numberOfLines={2}>{label}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}>
            {/* barra de tráfico: recibido + enviado, relativa a la app que más movió */}
            <View style={{ flex: 1, minWidth: 0, height: 4, borderRadius: 2, backgroundColor: selected ? 'rgba(255,255,255,0.25)' : t.control, flexDirection: 'row', overflow: 'hidden', direction: 'ltr' }}>
              <View style={{ width: `${(g.recv / Math.max(1, maxBytes)) * 100}%`, height: 4, backgroundColor: C.rx }} />
              <View style={{ width: `${(g.sent / Math.max(1, maxBytes)) * 100}%`, height: 4, backgroundColor: C.tx }} />
            </View>
            {g.system ? (
              <View style={{ paddingHorizontal: 5, height: 14, borderRadius: 7, justifyContent: 'center', backgroundColor: selected ? 'rgba(255,255,255,0.25)' : t.control }}>
                <Text style={{ fontSize: 9, fontWeight: 600, color: selected ? '#fff' : t.textDim }}>{tr('monitor.net.system')}</Text>
              </View>
            ) : (
              <View style={{ paddingHorizontal: 5, height: 14, borderRadius: 7, justifyContent: 'center', backgroundColor: selected ? 'rgba(255,255,255,0.25)' : t.control }}>
                <Text style={{ fontSize: 9, fontWeight: 600, color: selected ? '#fff' : t.textDim }}>{formatNumber(active, 0)}</Text>
              </View>
            )}
            {g.denied ? (
              <View style={{ paddingHorizontal: 5, height: 14, borderRadius: 7, justifyContent: 'center', backgroundColor: selected ? 'rgba(255,255,255,0.25)' : 'rgba(255,69,58,0.22)' }}>
                <Text style={{ fontSize: 9, fontWeight: 600, color: selected ? '#fff' : '#ff453a' }}>{`⊘ ${formatNumber(g.denied, 0)}`}</Text>
              </View>
            ) : null}
          </View>
        </View>
        <Activity hist={hist} selected={selected} />
        <Text style={{ width: 44, fontSize: 10, color: selected ? '#fff' : t.textDim, textAlign: 'end', whiteSpace: 'nowrap' }}>{total ? formatBytes(total) : '—'}</Text>
        {/* regla: permitir / denegar la red a la app (lord-netperm la aplica en el kernel) */}
        <RuleSwitch g={g} selected={selected} onConfirm={onConfirm} />
      </View>
      {open && node ? <TreeChildren node={node} ctx={ctx} /> : null}
      {open && !kids ? <Text style={{ fontSize: 11, color: t.textDim, height: 22, ...(direction() === 'rtl' ? { paddingRight: 40 } : { paddingLeft: 40 }) }}>
        {[g.listen.length ? tr('monitor.net.onlyListening', { ports: g.listen.map((c) => c.lport).join(', ') }) : '',
          g.icmp?.length ? tr('monitor.net.icmpSockets', { count: g.icmp.length }) : '',
          g.raw?.length ? tr('monitor.net.rawSockets', { what: [...new Set(g.raw.map((c) => ipProtoName(c.lport)))].join(', ') }) : '',
          g.packet?.length ? tr('monitor.net.packetSockets', { what: [...new Set(g.packet.map((c) => `${etherName(c.ethertype)}${c.iface ? ` · ${c.iface}` : ''}`))].join(', ') }) : '',
        ].filter(Boolean).join(' · ') || tr('monitor.net.noDestinations')}</Text> : null}
    </View>
  );
}

// Each app's rates over the last minute, one entry per second (from lord-netperm's counters, or the
// open sockets without them): the row's activity indicator and the detail's chart.
function useRateHistory(apps) {
  const ref = useRef({ at: 0, map: new Map() });
  const h = ref.current, now = Date.now();
  if (now - h.at >= 900) {
    h.at = now;
    const seen = new Set();
    for (const g of apps) {
      seen.add(g.key);
      const a = h.map.get(g.key) || [];
      a.push([g.rateRx || 0, g.rateTx || 0]);
      if (a.length > 61) a.shift();
      h.map.set(g.key, a);
    }
    for (const [k, a] of h.map) {
      if (seen.has(k)) continue;
      a.push([0, 0]);
      if (a.length > 61) a.shift();
      if (a.every((p) => !p[0] && !p[1])) h.map.delete(k);
    }
  }
  return h.map;
}

// The machine's traffic: current rates, the mirrored chart of the last 60 s and its totals. It
// redraws at the 4 samples per second with the window in front (1 per second behind). With a
// history zoom (0.8.2) the chart shows that window from the history file instead.
function SystemTraffic({ tr, range, setRange, end, setEnd, sysHist }) {
  const t = useTheme();
  const { focused } = useAppWindow() || {};
  const samples = useMonitor({ everyMs: range !== 'live' ? 0 : focused === false ? 1000 : 250 }) || [];
  const last = samples[samples.length - 1];
  const { buckets, shift, totals } = systemBuckets(samples, 60);
  return (
    <View style={{ gap: 6 }}>
      {range === 'live' ? (
        <View style={{ flexDirection: 'row', gap: 14, paddingHorizontal: 2 }}>
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 10, color: t.textDim }}>{`↓ ${tr('monitor.received')}`}</Text>
            <Text style={{ fontSize: 14, fontWeight: 600, color: C.rx }}>{fmt.rate(tr, last?.net.rx)}</Text>
          </View>
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 10, color: t.textDim }}>{`↑ ${tr('monitor.sent')}`}</Text>
            <Text style={{ fontSize: 14, fontWeight: 600, color: C.tx }}>{fmt.rate(tr, last?.net.tx)}</Text>
          </View>
        </View>
      ) : null}
      <HistoryControls range={range} setRange={setRange} end={end} setEnd={setEnd} oldest={sysHist?.h?.oldest || 0} />
      {range === 'live' ? <TrafficChart buckets={buckets} shift={shift} totals={totals} seconds={60} />
        : <HistoryChart data={sysHist} range={range} />}
    </View>
  );
}

// ---- lugares (países con conexiones) ----

// Each app's color on the map (its arcs) and in the list (the dot before its icon): the order in which
// apps first reached the internet in this Monitor, so the first twelve are all different and an app
// keeps its color while Monitor is open. Orange and green stay for the moving dots (sent, received).
const MAP_COLORS = ['#0a84ff', '#bf5af2', '#ff375f', '#64d2ff', '#ffd60a', '#5e5ce6', '#66d4cf', '#ac8e68', '#ff6482', '#da8fff', '#a2845e', '#e5e5ea'];
const mapColorOf = new Map();
export function appMapColor(key) {
  if (!mapColorOf.has(key)) mapColorOf.set(key, MAP_COLORS[mapColorOf.size % MAP_COLORS.length]);
  return mapColorOf.get(key);
}

// The map's places: every app's (or the selected app's) internet hosts by country; with a search or a
// filter, only the hosts of the flows it shows (`only`: a Set of "app|address").
// Short connections (a page, a curl, a package download) often close before their own rate is
// measured, while the app's rate (its cgroup counters) is always there: what the hosts' rates don't
// account for is spread over the app's internet hosts by their bytes, so the arcs move whenever the app
// does (an estimate per country, the direction is exact).
function placesOf(groups, appKey, only) {
  const out = new Map();
  for (const g of groups) {
    if (appKey && g.key !== appKey) continue;
    const net = [...g.hosts.values()].filter((h) => h.scope === 'internet' && h.cc);
    const share = (dir) => {
      const own = net.reduce((a, h) => a + (h[dir === 'rx' ? 'rateRx' : 'rateTx'] || 0), 0);
      const left = Math.max(0, (dir === 'rx' ? g.rateRx : g.rateTx) - own || 0);
      const bytes = net.reduce((a, h) => a + (dir === 'rx' ? h.recv : h.sent) + 1, 0);
      return (h) => (left > 0 && bytes > 0 ? left * ((dir === 'rx' ? h.recv : h.sent) + 1) / bytes : 0);
    };
    const extraRx = share('rx'), extraTx = share('tx');
    for (const h of net) {
      if (only && !only.has(`${g.key}|${h.remote}`)) continue;
      const p = out.get(h.cc) || { cc: h.cc, count: 0, sent: 0, recv: 0, rateTx: 0, rateRx: 0, apps: new Map(), hosts: [], links: new Map() };
      const l = p.links.get(g.key) || { app: g.key, color: appMapColor(g.key), tx: 0, rx: 0, bytes: 0 };
      l.tx += (h.rateTx || 0) + extraTx(h);
      l.rx += (h.rateRx || 0) + extraRx(h);
      l.bytes += h.sent + h.recv;
      p.links.set(g.key, l);
      p.count += h.count;
      p.sent += h.sent;
      p.recv += h.recv;
      p.rateTx += (h.rateTx || 0) + extraTx(h);
      p.rateRx += (h.rateRx || 0) + extraRx(h);
      p.apps.set(g.key, g);
      p.hosts.push({ ...h, app: g });
      out.set(h.cc, p);
    }
  }
  return [...out.values()].sort((x, y) => y.count - x.count || y.sent + y.recv - (x.sent + x.recv));
}

// ---- columna derecha ----

// A section of the right panel. With an id it folds: a click on its header hides the content, and
// which ones are folded is remembered (settings.monitor.netFolded), so they stay that way for every
// app and the next time Monitor opens.
function Section({ id, title, children }) {
  const t = useTheme();
  const [s, set] = useSettings();
  const m = s.monitor || {};
  const folded = !!id && (m.netFolded || []).includes(id);
  const toggle = () => {
    const list = (m.netFolded || []).filter((x) => x !== id);
    set({ monitor: { ...m, netFolded: folded ? list : [...list, id] } });
  };
  if (!id) {
    return (
      <View style={{ gap: 6 }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim }}>{title}</Text>
        {children}
      </View>
    );
  }
  return (
    <View style={{ gap: 6 }}>
      <View onClick={toggle} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, minHeight: 18, paddingBottom: folded ? 0 : 2,
        borderBottomWidth: 1, borderBottomColor: t.separator }}>
        <Text style={{ width: 12, fontSize: 9, color: t.textDim }}>{folded ? (direction() === 'rtl' ? '◂' : '▸') : '▾'}</Text>
        <Text style={{ flex: 1, minWidth: 0, fontSize: 11, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{title}</Text>
      </View>
      {folded ? null : children}
    </View>
  );
}

// The right panel's text column (270 − padding; the overlay scroll bar sits in the padding) and a rough width per
// character at 12 px: a pair that wouldn't fit on one line goes on two (the key, then the value)
// instead of wrapping inside the row, which the engine measures as one line (the text then ran
// over the next row).
const PANEL_TEXT_W = 244;
const fitsOneLine = (k, v, mono) => String(k).length * 6.3 + 8 + String(v).length * (mono ? 7.3 : 6.9) <= PANEL_TEXT_W;

function KV({ k, v, mono, color }) {
  const t = useTheme();
  if (!fitsOneLine(k, v, mono)) {
    return (
      <View style={{ gap: 2 }}>
        <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.35 }}>{k}</Text>
        <Text style={{ fontSize: 12, fontWeight: 600, color: color || t.text, textAlign: 'end', fontFamily: mono ? 'monospace' : undefined, lineHeight: 1.35 }}>{v}</Text>
      </View>
    );
  }
  return (
    <View style={{ flexDirection: 'row', gap: 8, alignItems: 'flex-start' }}>
      <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap' }}>{k}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 12, fontWeight: 600, color: color || t.text, textAlign: 'end', fontFamily: mono ? 'monospace' : undefined, whiteSpace: 'nowrap', overflow: 'hidden' }}>{v}</Text>
    </View>
  );
}

const upDown = (tr, g) => tr('monitor.net.downUp', { down: formatBytes(g.recv || 0), up: formatBytes(g.sent || 0) });

function Summary({ groups, conns, last, tr, fw, names, range, sysHist, launcher, lists }) {
  const t = useTheme();
  let fwAllowed = 0, fwDenied = 0;
  for (const v of Object.values(fw?.counts || {})) { fwAllowed += v[0] || 0; fwDenied += v[1] || 0; }
  const fwOn = !!fw?.status?.bpf;
  const all = conns?.list || [];
  const open = all.filter((c) => c.dir !== 'listen');
  const incoming = all.filter((c) => c.dir === 'in').length;
  const listening = all.filter((c) => c.dir === 'listen').length;
  const hosts = [];
  for (const g of groups) for (const h of g.hosts.values()) hosts.push({ ...h, app: g });
  hosts.sort((a, b) => b.sent + b.recv - (a.sent + a.recv));
  if (hosts.some((h) => h.sent + h.recv)) hosts.splice(0, hosts.length, ...hosts.filter((h) => h.sent + h.recv));   // with traffic, only those that moved bytes
  const denied = (fw?.events || []).filter((e) => e.v === 'deny' && e.dir === 'out' && (!e.at || Date.now() - e.at < 10 * 60 * 1000));
  const byDst = new Map();                    // the latest of each (app, destination), with how many
  for (const e of denied) {
    const k = `${e.app}|${(e.hosts && e.hosts[0]) || e.addr}`;
    byDst.set(k, { e, n: (byDst.get(k)?.n || 0) + 1 });
  }
  const deniedRecent = [...byDst.values()].slice(-4).reverse();
  return (
    <>
      <Section id="traffic" title={tr('monitor.net.traffic')}>
        <KV k={`↓ ${tr('monitor.received')}`} v={fmt.rate(tr, last.net.rx)} color={C.rx} />
        <KV k={`↑ ${tr('monitor.sent')}`} v={fmt.rate(tr, last.net.tx)} color={C.tx} />
        <KV k={tr('monitor.net.sinceBoot')} v={`↓ ${formatBytes(last.net.rxTotal)} · ↑ ${formatBytes(last.net.txTotal)}`} />
      </Section>
      <Section id="connections" title={tr('monitor.net.connections')}>
        <KV k={tr('monitor.net.allowed')} v={fwOn ? formatNumber(fwAllowed, 0) : formatNumber(open.length, 0)} />
        <KV k={tr('monitor.net.denied')} v={fwOn ? formatNumber(fwDenied, 0) : tr('firewall.off')} color={fwOn && fwDenied ? '#ff453a' : t.textDim} />
        <KV k={tr('monitor.net.incoming')} v={formatNumber(incoming, 0)} />
        <KV k={tr('monitor.net.listening')} v={formatNumber(listening, 0)} />
        {conns?.others ? <KV k={tr('monitor.net.others')} v={formatNumber(conns.others, 0)} color={t.textDim} /> : null}
        {fwOn && !fwDenied ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 4 }}>
            <Lumo pose="shield" size={40} />
            <Text style={{ flex: 1, fontSize: 11, color: t.textDim, lineHeight: 1.35 }}>{tr('monitor.net.allGood')}</Text>
          </View>
        ) : null}
      </Section>
      <Section id="topApps" title={tr('monitor.net.topApps')}>
        {groups.slice(0, 5).map((g) => (
          <View key={g.key} style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 6 }}>
            <View style={{ paddingTop: 1 }}><AppGlyph item={g.item} size={14} /></View>
            <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
              <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>{labelOf(tr, g)}</Text>
              <Text style={{ fontSize: 10, color: t.textDim, whiteSpace: 'nowrap', textAlign: 'start' }}>{upDown(tr, g)}</Text>
            </View>
          </View>
        ))}
        {!groups.length && <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.none')}</Text>}
      </Section>
      {range !== 'live' ? (
        <Section id="topApps" title={tr('monitor.hist.topApps', { range: tr(`monitor.hist.${range}`) })}>
          {(sysHist?.h?.top || []).slice(0, 6).map(([id, rx, tx]) => {
            const la = launcher.find((a) => a.id === id || `lpk:${a.id}` === id || `desktop:${a.id}` === id);
            const item = { app: id, appName: la?.name, name: la?.name || id, appIcon: la?.icon || '', self: id === 'lordos' };
            return (
              <View key={id} style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 6 }}>
                <View style={{ paddingTop: 1 }}><AppGlyph item={item} size={14} /></View>
                <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
                  <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>{id === 'lordos' ? tr('monitor.lordos') : la?.name || id.replace(/^(desktop|lpk):/, '')}</Text>
                  <Text style={{ fontSize: 10, color: t.textDim, whiteSpace: 'nowrap', textAlign: 'start' }}>{upDown(tr, { recv: rx, sent: tx })}</Text>
                </View>
              </View>
            );
          })}
          {!(sysHist?.h?.top || []).length && <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.none')}</Text>}
        </Section>
      ) : null}
      <Section id="denied" title={tr('monitor.net.recentlyDenied')}>
        {denied.length ? deniedRecent.map(({ e, n }, i) => (
          <View key={i} style={{ gap: 1 }}>
            <Text style={{ fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', lineHeight: 1.3 }} numberOfLines={2}>
              {`${(e.hosts && e.hosts[0]) || e.addr || '?'}${n > 1 ? ` ×${n}` : ''}`}</Text>
            <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>
              {`${labelOf(tr, groups.find((g) => g.key === e.app) || { key: e.app, name: e.app })} · ${tr(`monitor.net.by.${e.by === 'list' || e.by === 'restricted' || e.by === 'mode' ? e.by : 'rule'}`)}`}
            </Text>
          </View>
        )) : <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.noneDenied')}</Text>}
      </Section>
      <Section id="blocklists" title={tr('monitor.net.blocklists')}>
        <KV k={tr('monitor.net.listsOn')} v={formatNumber((lists?.lists || []).filter((l) => l.on).length, 0)} />
        <KV k={tr('monitor.net.listsEntries')} v={formatNumber((lists?.domains || 0) + (lists?.nets || 0), 0)} />
        <View onClick={() => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } })} style={{ flexDirection: 'row', justifyContent: 'flex-end' }}>
          <Text style={{ fontSize: 11, color: t.accent }}>{tr('monitor.net.listsManage')}</Text>
        </View>
      </Section>
      <Section id="destinations" title={tr('monitor.net.topHosts')}>
        {hosts.slice(0, 6).map((h) => (
          <View key={`${h.app.key}${h.remote}`} style={{ gap: 1 }}>
            <Text style={{ fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', lineHeight: 1.3 }} numberOfLines={2}>{names.get(h.remote) || h.remote}</Text>
            <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>{`${upDown(tr, h)} · ${placeOf(tr, h)} · ${labelOf(tr, h.app)}`}</Text>
          </View>
        ))}
        {!hosts.length && <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.none')}</Text>}
      </Section>
    </>
  );
}

// One address of the selected app: its name (if the resolver gave one) and, selected, the
// "Allow / Block this domain" actions (or "this address" when no name is known).
function HostRow({ h, name, selected, onSelect, can, onRule, tr }) {
  const t = useTheme();
  const what = name ? 'Domain' : 'Address';
  return (
    <View style={{ gap: 4, paddingVertical: 3, paddingHorizontal: 6, marginHorizontal: -6, borderRadius: 6, backgroundColor: selected ? (t.mode === 'dark' ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.04)') : 'transparent' }}>
      <View onClick={onSelect} style={{ flexDirection: 'row', gap: 6, alignItems: 'center' }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', lineHeight: 1.3 }} numberOfLines={2}>{name || h.remote}</Text>
          {name ? <Text style={{ fontSize: 10, color: t.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', lineHeight: 1.3 }} numberOfLines={2}>{h.remote}</Text> : null}
        </View>
        <Text style={{ fontSize: 10, color: t.textDim, whiteSpace: 'nowrap' }}>{placeOf(tr, h)}</Text>
      </View>
      {selected && can ? (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingBottom: 2 }}>
          <Button small label={tr(`fwrules.allowThis${what}`)} onClick={() => onRule('allow')} />
          <Button small label={tr(`fwrules.blockThis${what}`)} onClick={() => onRule('deny')} />
        </View>
      ) : null}
    </View>
  );
}

function Detail({ g, tr, user, names, host, onHost, onRule, hist, range, end }) {
  const t = useTheme();
  const can = firewall.available() && firewall.enforceable(g.key);
  const comp = COMPONENTS[g.key];
  let dns = [];
  if (g.key === 'system:dns') { try { dns = (lord.net?.dns?.()?.servers || []).slice(0, 4); } catch (e) { dns = []; } }
  const ports = new Set(), local = new Set();
  for (const c of g.conns) {
    if (c.dir === 'listen') local.add(`${c.proto.toUpperCase()} ${c.lport}`);
    else ports.add(`${c.proto.toUpperCase()} ${c.rport}`);
  }
  const hosts = [...g.hosts.values()].sort((a, b) => b.sent + b.recv - (a.sent + a.recv));
  return (
    <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <AppGlyph item={g.item} size={32} />
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 15, fontWeight: 600, color: t.text, lineHeight: 1.25, textAlign: 'start' }} numberOfLines={2}>{labelOf(tr, g)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim }}>{g.system ? tr('monitor.net.systemService') : tr('monitor.net.connCount', { count: g.conns.length - g.listen.length })}</Text>
        </View>
      </View>
      {comp ? <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr(comp.hint)}</Text> : null}
      {g.key === 'lordos:terminal' && g.programs?.size ? <KV k={tr('monitor.net.programs')} v={[...g.programs].join(', ')} /> : null}
      {g.key === 'system:dns' && dns.length ? <KV k={tr('monitor.net.dnsServers')} v={dns.join(', ')} mono /> : null}
      {g.tunnels ? g.tunnels.map((v) => <KV key={v.name} k={v.name} v={`↓ ${formatBytes(v.rx)} · ↑ ${formatBytes(v.tx)}`} mono />) : null}
      {g.system ? null : <Section id="process" title={tr('monitor.net.process')}>
        <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.path')}</Text>
        <Text style={{ fontSize: 11, color: t.text, fontFamily: 'monospace', lineHeight: 1.35, direction: 'ltr', textAlign: 'start' }}>{g.exe || '—'}</Text>
        <KV k={tr('monitor.user')} v={user || '—'} />
        <KV k={tr('monitor.pid')} v={[...g.pids].join(', ') || '—'} />
      </Section>}
      {g.system ? null : <Section id="rule" title={tr('monitor.net.rule')}>
        <KV k={tr('monitor.net.rule')} v={tr(g.rule === 'deny' ? 'firewall.rule.deny' : g.rule === 'allow' ? 'firewall.rule.allow' : 'firewall.rule.none')}
          color={g.rule === 'deny' ? '#ff453a' : '#30d158'} />
        {g.allowed || g.denied ? <KV k={tr('firewall.counts')} v={tr('firewall.countsValue', { allowed: g.allowed || 0, denied: g.denied || 0 })} /> : null}
        {g.blocked ? <KV k={tr('firewall.blocked')} v={formatBytes(g.blocked)} color="#ff453a" /> : null}
        {can ? (
          <View onClick={() => openAppRules(g.key)} style={{ flexDirection: 'row', justifyContent: 'flex-end' }}>
            <Text style={{ fontSize: 11, color: t.accent }}>{tr('fwrules.appRulesLink', { count: firewall.rules(g.key).length })}</Text>
          </View>
        ) : null}
      </Section>}
      <Section id="appConnections" title={tr('monitor.net.connections')}>
        <KV k={tr('monitor.net.active')} v={formatNumber(g.conns.length - g.listen.length, 0)} />
        <KV k={tr('monitor.net.incoming')} v={formatNumber(g.conns.filter((c) => c.dir === 'in').length, 0)} />
        <KV k={tr('monitor.net.listening')} v={formatNumber(g.listen.length, 0)} />
        <KV k={tr('monitor.net.destinationsCount')} v={formatNumber(g.hosts.size, 0)} />
      </Section>
      <Section id="data" title={tr('monitor.net.data')}>
        <KV k={`↓ ${tr('monitor.received')}`} v={`${formatBytes(g.recv)} · ${fmt.rate(tr, g.rateRx)}`} color={C.rx} />
        <KV k={`↑ ${tr('monitor.sent')}`} v={`${formatBytes(g.sent)} · ${fmt.rate(tr, g.rateTx)}`} color={C.tx} />
        {range === 'live' ? <AppTraffic hist={hist} /> : <AppHistory app={g.key} range={range} end={end} />}
      </Section>
      <Section id="ports" title={tr('monitor.net.ports')}>
        <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{[...ports].join(' · ') || '—'}</Text>
        {local.size ? <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.4 }}>{tr('monitor.net.listensOn', { ports: [...local].join(' · ') })}</Text> : null}
      </Section>
      <Section id="destinations" title={tr('monitor.net.destinations')}>
        {hosts.slice(0, 10).map((h) => (
          <HostRow key={h.remote} h={h} name={names.get(h.remote)} selected={host === h.remote} can={can} tr={tr}
            onSelect={() => onHost(host === h.remote ? null : h.remote)} onRule={(action) => onRule(h.remote, action)} />
        ))}
        {!hosts.length && <Text style={{ fontSize: 12, color: t.textDim }}>—</Text>}
        {hosts.length > 0 && can ? <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.35 }}>{tr('fwrules.pickAddress')}</Text> : null}
      </Section>
    </>
  );
}

// The selected app's last minute, in the same mirrored chart as the machine's (one bar per second).
function AppTraffic({ hist }) {
  const list = (hist || []).slice(-60);
  const buckets = Array.from({ length: 60 - list.length }, () => [0, 0]).concat(list);
  return <TrafficChart buckets={buckets} height={40} seconds={60} />;
}

// The selected app's share of the history window (the same zoom as the machine's chart).
function AppHistory({ app, range, end }) {
  const d = useHistory(range, end, app);
  return <HistoryChart data={d} range={range} height={40} />;
}

function PlaceDetail({ p, tr }) {
  const t = useTheme();
  const hosts = [...p.hosts].sort((a, b) => b.sent + b.recv - (a.sent + a.recv));
  return (
    <>
      <View style={{ gap: 2 }}>
        <Text style={{ fontSize: 15, fontWeight: 600, color: t.text, lineHeight: 1.25, textAlign: 'start' }} numberOfLines={2}>{countryName(p.cc)}</Text>
        <Text style={{ fontSize: 11, color: t.textDim }}>{tr('monitor.net.connCount', { count: p.count })}</Text>
      </View>
      <Section id="data" title={tr('monitor.net.data')}>
        <KV k={`↓ ${tr('monitor.received')}`} v={formatBytes(p.recv)} color={C.rx} />
        <KV k={`↑ ${tr('monitor.sent')}`} v={formatBytes(p.sent)} color={C.tx} />
      </Section>
      <Section id="placeApps" title={tr('monitor.net.placeApps')}>
        {[...p.apps.values()].map((g) => (
          <View key={g.key} style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 6 }}>
            <View style={{ paddingTop: 1 }}><AppGlyph item={g.item} size={14} /></View>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: t.text, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>{g.key === 'lordos' ? tr('monitor.lordos') : g.name}</Text>
          </View>
        ))}
      </Section>
      <Section id="destinations" title={tr('monitor.net.destinations')}>
        {hosts.slice(0, 14).map((h) => (
          <View key={`${h.app.key}${h.remote}`} style={{ gap: 1 }}>
            <View style={{ flexDirection: 'row', gap: 6 }}>
              <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start' }} numberOfLines={2}>{h.remote}</Text>
              <Text style={{ fontSize: 11, color: t.textDim }}>{formatBytes(h.sent + h.recv)}</Text>
            </View>
            <Text style={{ fontSize: 10, color: t.textDim, lineHeight: 1.3, textAlign: 'start' }} numberOfLines={2}>{`${[...h.ports].join(' · ')} · ${h.app.key === 'lordos' ? tr('monitor.lordos') : h.app.name}`}</Text>
          </View>
        ))}
      </Section>
    </>
  );
}

// ---- the list's flows (0.8.5) ----

// When each destination last moved bytes (ms), for "Most recent".
const lastSeen = new Map();
function seen(key, active) {
  if (active) { if (lastSeen.size > 8192 && !lastSeen.has(key)) lastSeen.delete(lastSeen.keys().next().value); lastSeen.set(key, Date.now()); }
  return lastSeen.get(key) || 0;
}

// One flow per (app, address, port) for sdk/netmodel.js: the kernel's per-port counters when
// lord-netperm sends them (COUNTS), the open sockets' ports otherwise. Apps with no destination yet
// (only listening, or a component that hasn't connected) keep a flow with no address, so their row stays.
function flowsOf(apps, names, tr) {
  const out = [];
  const listedCache = new Map();
  const listed = (list, addr) => {
    const k = `${list.join(',')}|${addr}`;
    if (!listedCache.has(k)) listedCache.set(k, firewall.listMatch(list, addr));
    return listedCache.get(k);
  };
  for (const g of apps) {
    const base = { app: g.key, appName: labelOf(tr, g), system: !!g.system };
    let any = false;
    for (const h of g.hosts.values()) {
      if (!h.remote) continue;
      any = true;
      const name = names.get(h.remote) || null;
      const all = allNamesOf(h.remote, name);
      const common = { ...base, name, names: all, addr: h.remote, cc: h.cc || '', scope: h.scope || '', denied: h.denied ? 1 : 0, blocked: h.blocked || 0, listed: listed(all, h.remote) };
      const ps = (h.portStats || []).filter((p) => Array.isArray(p) && p.length >= 7);
      if (ps.length) {
        const share = Math.max(1, ps.length);
        for (const p of ps) {
          const active = (p[5] || 0) + (p[6] || 0) > 0;
          out.push({ ...common, port: Number(p[0]) || 0, proto: p[1] === 'udp' ? 'udp' : p[1] === 'tcp' ? 'tcp' : 'other', dir: p[2] === 'in' ? 'in' : 'out',
            rx: p[3] || 0, tx: p[4] || 0, rateRx: p[5] || 0, rateTx: p[6] || 0, conns: Math.ceil((h.count || 0) / share), last: seen(`${g.key}|${h.remote}|${p[0]}`, active) });
        }
        continue;
      }
      const ports = [...(h.ports || [])].map((x) => /^(TCP|UDP) (\d+)$/.exec(x)).filter(Boolean);
      const active = (h.rateRx || 0) + (h.rateTx || 0) > 0;
      const dir = g.conns.some((c) => c.remote === h.remote && c.dir === 'in') ? 'in' : 'out';
      if (!ports.length) ports.push([null, 'TCP', '0']);
      ports.forEach((m, i) => out.push({ ...common, port: Number(m[2]) || 0, proto: m[1] === 'UDP' ? 'udp' : 'tcp', dir,
        rx: i ? 0 : h.recv || 0, tx: i ? 0 : h.sent || 0, rateRx: i ? 0 : h.rateRx || 0, rateTx: i ? 0 : h.rateTx || 0, conns: i ? 0 : h.count || 0,
        last: seen(`${g.key}|${h.remote}`, active) }));
    }
    if (!any) out.push({ ...base, name: null, addr: '', port: 0, proto: 'other', dir: g.listen.length ? 'in' : 'out', rx: g.recv || 0, tx: g.sent || 0,
      rateRx: g.rateRx || 0, rateTx: g.rateTx || 0, conns: 0, denied: g.denied || 0, cc: '', scope: '', last: 0, listed: null, empty: true });
  }
  return out;
}

// The VPN tunnels as one row (0.8.5): LordOS's own VPN interfaces (Settings → Network), their
// interface counters and rates. Shown, not blockable: the tunnel is the system's network.
function useVpnGroup(tr) {
  const [st, setSt] = useState({ at: 0, list: [] });
  useEffect(() => {
    const prev = new Map();
    const read = () => {
      let ifs = [];
      try { ifs = typeof lord !== 'undefined' && lord.net ? lord.net.interfaces() || [] : []; } catch (e) { ifs = []; }
      const now = Date.now();
      const list = ifs.filter((i) => i.managed === 'lordos' && (i.type === 'wireguard' || i.type === 'tun' || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name)) && i.up)
        .map((i) => {
          const p = prev.get(i.name);
          const dt = p ? Math.max(0.5, (now - p.t) / 1000) : 0;
          const r = { name: i.name, rx: i.rxBytes || 0, tx: i.txBytes || 0, rateRx: dt ? Math.max(0, (i.rxBytes - p.rx) / dt) : 0, rateTx: dt ? Math.max(0, (i.txBytes - p.tx) / dt) : 0 };
          prev.set(i.name, { t: now, rx: i.rxBytes || 0, tx: i.txBytes || 0 });
          return r;
        });
      setSt({ at: now, list });
    };
    read();
    const id = setInterval(read, 2000);
    return () => clearInterval(id);
  }, []);
  if (!st.list.length) return null;
  const g = { key: 'lordos:vpn', item: { app: 'lordos:vpn' }, name: null, exe: '', pids: new Set(), conns: [], hosts: new Map(), listen: [], incoming: 0,
    sent: 0, recv: 0, rateTx: 0, rateRx: 0, rule: null, allowed: 0, denied: 0, programs: new Set(), system: true, tunnels: st.list };
  for (const v of st.list) {
    g.recv += v.rx; g.sent += v.tx; g.rateRx += v.rateRx; g.rateTx += v.rateTx;
    g.hosts.set(v.name, { remote: v.name, cc: '', scope: 'local', ports: new Set(), recv: v.rx, sent: v.tx, rateRx: v.rateRx, rateTx: v.rateTx, count: 1 });
  }
  void tr;
  return g;
}

// ---- pestaña ----

export function NetworkTab({ last, prefs }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const conns = useStore(monitorConnections);
  const fw = useStore(firewallState);
  const info = monitor.info();
  const sys = useSystemInfo(60000);
  const { apps: connApps } = useGroups(conns);
  const launcher = useLauncherApps();
  const fwApps = useMemo(() => withFirewall(connApps, fw, launcher), [connApps, fw, launcher]);
  // the remote desktop (lord-sessiond, root) as one more app while it's on (RemoteDesktop.jsx), and
  // the VPN tunnels (0.8.5)
  const rd = useRemoteDesktop();
  const rdg = remoteGroup(rd, tr);
  const vpn = useVpnGroup(tr);
  // the bridge's own sockets (its WebRTC UDP ports, found by its cgroup in conn.c) join that row
  const bridge = fwApps.find((g) => g.key === REMOTE_KEY);
  if (bridge && rdg) { rdg.listen = [...rdg.listen, ...bridge.listen]; rdg.conns = [...rdg.conns, ...bridge.conns]; }
  else if (bridge) bridge.name = tr('monitor.net.remoteDesktop');
  const apps = [...fwApps.filter((g) => !(rdg && g.key === REMOTE_KEY)), ...(rdg ? [rdg] : []), ...(vpn ? [vpn] : [])].sort((a, b) => b.sent + b.recv - (a.sent + a.recv) || b.conns.length - a.conns.length);
  const [view, setView, mprefs, setPrefs] = useNetView();
  const [chips, setChips] = useState([]);
  const [text, setText] = useState('');
  const [open, setOpen] = useState(() => new Set());
  const [selected, setSelected] = useState(null);       // app elegida (clave)
  const [place, setPlace] = useState(null);             // país elegido en el mapa
  const [host, setHost] = useState(null);               // the selected app's address with the actions open
  const [sheet, setSheet] = useState(null);             // { app, label, dst, action, suggestions, note }
  const [all, setAll] = useState(() => new Set());      // tree rows whose every child is listed
  const [confirm, setConfirm] = useState(null);         // a component whose blocking needs a warning (Store)
  const [range, setRange] = useState('live');           // the traffic chart's zoom: 'live' or a history window
  const [end, setEnd] = useState(null);                 // the history window's end (Unix s), null = now
  const sysHist = useHistory(range === 'live' ? null : range, end, null);
  const restrictedOf = useMemo(() => firewall.state().restricted || {}, [fw.rev]);
  const mode = useMemo(() => firewall.state().mode || 'silent', [fw.rev]);
  const names = useMemo(() => namesOf(fw.events), [fw.events]);
  const hist = useRateHistory(apps);
  const lists = useMemo(() => firewall.lists(), [fw.rev, fw.counts]);
  const listName = (id) => {
    const l = lists?.lists?.find((x) => x.id === id);
    return l ? (l.custom ? l.url.replace(/^https:\/\//, '').split('/')[0] : tOptional(`firewall.lists.name.${id}`) || id) : id;
  };
  const ruleFor = (g, addr, action) => {
    const name = names.get(addr);
    setSheet({ app: g.key, label: labelOf(tr, g), dst: name || addr, action,
      suggestions: name ? nameChoices(name) : [], note: name ? tr('fwrules.sheet.nameNote') : tr('fwrules.sheet.noName') });
  };
  const enabled = prefs.connections !== false;
  const byKey = new Map(apps.map((g) => [g.key, g]));

  // the list: flows → filters and chips → the tree (sdk/netmodel.js); in a history window, the
  // apps of that window from the history file (it never keeps addresses or names)
  const live = range === 'live';
  const flows = live ? flowsOf(apps, names, tr) : [];
  const hasRule = (f) => firewall.enforceable(f.app) && firewall.rules(f.app).some((r) => r.dst === (f.name || f.addr) || r.dst === f.addr);
  const shownFlows = filterFlows(flows, view, { chips, rest: text.trim() }, { hasRule, countryName });
  const tree = sortTree(groupFlows(shownFlows, view.group), view.sort);
  const filtered = chips.length > 0 || !!text.trim() || view.dir !== 'all' || view.proto !== 'all' || view.status !== 'all';
  const histRoots = !live ? (sysHist?.h?.top || []).map(([id, rx, tx]) => {
    const la = launcher.find((a) => a.id === id || `lpk:${a.id}` === id || `desktop:${a.id}` === id);
    const g = byKey.get(id) || { key: id, item: COMPONENTS[id] ? { app: id } : { app: id, appName: la?.name, name: la?.name || id, appIcon: la?.icon || '', self: id === 'lordos' },
      name: COMPONENTS[id] || id === 'lordos' ? null : la?.name || id.replace(/^(desktop|lpk):/, ''), conns: [], listen: [], hosts: new Map(), rule: firewall.appRule(id), denied: 0, programs: new Set() };
    return { ...g, recv: rx, sent: tx, rateRx: 0, rateTx: 0, histOnly: true };
  }).filter((g) => {
    const f = { app: g.key, appName: labelOf(tr, g), name: null, addr: '', port: 0, proto: 'other', dir: 'out', cc: '' };
    return netModel.matchesSearch(f, { chips: chips.filter((c) => c.type === 'app'), rest: text.trim() }, countryName);
  }) : [];

  const maxBytes = Math.max(1, ...apps.map((g) => g.sent + g.recv), ...histRoots.map((g) => g.sent + g.recv));
  const sel = apps.find((g) => g.key === selected) || histRoots.find((g) => g.key === selected) || null;
  const places = placesOf(apps, sel ? sel.key : null, filtered ? new Set(shownFlows.map((f) => `${f.app}|${f.addr}`)) : null);
  const selPlace = places.find((p) => p.cc === place) || null;
  const toggle = (key) => setOpen((s0) => { const n = new Set(s0); if (n.has(key)) n.delete(key); else n.add(key); return n; });
  const ctx = {
    tr, open, all, mode, listName,
    toggle,
    toggleAll: (key) => setAll((s0) => { const n = new Set(s0); if (n.has(key)) n.delete(key); else n.add(key); return n; }),
    selected, selectedHost: host,
    isSystem: (id) => isSystemId(id) || id === REMOTE_KEY,
    restrictedOf: (id) => restrictedOf[id] || [],
    appLabel: (n) => labelOf(tr, byKey.get(n.app) || { key: n.app, name: n.label }),
    itemOf: (n) => byKey.get(n.app)?.item || { app: n.app, appName: n.label },
    appOf: (n, path) => {
      if (n.kind === 'app') return n.app;
      const seg = String(path || '').split('\u0001').find((k) => k.startsWith('a:'));
      if (seg) return seg.slice(2);
      const ids = new Set(n.flows.map((f) => f.app));
      return ids.size === 1 ? [...ids][0] : null;
    },
    onPick: (n, appOf) => {
      if (n.kind === 'country' && n.cc) { setSelected(null); setHost(null); setPlace(n.cc); return; }
      if (!appOf) return;
      setPlace(null);
      setSelected(appOf);
      setHost(n.addr || n.flows.find((f) => f.addr)?.addr || null);
    },
  };
  const column = { borderRadius: 12, backgroundColor: card.bg, border: `1px solid ${t.separator}`, padding: 10 };
  const roots = live ? tree : histRoots;
  const extra = [
    { label: tr('monitor.net.view.menuItem'), checked: mprefs.menuItem === true, onClick: () => setPrefs({ menuItem: mprefs.menuItem !== true }) },
    { label: tr('monitor.net.view.blocklists'), onClick: () => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } }) },
  ];
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: 12, paddingHorizontal: 20, paddingVertical: 16 }}>
      {/* left */}
      <View style={{ ...column, width: 336, gap: 8 }}>
        <TokenSearch chips={chips} setChips={setChips} text={text} setText={setText} tr={tr} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <ViewButton view={view} setView={setView} extra={extra} tr={tr} />
          <Text style={{ flex: 1, fontSize: 10, color: t.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' }}>
            {live ? tr(`monitor.net.count.${view.group}`, { count: roots.length }) : tr('monitor.net.count.app', { count: roots.length })}
          </Text>
        </View>
        <View style={{ flexDirection: 'row', paddingHorizontal: 8 }}>
          <Text style={{ flex: 1, fontSize: 10, fontWeight: 600, color: t.textDim }}>{tr(`monitor.net.view.group.${live ? view.group : 'app'}`)}</Text>
          <Text style={{ width: 75, fontSize: 10, fontWeight: 600, color: t.textDim, textAlign: 'end' }}>{tr('monitor.net.dataCol')}</Text>
          <Text style={{ width: 38, fontSize: 10, fontWeight: 600, color: t.textDim, textAlign: 'end' }}>{tr('monitor.net.ruleCol')}</Text>
        </View>
        <ScrollView style={{ flex: 1, marginHorizontal: -10 }} contentStyle={{ paddingHorizontal: 10 }}>
          {!enabled && <Text style={{ fontSize: 12, color: t.textDim, padding: 8, lineHeight: 1.4 }}>{tr('monitor.net.disabled')}</Text>}
          {enabled && !conns && <Text style={{ fontSize: 12, color: t.textDim, padding: 8 }}>{tr('monitor.loading')}</Text>}
          {enabled && conns && !conns.ok && <Text style={{ fontSize: 12, color: '#ff9f0a', padding: 8 }}>{tr('monitor.net.noDiag')}</Text>}
          {!live ? <Text style={{ fontSize: 10, color: t.textDim, paddingHorizontal: 8, paddingBottom: 4, lineHeight: 1.35 }}>{tr('monitor.net.rangeNote', { range: tr(`monitor.hist.${range}`) })}</Text> : null}
          {roots.map((n) => {
            if (!live) return <AppRow key={n.key} g={n} tr={tr} open={false} selected={selected === n.key} maxBytes={maxBytes} hist={null}
              onToggle={() => {}} onSelect={() => { setPlace(null); setHost(null); setSelected(selected === n.key ? null : n.key); }} />;
            if (view.group === 'app') {
              const g = byKey.get(n.app);
              if (!g) return null;
              // filtered, the row adds up what matched; otherwise the app's own totals (the kernel's)
              const shownG = filtered ? { ...g, recv: n.rx, sent: n.tx, rateRx: n.rateRx, rateTx: n.rateTx } : g;
              const onMap = [...g.hosts.values()].some((h) => h.scope === 'internet' && h.cc);
              return <AppRow key={n.key} g={shownG} node={n} ctx={ctx} tr={tr} open={open.has(n.key)} selected={selected === g.key} maxBytes={maxBytes} hist={hist.get(g.key)}
                mapColor={onMap ? appMapColor(g.key) : null}
                onConfirm={(x) => setConfirm(x)} onToggle={() => toggle(n.key)}
                onSelect={() => { setPlace(null); setHost(null); setSelected(selected === g.key ? null : g.key); }} />;
            }
            return <Child key={n.key} node={n} depth={0} path="" ctx={ctx} />;
          })}
          {enabled && conns && !roots.length && <Text style={{ fontSize: 12, color: t.textDim, padding: 8 }}>{chips.length || text ? tr('monitor.net.noMatch') : tr('monitor.net.none')}</Text>}
          {/* a click on the empty space under the rows clears the selection (the map shows every app again) */}
          <View style={{ flexGrow: 1, minHeight: 160 }} onClick={() => { if (selected || place || host) { setSelected(null); setPlace(null); setHost(null); } }} />
        </ScrollView>
        <View style={{ height: 1, backgroundColor: t.separator }} />
        <SystemTraffic tr={tr} range={range} setRange={setRange} end={end} setEnd={setEnd} sysHist={sysHist} />
      </View>
      {/* centro: el mapa ocupa toda la altura */}
      <View style={{ ...column, flex: 1, minWidth: 0, gap: 8 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 2 }}>
          <Glyph name="globe" color={t.accent} />
          <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
            {(sel ? tr('monitor.net.mapOf', { app: labelOf(tr, sel) }) : tr('monitor.net.map')) + ' · ' +
              tr('monitor.net.mapLegend', { countries: places.length, count: places.reduce((a, p) => a + p.count, 0) })}
          </Text>
          {(sel || selPlace) && <View onClick={() => { setSelected(null); setPlace(null); }}><Text style={{ fontSize: 11, color: t.accent }}>{tr('monitor.net.showAll')}</Text></View>}
        </View>
        <MapView places={places} home={info?.home} selected={place} onSelect={setPlace} fitKey={selected || ''} geoOn={prefs.geo !== false && info?.geo} />
        <View style={{ flexDirection: 'row', gap: 8, paddingHorizontal: 2 }}>
          <Text style={{ flex: 1, fontSize: 9, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{tr('monitor.net.privacyShort')}</Text>
          <View onClick={() => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } })}><Text style={{ fontSize: 9, color: t.accent, whiteSpace: 'nowrap' }}>{tr('monitor.net.privacyLink')}</Text></View>
        </View>
      </View>
      {/* derecha */}
      <View style={{ ...column, width: 270, padding: 12 }}>
        <ScrollView style={{ flex: 1, marginHorizontal: -12 }} contentStyle={{ paddingHorizontal: 12 }}>
          <View style={{ gap: 16 }}>
            {selPlace ? <PlaceDetail p={selPlace} tr={tr} /> : sel?.key === REMOTE_KEY ? <RemoteDetail rd={rd} Section={Section} KV={KV} tr={tr} /> : sel ? (
              <Detail g={sel} tr={tr} user={sys.user} names={names} host={host} onHost={setHost} hist={hist.get(sel.key)} range={range} end={end} onRule={(addr, action) => ruleFor(sel, addr, action)} />
            ) : <Summary groups={apps} conns={conns} last={last} tr={tr} fw={fw} names={names} range={range} sysHist={sysHist} launcher={launcher} lists={lists} />}
          </View>
        </ScrollView>
      </View>
      {sheet ? (
        <RuleSheet overlay app={sheet.app} appLabel={sheet.label} initial={{ dst: sheet.dst, action: sheet.action }} suggestions={sheet.suggestions} note={sheet.note}
          onDone={() => setSheet(null)} />
      ) : null}
      {confirm ? (
        <ConfirmDialog title={tr('monitor.comp.blockTitle', { app: labelOf(tr, confirm) })} detail={tr(COMPONENTS[confirm.key]?.warn || 'monitor.comp.storeWarn')}
          okLabel={tr('monitor.comp.blockAnyway')} cancelLabel={tr('monitor.comp.keep')} destructive
          onOk={() => { firewall.setAppRule(confirm.key, 'deny'); setConfirm(null); }} onCancel={() => setConfirm(null)} />
      ) : null}
    </View>
  );
}
