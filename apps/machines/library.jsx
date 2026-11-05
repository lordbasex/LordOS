// The Control Center (the approved mock "Main"): summary bars for machines running, the memory they use and free
// disk space; a card per machine with its picture (live while its window shows it, the last one when it's off), its
// system, what it's doing, the three lights (power, agent, disk activity), CPU and memory bars and what it uses on
// disk. Double-click opens or starts; right-click has the machine's menu. FirstRun, Requirements and the Pi page are
// the other screens of this window.
import {
  View, Text, Image, ScrollView, useAppWindow, admin, fs, wm, windows, revealPath, ConfirmDialog, useSystemInfo, statusText,
  t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, actionsFor, missingFor, usage, summaryOf, doingOf, thumbSrc, systemOf } from './model.js';
import { Assistant } from './assistant.jsx';
import { usePal, has, gb, bytes, gib, OsMark, Lights, Bar, Btn, PopMenu, markColors, overlayRoot } from './ui.jsx';
import { readMeta } from './meta.js';

const winIdOf = (m) => `machines#vm-${m.id || m.folder}`;
// opens (or brings forward) the machine's window; a new one gets the guest's usual size and the middle of the screen
export function openMachine(m) {
  const instance = `vm-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { machine: m.dir, name: m.name } });
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
export function openLogs(m) {
  const instance = `log-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { logs: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 980, 600); wm.center(id); }
}

function Summary({ label, value, frac, color }) {
  const p = usePal();
  return (
    <View style={{ width: 170, gap: 5 }}>
      <View style={{ flexDirection: 'row' }}>
        <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{label}</Text>
        <Text style={{ fontSize: 12, fontWeight: 700, color: p.text }}>{value}</Text>
      </View>
      <Bar frac={frac} color={color} />
    </View>
  );
}

function Card({ m, u, sel, onSelect, onMenu }) {
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
  const doing = tr(`machines.doing.${doingOf(m)}`);
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
        <View style={{ position: 'absolute', right: 10, top: 10 }}><Lights power={on} agent={false} disk={!!u?.disk} /></View>
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
    <View style={{ flex: 1, flexDirection: 'row', gap: 16 }}>
      <View style={{ flex: 1, padding: 28, alignItems: 'center', justifyContent: 'center', gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
        <AppMark size={112} />
        <Text style={{ marginTop: 8, fontSize: 22, fontWeight: 700, color: p.text }}>{tr('machines.empty')}</Text>
        <Text style={{ maxWidth: 380, fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'center' }}>{tr('machines.first.hint')}</Text>
        <Btn big primary label={tr('machines.newMachine')} onClick={onNew} />
        <Btn link label={tr('machines.first.import')} onClick={onImport} />
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
    const go = () => admin.run('vm-enable', list.join(' '), (line) => setMsg({ ok: true, text: statusText(line) }));
    try {
      try { await go(); } catch (e) { if (e.code === 'pkg.busy') throw e; await admin.run('pkg-update', ''); await go(); }
      setMsg({ ok: true, text: tr('machines.req.installed') });
    } catch (e) { setMsg({ ok: false, text: e.message }); }
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
      {msg ? <Text style={{ fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{admin.available() && info.admin ? tr('machines.req.fromStore') : tr('machines.req.notAdmin')}</Text>
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
  const prev = useRef({});
  const root = useRef(null);
  overlayRoot.current = root.current;

  // the list with each machine's disk use, card picture time, app notes, and CPU/memory/disk activity while it runs
  const read = () => {
    if (!has()) return;
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
      ...(host?.arch === 'arm64' ? [{ label: tr('machines.pi.menu'), onClick: () => setPage('pi') }] : []),
    ] }]);
  }, [host?.arch]);
  if (!host) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.noBackend')}</Text></View>;

  const sorted = [...list].sort((a, b) => a.name.localeCompare(b.name));
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
      { label: tr('machines.cmd.pause'), disabled: !a.pause, onClick: () => stop('pause') },
      { label: tr('machines.cmd.resume'), disabled: !a.resume, onClick: () => stop('resume') },
      { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => stop('shutdown') },
      { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => stop('reset') },
      { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => stop('poweroff') },
      '-',
      { label: tr('machines.cmd.configure'), onClick: () => openConfigure(m) },
      { label: tr('machines.cmd.logs'), onClick: () => openLogs(m) },
      { label: tr('machines.cmd.showInFiles'), onClick: () => revealPath(m.dir) },
      m.iso && a.remove ? { label: tr('machines.eject'), onClick: () => { setError(lord.vm.eject(m.dir) ? '' : tr('machines.ejectFailed', { name: m.name })); refresh(); } } : null,
      '-',
      { label: tr('machines.cmd.remove'), destructive: true, disabled: !a.remove, onClick: () => setDel(m) },
    ];
  };
  const rl = root.current ? lord.getLayout(root.current.id) : [0, 0, 1000, 700];
  const cols = sorted.length <= 2 ? 2 : 3;
  // two rows fill the window (the mock's grid); more scroll
  const rowH = Math.max(230, Math.floor((rl[3] - 32 - 14 - 46 - 12) / 2));
  const rows = [];
  for (let i = 0; i < sorted.length; i += cols) rows.push(sorted.slice(i, i + cols));

  let body;
  if (page === 'new') body = <Assistant host={host} mode={newMode} names={list.map((m) => m.name)} onClose={() => { setPage('main'); setNewMode(null); refresh(); }}
    onCreated={(m) => { setPage('main'); setNewMode(null); refresh(); if (m) openMachine(m); }} onConfigure={(m) => { setPage('main'); refresh(); if (m) openConfigure(m, 'cpu'); }}
    onNeeds={() => setPage('requirements')} />;
  else if (page === 'requirements') body = <Requirements host={host} onBack={() => setPage('main')} onChanged={refresh} />;
  else if (page === 'pi') body = <PiView host={host} list={list} onBack={() => setPage('main')} />;
  else if (!sorted.length) body = <FirstRun host={host} onNew={() => setPage('new')} onImport={() => { setNewMode('import'); setPage('new'); }} onCheck={refresh} />;
  else body = (
    <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 12 }}>
      {rows.map((r, i) => (
        <View key={i} style={{ flexDirection: 'row', gap: 12, height: rowH }}>
          {r.map((m) => <Card key={m.dir} m={m} u={usages[m.dir]} sel={sel === m.dir} onSelect={() => setSel(m.dir)}
            onMenu={(e) => setMenu({ x: e.x, y: e.y, m })} />)}
          {Array.from({ length: cols - r.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />)}
        </View>
      ))}
    </ScrollView>
  );

  const full = page === 'new' || page === 'requirements' || page === 'pi';
  return (
    <View ref={root} style={{ flex: 1, padding: 16, gap: 14, backgroundColor: p.bg }}>
      {!full ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
          <Text style={{ fontSize: 20, fontWeight: 700, color: p.text }}>{tr('machines.title')}</Text>
          <View style={{ flex: 1 }} />
          {sorted.length ? <>
            <Summary label={tr('machines.sum.running')} value={tr('machines.sum.of', { a: sum.running.a, b: sum.running.b })} frac={sum.running.frac} color={p.ok} />
            {sum.memory ? <Summary label={tr('machines.sum.memory')} value={tr('machines.sum.ofGb', { a: gib(sum.memory.a), b: gib(sum.memory.b) })} frac={sum.memory.frac} color={p.mem} /> : null}
            {sum.free ? <Summary label={tr('machines.sum.free')} value={bytes(sum.free.a)} frac={sum.free.frac} color={p.disk} /> : null}
          </> : null}
          {host.arch === 'arm64' && host.board ? <Btn label={tr('machines.pi.button')} onClick={() => setPage('pi')} /> : null}
          {missing.length || (host.kvmDevice && !host.kvm) ? <Btn label={tr('machines.req.button')} onClick={() => setPage('requirements')} style={{ backgroundColor: p.warnBg }} /> : null}
          <Btn primary label={tr('machines.new')} onClick={() => setPage('new')} />
        </View>
      ) : null}
      {error ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
      {body}
      {menu ? <PopMenu x={Math.min(menu.x - rl[0], rl[2] - 232)} y={Math.min(menu.y - rl[1], rl[3] - 330)}
        items={menuFor(menu.m)} onClose={() => setMenu(null)} /> : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.deleteDetail')} okLabel={tr('machines.deleteOk')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => remove(del)} onCancel={() => setDel(null)} /> : null}
    </View>
  );
}
