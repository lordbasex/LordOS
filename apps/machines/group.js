// group.js — a machine group in the library (docs/VIRTUALIZATION.md 19.8): one card for the group (the workstation's),
// its menu, Start Fresh's dialog, the group's memory budget and its Configure view's locks. Pure functions over
// lord.vm.list() and lord.vm.groups(), tested by server/tests/vm_group_ui_test.mjs; the start and stop order itself is
// model.js groupPlan.
import { groupMembers, groupState, memLimit, stateView } from './model.js';

// the library's cards: the machines that aren't a group's member, and one entry per group
// → { machines: [...], groups: [{ group, gw, ws, state }] }
export function libraryItems(list, groups) {
  const gs = (groups || []).filter((g) => g && !g.error && Array.isArray(g.members));
  const inGroup = new Set(gs.map((g) => g.dir));
  const machines = (list || []).filter((m) => !(m?.groupDir && inGroup.has(m.groupDir)));
  return { machines, groups: gs.map((g) => { const ms = groupMembers(g, list); return { group: g, gw: ms.gateway || null, ws: ms.workstation || null, state: groupState(g, list) }; }) };
}

// a disposable session's cap (lordvm.c session_cap_mb): as much as the workstation's memory, at least 1 GB
export const SESSION_MIN_MB = 1024;
export const sessionCapMb = (ws) => Math.max(SESSION_MIN_MB, ws?.memoryMb || 0);

// the group's memory budget at its start (19.5, "about 4 GB free"): its members not running yet, each with QEMU's
// overhead, the disposable session's cap counted with the workstation; against what machines may use here
// (host.memMaxMb) minus the machines already running (the group's own included). → { ok, need, free }
export function groupBudget(group, list, host) {
  const ms = groupMembers(group, list);
  const running = (list || []).filter((x) => stateView(x.state).active).reduce((a, x) => a + memLimit(x) + (x.disposable ? sessionCapMb(x) : 0), 0);
  let need = 0;
  for (const m of Object.values(ms)) if (!stateView(m.state).active) need += memLimit(m) + (group?.disposable && m.role === 'workstation' ? sessionCapMb(m) : 0);
  const total = host?.memMaxMb || 0, free = Math.max(0, total - running);
  return { ok: need === 0 || need <= free, need, free };
}

// the card's numbers: both members' vCPUs and memory together
export function groupSpec(gw, ws) {
  return { vcpus: (gw?.vcpus || 0) + (ws?.vcpus || 0), memoryMb: (gw?.memoryMb || 0) + (ws?.memoryMb || 0) };
}

// the card's menu (the approved mock): keys in order, '-' between parts; each { k, disabled, destructive }
// start: the group (gateway first); show*: that member's own window (only while it runs); shutdown: the workstation, then
// the gateway (ACPI); poweroff: both at once; snapshot: both members' disks, the group off; configure; logs: the
// workstation's; fresh: Destroy and start fresh… (the group off); disposable: the switch (applies at the next start)
export function groupMenu(item) {
  const st = item?.state || 'damaged';
  const on = (m) => !!m && stateView(m.state).active;
  const anyOn = on(item?.gw) || on(item?.ws), damaged = st === 'damaged';
  return [
    { k: 'start', disabled: damaged || st === 'running' },
    { k: 'showWorkstation', disabled: !on(item?.ws) },
    { k: 'showGateway', disabled: !on(item?.gw) },
    { k: 'shutdown', disabled: !anyOn },
    { k: 'poweroff', disabled: !anyOn, destructive: true },
    '-',
    { k: 'snapshot', disabled: damaged || anyOn },
    { k: 'configure', disabled: damaged },
    { k: 'disposable', checked: !!item?.group?.disposable, disabled: damaged },
    { k: 'logs', disabled: damaged },
    { k: 'showInFiles' },
    '-',
    { k: 'fresh', disabled: damaged || anyOn, destructive: true },
  ];
}

// the confirm dialog of Destroy and start fresh… (19.7): what goes, for the box "Also reset the Gateway"
// → { members: ['workstation'] or ['workstation', 'gateway'], snapshots: how many go, guards: true when Tor's entry
// guards go too }
export function freshPlan(item, gateway, snapsOf = () => 0) {
  const members = gateway ? ['workstation', 'gateway'] : ['workstation'];
  const snapshots = members.reduce((a, r) => a + (r === 'gateway' ? snapsOf(item?.gw) : snapsOf(item?.ws)), 0);
  return { members, snapshots, guards: !!gateway };
}

// whether a group's card shows with the library's chips and search (model.js filterMachines' rules): all, running (any
// member active), linux (both members are Linux machines), never under the other system's chip
export function groupShows(item, { query, show } = {}) {
  const q = (query || '').trim().toLowerCase();
  if (show === 'windows' || show === 'templates') return false;
  if (show === 'running' && (item?.state === 'off' || item?.state === 'damaged')) return false;
  return !q || String(item?.group?.name || '').toLowerCase().includes(q) || 'whonix tor'.includes(q);
}

// Configure's group view (19.8): the anonymity profile's rows, shown off and locked, each with its short reason key
// (machines.group.lock.<k>, machines.group.why.<k>)
export const LOCKED_ROWS = ['clipboard', 'transfer', 'shares', 'agent', 'graphics3d', 'microphone', 'ports', 'suspend', 'thumbnail', 'identity'];
