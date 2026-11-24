// groupui.jsx — a machine group in the library (docs/VIRTUALIZATION.md 19.8; the approved mockups, 2026-11-23): ONE card
// for the group (the workstation's live picture while its window shows it, else the group's mark; a "Tor" chip and
// "Group of 2 machines"), its menu (group.js groupMenu), the group's start and stop in order (model.js groupPlan),
// "Destroy and start fresh…" (the confirm dialog: the workstation only, "Also reset the Gateway" with the warning about
// Tor's entry guards) and "Group snapshot…".
import { View, Text, TextInput, ConfirmDialog, Checkbox, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { groupPlan, groupStartFailed, stateView } from './model.js';
import { groupBudget, groupSpec, freshPlan, groupDamage, renameProblemKey } from './group.js';
import { fetchGroupArchive } from './whonix.jsx';
import CATALOG from '../../assets/machines/systems.json';
import { usePal, gb, gib, startPrep } from './ui.jsx';
import { Sym } from './kit.jsx';

// the group's mark: two machines and a lock, on Tor's purple (no maker's logo)
export function GroupMark({ size = 56 }) {
  const s = size;
  return (
    <View style={{ width: s * 1.4, height: s, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ position: 'absolute', left: 0, top: s * 0.12, width: s * 0.78, height: s * 0.56, borderRadius: s * 0.1, backgroundColor: 'rgba(255,255,255,0.35)' }} />
      <View style={{ position: 'absolute', left: s * 0.5, top: s * 0.3, width: s * 0.86, height: s * 0.6, borderRadius: s * 0.1, backgroundColor: '#ffffff' }} />
      <View style={{ position: 'absolute', left: s * 0.5 + s * 0.3, top: s * 0.42, width: s * 0.26, height: s * 0.36, alignItems: 'center' }}>
        <View style={{ width: s * 0.18, height: s * 0.16, borderTopLeftRadius: s * 0.09, borderTopRightRadius: s * 0.09, border: `${Math.max(2, s * 0.04)}px solid #59316b`, borderBottomWidth: 0 }} />
        <View style={{ width: s * 0.26, height: s * 0.18, borderRadius: s * 0.04, backgroundColor: '#59316b' }} />
      </View>
    </View>
  );
}

export const groupColors = ['#7d4698', '#3b1f4a'];

// the "Tor" chip and the second status dot (the gateway's)
export function TorChip({ on }) {
  return (
    <View style={{ height: 22, paddingHorizontal: 9, borderRadius: 11, flexDirection: 'row', alignItems: 'center', gap: 5, backgroundColor: 'rgba(125,70,152,0.92)' }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: on ? '#30d158' : 'rgba(255,255,255,0.45)' }} />
      <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff', whiteSpace: 'nowrap' }}>{tr('machines.group.tor')}</Text>
    </View>
  );
}

// "Group of 2 machines · 2 CPU · 4 GB"
export function groupLine(item) {
  if (item.unreadable) return tr('machines.group.unreadableLine');
  const s = groupSpec(item.gw, item.ws);
  return [tr('machines.group.ofN', { n: item.group.members.length }), tr('machines.lib.spec.cpu', { n: s.vcpus }), gb(s.memoryMb)].join(' · ');
}

// the group's state in words and color (the card's pill): running, partial (the gateway alone), starting, off, damaged
export function groupStateOf(item) {
  const ws = item.ws, gw = item.gw;
  if (item.state === 'damaged') {
    const d = groupDamage(item);
    return { text: tr(d.unreadable ? 'machines.group.damagedFile' : !d.members.length && d.bases.length ? 'machines.group.damagedBase' : 'machines.group.damaged'), color: '#ff9a9a', on: false };
  }
  const wv = ws ? stateView(ws.state, ws.reason) : null, gv = gw ? stateView(gw.state, gw.reason) : null;
  if (item.state === 'running') return { text: tr('machines.lib.state.running'), color: '#30d158', on: true };
  if (ws?.state === 'starting' || gw?.state === 'starting') return { text: tr('machines.group.starting'), color: '#30d158', on: true };
  if (ws?.state === 'stopping' || gw?.state === 'stopping') return { text: tr('machines.group.stopping'), color: '#ffd60a', on: true };
  if (gv?.active && !wv?.active) return { text: tr('machines.group.gatewayOnly'), color: '#ffd60a', on: true };
  if (wv?.active && !gv?.active) return { text: tr('machines.group.noGateway'), color: '#ff9a9a', on: true };
  return { text: tr('machines.group.off'), color: null, on: false };
}

// ---- starting and stopping a group (groupPlan step by step, every 700 ms) ----
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// start: the budget first (the whole group), the gateway with no window, then the workstation in its window (open(ws));
// the link is made by cli.js's watch once both run. Resolves '' or the reason (English for the log, or a key's text)
export async function startGroup(item, open, host) {
  const b = groupBudget(item.group, lord.vm.list(), host || lord.vm.host());
  if (!b.ok) return tr('machines.group.noMemory', { need: b.need, free: b.free });
  const until = Date.now() + 10 * 60000;
  let asked = '';
  while (Date.now() < until) {
    const list = lord.vm.list();
    const step = groupPlan(item.group, list, 'start');
    if (step.fail) return tr('machines.group.damaged');
    if (step.done) return '';
    if (step.wait) { await sleep(700); continue; }
    if (asked === step.role) {
      // asked before and it's back to off: that member failed (19.9)
      const m = step.member;
      return tr(`machines.group.${groupStartFailed(step.role)}`, { name: item.group.name, why: m.reason || '' });
    }
    asked = step.role;
    if (step.role === 'workstation') open(step.member);
    else { await startPrep(step.member); lord.vm.startDetached(step.member.dir); }
    await sleep(1500);
  }
  return tr('machines.group.timeout');
}
// stop: 'shutdown' (the guests', in stop order: the workstation, then the gateway) or 'poweroff' (both, at once)
export async function stopGroup(item, how) {
  if (how === 'poweroff') { for (const m of [item.ws, item.gw]) if (m && stateView(m.state).active) lord.vm.stop(m.dir, 'poweroff'); return ''; }
  const until = Date.now() + 6 * 60000;
  const asked = new Set();
  while (Date.now() < until) {
    const step = groupPlan(item.group, lord.vm.list(), 'stop');
    if (step.done || step.fail) return '';
    if (step.member && !asked.has(step.role)) { asked.add(step.role); lord.vm.stop(step.member.dir, 'shutdown'); }
    await sleep(700);
  }
  return '';
}

// ---- Destroy and start fresh… (19.7) ----
export function FreshDialog({ item, onDone, onCancel }) {
  const p = usePal();
  const [gw, setGw] = useState(!!item.group.freshGateway);
  const snaps = (m) => { try { return m ? (lord.vm.snaps(m.dir) || []).length : 0; } catch (e) { return 0; } };
  const plan = freshPlan(item, gw, snaps);
  const go = () => {
    // the box is remembered (group.json fresh_gateway), then lord-vm --group-fresh
    if (!!item.group.freshGateway !== gw) lord.vm.groupSet(item.group.dir, { freshGateway: gw });
    // the job is started by the library (groupJob: a base missing or changed fetches the verified download first)
    onDone(() => lord.vm.groupFresh(item.group.dir, gw));
  };
  return (
    <ConfirmDialog destructive title={tr('machines.group.fresh.title', { name: item.group.name })}
      detail={tr(gw ? 'machines.group.fresh.detailBoth' : 'machines.group.fresh.detail')}
      okLabel={tr('machines.group.fresh.ok')} cancelLabel={tr('machines.cancel')} onOk={go} onCancel={onCancel}>
      {plan.snapshots ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.group.fresh.snaps', { n: plan.snapshots })}</Text> : null}
      <View style={{ marginTop: 4 }}><Checkbox checked={gw} onChange={setGw} label={tr('machines.group.fresh.gateway')} /></View>
      {gw ? (
        <View style={{ flexDirection: 'row', gap: 8, padding: 10, borderRadius: 8, backgroundColor: p.warnBg }}>
          <Sym name="security" size={16} color={p.text} />
          <Text style={{ flex: 1, fontSize: 12, lineHeight: 1.45, color: p.text, textAlign: 'start' }}>{tr('machines.group.fresh.guards')}</Text>
        </View>
      ) : null}
    </ConfirmDialog>
  );
}

// ---- Group snapshot… (both members' disks, the group off) ----
export function GroupSnapDialog({ item, onDone, onCancel }) {
  const p = usePal();
  const [label, setLabel] = useState('');
  const take = () => {
    const name = label.trim() || tr('machines.group.snap.default');
    const jobs = [item.gw, item.ws].filter(Boolean).map((m) => lord.vm.snapTake(m.dir, name)).filter(Boolean).map((r) => r.job);
    onDone(jobs);
  };
  return (
    <ConfirmDialog title={tr('machines.group.snap.title', { name: item.group.name })} detail={tr('machines.group.snap.detail')}
      okLabel={tr('machines.group.snap.ok')} cancelLabel={tr('machines.cancel')} onOk={take} onCancel={onCancel}>
      <TextInput value={label} onChange={setLabel} placeholder={tr('machines.group.snap.default')} />
      <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.group.snap.gatewayNote')}</Text>
    </ConfirmDialog>
  );
}

// ---- Delete, Rename and Repair (19.8, 19.9) ----
// a lord-vm job of the group, followed to its end; one that needs the verified download (exit 4: a base missing or
// changed, the archive not in the image store) fetches it again (fetchGroupArchive: the group's own release, the same
// SHA-512) and runs once more. onState({ phase: 'work' | 'download', done, total }). Resolves '' or why it failed
const lordVmWhy = (out) => String(out || '').trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || String(out || '').trim();
export async function groupJob(item, start, onState) {
  for (let tries = 0; tries < 2; tries++) {
    const id = start();
    if (!id) return tr('machines.cfg.refused');
    onState?.({ phase: 'work' });
    let r = null;
    for (;;) { r = lord.vm.job(id); if (!r || r.done) break; await sleep(400); }
    if (!r) return tr('machines.as.failed');
    if (r.ok) return '';
    if (r.exit === 4 && tries === 0 && !item.unreadable) {
      onState?.({ phase: 'download', done: 0, total: 0 });
      try { await fetchGroupArchive(CATALOG, item.group, (g) => onState?.({ phase: 'download', ...g })); }
      catch (e) { return e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed'); }
      continue;
    }
    return lordVmWhy(r.out);
  }
  return tr('machines.as.failed');
}

// Delete… : the group and both members for good (no Trash), and the Whonix images when no other group uses them
export function DeleteGroupDialog({ item, onOk, onCancel }) {
  return (
    <ConfirmDialog destructive title={tr('machines.group.delete.title', { name: item.group.name })}
      detail={tr(item.unreadable ? 'machines.group.delete.detailDamaged' : 'machines.group.delete.detail')}
      okLabel={tr('machines.group.delete.ok')} cancelLabel={tr('machines.cancel')} onOk={onOk} onCancel={onCancel} />
  );
}

// Rename… : the group's name, its members' names and its folder (the group off); the field says at once why a name can't
// be used (lord.vm.groupRenamePlan, the same answer lord-vm gives)
export function RenameGroupDialog({ item, onOk, onCancel }) {
  const p = usePal();
  const [name, setName] = useState(item.group.name);
  const n = name.trim();
  const plan = n && n !== item.group.name && typeof lord.vm.groupRenamePlan === 'function' ? lord.vm.groupRenamePlan(item.group.dir, n) : null;
  const bad = renameProblemKey(plan, n, item.group.name);
  const go = () => { if (n && !bad && n !== item.group.name) onOk(n); else if (n === item.group.name) onCancel(); };
  return (
    <ConfirmDialog title={tr('machines.group.rename.title', { name: item.group.name })} detail={tr('machines.group.rename.detail')}
      okLabel={tr('machines.group.rename.ok')} cancelLabel={tr('machines.cancel')} onOk={go} onCancel={onCancel}>
      <TextInput value={name} onChange={setName} onSubmit={go} autoFocus />
      {bad ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(bad, { name: plan?.other || n, folder: plan?.folder || '' })}</Text>
        : plan?.moves ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.group.rename.folder', { folder: plan.folder })}</Text> : null}
    </ConfirmDialog>
  );
}

// Repair… : what is damaged and what Repair does about it (each member made again from its base, each base made again
// from the verified download, fetched first when it isn't here), after a confirmation
export function RepairGroupDialog({ item, onOk, onCancel }) {
  const p = usePal();
  const d = groupDamage(item);
  const role = (r) => tr(`machines.group.role.${r}`);
  return (
    <ConfirmDialog destructive title={tr('machines.group.repair.title', { name: item.group.name })} detail={tr('machines.group.repair.detail')}
      okLabel={tr('machines.group.repair.ok')} cancelLabel={tr('machines.cancel')} onOk={onOk} onCancel={onCancel}>
      {d.members.map((r) => <Text key={`m${r}`} style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{`• ${tr('machines.group.repair.member', { role: role(r) })}`}</Text>)}
      {d.bases.map((r) => <Text key={`b${r}`} style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{`• ${tr('machines.group.repair.base', { role: role(r) })}`}</Text>)}
      {d.fetch ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.group.repair.fetch')}</Text> : null}
    </ConfirmDialog>
  );
}

// the card's line while one of these jobs runs: { phase, done, total } → its text
export function groupWorkText(w) {
  if (!w) return '';
  if (w.phase === 'download') return w.total > 1 ? tr('machines.group.work.downloadPct', { pct: Math.floor((w.done / w.total) * 100) }) : tr('machines.group.work.download');
  return tr(`machines.group.work.${w.kind || 'repair'}`);
}

// the memory the group asks for, for the card's line while it's off ("4.4 GB to run")
export const groupNeedText = (item, host) => gib(groupBudget(item.group, [item.gw, item.ws].filter(Boolean).map((m) => ({ ...m, state: 'stopped' })), host).need * 1048576);
