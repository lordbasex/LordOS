// Configure (the approved mock "Configure"): a sidebar of sections with a one-line summary each, and the section's
// pane. What lord-vm can change (name, CPU, memory, network, disk size, the ISO) goes through `lord-vm --set`, only
// while the machine is off; what the app keeps (system, notes) is saved at once. The name is the exception (6.26): while
// the machine runs it changes alone (`lord-vm --rename`), and its folders follow it once it's off.
import { View, Text, TextInput, Stepper, Segmented, Toggle, Slider, FilePicker, ScrollView, useAppWindow, wm, revealPath, openFolder, clipboard, t as tr, useSidebar, SidebarToggle, SidebarOverlay, screen, useSound, microphones, settings, formatNumber } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, memLimit, systemOf, closeChoice, CLOSE_CHOICES, shareTag, mountCommand, NET_MODES, PORTS_MAX, PORT_HOST_MIN, usesPorts, portRule,
  portProblems, newPortRule, portsForMode, portInput, defaultCard, netSummary, renameNote, folderOffer, renameOutcome, folderOf, no3dReason, gfxFor, soundOf, soundSummary, soundDevs, soundDevChosen, bootList, bootMove, bootSummary, resolutions, gvtGpu, gvtSpare, gvtType, gpuIds, memlockNeed, memlockFits, memlockMaxMemory, toolsAutoOn, vcpuAdvice } from './model.js';
import { usePal, has, gb, bytes, Bar, Btn, MenuBtn, OsMark, vmRefusal, soundDevText, soundDevItems, noteMoved } from './ui.jsx';
import { readMeta, writeMeta, readCpuUse } from './meta.js';
import { defaultsOf } from './store.js';
import { SideRow, PaneHeader, Pill, Section } from './kit.jsx';
import { BUILTIN, builtinProfile, onHost } from './profiles.js';

const SECTIONS = ['general', 'startup', 'cpu', 'display', 'network', 'sound', 'disks', 'sharing', 'agent', 'boot', 'security'];
// each section's glyph (assets/machines/symbols)
const ICONS = { general: 'general', startup: 'startup', cpu: 'cpu', display: 'display', network: 'network', sound: 'sound', disks: 'disks',
  sharing: 'sharing', agent: 'agent', boot: 'boot', security: 'security' };

// a group of rows on a rounded card, with a title above it when it has one
function Group({ children, title }) { return <Section title={title}>{children}</Section>; }
function Row({ label, value, children, last, note }) {
  const p = usePal();
  return (
    <View style={{ paddingHorizontal: 14, paddingVertical: 12, gap: 8, borderBottomWidth: last ? 0 : 1, borderColor: p.line }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{label}</Text>
        {value != null ? <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, direction: 'ltr' }}>{value}</Text> : null}
      </View>
      {children}
      {note ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{note}</Text> : null}
    </View>
  );
}
// a control that can't change now (the machine runs): dimmed and not clickable
const Off = ({ off, children }) => <View style={{ opacity: off ? 0.45 : 1, pointerEvents: off ? 'none' : 'auto' }}>{children}</View>;

// Configure → Network (6.5): the mode, the wired card of a bridged machine, the forwarded ports of a shared or host-only
// one. Everything here needs the machine off; lord-vm checks it all again (machine.c, vm_ports_check).
function NetworkPane({ m, cur, set, running, offNote, host, issues }) {
  const p = usePal();
  const cards = host?.cards || [];
  const mode = (x) => {
    const patch = { network: x, ports: portsForMode(cur.ports, x) };
    if (x === 'bridged' && !cur.bridge) patch.bridge = defaultCard(cards, '');
    set(patch);
  };
  const setRule = (i, patch) => set({ ports: cur.ports.map((r, k) => (k === i ? { ...r, ...patch } : r)) });
  const problem = (i) => issues.find((x) => x.i === i);
  const card = cards.find((c) => c.name === cur.bridge);
  const cardLabel = (c) => (c.kind === 'wifi' ? tr('machines.cfg.bridgeWifi', { name: c.name }) : c.up ? c.name : tr('machines.cfg.bridgeCardDown', { name: c.name }));
  const lan = cur.network === 'shared' && cur.ports.some((r) => r.open === 'lan');
  const tooMany = issues.find((x) => x.i === -1);
  return <>
    <Group>
      <Row label={tr('machines.cfg.netMode')} note={tr(`machines.cfg.netNote.${cur.network}`) + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Segmented small value={cur.network} onChange={mode} options={NET_MODES.map((k) => [k, tr(`machines.cfg.net.${k}`)])} /></Off>
      </Row>
      {cur.network === 'bridged' ? (
        <Row label={tr('machines.cfg.bridgeCard')} note={cards.some((c) => c.kind === 'wired') ? tr('machines.cfg.bridgeNote') : tr('machines.cfg.bridgeNoCards')}>
          <Off off={running}><View style={{ alignSelf: 'flex-start' }}>
            <MenuBtn label={card ? cardLabel(card) : cur.bridge || tr('machines.cfg.bridgePick')}
              items={cards.map((c) => ({ label: cardLabel(c), disabled: c.kind !== 'wired', onClick: () => set({ bridge: c.name }) }))} />
          </View></Off>
        </Row>
      ) : null}
      <Row label={tr('machines.cfg.mac')} value={m.mac || '—'} last />
    </Group>
    {usesPorts(cur.network) ? (
      <Group title={tr('machines.cfg.ports')}>
        {cur.ports.length ? cur.ports.map((r, i) => {
          const bad = problem(i);
          return (
            <Row key={i} label={tr('machines.cfg.portRule', { n: i + 1 })} note={bad ? tr(`machines.cfg.portProblem.${bad.code}`, { min: PORT_HOST_MIN }) : null}>
              <Off off={running}><View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 8, direction: 'ltr' }}>
                <Segmented small value={r.proto} onChange={(x) => setRule(i, { proto: x })} options={[['tcp', 'TCP'], ['udp', 'UDP']]} />
                <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.cfg.portHost')}</Text>
                <TextInput value={r.host ? String(r.host) : ''} onChange={(x) => setRule(i, { host: portInput(x) })} style={{ width: 64 }} />
                <Text style={{ fontSize: 12, color: p.dim }}>→ {tr('machines.cfg.portGuest')}</Text>
                <TextInput value={r.guest ? String(r.guest) : ''} onChange={(x) => setRule(i, { guest: portInput(x) })} style={{ width: 64 }} />
                {cur.network === 'shared' ? <Segmented small value={r.open} onChange={(x) => setRule(i, { open: x })}
                  options={[['this-computer', tr('machines.cfg.portOpen.local')], ['lan', tr('machines.cfg.portOpen.lan')]]} /> : null}
                <Btn label={tr('machines.cfg.portRemove')} onClick={() => set({ ports: cur.ports.filter((_, k) => k !== i) })} />
              </View></Off>
            </Row>
          );
        }) : <Row label={tr('machines.cfg.portsNone')} />}
        <Row label="" last note={tooMany ? tr('machines.cfg.portProblem.tooMany', { n: PORTS_MAX }) : tr(cur.network === 'hostonly' ? 'machines.cfg.portsNoteHostOnly' : 'machines.cfg.portsNote')}>
          <Off off={running}><View style={{ alignSelf: 'flex-start' }}>
            <Btn label={tr('machines.cfg.portsAdd')} disabled={running || cur.ports.length >= PORTS_MAX} onClick={() => set({ ports: [...cur.ports, newPortRule(cur.ports)] })} />
          </View></Off>
        </Row>
      </Group>
    ) : null}
    {lan ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start', lineHeight: 1.5 }}>{tr('machines.cfg.portsLanWarn')}</Text></View> : null}
  </>;
}

// onClose: shown as a sheet over the library or the machine's window (kit.jsx), with Cancel and Done; without it, a
// window of its own (args.configure) as before
// what a rename's job said, kept for the Configure that opens on the machine's new folder (this one goes with the old)
let carried = null;
// lord-vm's problem ('name-taken') as Configure's text key
const PROBLEMS = { 'name-taken': 'nameTaken', 'folder-taken': 'folderTaken', 'transfer-taken': 'transferTaken' };
const problemKey = (pr) => `machines.cfg.rn.${PROBLEMS[pr] || 'badName'}`;

// The vCPU recommendation (docs/VIRTUALIZATION.md, "vCPU recommendation"): what the machine's vCPUs used over its runs
// (lord-vm's cpu-usage.json) and the count suggested from it, with the reason. "Use N" writes it now when the machine is
// off, or for its next start while it runs or is suspended (app.json nextVcpus, applied by startPrep); never by itself.
function VcpuAdvice({ m, host, meta, later, busy, use, keep }) {
  const p = usePal();
  const adv = vcpuAdvice(readCpuUse(m.dir), m.vcpus, host);
  const next = meta.nextVcpus && meta.nextVcpus !== m.vcpus ? meta.nextVcpus : 0;
  if (!adv && !next) return null;
  return (
    <View style={{ gap: 8, alignItems: 'flex-start', padding: 14, borderRadius: 12, backgroundColor: p.card, border: `1px solid ${p.line}` }}>
      {adv ? <>
        <Text style={{ fontSize: 13, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr(`machines.cfg.vcpu.${adv.kind}`, { n: adv.n })}</Text>
        <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
          {tr('machines.cfg.vcpu.why', { avg: formatNumber(adv.avg, 1), total: adv.vcpus, hours: formatNumber(Math.round(adv.hours)), peak: formatNumber(adv.peak, 1) })}
          {' '}{tr(`machines.cfg.vcpu.${adv.kind}Note`)}</Text>
      </> : null}
      {next ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{tr('machines.cfg.vcpu.pending', { n: next })}</Text>
        <Btn label={tr('machines.cfg.vcpu.keep', { n: m.vcpus })} onClick={keep} />
      </View> : <Btn primary label={tr('machines.cfg.vcpu.use', { n: adv.n })} disabled={busy} onClick={() => use(adv.n)} />}
      {!next && later ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.cfg.vcpu.later')}</Text> : null}
    </View>
  );
}

export function Configure({ dir, section: section0, onClose }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [m, setM] = useState(null);
  const [host] = useState(() => (has() ? lord.vm.host() : null));
  // the computer's graphics cards (lord.vm.gpus): a spare Intel iGPU shared with machines (docs/MACHINES-GPU.md, Mode A)
  const [gpus, setGpus] = useState(null);
  const [sec, setSec] = useState(SECTIONS.includes(section0) ? section0 : 'general');
  const [draft, setDraft] = useState({});
  const [meta, setMeta] = useState(() => readMeta(dir));
  const [job, setJob] = useState(0);
  const [msg, setMsg] = useState(() => { const c = carried && carried.dir === dir ? carried.msg : null; carried = null; return c; });
  const [picking, setPicking] = useState(false);
  const [pickShare, setPickShare] = useState(false);
  const [disk, setDisk] = useState(null);
  const [sndDraft, setSndDraft] = useState({});
  const sndSt = useSound();                          // the outputs and inputs, as Settings → Sound lists them
  const volTimer = useRef(0);
  useEffect(() => () => clearTimeout(volTimer.current), []);
  // a small window: the sections hide behind a button beside the title and open over the pane
  const side = useSidebar(640, 'machines-configure');
  const load = () => {
    const x = has() ? lord.vm.list().find((y) => y.dir === dir) : null;
    setM(x || null);
    if (x) { const s = lord.vm.stats(dir); setDisk(s?.diskBytes ?? null); }
    // its shares' free instances change as other machines start and stop
    if (x && typeof lord.vm?.gpus === 'function') { try { setGpus(lord.vm.gpus()); } catch (e) { setGpus(null); } }
  };
  useEffect(() => { load(); const timer = setInterval(load, 2000); return () => clearInterval(timer); }, []);
  useEffect(() => { if (m && !onClose) wm.setTitle(winId, tr('machines.cfg.title', { name: m.name })); }, [m?.name]);
  const [closing, setClosing] = useState(false);     // Done pressed: close once the save is through
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      setSndDraft({});
      // a new name (6.26): where its folder went, or why it stayed
      const out = renameOutcome(r.out);
      if (r.ok) {
        setDraft({});
        const text = out.folder === 'moved' ? tr('machines.cfg.rn.moved', { folder: folderOf(out.dir) }) + (out.why ? ` ${tr('machines.cfg.rn.note', { why: out.why })}` : '')
          : out.folder === 'kept' ? tr('machines.cfg.rn.kept', { why: out.why || '' })
          : out.folder === 'later' ? tr('machines.cfg.rn.later', { folder: out.to || '' }) : tr('machines.cfg.saved');
        setMsg({ ok: out.folder !== 'kept', text });
        if (closing && onClose) onClose();
        if (out.folder === 'moved' && out.dir && out.dir !== dir) { carried = { dir: out.dir, msg: { ok: true, text } }; noteMoved(dir, out.dir); return; }
      } else if (out.problem) { setClosing(false); setMsg({ ok: false, text: tr(problemKey(out.problem), { name: out.other || out.name || '', folder: out.folder || '' }) }); }
      else { setClosing(false); setMsg({ ok: false, text: r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() }); }
      load();
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  if (!m) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.win.gone')}</Text></View>;

  const running = stateView(m.state).active;
  const cur = { name: m.name, vcpus: m.vcpus, memoryMb: m.memoryMb, diskGb: m.diskGb, network: m.network, bridge: m.bridge || '', ports: (m.ports || []).map(portRule),
    agent: !!m.agent, clipboard: m.clipboard || 'off', clipboardAsk: !!m.clipboardAsk, shares: m.shares || [], transfer: !!m.transfer,
    graphics: m.graphics || '2d', resolution: m.resolution || 'follow', bootOrder: m.bootOrder || 'auto', bootMenu: !!m.bootMenu,
    pointer: m.pointer || 'lordos', gpu: m.gpu || '', captureFullscreen: !!m.captureFullscreen, perfProfile: !!m.perfProfile, cpuPin: m.cpuPin || 'off', ...draft };
  const set = (patch) => { setMsg(null); setDraft((d) => ({ ...d, ...patch })); };
  // the shares list is compared by value
  const differs = (k, val) => (Array.isArray(val) ? JSON.stringify(val) !== JSON.stringify(k === 'ports' ? (m.ports || []).map(portRule) : m[k] || [])
    : val !== (k === 'bridge' ? m.bridge || '' : k === 'bootOrder' ? m.bootOrder || 'auto' : k === 'bootMenu' ? !!m.bootMenu : k === 'pointer' ? m.pointer || 'lordos' : k === 'gpu' ? m.gpu || '' : k === 'captureFullscreen' ? !!m.captureFullscreen : k === 'perfProfile' ? !!m.perfProfile : k === 'cpuPin' ? m.cpuPin || 'off' : m[k]));
  // the network's rules are checked as lord-vm checks them: Save waits until they are right
  const portIssues = portProblems(cur.ports, cur.network);
  const netBad = (usesPorts(cur.network) && portIssues.length > 0) || (cur.network === 'bridged' && !cur.bridge);
  const dirty = Object.keys(draft).some((k) => differs(k, draft[k]));
  // the name (6.26): what the draft's name would do (lord.vm.renamePlan reads the library), and while it's off whether
  // its folder still says another name
  const plan = has() && typeof lord.vm.renamePlan === 'function' && cur.name !== m.name ? lord.vm.renamePlan(dir, cur.name) : null;
  const rn = renameNote(plan, { name: cur.name, current: m.name, running });
  const offer = !running && !dirty && has() && typeof lord.vm.renamePlan === 'function' ? folderOffer(lord.vm.renamePlan(dir, m.name), running) : null;
  // while it runs only the name may change, alone
  const onlyName = Object.keys(draft).every((k) => k === 'name' || !differs(k, draft[k]));
  const save = (patch) => {
    const change = patch || Object.fromEntries(Object.entries(draft).filter(([k, val]) => differs(k, val)));
    if (!Object.keys(change).length) return;
    if (!rn.ok && 'name' in change) { setMsg({ ok: false, text: tr(rn.note.key, rn.note.vars) }); return; }
    const id = running && 'name' in change ? lord.vm.rename(dir, change.name) : lord.vm.set(dir, change);
    if (!id) setMsg({ ok: false, text: vmRefusal(tr('machines.cfg.refused')) }); else { setJob(id); setMsg({ ok: true, text: tr('machines.cfg.saving') }); }
  };
  // sound (6.9): lord.vm.sound applies it to a running machine now, or writes it with lord-vm --set (a job) when it's off
  const setSound = (patch) => {
    setSndDraft((d) => ({ ...d, ...patch }));
    const r = has() ? lord.vm.sound(dir, patch) : null;
    if (!r) { setSndDraft({}); setMsg({ ok: false, text: tr('machines.cfg.soundFailed') }); return; }
    if (r.job) setJob(r.job); else { setSndDraft({}); load(); }
  };
  // the slider: shown as it moves, applied once it rests for a quarter second
  const volumeTo = (v) => {
    setSndDraft((d) => ({ ...d, volume: v }));
    clearTimeout(volTimer.current);
    volTimer.current = setTimeout(() => setSound({ volume: v }), 250);
  };
  const emulated = m.accel === 'tcg';
  // the profile it was made with, while its CPU, memory and network are still that profile's here
  const profFor = (k) => onHost(builtinProfile(k, host), host, { gpuReady: true, no3d: no3dReason(host, m.windowsDevices, m.arch) });
  const prof = (() => { const k = meta.profile, x = { ...m, ...draft }; if (!BUILTIN.includes(k)) return ''; const s = profFor(k); return s.vcpus === x.vcpus && s.memoryMb === x.memoryMb && s.network === x.network ? k : ''; })();
  const summary = {
    general: [meta.osName || systemOf(m.name).name, m.arch, prof ? tr(`machines.as.profile.${prof}`) : null].filter(Boolean).join(', '),
    startup: tr(`machines.cfg.close.${closeChoice(meta)}`) + (m.suspended ? `, ${tr('machines.doing.suspended')}` : ''),
    cpu: tr('machines.cfg.sum.cpu', { n: m.vcpus, mem: gb(m.memoryMb) }),
    display: m.display === 'window' ? (m.resolution && m.resolution !== 'follow' ? m.resolution.replace('x', ' × ') : tr('machines.cfg.sum.display')) + (m.graphics === '3d' ? `, ${tr('machines.cfg.graphics3d')}` : '') + (m.gpu ? `, ${tr('machines.cfg.sum.gpu')}` : '') : tr('machines.cfg.sum.noDisplay'),
    network: (() => {
      const ns = netSummary(m);
      return [tr(`machines.cfg.net.${ns.mode}`), ns.card || null, ns.ports ? tr('machines.cfg.sum.ports', { n: ns.ports }) : null].filter(Boolean).join(', ');
    })(),
    sound: (() => {
      const x = soundSummary(m), out = soundDevs(m).output;
      return [tr(x.key, { volume: x.volume }), tr(x.mic), out ? soundDevText(soundDevChosen(sndSt.sinks, out), true) : null].filter(Boolean).join(', ');
    })(),
    disks: tr('machines.cfg.sum.disks', { n: m.diskGb }) + (m.dataDisks ? ` + ${m.dataDisks}` : '') + (m.devices === 'compat' ? `, ${tr('machines.cfg.devCompatShort')}` : '') + (m.iso ? `, ${tr('machines.isoAttached')}` : ''),
    // "Nothing shared" only when neither the clipboard nor a folder is
    sharing: m.shares?.length || m.transfer ? [m.transfer ? tr('machines.cfg.transfer') : null, m.clipboard && m.clipboard !== 'off' ? tr(`machines.cfg.clipSum.${m.clipboard}`) : null,
      m.shares?.length === 1 ? tr('machines.cfg.shareSum1') : m.shares?.length ? tr('machines.cfg.shareSum', { n: m.shares.length }) : null].filter(Boolean).join(', ')
      : tr(`machines.cfg.clipSum.${m.clipboard || 'off'}`),
    agent: m.agent ? (m.agentState === 'connected' ? tr('machines.agent.connected') : tr('machines.cfg.agentOn')) : tr('machines.cfg.agentOffSum'),
    boot: [m.firmware === 'uefi' ? 'UEFI' : 'BIOS', bootSummary(m.bootOrder) ? tr(bootSummary(m.bootOrder)) : null, m.tpm ? tr('machines.cfg.tpm') : null,
      m.firmware === 'uefi' && m.secureBoot ? tr('machines.cfg.secureBoot') : null].filter(Boolean).join(', '),
    security: tr('machines.cfg.sum.security'),
  };
  const offNote = running ? tr('machines.cfg.needsOff') : null;

  let pane = null;
  if (sec === 'general') pane = <>
    <Group>
      <Row label={tr('machines.new.name')}>
        <TextInput value={cur.name} onChange={(x) => set({ name: x })} />
        {rn.note ? <Text style={{ fontSize: 12, color: rn.note.tone === 'bad' ? '#ff453a' : p.dim, textAlign: 'start', lineHeight: 1.4 }}>{tr(rn.note.key, rn.note.vars)}</Text> : null}
      </Row>
      <Row label={tr('machines.cfg.system')}>
        <View style={{ flexDirection: 'row', gap: 10, alignItems: 'center' }}>
          <OsMark id={meta.os || systemOf(meta.osName, m.name).id} size={32} />
          <View style={{ flex: 1 }}><TextInput value={meta.osName || ''} placeholder={tr('machines.cfg.systemPlaceholder')}
            onChange={(x) => { writeMeta(dir, { osName: x }); setMeta(readMeta(dir)); }} /></View>
        </View>
      </Row>
      <Row label={tr('machines.cfg.profile')} note={tr('machines.cfg.profileNote') + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><View style={{ alignSelf: 'flex-start' }}><MenuBtn label={prof ? tr(`machines.as.profile.${prof}`) : tr('machines.cfg.profileCustom')}
          items={BUILTIN.map((k) => { const s = profFor(k); const gfx = builtinProfile(k, host).resources.graphics; return { label: tr(`machines.as.profile.${k}`), checked: prof === k,
            hint: tr('machines.as.spec', { cpus: s.vcpus, mem: gb(s.memoryMb), gfx: tr(`machines.as.gfx.${gfx}`) }),
            onClick: () => { const g = gfxFor(gfx, host, m.windowsDevices, m.arch); set({ vcpus: s.vcpus, memoryMb: s.memoryMb, network: s.network, graphics: g, ...(g === '3d' ? { gpu: '' } : {}) }); writeMeta(dir, { profile: k }); setMeta(readMeta(dir)); } }; })} /></View></Off>
      </Row>
      <Row label={tr('machines.cfg.arch')} value={emulated ? `${m.arch}, ${tr('machines.emulatedLower')}` : m.arch} />
      <Row label={tr('machines.cfg.created')} value={(m.created || '').replace('T', ' ').replace('Z', ' UTC')} />
      {m.imported ? <Row label={tr('machines.cfg.importedFrom')} note={tr('machines.cfg.importedNote')}>
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{meta.importedFrom || '—'}</Text>
      </Row> : null}
      <Row label={tr('machines.cfg.folder')} last note={offer ? tr('machines.cfg.rn.offer', { folder: offer.folder }) : null}>
        <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{dir}</Text>
          {offer ? <Btn label={tr('machines.cfg.rn.renameFolder')} disabled={!!job} onClick={() => save({ name: m.name })} /> : null}
          <Btn label={tr('machines.cmd.showInFiles')} onClick={() => revealPath(dir)} />
        </View>
      </Row>
    </Group>
    <Group><Row label={tr('machines.cfg.notes')} last>
      <TextInput value={meta.notes || ''} placeholder={tr('machines.cfg.notesPlaceholder')} onChange={(x) => { writeMeta(dir, { notes: x }); setMeta(readMeta(dir)); }} />
    </Row></Group>
  </>;
  else if (sec === 'startup') pane = <>
    <Group>
      <Row label={tr('machines.cfg.onClose')} note={tr('machines.cfg.onCloseNote')}>
        <Segmented small value={closeChoice(meta)} onChange={(x) => { writeMeta(dir, { onClose: x }); setMeta(readMeta(dir)); }}
          options={CLOSE_CHOICES.map((k) => [k, tr(`machines.cfg.close.${k}`)])} />
      </Row>
      <Row label={tr('machines.cfg.keepAwake')} last note={tr('machines.cfg.keepAwakeNote')}>
        <Toggle on={meta.keepAwake !== false} onChange={(x) => { writeMeta(dir, { keepAwake: x }); setMeta(readMeta(dir)); }} />
      </Row>
    </Group>
    {m.suspended ? <Group><Row label={tr('machines.cfg.saved')} value={bytes(m.stateBytes || 0)} last note={tr('machines.cfg.savedNote')} /></Group> : null}
  </>;
  else if (sec === 'cpu') {
    const memMax0 = host?.memMaxMb || m.memoryMb;
    // a machine with a graphics card has its memory locked while it runs, within "Memory machines may lock" (Settings →
    // Graphics cards): the slider stops where memory + 512 MB reaches that cap (a machine already over it keeps its value)
    const memlock = cur.gpu ? gpus?.memlock || null : null;
    const lockMax = memlock ? memlockMaxMemory(memlock) : Infinity;
    const cpus = host?.cpus || m.vcpus, memMax = Math.max(Math.min(memMax0, lockMax), Math.min(m.memoryMb, memMax0), 256);
    const lockOver = memlock && !memlockFits({ memoryMb: cur.memoryMb }, memlock);
    pane = <>
      <Group>
        <Row label={tr('machines.cfg.cores')} value={tr('machines.cfg.ofN', { a: cur.vcpus, b: cpus })}>
          <Off off={running}><View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
            <View style={{ flex: 1 }}><Slider value={cur.vcpus} min={1} max={cpus} step={1} width={340} onChange={(x) => set({ vcpus: Math.round(x) })} /></View>
            <Stepper value={cur.vcpus} min={1} max={cpus} onChange={(x) => set({ vcpus: x })} />
          </View></Off>
        </Row>
        <Row label={tr('machines.cfg.memory')} value={tr('machines.cfg.ofN', { a: gb(cur.memoryMb), b: gb(host?.ramMb || memMax) })} last>
          <Off off={running}><View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
            <View style={{ flex: 1 }}><Slider value={cur.memoryMb} min={256} max={memMax} step={256} width={340} onChange={(x) => set({ memoryMb: Math.round(x / 256) * 256 })} /></View>
            <Stepper value={cur.memoryMb} min={256} max={memMax} step={256} width={80} format={(x) => `${x} MB`} onChange={(x) => set({ memoryMb: x })} />
          </View></Off>
        </Row>
      </Group>
      {/* vCPU pinning (machine.json cpu_pin, docs/VIRTUALIZATION.md 3.7.1): lord-vm gives each vCPU a core of its own,
          away from the desktop's; a machine without a choice shows what this computer gives it */}
      <Group>
        <Row label={tr('machines.cfg.pin')} last note={[tr('machines.cfg.pinNote'), offNote].filter(Boolean).join(' ')}>
          <Off off={running}><Segmented small value={cur.cpuPin} onChange={(x) => set({ cpuPin: x })}
            options={[['off', tr('machines.cfg.off')], ['auto', tr('machines.cfg.pinAuto')]]} /></Off>
        </Row>
      </Group>
      <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
        {tr('machines.cfg.cpuNote', { limit: gb(memLimit({ ...m, ...cur })), max: gb(memMax0) })}{offNote ? ` ${offNote}` : ''}</Text>
      {memlock ? <View style={{ gap: 8, alignItems: 'flex-start' }}>
        <Text style={{ fontSize: 12, lineHeight: 1.5, color: lockOver ? '#ff9f0a' : p.dim, textAlign: 'start' }}>
          {tr(lockOver ? 'machines.cfg.memlockOver' : 'machines.cfg.memlockNote', { n: gb(memlockNeed({ memoryMb: cur.memoryMb })), cap: gb(memlock.capMb) })}</Text>
        <Btn label={tr('machines.cfg.memlockChange')} onClick={() => wm.open('settings', { args: { section: 'gpus', nonce: Date.now() } })} />
      </View> : null}
      {cur.vcpus > (host?.vcpusRecommended || 99) ? <Text style={{ fontSize: 12, color: '#ff9f0a', textAlign: 'start' }}>{tr('machines.new.warn.vcpus', { n: host.vcpusRecommended })}</Text> : null}
      <VcpuAdvice m={m} host={host} meta={meta} later={running || !!m.suspended} busy={!!job}
        use={(n) => { if (running || m.suspended) { writeMeta(dir, { nextVcpus: n }); setMeta(readMeta(dir)); } else { setDraft((d) => { const { vcpus, ...rest } = d; return rest; }); save({ vcpus: n }); } }}
        keep={() => { writeMeta(dir, { nextVcpus: undefined }); setMeta(readMeta(dir)); }} />
    </>;
  } else if (sec === 'display') {
    // 3D (6.10): Linux machines on a computer that can give it; a machine that has it can always go back to 2D
    const why3d = no3dReason(host, m.windowsDevices, m.arch);
    // a graphics card (Mode A, Intel GVT-g): amd64 UEFI machines, when a spare Intel iGPU is shared with machines; a vGPU
    // and 3D are exclusive, so choosing one sets the other back
    const igpu = gvtGpu(gpus), spare = igpu ? null : gvtSpare(gpus);
    const offerGpu = m.arch === 'amd64' && m.firmware === 'uefi' && (igpu || spare || cur.gpu);
    let gpuRow = null;
    if (offerGpu) {
      const card = igpu || spare;
      const gname = card ? tr('machines.cfg.gpuName', { id: gpuIds(card) }) : tr('machines.cfg.gpuMissing');
      const typeOf = (c) => (igpu ? gvtType(igpu, c, cur.resolution) : null);
      const freeText = (t) => (!t ? tr('machines.cfg.gpuFreeNone') : (t.available | 0) > 0 ? tr('machines.cfg.gpuFree', { n: t.available | 0 }) : tr('machines.cfg.gpuFreeNone'));
      // a share with no free instance can't be chosen, unless it's already this machine's
      const full = (c) => !igpu || (!(typeOf(c)?.available > 0) && (m.gpu || '') !== c);
      const pick = (x) => set(x && cur.graphics === '3d' ? { gpu: x, graphics: '2d' } : { gpu: x });
      const gnote = [tr('machines.cfg.gpuNote'), m.windowsDevices ? tr('machines.win.gpuDriverNote') : null,
        igpu ? tr('machines.cfg.gpuFreeLine', { whole: freeText(typeOf('intel-whole')), shared: freeText(typeOf('intel-shared')) }) : null,
        !igpu ? tr(spare ? 'machines.cfg.gpuTurnOn' : 'machines.cfg.gpuGone') : null, offNote].filter(Boolean).join(' ');
      gpuRow = <Row label={tr('machines.cfg.gpu')} value={gname} last={!m.windowsDevices || !!cur.gpu} note={gnote}>
        <Off off={running || (!igpu && !cur.gpu)}><Segmented small value={cur.gpu} onChange={pick}
          options={[['', tr('machines.cfg.gpuNone')], ['intel-whole', tr('machines.cfg.gpuWhole'), full('intel-whole')],
            ['intel-shared', tr('machines.cfg.gpuShared'), full('intel-shared')]]} /></Off>
        {spare && !igpu ? <View style={{ alignSelf: 'flex-start' }}>
          <Btn label={tr('machines.cfg.gpuOpenSettings')} onClick={() => wm.open('settings', { args: { section: 'gpus', nonce: Date.now() } })} />
        </View> : null}
      </Row>;
    }
    const note = [cur.graphics === '3d' ? tr('machines.cfg.graphics3dNote') : tr('machines.cfg.graphics2dNote'), why3d ? tr(`machines.cfg.no3d.${why3d}`) : null, offNote]
      .filter(Boolean).join(' ');
    pane = <>
    <Group>
      {/* the size the machine starts with (its EDID's preferred mode): every guest takes it at boot, also those whose
          drivers don't follow the window while they run */}
      <Row label={tr('machines.cfg.resolution')} note={[tr(cur.resolution === 'follow' ? 'machines.cfg.resolutionFollowNote' : 'machines.cfg.resolutionFixedNote'), offNote].filter(Boolean).join(' ')}>
        <Off off={running}><MenuBtn label={cur.resolution === 'follow' ? tr('machines.cfg.followsWindow') : cur.resolution.replace('x', ' × ')}
          items={[{ label: tr('machines.cfg.followsWindow'), checked: cur.resolution === 'follow', onClick: () => set({ resolution: 'follow' }) }, '-',
            ...resolutions(screen.width, screen.height, null).map(([w, h]) => ({ label: `${w} × ${h}`, checked: cur.resolution === `${w}x${h}`, onClick: () => set({ resolution: `${w}x${h}` }) }))]} /></Off>
      </Row>
      <Row label={tr('machines.cfg.graphics')} last={!m.windowsDevices && !gpuRow} note={note}>
        <Off off={running || (why3d && cur.graphics !== '3d')}><Segmented small value={cur.graphics} onChange={(x) => set(x === '3d' ? { graphics: x, gpu: '' } : { graphics: x })}
          options={[['2d', tr('machines.cfg.graphics2d')], ['3d', tr('machines.cfg.graphics3d'), !!why3d && cur.graphics !== '3d']]} /></Off>
      </Row>
      {/* who draws a Windows guest's pointer (7.1.2): LordOS, from the shape the guest hands over (no round trip through
          the guest for each move), or its display driver, into the picture; applied by the guest tools at the next start.
          Not with an Intel graphics card: HWCursor is the virtio driver's, and Intel's puts the pointer on the vGPU's cursor
          plane, which LordOS always draws */}
      {gpuRow}
      {m.windowsDevices && !cur.gpu ? <Row label={tr('machines.cfg.pointerDraw')} last note={[tr('machines.cfg.pointerDrawNote'), offNote].filter(Boolean).join(' ')}>
        <Off off={running}><Segmented small value={cur.pointer} onChange={(x) => set({ pointer: x })}
          options={[['lordos', tr('machines.cfg.pointerLordos')], ['guest', tr('machines.cfg.pointerGuest')]]} /></Off>
      </Row> : null}
    </Group>
    {/* the input: "Capture mouse" on entering full screen (machine.json capture_fullscreen, 6.25) */}
    <Group>
      <Row label={tr('machines.cfg.captureFs')} last note={[tr('machines.cfg.captureFsNote'), offNote].filter(Boolean).join(' ')}>
        <Off off={running}><Toggle on={cur.captureFullscreen} onChange={(x) => set({ captureFullscreen: x })} /></Off>
      </Row>
    </Group>
  </>;
  }
  else if (sec === 'network') pane = <NetworkPane m={m} cur={cur} set={set} running={running} offNote={offNote} host={host} issues={portIssues} />;
  else if (sec === 'sound') {
    // host-side choices (6.9): saved at once, and a running machine's window applies them at once too
    const snd = { ...soundOf(m), ...soundDevs(m), ...sndDraft };
    // the output and input (schema 8): every device Settings → Sound lists, by its name there; applied at once
    const sinks = sndSt.sinks, mics = microphones(sndSt);
    pane = <>
      <Group>
        <Row label={tr('machines.cfg.sound')} note={tr('machines.cfg.soundNote')}>
          <Toggle on={snd.sound} onChange={(x) => setSound({ sound: x })} />
        </Row>
        <Row label={tr('machines.cfg.volume')} value={`${snd.volume}%`}>
          <Off off={!snd.sound}><Slider value={snd.volume} min={0} max={100} step={5} width={340}
            onChange={(x) => volumeTo(Math.round(x / 5) * 5)} /></Off>
        </Row>
        <Row label={tr('machines.cfg.soundOutput')} last note={tr('machines.cfg.soundOutputNote')}>
          <View style={{ alignSelf: 'flex-start' }}>
            <MenuBtn label={soundDevText(soundDevChosen(sinks, snd.output), true)} items={soundDevItems(sinks, snd.output, (x) => setSound({ output: x }))} />
          </View>
        </Row>
      </Group>
      <Group>
        <Row label={tr('machines.cfg.microphone')} note={tr('machines.cfg.microphoneNote')}>
          <Toggle on={snd.microphone} onChange={(x) => setSound({ microphone: x })} />
        </Row>
        <Row label={tr('machines.cfg.soundInput')} last note={tr('machines.cfg.soundInputNote')}>
          <View style={{ alignSelf: 'flex-start' }}>
            <MenuBtn label={soundDevText(soundDevChosen(mics, snd.input), true)} items={soundDevItems(mics, snd.input, (x) => setSound({ input: x }))} />
          </View>
        </Row>
      </Group>
      <Group>
        <Row label={tr('machines.cfg.keyboard')} value={tr('machines.cfg.keyboardValue')} />
        <Row label={tr('machines.cfg.pointer')} value={tr('machines.cfg.pointerValue')} last note={tr('machines.cfg.pointerNote')} />
      </Group>
    </>;
  }
  else if (sec === 'disks') pane = <>
    <Group>
      <Row label={tr('machines.cfg.systemDisk')} value={tr('machines.disk', { n: cur.diskGb })} note={tr('machines.cfg.diskNote', { used: disk != null ? bytes(disk) : '—' }) + (offNote ? ` ${offNote}` : '')} last>
        <Off off={running}><Stepper value={cur.diskGb} min={m.diskGb} max={2048} width={72} format={(x) => `${x} GB`} onChange={(x) => set({ diskGb: x })} /></Off>
      </Row>
    </Group>
    {m.dataDisks ? <Group><Row label={tr('machines.cfg.dataDisks')} value={String(m.dataDisks)} last /></Group> : null}
    {m.imported || m.devices === 'compat' ? <Group>
      {/* an imported machine starts on devices every system has drivers for; the fast ones need the guest's drivers first */}
      <Row label={tr('machines.cfg.devices')} value={m.devices === 'compat' ? tr('machines.cfg.devCompat', { nic: m.nicModel }) : tr('machines.cfg.devFast')} last
        note={(m.devices === 'compat' ? tr('machines.cfg.devCompatNote') : tr('machines.cfg.devFastNote')) + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><View style={{ alignSelf: 'flex-start' }}><Btn label={m.devices === 'compat' ? tr('machines.cfg.devToFast') : tr('machines.cfg.devToCompat')} disabled={running || !!job}
          onClick={() => save(m.devices === 'compat' ? { devices: 'virtio', nicModel: 'virtio' } : { devices: 'compat', nicModel: 'e1000e' })} /></View></Off>
      </Row>
    </Group> : null}
    <Group>
      <Row label={tr('machines.cfg.cd')} value={m.iso ? m.iso.split('/').pop() : tr('machines.cfg.cdEmpty')} last note={running ? tr('machines.cfg.cdRunning') : null}>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Btn label={tr('machines.cfg.cdAttach')} disabled={running || !!job} onClick={() => setPicking(true)} />
          <Btn label={tr('machines.eject')} disabled={running || !m.iso || !!job} onClick={() => save({ iso: 'none' })} />
        </View>
      </Row>
    </Group>
  </>;
  else if (sec === 'sharing') pane = <>
    {/* the transfer folder (6.7.1): on for new machines; a device of the machine, so it changes with the machine off */}
    <Group>
      <Row label={tr('machines.cfg.transfer')} note={tr('machines.cfg.transferNote') + (m.windowsDevices ? ` ${tr('machines.win.transferNote')}` : '') + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Toggle on={!!cur.transfer} onChange={(x) => set({ transfer: x })} /></Off>
      </Row>
      <Row label={tr('machines.cfg.transferWhere')} last>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{m.transferPath || ''}</Text>
          <Btn label={tr('machines.tb.transferOpen')} disabled={!m.transfer} onClick={() => { const x = lord.vm.transferFolder(dir); if (x) openFolder(x); }} style={{ height: 28 }} />
        </View>
      </Row>
    </Group>
    <Group>
      <Row label={tr('machines.cfg.clipboard')} note={tr('machines.cfg.clipboardNote') + (offNote ? ` ${offNote}` : '')} last={cur.clipboard === 'off'}>
        <Off off={running}><Segmented small value={cur.clipboard} onChange={(x) => set({ clipboard: x, ...(x !== 'off' ? { agent: true } : {}) })}
          options={['off', 'to-guest', 'from-guest', 'both'].map((k) => [k, tr(`machines.cfg.clip.${k}`)])} /></Off>
      </Row>
      {/* how far the machine is trusted with it (6.8, machine.json clipboard_ask): on demand while in use, or asked each time */}
      {cur.clipboard !== 'off' ? (
        <Row label={tr('machines.cfg.clipTrust')} note={tr('machines.cfg.clipTrustNote') + (offNote ? ` ${offNote}` : '')} last>
          <Off off={running}><Segmented small value={cur.clipboardAsk ? 'ask' : 'trusted'} onChange={(x) => set({ clipboardAsk: x === 'ask' })}
            options={['trusted', 'ask'].map((k) => [k, tr(`machines.cfg.clipTrust.${k}`)])} /></Off>
        </Row>
      ) : null}
    </Group>
    <Group>
      <Row label={tr('machines.cfg.shares')} note={tr('machines.cfg.sharesNote') + (offNote ? ` ${offNote}` : '')} last={!cur.shares.length}>
        <Off off={running}><View style={{ alignSelf: 'flex-start' }}><Btn label={tr('machines.cfg.shareAdd')} disabled={running || !!job || cur.shares.length >= 4} onClick={() => setPickShare(true)} /></View></Off>
      </Row>
      {cur.shares.map((x, i) => (
        <Row key={x.tag} label={x.path.split('/').pop()} last={i === cur.shares.length - 1}
          note={tr('machines.cfg.shareMount', { cmd: mountCommand(x.tag) })}>
          <Off off={running}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
              <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{x.path}</Text>
              <Text style={{ fontSize: 12, color: p.text }}>{tr('machines.cfg.shareWritable')}</Text>
              <Toggle on={!!x.writable} onChange={(w) => set({ shares: cur.shares.map((y, k) => (k === i ? { ...y, writable: w } : y)) })} />
              <Btn label={tr('machines.cfg.shareCopy')} onClick={() => clipboard.set(mountCommand(x.tag))} style={{ height: 28 }} />
              <Btn label={tr('machines.cfg.shareRemove')} danger onClick={() => set({ shares: cur.shares.filter((_, k) => k !== i) })} style={{ height: 28 }} />
            </View>
          </Off>
        </Row>
      ))}
    </Group>
  </>;
  else if (sec === 'agent') pane = <>
    <Group>
      <Row label={tr('machines.cfg.agent')} note={tr('machines.cfg.agentNote') + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Toggle on={!!cur.agent} onChange={(x) => set({ agent: x, ...(!x ? { clipboard: 'off' } : {}) })} /></Off>
      </Row>
      <Row label={tr('machines.cfg.agentInstall')} note={tr('machines.cfg.agentInstallNote')} />
      {/* the guest's performance profile (7.1.4, machine.json perf_profile): applied by the LordOS tools for Windows (w11;
          Windows Search through lord-vm's fixed script) or the Linux clipboard helper (7) while the machine runs, put
          back after; off by default */}
      <Row label={tr('machines.cfg.perfProfile')} note={[tr(m.windowsDevices ? 'machines.win.perfProfileNote' : 'machines.cfg.perfProfileNote'), offNote].filter(Boolean).join(' ')}>
        <Off off={running}><Toggle on={cur.perfProfile} onChange={(x) => set({ perfProfile: x })} /></Off>
      </Row>
      {/* the LordOS tools brought up to date by themselves (model.js toolsAutoStep): app.json, at once, also while it runs */}
      <Row label={tr('machines.cfg.toolsAuto')} last note={tr('machines.cfg.toolsAutoNote')}>
        <Toggle on={toolsAutoOn(meta, defaultsOf(settings.get().machinesDefaults))} onChange={(x) => { writeMeta(dir, { toolsAuto: x }); setMeta(readMeta(dir)); }} />
      </Row>
    </Group>
  </>;
  else if (sec === 'boot') pane = <>
    <Group>
      <Row label={tr('machines.cfg.firmware')} value={m.firmware === 'uefi' ? 'UEFI' : 'BIOS'} />
      {/* TPM 2.0 (swtpm) and Secure Boot: machine.json's tpm and secure_boot, set when the machine is made */}
      <Row label={tr('machines.cfg.tpm')} value={tr(m.tpm ? 'machines.cfg.on' : 'machines.cfg.off')} note={tr('machines.cfg.setAtCreate')} />
      {m.firmware === 'uefi' ? <Row label={tr('machines.cfg.secureBoot')} value={tr(m.secureBoot ? 'machines.cfg.on' : 'machines.cfg.off')} note={tr('machines.cfg.setAtCreate')} /> : null}
    </Group>
    {/* where it starts from (machine.json's boot_order): automatic, or the devices in the order the firmware tries them */}
    <Group>
      <Row label={tr('machines.cfg.bootOrder')} last={cur.bootOrder === 'auto'}
        value={cur.bootOrder === 'auto' ? (m.iso ? tr('machines.cfg.bootDiskThenCd') : tr('machines.cfg.bootDisk')) : null}
        note={(cur.bootOrder === 'auto' ? tr('machines.cfg.bootNote') : tr('machines.cfg.bootCustomNote')) + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Segmented small value={cur.bootOrder === 'auto' ? 'auto' : 'custom'}
          onChange={(x) => set({ bootOrder: x === 'auto' ? 'auto' : bootList(m.bootOrder !== 'auto' && m.bootOrder ? m.bootOrder : 'auto').join(',') })}
          options={[['auto', tr('machines.cfg.bootAuto')], ['custom', tr('machines.cfg.bootCustom')]]} /></Off>
      </Row>
      {cur.bootOrder === 'auto' ? null : bootList(cur.bootOrder).map((d, i, a) => (
        <Row key={d} label={`${i + 1}. ${tr(`machines.cfg.bootDev.${d}`)}`} last={i === a.length - 1}
          note={d === 'network' && m.network === 'isolated' ? tr('machines.cfg.bootNetNone') : null}>
          <Off off={running}><View style={{ flexDirection: 'row', gap: 8 }}>
            <Btn label={tr('machines.cfg.bootUp')} disabled={running || i === 0} onClick={() => set({ bootOrder: bootMove(a, i, -1).join(',') })} style={{ height: 28 }} />
            <Btn label={tr('machines.cfg.bootDown')} disabled={running || i === a.length - 1} onClick={() => set({ bootOrder: bootMove(a, i, 1).join(',') })} style={{ height: 28 }} />
          </View></Off>
        </Row>
      ))}
    </Group>
    <Group>
      <Row label={tr('machines.cfg.bootMenu')} last note={tr('machines.cfg.bootMenuNote') + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Toggle on={cur.bootMenu} onChange={(x) => set({ bootMenu: x })} /></Off>
      </Row>
    </Group>
  </>;
  else if (sec === 'security') pane = <Group>
    {['sandbox', 'files', 'network', 'limits', 'display', 'secrets'].map((k, i, a) => {
      // the network's line follows the mode: passt listening on the forwarded ports, or the bridged card's device
      const note = k !== 'network' ? k : m.network === 'bridged' ? 'networkBridged' : usesPorts(m.network) && m.ports?.length ? 'networkPorts' : k;
      return <Row key={k} label={tr(`machines.cfg.sec.${k}`)} last={i === a.length - 1} note={tr(`machines.cfg.secNote.${note}`, { limit: gb(memLimit(m)) })} />;
    })}
  </Group>;

  const savable = ['general', 'cpu', 'display', 'network', 'disks', 'sharing', 'agent', 'boot'].includes(sec);
  const list = (
    <View style={{ width: 250, ...(side.narrow ? { flex: 1 } : null), paddingHorizontal: 10, paddingVertical: 12, gap: 2,
      backgroundColor: p.dark ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.025)', borderEndWidth: 1, borderColor: p.line }}>
      <Text style={{ marginHorizontal: 10, marginBottom: 6, fontSize: 12, fontWeight: 700, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{m.name}</Text>
      <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 2 }}>
        {SECTIONS.map((k) => <SideRow key={k} icon={ICONS[k]} title={tr(`machines.cfg.sec.title.${k}`)} detail={summary[k]} selected={sec === k}
          onClick={() => { setSec(k); if (side.narrow) side.close(); }} />)}
      </ScrollView>
    </View>
  );
  // Done keeps every change made in any section: the draft holds only fields lord-vm --set writes, whichever pane is
  // showing when Done is pressed (it used to close without saving unless the pane itself was a saving one)
  const done = () => {
    if (!dirty) { onClose(); return; }
    if (netBad) return;
    if (running && !onlyName) { setMsg({ ok: false, text: tr('machines.cfg.runningBanner') }); return; }
    if (!rn.ok) { setMsg({ ok: false, text: tr(rn.note.key, rn.note.vars) }); return; }
    setClosing(true); save();
  };
  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: onClose ? 'transparent' : p.bg }}>
      {side.narrow ? null : list}
      <View style={{ flex: 1, minWidth: 0, padding: side.narrow ? 16 : 22, gap: 16 }}>
        {side.narrow ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <SidebarToggle shown={side.shown} onClick={side.toggle} />
            <View style={{ flex: 1, minWidth: 0 }}><PaneHeader icon={ICONS[sec]} title={tr(`machines.cfg.sec.title.${sec}`)} detail={summary[sec]} /></View>
          </View>
        ) : <PaneHeader icon={ICONS[sec]} title={tr(`machines.cfg.sec.title.${sec}`)} detail={summary[sec]} />}
        {running ? <View style={{ padding: 10, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.cfg.runningBanner')}</Text></View> : null}
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 14 }}>{pane}</ScrollView>
        {msg ? <Text style={{ fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
        {onClose ? (
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 10 }}>
            <Pill label={tr('machines.cancel')} disabled={!!job} onClick={() => { setDraft({}); setMsg(null); onClose(); }} />
            <Pill primary label={tr('machines.cfg.done')} disabled={!!job || (dirty && (netBad || !rn.ok))} onClick={done} />
          </View>
        ) : savable || dirty ? (
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 10 }}>
            <Pill label={tr('machines.cancel')} disabled={!dirty || !!job} onClick={() => { setDraft({}); setMsg(null); }} />
            <Pill primary label={tr('machines.cfg.save')} disabled={!dirty || (running && !onlyName) || !!job || netBad || !rn.ok} onClick={() => save()} />
          </View>
        ) : null}
      </View>
      {pickShare ? <FilePicker mode="folder" title={tr('machines.cfg.shareAdd')} onClose={() => setPickShare(false)}
        onPick={(x) => {
          setPickShare(false);
          const path = Array.isArray(x) ? x[0] : x;
          if (!path || cur.shares.some((y) => y.path === path)) return;
          set({ shares: [...cur.shares, { tag: shareTag(path.split('/').pop(), [...cur.shares.map((y) => y.tag), 'LordOS']), path, writable: false }] });
        }} /> : null}
      {picking ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPicking(false)}
        onPick={(x) => { setPicking(false); save({ iso: Array.isArray(x) ? x[0] : x }); }} /> : null}
      {side.narrow && side.shown ? <SidebarOverlay width={260} onClose={side.close}>{list}</SidebarOverlay> : null}
    </View>
  );
}
