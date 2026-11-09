// Configure (the approved mock "Configure"): a sidebar of sections with a one-line summary each, and the section's
// pane. What lord-vm can change (name, CPU, memory, network, disk size, the ISO) goes through `lord-vm --set`, only
// while the machine is off; what the app keeps (system, notes) is saved at once. Sections of later phases say so.
import { View, Text, TextInput, Stepper, Segmented, Toggle, FilePicker, ScrollView, useAppWindow, wm, revealPath, clipboard, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { stateView, memLimit, systemOf, closeChoice, CLOSE_CHOICES, PROFILES, profileSize, profileOf, shareTag, mountCommand } from './model.js';
import { usePal, has, gb, bytes, Bar, Btn, MenuBtn, OsMark, vmRefusal } from './ui.jsx';
import { readMeta, writeMeta } from './meta.js';

const SECTIONS = ['general', 'startup', 'cpu', 'display', 'network', 'sound', 'disks', 'sharing', 'agent', 'boot', 'security'];

function Group({ children }) {
  const p = usePal();
  return <View style={{ borderRadius: 12, backgroundColor: p.card }}>{children}</View>;
}
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
function Later({ text }) {
  const p = usePal();
  return <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.raised }}><Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.5 }}>{text || tr('machines.cfg.later')}</Text></View>;
}

export function Configure({ dir, section: section0 }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [m, setM] = useState(null);
  const [host] = useState(() => (has() ? lord.vm.host() : null));
  const [sec, setSec] = useState(SECTIONS.includes(section0) ? section0 : 'general');
  const [draft, setDraft] = useState({});
  const [meta, setMeta] = useState(() => readMeta(dir));
  const [job, setJob] = useState(0);
  const [msg, setMsg] = useState(null);
  const [picking, setPicking] = useState(false);
  const [pickShare, setPickShare] = useState(false);
  const [disk, setDisk] = useState(null);
  const load = () => {
    const x = has() ? lord.vm.list().find((y) => y.dir === dir) : null;
    setM(x || null);
    if (x) { const s = lord.vm.stats(dir); setDisk(s?.diskBytes ?? null); }
  };
  useEffect(() => { load(); const timer = setInterval(load, 2000); return () => clearInterval(timer); }, []);
  useEffect(() => { if (m) wm.setTitle(winId, tr('machines.cfg.title', { name: m.name })); }, [m?.name]);
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (r.ok) { setDraft({}); setMsg({ ok: true, text: tr('machines.cfg.saved') }); }
      else setMsg({ ok: false, text: r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() });
      load();
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  if (!m) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.win.gone')}</Text></View>;

  const running = stateView(m.state).active;
  const cur = { name: m.name, vcpus: m.vcpus, memoryMb: m.memoryMb, diskGb: m.diskGb, network: m.network, agent: !!m.agent, clipboard: m.clipboard || 'off', shares: m.shares || [], ...draft };
  const set = (patch) => { setMsg(null); setDraft((d) => ({ ...d, ...patch })); };
  // the shares list is compared by value
  const differs = (k, val) => (Array.isArray(val) ? JSON.stringify(val) !== JSON.stringify(m[k] || []) : val !== m[k]);
  const dirty = Object.keys(draft).some((k) => differs(k, draft[k]));
  const save = (patch) => {
    const change = patch || Object.fromEntries(Object.entries(draft).filter(([k, val]) => differs(k, val)));
    if (!Object.keys(change).length) return;
    const id = lord.vm.set(dir, change);
    if (!id) setMsg({ ok: false, text: vmRefusal(tr('machines.cfg.refused')) }); else { setJob(id); setMsg({ ok: true, text: tr('machines.cfg.saving') }); }
  };
  const emulated = m.accel === 'tcg';
  const prof = profileOf({ ...m, ...draft }, host, meta.profile);
  const summary = {
    general: [meta.osName || systemOf(m.name).name, m.arch, prof ? tr(`machines.as.profile.${prof}`) : null].filter(Boolean).join(', '),
    startup: tr(`machines.cfg.close.${closeChoice(meta)}`) + (m.suspended ? `, ${tr('machines.doing.suspended')}` : ''),
    cpu: tr('machines.cfg.sum.cpu', { n: m.vcpus, mem: gb(m.memoryMb) }),
    display: m.display === 'window' ? tr('machines.cfg.sum.display') : tr('machines.cfg.sum.noDisplay'),
    network: tr(`machines.cfg.net.${m.network}`),
    sound: tr('machines.cfg.sum.sound'),
    disks: tr('machines.cfg.sum.disks', { n: m.diskGb }) + (m.dataDisks ? ` + ${m.dataDisks}` : '') + (m.devices === 'compat' ? `, ${tr('machines.cfg.devCompatShort')}` : '') + (m.iso ? `, ${tr('machines.isoAttached')}` : ''),
    // "Nothing shared" only when neither the clipboard nor a folder is
    sharing: m.shares?.length ? [m.clipboard && m.clipboard !== 'off' ? tr(`machines.cfg.clipSum.${m.clipboard}`) : null,
      m.shares.length === 1 ? tr('machines.cfg.shareSum1') : tr('machines.cfg.shareSum', { n: m.shares.length })].filter(Boolean).join(', ')
      : tr(`machines.cfg.clipSum.${m.clipboard || 'off'}`),
    agent: m.agent ? (m.agentState === 'connected' ? tr('machines.agent.connected') : tr('machines.cfg.agentOn')) : tr('machines.cfg.agentOffSum'),
    boot: m.firmware === 'uefi' ? 'UEFI' : 'BIOS',
    security: tr('machines.cfg.sum.security'),
  };
  const offNote = running ? tr('machines.cfg.needsOff') : null;

  let pane = null;
  if (sec === 'general') pane = <>
    <Group>
      <Row label={tr('machines.new.name')} note={offNote}>
        <Off off={running}><TextInput value={cur.name} onChange={(x) => set({ name: x })} /></Off>
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
          items={PROFILES.map((k) => { const s = profileSize(k, host); return { label: tr(`machines.as.profile.${k}`), checked: prof === k,
            hint: tr('machines.as.spec', { cpus: s.vcpus, mem: gb(s.memoryMb), gfx: tr(`machines.as.gfx.${s.gfx}`) }),
            onClick: () => { set({ vcpus: s.vcpus, memoryMb: s.memoryMb, network: s.network }); writeMeta(dir, { profile: k }); setMeta(readMeta(dir)); } }; })} /></View></Off>
      </Row>
      <Row label={tr('machines.cfg.arch')} value={emulated ? `${m.arch}, ${tr('machines.emulatedLower')}` : m.arch} />
      <Row label={tr('machines.cfg.created')} value={(m.created || '').replace('T', ' ').replace('Z', ' UTC')} />
      {m.imported ? <Row label={tr('machines.cfg.importedFrom')} note={tr('machines.cfg.importedNote')}>
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{meta.importedFrom || '—'}</Text>
      </Row> : null}
      <Row label={tr('machines.cfg.folder')} last>
        <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{dir}</Text>
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
    const cpus = host?.cpus || m.vcpus, memMax = host?.memMaxMb || m.memoryMb;
    pane = <>
      <Group>
        <Row label={tr('machines.cfg.cores')} value={tr('machines.cfg.ofN', { a: cur.vcpus, b: cpus })}>
          <Bar frac={cur.vcpus / cpus} color={p.accent} />
          <Off off={running}><Stepper value={cur.vcpus} min={1} max={cpus} onChange={(x) => set({ vcpus: x })} /></Off>
        </Row>
        <Row label={tr('machines.cfg.memory')} value={tr('machines.cfg.ofN', { a: gb(cur.memoryMb), b: gb(host?.ramMb || memMax) })} last>
          <Bar frac={cur.memoryMb / (host?.ramMb || memMax)} color={p.accent} />
          <Off off={running}><Stepper value={cur.memoryMb} min={256} max={memMax} step={256} width={80} format={(x) => `${x} MB`} onChange={(x) => set({ memoryMb: x })} /></Off>
        </Row>
      </Group>
      <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
        {tr('machines.cfg.cpuNote', { limit: gb(memLimit({ ...m, ...cur })), max: gb(memMax) })}{offNote ? ` ${offNote}` : ''}</Text>
      {cur.vcpus > (host?.vcpusRecommended || 99) ? <Text style={{ fontSize: 12, color: '#ff9f0a', textAlign: 'start' }}>{tr('machines.new.warn.vcpus', { n: host.vcpusRecommended })}</Text> : null}
    </>;
  } else if (sec === 'display') pane = <>
    <Group>
      <Row label={tr('machines.cfg.resolution')} value={tr('machines.cfg.followsWindow')} note={tr('machines.cfg.resolutionNote')} />
      <Row label={tr('machines.cfg.graphics')} value={tr('machines.cfg.graphics2d')} last />
    </Group>
    <Later text={tr('machines.cfg.later3d')} />
  </>;
  else if (sec === 'network') pane = <>
    <Group>
      <Row label={tr('machines.cfg.netMode')} note={tr(`machines.cfg.netNote.${cur.network}`) + (offNote ? ` ${offNote}` : '')}>
        <Off off={running}><Segmented small value={cur.network} onChange={(x) => set({ network: x })} options={[['shared', tr('machines.cfg.net.shared')], ['isolated', tr('machines.cfg.net.isolated')]]} /></Off>
      </Row>
      <Row label={tr('machines.cfg.mac')} value={m.mac || '—'} last />
    </Group>
    <Later text={tr('machines.cfg.laterPorts')} />
  </>;
  else if (sec === 'sound') pane = <>
    <Group>
      <Row label={tr('machines.cfg.keyboard')} value={tr('machines.cfg.keyboardValue')} />
      <Row label={tr('machines.cfg.pointer')} value={tr('machines.cfg.pointerValue')} last note={tr('machines.cfg.pointerNote')} />
    </Group>
    <Later text={tr('machines.cfg.laterSound')} />
  </>;
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
    <Group>
      <Row label={tr('machines.cfg.clipboard')} note={tr('machines.cfg.clipboardNote') + (offNote ? ` ${offNote}` : '')} last>
        <Off off={running}><Segmented small value={cur.clipboard} onChange={(x) => set({ clipboard: x, ...(x !== 'off' ? { agent: true } : {}) })}
          options={['off', 'to-guest', 'from-guest', 'both'].map((k) => [k, tr(`machines.cfg.clip.${k}`)])} /></Off>
      </Row>
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
      <Row label={tr('machines.cfg.agentInstall')} last note={tr('machines.cfg.agentInstallNote')} />
    </Group>
  </>;
  else if (sec === 'boot') pane = <>
    <Group>
      <Row label={tr('machines.cfg.firmware')} value={m.firmware === 'uefi' ? 'UEFI' : 'BIOS'} />
      <Row label={tr('machines.cfg.bootOrder')} value={m.iso ? tr('machines.cfg.bootDiskThenCd') : tr('machines.cfg.bootDisk')} last note={tr('machines.cfg.bootNote')} />
    </Group>
    <Later text={tr('machines.cfg.laterTpm')} />
  </>;
  else if (sec === 'security') pane = <Group>
    {['sandbox', 'files', 'network', 'limits', 'display', 'secrets'].map((k, i, a) => (
      <Row key={k} label={tr(`machines.cfg.sec.${k}`)} last={i === a.length - 1} note={tr(`machines.cfg.secNote.${k}`, { limit: gb(memLimit(m)) })} />
    ))}
  </Group>;

  const savable = ['general', 'cpu', 'network', 'disks', 'sharing', 'agent'].includes(sec);
  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: p.bg }}>
      <View style={{ width: 260, paddingHorizontal: 10, paddingVertical: 14, gap: 1, backgroundColor: p.raised }}>
        <Text style={{ marginHorizontal: 8, marginBottom: 8, fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{m.name}</Text>
        {SECTIONS.map((k) => (
          <View key={k} onClick={() => setSec(k)} style={{ paddingHorizontal: 8, paddingVertical: 6, borderRadius: 8, backgroundColor: sec === k ? p.accent : 'transparent' }}>
            <Text style={{ fontSize: 13, fontWeight: 600, color: sec === k ? '#fff' : p.text, textAlign: 'start' }}>{tr(`machines.cfg.sec.title.${k}`)}</Text>
            <Text style={{ fontSize: 12, color: sec === k ? '#e6f1ff' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{summary[k]}</Text>
          </View>
        ))}
      </View>
      <View style={{ flex: 1, minWidth: 0, padding: 22, gap: 14 }}>
        <Text style={{ fontSize: 18, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.cfg.sec.title.${sec}`)}</Text>
        {running ? <View style={{ padding: 10, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.cfg.runningBanner')}</Text></View> : null}
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 14 }}>{pane}</ScrollView>
        {msg ? <Text style={{ fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
        {savable ? (
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
            <Btn label={tr('machines.cancel')} disabled={!dirty || !!job} onClick={() => { setDraft({}); setMsg(null); }} />
            <Btn primary label={tr('machines.cfg.save')} disabled={!dirty || running || !!job} onClick={() => save()} />
          </View>
        ) : null}
      </View>
      {pickShare ? <FilePicker mode="folder" title={tr('machines.cfg.shareAdd')} onClose={() => setPickShare(false)}
        onPick={(x) => {
          setPickShare(false);
          const path = Array.isArray(x) ? x[0] : x;
          if (!path || cur.shares.some((y) => y.path === path)) return;
          set({ shares: [...cur.shares, { tag: shareTag(path.split('/').pop(), cur.shares.map((y) => y.tag)), path, writable: false }] });
        }} /> : null}
      {picking ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPicking(false)}
        onPick={(x) => { setPicking(false); save({ iso: Array.isArray(x) ? x[0] : x }); }} /> : null}
    </View>
  );
}
