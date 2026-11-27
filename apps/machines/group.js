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
  // a group whose group.json can't be read (19.9) is a damaged card too, named by its folder: it can only be deleted
  const unreadable = (groups || []).filter((g) => g && g.error === 'invalid' && typeof g.dir === 'string').map((g) => ({
    group: { dir: g.dir, name: String(g.name || g.dir.split('/').pop()).replace(/\.lordgroup$/, ''), members: [], error: 'invalid' },
    gw: null, ws: null, state: 'damaged', unreadable: true,
  }));
  return { machines, groups: [...gs.map((g) => { const ms = groupMembers(g, list); return { group: g, gw: ms.gateway || null, ws: ms.workstation || null, state: groupState(g, list) }; }), ...unreadable] };
}

// what is damaged in a group (19.9), for the card's Repair dialog: the members missing or not the group's own, the bases
// missing, whether group.json itself can't be read (then only Delete), and whether the verified download must be fetched
// again first (a base is missing and the archive isn't in the image store). → { members, bases, unreadable, fetch }
export function groupDamage(item) {
  if (item?.unreadable) return { members: [], bases: [], unreadable: true, fetch: false };
  const roles = (item?.group?.members || []).map((x) => x.role);
  const members = roles.filter((r) => !(r === 'gateway' ? item?.gw : item?.ws));
  const bases = (item?.group?.basesMissing || []).filter((r) => roles.includes(r));
  return { members, bases, unreadable: false, fetch: bases.length > 0 && !item?.group?.archiveThere };
}

// the archive a group's release lists now is the one group.json recorded (never another image under the group's name)
export const archiveMatches = (img, group) => !!img?.sha && !!group?.archiveSha512 && String(img.sha).toLowerCase() === String(group.archiveSha512).toLowerCase();

// the Workstation window's bar (19.8, 19.9): while the Gateway is down the Workstation has no network (the link is a
// pulled cable), and the bar says so with Start Gateway. ws: the workstation as lord.vm.list() gives it (its window's
// machine), gw: its gateway (or null). → null (nothing to say), 'off' (Start Gateway), 'starting' (the gateway on its way)
export function gatewayBar(ws, gw) {
  if (!ws || ws.role !== 'workstation' || !ws.groupDir || !stateView(ws.state).active || ws.state === 'stopping') return null;
  if (!gw) return null;
  if (gw.state === 'starting') return 'starting';
  return stateView(gw.state).active && gw.state !== 'stopping' ? null : 'off';
}
// ▶ in a Workstation's own window (QA 0.8.9.6.2, BUG-N): its Gateway first, as the library and the CLI start a group
// (groupPlan), and the Workstation once the Gateway runs. The Gateway to start first, or null (not a group's
// Workstation, no Gateway, or it already runs or is starting).
export function gatewayFirst(ws, list) {
  if (!ws || ws.role !== 'workstation' || !ws.groupDir) return null;
  const gw = gatewayOf(ws, list);
  if (!gw || gw.state === 'starting' || (stateView(gw.state).active && gw.state !== 'stopping')) return null;
  return gw;
}
// whether the Workstation may start now that its Gateway was asked to: it runs; 'failed' when it stopped again
export function gatewayReady(gw) {
  if (!gw) return 'failed';
  if (gw.state === 'running') return true;
  return gw.state === 'starting' || gw.state === 'stopped' || stateView(gw.state).active ? false : 'failed';
}

// the workstation's gateway in lord.vm.list(): the member of the same group with the gateway's role
export const gatewayOf = (ws, list) => (list || []).find((m) => m && ws && m.groupDir === ws.groupDir && m.role === 'gateway' && m.groupOk && !m.error) || null;

// the rename dialog's answer to lord.vm.groupRenamePlan (problem: '' | 'bad-name' | 'name-taken' | 'folder-taken' |
// 'not-a-group'): the key of what to say under the field, '' when the name can be used
export function renameProblemKey(plan, name, current) {
  const n = String(name || '').trim();
  if (!n || n === current) return '';
  const pr = plan?.problem || '';
  return pr === 'name-taken' ? 'machines.group.rename.taken' : pr === 'folder-taken' ? 'machines.group.rename.folderTaken' : pr ? 'machines.new.err.nameBad' : '';
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
// stopGateway: the gateway alone, offered while it runs without its workstation (the workstation failed to start, 19.9);
// rename: the group's name and folder (off); repair: only on a damaged card; delete: the group and both members (off)
export function groupMenu(item) {
  const st = item?.state || 'damaged';
  const on = (m) => !!m && stateView(m.state).active;
  const anyOn = on(item?.gw) || on(item?.ws), damaged = st === 'damaged';
  return [
    { k: 'start', disabled: damaged || st === 'running' },
    { k: 'showWorkstation', disabled: !on(item?.ws) },
    { k: 'showGateway', disabled: !on(item?.gw) },
    ...(on(item?.gw) && !on(item?.ws) ? [{ k: 'stopGateway' }] : []),
    { k: 'shutdown', disabled: !anyOn },
    { k: 'poweroff', disabled: !anyOn, destructive: true },
    '-',
    { k: 'snapshot', disabled: damaged || anyOn },
    { k: 'configure', disabled: damaged },
    { k: 'disposable', checked: !!item?.group?.disposable, disabled: damaged },
    { k: 'logs', disabled: damaged },
    { k: 'rename', disabled: damaged || anyOn },
    { k: 'showInFiles' },
    '-',
    ...(damaged ? [{ k: 'repair', disabled: anyOn || !!item?.unreadable }] : []),
    { k: 'fresh', disabled: damaged || anyOn, destructive: true },
    { k: 'delete', disabled: anyOn, destructive: true },
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
