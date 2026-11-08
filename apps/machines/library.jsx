// The Control Center (the approved mock "Main"): summary bars for machines running, the memory they use and free
// disk space; a card per machine with its picture (live while its window shows it, the last one when it's off), its
// system, what it's doing, the three lights (power, agent, disk activity), CPU and memory bars and what it uses on
// disk. Double-click opens or starts; right-click has the machine's menu. FirstRun, Requirements and the Pi page are
// the other screens of this window.
import {
  View, Text, Image, ScrollView, useAppWindow, useStore, admin, fs, wm, windows, revealPath, ConfirmDialog, useSystemInfo, statusText, useSettings, settings,
  Toolbar, ToolbarButton, ToolbarGroup, ToolbarSpacer, MenuButton, SearchField, EmptyState,
  t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, actionsFor, missingFor, usage, summaryOf, doingOf, thumbSrc, systemOf, libView, filterMachines, sortMachines } from './model.js';
import { Assistant } from './assistant.jsx';
import { usePal, has, gb, bytes, gib, OsMark, Lights, Bar, Btn, PopMenu, StateDot, markColors, overlayRoot } from './ui.jsx';
import { readMeta } from './meta.js';
import { CloneSheet } from './clone.jsx';

const winIdOf = (m) => `machines#vm-${m.id || m.folder}`;
// how long the Requirements page waits for a word from the package engine before it says the network isn't answering
const STALL_MS = 90000;
// opens (or brings forward) the machine's window; a new one gets the guest's usual size and the middle of the screen
export function openMachine(m, opts) {
  const instance = `vm-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { machine: m.dir, name: m.name, ...(opts?.loadvm ? { loadvm: opts.loadvm } : {}) } });
  if (fresh) { wm.resize(id, 1024, 768 + 48); wm.center(id); }
  return id;
}
export function openConfigure(m, section) {
  const instance = `cfg-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { configure: m.dir, name: m.name, section: section || 'general' } });
  if (fresh) { wm.resize(id, 980, 640); wm.center(id); }
}
export function openSnapshots(m) {
  const instance = `snap-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { snapshots: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 1100, 560); wm.center(id); }
}
export function openStorage() {
  const id = 'machines#storage';
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance: 'storage', args: { storage: true } });
  if (fresh) { wm.resize(id, 1040, 620); wm.center(id); }
}
export function openLogs(m) {
  const instance = `log-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { logs: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 980, 600); wm.center(id); }
}

// the status line under the library (5.1): machines running, the memory they use, free space (a click opens Storage)
function Summary({ label, value, frac, color, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} title={onClick ? tr('machines.storage.menu') : undefined} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
      <Text style={{ fontSize: 12, color: p.dim }}>{label}</Text>
      <View style={{ width: 70 }}><Bar frac={frac} color={color} height={5} /></View>
      <Text style={{ fontSize: 12, fontWeight: 600, color: p.text }}>{value}</Text>
    </View>
  );
}

function Card({ m, u, sel, onSelect, onMenu, windowed }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const v = stateView(m.state, m.reason);
  const a = actionsFor(m.state);
  const meta = m.meta || {};
  const sys = meta.os ? { id: meta.os, name: meta.osName || '' } : systemOf(m.name);
  const [c1, c2] = markColors(sys.id);
  const src = thumbSrc(m.dir, m.thumbTime);
  const on = v.active;
  const emulated = m.accel === 'tcg';
  const system = [sys.name || tr('machines.card.system'), m.arch, emulated ? tr('machines.emulatedLower') : null].filter(Boolean).join(', ');
  const doing = tr(`machines.doing.${doingOf(m, windowed)}`);
  const go = () => (a.open || a.start ? openMachine(m) : null);
  return (
    <View onClick={onSelect} onDoubleClick={go} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ flex: 1, minHeight: 0, borderRadius: 14, overflow: 'hidden', backgroundColor: p.card,
        border: `2px solid ${sel ? p.accent : 'transparent'}` }}>
      <View style={{ flex: 1, minHeight: 0, ...(src ? { backgroundColor: '#000' } : { background: `linear-gradient(135deg, ${c1}, ${c2})` }) }}>
        {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover', opacity: on ? 1 : 0.72 }} /> : (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' }}><OsMark id={sys.id} size={56} /></View>
        )}
        <View style={{ position: 'absolute', left: 10, top: 10, paddingHorizontal: 9, height: 20, borderRadius: 10, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.62)' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: v.why ? '#ff9a9a' : '#f2f2f5' }}>{doing}</Text>
        </View>
        <View style={{ position: 'absolute', right: 10, top: 10 }}><Lights power={on} agent={on && m.agentState === 'connected'} disk={!!u?.disk} /></View>
        {hover && (a.start || a.open) ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
            <View onClick={(e) => { e.stopPropagation?.(); go(); }} title={a.open ? tr('machines.open') : tr('machines.start')}
              style={{ width: 64, height: 64, borderRadius: 32, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.55)', border: '2px solid rgba(255,255,255,0.85)' }}>
              <Text style={{ fontSize: 26, color: '#fff', marginLeft: a.open ? 0 : 4 }}>{a.open ? '⤢' : '▶'}</Text>
            </View>
          </View>
        ) : null}
      </View>
      <View style={{ paddingHorizontal: 12, paddingVertical: 10, gap: 6 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Text style={{ flex: 1, fontSize: 14, fontWeight: 700, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.name}</Text>
          <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap' }}>{system}</Text>
        </View>
        <View style={{ flexDirection: 'row', gap: 10, alignItems: 'center' }}>
          <View style={{ flex: 1, gap: 3 }}>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.card.cpu', { v: on && u?.cpu != null ? `${Math.round(u.cpu)} %` : '—' })}</Text>
            <Bar frac={on && u?.cpu != null ? u.cpu / 100 : 0} color={p.cpu} height={4} />
          </View>
          <View style={{ flex: 1, gap: 3 }}>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.card.memory', { v: on && u?.memBytes ? `${gib(u.memBytes)} GB` : '—' })}</Text>
            <Bar frac={on && u?.memBytes ? u.memBytes / (m.memoryMb * 1048576) : 0} color={p.mem} height={4} />
          </View>
          <View style={{ alignItems: 'flex-end' }}>
            <Text style={{ fontSize: 12, fontWeight: 700, color: p.text }}>{m.diskBytes != null ? bytes(m.diskBytes) : '—'}</Text>
            <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.card.onDisk')}</Text>
          </View>
        </View>
      </View>
    </View>
  );
}

// one machine in the list view: picture, name and system, what it's doing, CPU, memory, disk; the same clicks as a card
function ListRow({ m, u, sel, onSelect, onMenu, windowed }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const v = stateView(m.state, m.reason);
  const a = actionsFor(m.state);
  const sys = m.meta?.os ? { id: m.meta.os, name: m.meta.osName || '' } : systemOf(m.name);
  const src = thumbSrc(m.dir, m.thumbTime);
  const on = v.active;
  const go = () => (a.open || a.start ? openMachine(m) : null);
  const cell = (text, w, strong) => <Text style={{ width: w, fontSize: 12, fontWeight: strong ? 600 : 400, color: strong ? p.text : p.dim, textAlign: 'end', whiteSpace: 'nowrap' }}>{text}</Text>;
  return (
    <View onClick={onSelect} onDoubleClick={go} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ height: 64, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 10,
        backgroundColor: sel ? (p.dark ? 'rgba(255,255,255,0.09)' : 'rgba(0,0,0,0.06)') : hover ? p.raised : 'transparent', border: `1px solid ${sel ? p.accent : 'transparent'}` }}>
      <View style={{ width: 80, height: 48, borderRadius: 7, overflow: 'hidden', alignItems: 'center', justifyContent: 'center', backgroundColor: '#000' }}>
        {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, width: 80, height: 48, objectFit: 'cover', opacity: on ? 1 : 0.72 }} /> : <OsMark id={sys.id} size={30} />}
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.name}</Text>
        <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{[sys.name || tr('machines.card.system'), m.arch, m.accel === 'tcg' ? tr('machines.emulatedLower') : null].filter(Boolean).join(', ')}</Text>
      </View>
      <View style={{ width: 130, flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <StateDot tone={v.tone} />
        <Text style={{ fontSize: 12, color: v.why ? '#ff9a9a' : p.text, whiteSpace: 'nowrap' }}>{tr(`machines.doing.${doingOf(m, windowed)}`)}</Text>
      </View>
      {cell(on && u?.cpu != null ? `${Math.round(u.cpu)} %` : '—', 56)}
      {cell(on && u?.memBytes ? `${gib(u.memBytes)} GB` : gb(m.memoryMb), 70)}
      {cell(m.diskBytes != null ? bytes(m.diskBytes) : '—', 70, true)}
      <View onClick={(e) => { e.stopPropagation?.(); go(); }} title={a.open ? tr('machines.open') : tr('machines.start')}
        style={{ width: 30, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center', opacity: hover || sel ? 1 : 0, backgroundColor: p.btn }}>
        <Text style={{ fontSize: 13, color: p.text, marginLeft: a.open ? 0 : 2 }}>{a.open ? '⤢' : '▶'}</Text>
      </View>
    </View>
  );
}

// FirstRun (empty library) and its "one step first" side when virtualization isn't usable yet
function FirstRun({ host, onNew, onImport, onCheck }) {
  const p = usePal();
  const info = useSystemInfo(60000);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState('');
  const allow = async () => {
    setBusy(true); setMsg('');
    try { setMsg(statusText(await admin.run('vm-kvm', ''))); } catch (e) { setMsg(e.message); }
    setBusy(false); onCheck();
  };
  const side = host.kvmDevice && !host.kvm ? 'allow' : !host.kvmDevice ? 'off' : null;
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: 16, padding: 16 }}>
      <View style={{ flex: 1, padding: 28, alignItems: 'center', justifyContent: 'center', gap: 10, borderRadius: 18, backgroundColor: side ? p.raised : 'transparent' }}>
        <AppMark size={96} />
        <Text style={{ marginTop: 8, fontSize: 20, fontWeight: 700, color: p.text }}>{tr('machines.empty')}</Text>
        <Text style={{ maxWidth: 420, fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'center' }}>{tr('machines.first.addHint')}</Text>
        <View style={{ flexDirection: 'row', gap: 8, marginTop: 6 }}>
          <Btn primary label={tr('machines.newMachine')} onClick={onNew} />
          <Btn label={tr('machines.first.import')} onClick={onImport} />
        </View>
      </View>
      {side ? (
        <View style={{ flex: 1, padding: 28, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
          {side === 'allow' ? <>
            <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.first.stepTitle')}</Text>
            <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.first.stepText')}</Text>
            <Btn big primary label={busy ? tr('machines.req.allowing') : tr('machines.first.turnOn')} disabled={busy || !admin.available() || !info.admin} onClick={allow} style={{ alignSelf: 'flex-start' }} />
            {msg ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{msg}</Text> : null}
          </> : <>
            <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.first.offTitle')}</Text>
            <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.first.offText')}</Text>
            <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.first.emulated')}</Text></View>
            <View style={{ flexDirection: 'row', gap: 8 }}><Btn big label={tr('machines.first.checkAgain')} onClick={onCheck} /><Btn big label={tr('machines.first.continueEmulated')} onClick={onNew} /></View>
          </>}
        </View>
      ) : null}
    </View>
  );
}

export function AppMark({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #8aa4ff, #2f3fb8)' }}>
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.23, width: s * 0.6, height: s * 0.45, borderRadius: s * 0.07, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.32, width: s * 0.4, height: s * 0.27, borderRadius: s * 0.06, background: 'linear-gradient(160deg, #5ad1a8, #2a8f7a)' }} />
      <View style={{ position: 'absolute', left: s * 0.36, top: s * 0.74, width: s * 0.28, height: s * 0.05, borderRadius: s * 0.025, backgroundColor: '#fff' }} />
    </View>
  );
}

// Requirements: what this computer has for machines, with the closed admin action that installs what's missing
export function Requirements({ host, onBack, onChanged, archs }) {
  const p = usePal();
  const info = useSystemInfo(60000);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const list = archs || [host.arch];
  const missing = missingFor(host, list);
  const have = (pk) => !missing.includes(pk);
  const rows = [
    ['kvm', host.kvm ? 'ready' : host.kvmDevice ? 'notAllowed' : 'off'],
    ...list.map((a) => [`engine.${a}`, have(a === 'amd64' ? 'qemu-system-x86' : 'qemu-system-arm') ? 'ready' : 'missing']),
    ...list.map((a) => [`firmware.${a}`, have(a === 'amd64' ? 'ovmf' : 'qemu-efi-aarch64') ? 'ready' : 'missing']),
    ['display', have('qemu-system-modules-opengl') ? 'ready' : 'missing'],
    ['disk', have('qemu-utils') ? 'ready' : 'missing'],
    ['network', have('passt') ? 'ready' : 'missing'],
  ];
  const install = async () => {
    setBusy(true); setMsg(null);
    // a network that doesn't answer: the engine gives up on its own, but the page never waits more than STALL_MS
    // without a word from it
    let last = Date.now(), stalled = false;
    // the engine's progress lines ("progress lists 1 5", "progress download 1048576 2097152") said in words
    const said = (line) => {
      last = Date.now();
      const m = /^progress (\w+) (\d+) (\d+)/.exec(String(line || ''));
      const mb = (x) => Math.round(Number(x) / 1048576 * 10) / 10;
      setMsg({ ok: true, text: !m ? statusText(line) : m[1] === 'download' ? tr('machines.req.progress.download', { a: mb(m[2]), b: mb(m[3]) })
        : m[1] === 'lists' ? tr('machines.req.progress.lists', { a: m[2], b: m[3] }) : tr('machines.req.installing') });
    };
    const watch = setInterval(() => {
      if (Date.now() - last < STALL_MS || stalled) return;
      stalled = true; setMsg({ ok: false, text: tr('machines.req.stalled') }); setBusy(false);
    }, 2000);
    const go = () => admin.run('vm-enable', list.join(' '), said);
    try {
      try { await go(); } catch (e) { if (e.code === 'pkg.busy') throw e; last = Date.now(); await admin.run('pkg-update', '', said); await go(); }
      if (!stalled) setMsg({ ok: true, text: tr('machines.req.installed') });
    } catch (e) {
      // no package source answered (the network is down or blocks them): said as that, not as a verification problem
      if (!stalled) setMsg({ ok: false, text: e.code === 'pkg.noVerifiedSource' ? tr('machines.req.offline') : e.message });
    }
    clearInterval(watch);
    setBusy(false); onChanged();
  };
  return (
    <View style={{ flex: 1, paddingHorizontal: 30, paddingVertical: 26, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
      <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.req.page')}</Text>
      <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.req.for', { arch: list.join(', ') })}</Text>
      {rows.map(([k, st]) => (
        <View key={k} style={{ minHeight: 46, paddingHorizontal: 14, paddingVertical: 6, flexDirection: 'row', alignItems: 'center', gap: 12, borderRadius: 10, backgroundColor: p.card }}>
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.req.row.${k.split('.')[0]}`, { arch: k.split('.')[1] || '' })}</Text>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(`machines.req.use.${k.split('.')[0]}`)}</Text>
          </View>
          <Text style={{ fontSize: 12, fontWeight: 700, color: st === 'ready' ? '#8ce8b0' : '#ffc56e' }}>{tr(`machines.req.st.${st}`)}</Text>
        </View>
      ))}
      <View style={{ flex: 1 }} />
      {/* how it's going, in the place of the note under the rows, so the page keeps its height */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {msg ? <Text style={{ flex: 1, fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text>
          : <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{admin.available() && info.admin ? tr('machines.req.fromStore') : tr('machines.req.notAdmin')}</Text>}
        {onBack ? <Btn label={tr('machines.back')} onClick={onBack} /> : null}
        <Btn primary label={busy ? tr('machines.req.installing') : tr('machines.req.installMissing')} disabled={busy || !missing.length || !admin.available() || !info.admin} onClick={install} />
      </View>
    </View>
  );
}

// PiView: what this board can run, and the warnings that matter on it
function PiView({ host, list, onBack }) {
  const p = usePal();
  const rows = [
    ['native', 'arm64', '#8ce8b0'], ['slow', 'amd64', '#ffc56e'], ['limited', 'gpu', '#ffc56e'], ['no', 'passthrough', '#ff9a9a'], ['no', 'nested', '#ff9a9a'],
  ];
  const emu = list.filter((m) => m.accel === 'tcg');
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: 16 }}>
      <View style={{ flex: 1, padding: 24, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
        <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.title', { board: host.board || tr('machines.pi.thisComputer'), ram: gb(host.ramMb) })}</Text>
        <Text style={{ fontSize: 14, color: p.dim, textAlign: 'start' }}>{tr('machines.pi.native', { cpus: host.cpus, mem: gb(host.memMaxMb) })}</Text>
        {rows.map(([st, k, c]) => (
          <View key={k} style={{ minHeight: 48, paddingHorizontal: 14, flexDirection: 'row', alignItems: 'center', gap: 14, borderRadius: 10, backgroundColor: p.card }}>
            <Text style={{ width: 76, fontSize: 12, fontWeight: 700, color: c, textAlign: 'start' }}>{tr(`machines.pi.st.${st}`)}</Text>
            <View style={{ flex: 1 }}>
              <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.pi.row.${k}`)}</Text>
              <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(`machines.pi.note.${k}`)}</Text>
            </View>
          </View>
        ))}
        <View style={{ flex: 1 }} />
        <Btn label={tr('machines.back')} onClick={onBack} style={{ alignSelf: 'flex-start' }} />
      </View>
      <View style={{ width: 380, gap: 12 }}>
        {emu.length ? (
          <View style={{ padding: 18, borderRadius: 18, backgroundColor: p.warnBg }}>
            <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.emuTitle')}</Text>
            <Text style={{ marginTop: 8, fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.pi.emuText', { name: emu[0].name })}</Text>
          </View>
        ) : null}
        {host.libDevice === 'sd' ? (
          <View style={{ padding: 18, borderRadius: 18, backgroundColor: p.raised }}>
            <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.sdTitle')}</Text>
            <Text style={{ marginTop: 8, fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.pi.sdText')}</Text>
          </View>
        ) : null}
      </View>
    </View>
  );
}

export function Library() {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [host, setHost] = useState(() => (has() ? lord.vm.host() : null));
  const [list, setList] = useState([]);
  const [sel, setSel] = useState(null);
  const [page, setPage] = useState('main');          // main, new, requirements, pi
  const [newMode, setNewMode] = useState(null);
  const [menu, setMenu] = useState(null);
  const [del, setDel] = useState(null);
  const [error, setError] = useState('');
  const [usages, setUsages] = useState({});
  const [discard, setDiscard] = useState(null);
  const [copying, setCopying] = useState(null);
  const [query, setQuery] = useState('');
  const [s0] = useSettings();
  const lv = libView(s0.machinesView);
  const putView = (patch) => settings.set({ machinesView: { ...libView(settings.get().machinesView), ...patch } });
  const wins = useStore(windows, (ws) => ws.map((w) => w.id).join(' '));
  const prev = useRef({});
  // lord-vm jobs started from here (start with no window, discard a saved state): followed until they end, so the
  // engine frees them, and a failure is shown
  const jobs = useRef([]);
  const follow = (id, name) => { if (!id) { setError(tr('machines.cfg.refused')); return; } jobs.current.push({ id, name }); };
  const root = useRef(null);
  overlayRoot.current = root.current;

  // the list with each machine's disk use, card picture time, app notes, and CPU/memory/disk activity while it runs
  const read = () => {
    if (!has()) return;
    jobs.current = jobs.current.filter((j) => {
      const r = lord.vm.job(j.id);
      if (!r || !r.done) return !!r;
      if (!r.ok) setError(`${j.name}: ${r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim()}`);
      return false;
    });
    const ms = lord.vm.list();
    const next = {};
    for (const m of ms) {
      const s = lord.vm.stats(m.dir) || {};
      m.diskBytes = s.diskBytes;
      try { const st = fs.stat(`${m.dir}/thumb.png`); m.thumbTime = st?.mtime || 0; } catch (e) { m.thumbTime = 0; }
      m.meta = readMeta(m.dir);
      const u = usage(prev.current[m.dir], s, m.vcpus);
      if (u) next[m.dir] = u;
      prev.current[m.dir] = s.at ? s : null;
    }
    setList(ms);
    setUsages(next);
  };
  const refresh = () => { if (has()) { setHost(lord.vm.host()); read(); } };
  useEffect(() => {
    wm.setTitle(winId, tr('machines.title'));
    read();
    const timer = setInterval(read, 1500);
    return () => clearInterval(timer);
  }, []);
  useEffect(() => {
    wm.setMenus(winId, [{ label: tr('machines.menu.machine'), items: [
      { label: tr('machines.newMachine'), onClick: () => setPage('new') },
      { label: tr('machines.req.menu'), onClick: () => setPage('requirements') },
      { label: tr('machines.storage.menu'), onClick: openStorage },
      ...(host?.arch === 'arm64' ? [{ label: tr('machines.pi.menu'), onClick: () => setPage('pi') }] : []),
    ] }]);
  }, [host?.arch]);
  if (!host) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.noBackend')}</Text></View>;

  const sorted = sortMachines(filterMachines(list, { query, show: lv.show }), lv.sort);
  const sum = summaryOf(list, usages, host);
  const missing = missingFor(host, [host.arch]);
  const remove = (m) => {
    setDel(null);
    try { fs.trash(m.dir); setError(''); } catch (e) { setError(tr('machines.deleteFailed', { name: m.name })); }
    refresh();
  };
  const menuFor = (m) => {
    const a = actionsFor(m.state);
    const stop = (how) => lord.vm.stop(m.dir, how);
    return [
      a.start ? { label: tr('machines.start'), onClick: () => openMachine(m) } : { label: tr('machines.open'), disabled: !a.open, onClick: () => openMachine(m) },
      a.start ? { label: tr('machines.cmd.startNoWindow'), onClick: () => follow(lord.vm.startDetached(m.dir), m.name) } : null,
      { label: tr('machines.cmd.pause'), disabled: !a.pause, onClick: () => stop('pause') },
      { label: tr('machines.cmd.resume'), disabled: !a.resume, onClick: () => stop('resume') },
      { label: tr('machines.cmd.suspend'), disabled: !a.suspend, onClick: () => stop('suspend') },
      { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => stop('shutdown') },
      { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => stop('reset') },
      { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => stop('poweroff') },
      '-',
      { label: tr('machines.cmd.configure'), onClick: () => openConfigure(m) },
      { label: tr('machines.cmd.snapshots'), onClick: () => openSnapshots(m) },
      { label: tr('machines.cmd.clone'), onClick: () => setCopying(m) },
      { label: tr('machines.cmd.logs'), onClick: () => openLogs(m) },
      { label: tr('machines.cmd.showInFiles'), onClick: () => revealPath(m.dir) },
      m.iso && a.remove && !m.suspended ? { label: tr('machines.eject'), onClick: () => { setError(lord.vm.eject(m.dir) ? '' : tr('machines.ejectFailed', { name: m.name })); refresh(); } } : null,
      m.suspended && a.start ? { label: tr('machines.fail.discardState'), destructive: true, onClick: () => setDiscard(m) } : null,
      '-',
      { label: tr('machines.cmd.remove'), destructive: true, disabled: !a.remove, onClick: () => setDel(m) },
    ];
  };
  const rl = root.current ? lord.getLayout(root.current.id) : [0, 0, 1000, 700];
  // as many cards as fit at 300 px or more (as Remote Desk), each with the guest's 16:10 picture and its two lines
  const cw = rl[2] - 40;
  const cols = Math.max(1, Math.floor((cw + 14) / (300 + 14)));
  const cardW = Math.floor((cw - 14 * (cols - 1)) / cols);
  const rowH = Math.round(cardW * 10 / 16) + 72;
  const rows = [];
  for (let i = 0; i < sorted.length; i += cols) rows.push(sorted.slice(i, i + cols));
  const cardProps = (m) => ({ m, u: usages[m.dir], sel: sel === m.dir, onSelect: () => setSel(m.dir), windowed: wins.split(' ').includes(winIdOf(m)),
    onMenu: (e) => setMenu({ x: e.x, y: e.y, m }) });

  let body;
  if (page === 'new') body = <Assistant host={host} mode={newMode} names={list.map((m) => m.name)} onClose={() => { setPage('main'); setNewMode(null); refresh(); }}
    onCreated={(m) => { setPage('main'); setNewMode(null); refresh(); if (m) openMachine(m); }} onConfigure={(m) => { setPage('main'); refresh(); if (m) openConfigure(m, 'cpu'); }}
    onNeeds={() => setPage('requirements')} />;
  else if (page === 'requirements') body = <Requirements host={host} onBack={() => setPage('main')} onChanged={refresh} />;
  else if (page === 'pi') body = <PiView host={host} list={list} onBack={() => setPage('main')} />;
  else if (!list.length) body = <FirstRun host={host} onNew={() => setPage('new')} onImport={() => { setNewMode('import'); setPage('new'); }} onCheck={refresh} />;
  else if (!sorted.length) body = <EmptyState icon="⌕" hint={query.trim() ? tr('machines.lib.noMatch', { q: query.trim() }) : tr(`machines.lib.none.${lv.show}`)} />;
  else if (lv.view === 'list') body = (
    <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 16, gap: 2 }}>
      {sorted.map((m) => <ListRow key={m.dir} {...cardProps(m)} />)}
    </ScrollView>
  );
  else body = (
    <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 20, gap: 14 }}>
      {rows.map((r, i) => (
        <View key={i} style={{ flexDirection: 'row', gap: 14, height: rowH }}>
          {r.map((m) => <Card key={m.dir} {...cardProps(m)} />)}
          {Array.from({ length: cols - r.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />)}
        </View>
      ))}
    </ScrollView>
  );

  const full = page === 'new' || page === 'requirements' || page === 'pi';
  const sortItems = ['name', 'state', 'last', 'disk'].map((k) => ({ label: tr(`machines.lib.sort.${k}`), checked: lv.sort === k, onClick: () => putView({ sort: k }) }));
  const showItems = ['all', 'running', 'off'].map((k) => ({ label: tr(`machines.lib.show.${k}`), checked: lv.show === k, onClick: () => putView({ show: k }) }));
  const addItems = [
    { label: tr('machines.newMachine'), detail: tr('machines.lib.newHint'), onClick: () => setPage('new') },
    { label: tr('machines.first.import'), detail: tr('machines.lib.importHint'), onClick: () => { setNewMode('import'); setPage('new'); } },
  ];
  return (
    <View ref={root} style={{ flex: 1, backgroundColor: p.bg }}>
      {!full ? (
        <Toolbar>
          <ToolbarButton icon="⛁" title={tr('machines.storage.menu')} onClick={openStorage} />
          {host.arch === 'arm64' && host.board ? <ToolbarButton icon="ⓘ" title={tr('machines.pi.menu')} onClick={() => setPage('pi')} /> : null}
          {missing.length || (host.kvmDevice && !host.kvm) ? <ToolbarButton icon="⚠" color={p.warn} title={tr('machines.req.menu')} onClick={() => setPage('requirements')} /> : null}
          <ToolbarSpacer />
          <MenuButton icon="+" title={tr('machines.lib.add')} items={addItems} minWidth={300} />
          {list.length ? <>
            <ToolbarGroup>
              <ToolbarButton small icon="▦" title={tr('machines.lib.viewGrid')} active={lv.view === 'grid'} onClick={() => putView({ view: 'grid' })} />
              <ToolbarButton small icon="≡" title={tr('machines.lib.viewList')} active={lv.view === 'list'} onClick={() => putView({ view: 'list' })} />
            </ToolbarGroup>
            <MenuButton icon="⇅" title={tr('machines.lib.sort')} items={sortItems} />
            <MenuButton icon={lv.show === 'all' ? '▽' : '▼'} title={tr('machines.lib.show')} items={showItems} />
            <SearchField value={query} onChange={setQuery} placeholder={tr('machines.lib.search')} width={180} />
          </> : null}
        </Toolbar>
      ) : null}
      {error ? <Text style={{ marginHorizontal: 20, marginTop: 10, fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
      {full ? <View style={{ flex: 1, padding: 16 }}>{body}</View> : body}
      {!full && list.length ? (
        <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 22, paddingHorizontal: 20, borderTopWidth: 1, borderColor: p.line }}>
          <Summary label={tr('machines.sum.running')} value={tr('machines.sum.of', { a: sum.running.a, b: sum.running.b })} frac={sum.running.frac} color={p.ok} />
          {sum.memory ? <Summary label={tr('machines.sum.memory')} value={tr('machines.sum.ofGb', { a: gib(sum.memory.a), b: gib(sum.memory.b) })} frac={sum.memory.frac} color={p.mem} /> : null}
          {sum.free ? <Summary label={tr('machines.sum.free')} value={bytes(sum.free.a)} frac={sum.free.frac} color={p.disk} onClick={openStorage} /> : null}
        </View>
      ) : null}
      {menu ? <PopMenu x={Math.min(menu.x - rl[0], rl[2] - 232)} y={Math.min(menu.y - rl[1], rl[3] - 330)}
        items={menuFor(menu.m)} onClose={() => setMenu(null)} /> : null}
      {copying ? <CloneSheet m={copying} running={stateView(copying.state, copying.reason).active} onClose={() => setCopying(null)}
        onDone={(out) => { setCopying(null); refresh(); if (out?.dir) setSel(out.dir); }} /> : null}
      {discard ? <ConfirmDialog title={tr('machines.discard.title', { name: discard.name })} detail={tr('machines.discard.detail')} okLabel={tr('machines.fail.discardState')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => { const d = discard; setDiscard(null); follow(lord.vm.discardState(d.dir), d.name); }} onCancel={() => setDiscard(null)} /> : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.deleteDetail')} okLabel={tr('machines.deleteOk')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => remove(del)} onCancel={() => setDel(null)} /> : null}
    </View>
  );
}
