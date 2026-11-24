// procgroupmodel.js — Monitor → Processes: the Grouped view and the steady order of the table (docs/MONITOR.md,
// "Grouped view" and "A steady list"). Pure logic, tested by server/tests/monitor_procgroup_test.mjs.
//
// Groups. Each process joins exactly one group, and a group is always one user's:
//   1. the kernel's threads (prot 'kernel'): one group;
//   2. the app scope lord-server read from the process's cgroup (p.app: a Linux app's app-lordos-<id>.scope, an
//      isolated app's lpk-, a Flatpak app's fp-, a web app's lordos-web-<app>): every process of that app, the ones
//      a browser double-forks (its crash reporter) included, because the scope is root-owned and inherited;
//   3. LordOS itself (the session's lord-server, its helpers and services, any lord-* program): one group per user,
//      which is never ended as a whole (its members can still be ended one by one, under the usual rules);
//   4. otherwise the process tree: the process's top ancestor below a boundary (PID 1, the kernel, a service
//      manager, LordOS, another app's scope or another user), and trees whose top process is the same program are
//      one group (several runs of the same program from a terminal, orphans of the same executable).
// A group's memory adds up each process's proportional memory when lord-server read it (memtop.js memOf), so
// shared pages are not counted once per process.
//
// A steady list. While the order is held (the pointer over the list, a menu or sheet open, and HOLD_MS after the
// pointer leaves) holdOrder keeps each row where it was: values change in place, a new process goes at the end of
// its group (a new group at the end), and one that exited stays, greyed ("gone"), until the order is released.
// A process is always known by its PID and start time (procKey), never by its place, so a reused PID is another one.
import { memOf } from './memtop.js';

export const HOLD_MS = 2000;
export const procKey = (p) => (p ? `${p.pid}:${p.st}` : '');
export const groupRowKey = (key) => `g|${key}`;

const WRAPPERS = new Set(['bwrap']);
const MANAGERS = new Set(['systemd', '(sd-pam)', 'init']);
const baseName = (s) => String(s || '').split('/').pop().replace(/ \(deleted\)$/, '');
export const programOf = (p) => baseName(p.exe) || p.name || String(p.pid);
const isLordOS = (p) => p.app === 'lordos' || (!p.app && (/^lord-/.test(p.name || '') || /^lord-/.test(baseName(p.exe))));

// Is the order held right now? hover: the pointer is over the list; overlay: a menu or a sheet is open; leftAt: when
// the pointer left the list (0: never).
export function isPaused({ hover = false, overlay = false, leftAt = 0, now = Date.now(), holdMs = HOLD_MS } = {}) {
  return !!hover || !!overlay || (leftAt > 0 && now - leftAt < holdMs);
}

// The group key of every process of list: Map pid → { key, kind }. kind: 'kernel', 'app', 'lordos' or 'tree'.
export function groupKeys(list) {
  const byPid = new Map();
  for (const p of list || []) byPid.set(p.pid, p);
  const out = new Map();
  const boundary = (q, p) => !q || q.pid <= 1 || q.prot === 'kernel' || !!q.app || isLordOS(q) || q.uid !== p.uid || MANAGERS.has(q.name) || MANAGERS.has(programOf(q));
  for (const p of list || []) {
    let key, kind;
    if (p.prot === 'kernel') { kind = 'kernel'; key = 'kernel'; }
    else if (p.app && p.app !== 'lordos') { kind = 'app'; key = `app:${p.uid}:${p.app}`; }
    else if (isLordOS(p)) { kind = 'lordos'; key = `lordos:${p.uid}`; }
    else {
      let top = p;
      for (let i = 0; i < 64; i++) {
        const up = byPid.get(top.ppid);
        if (boundary(up, p) || up === top) break;
        top = up;
      }
      kind = 'tree';
      key = `tree:${p.uid}:${programOf(top)}`;
      out.set(p.pid, { key, kind, top });
      continue;
    }
    out.set(p.pid, { key, kind, top: null });
  }
  return out;
}

// The groups of list (the whole process list: the tree needs every parent), each:
//   { key, kind, uid, user, procs: [process], item (the one whose name and icon the group shows), lead (its main
//     process: a tree's top, else the oldest that isn't a wrapper), cpu, mem, threads, count, stoppable }
export function buildGroups(list) {
  const keys = groupKeys(list);
  const groups = new Map();
  for (const p of list || []) {
    const k = keys.get(p.pid);
    let g = groups.get(k.key);
    if (!g) groups.set(k.key, (g = { key: k.key, kind: k.kind, uid: p.uid, user: p.user, procs: [], item: null, lead: null, cpu: 0, mem: 0, threads: 0, count: 0, tops: [] }));
    g.procs.push(p);
    g.cpu += p.cpu || 0;
    g.mem += memOf(p);
    g.threads += p.threads || 0;
    g.count++;
    if (k.top === p) g.tops.push(p);
  }
  for (const g of groups.values()) {
    const real = g.procs.filter((p) => !WRAPPERS.has(p.name));
    const pool = real.length ? real : g.procs;
    const oldest = (arr) => arr.reduce((a, b) => ((b.start ?? b.st) < (a.start ?? a.st) || ((b.start ?? b.st) === (a.start ?? a.st) && b.pid < a.pid) ? b : a));
    g.lead = g.tops.length ? oldest(g.tops) : oldest(pool);
    // the name and icon: the app's (any member that carries the app's name), else the lead's
    g.item = (g.kind === 'app' && pool.find((p) => p.appName)) || g.lead;
    g.stoppable = g.kind === 'app' || g.kind === 'tree';
    delete g.tops;
  }
  return [...groups.values()];
}

// The text a search matches: a process's names, command and PID.
export const procText = (p) => `${p.name} ${p.cmd || ''} ${p.appName || ''} ${p.pid}`.toLowerCase();

// The groups to show: one user's (uid) or everyone's (uid null), those whose name or any process matches needle
// (lower case); match: 'group' (its name matched: all its processes), 'procs' (only some of its processes did: those
// in shown) or 'all' (no search). label(group) is the group's shown name.
export function filterGroups(groups, { uid = null, needle = '', label = (g) => g.key } = {}) {
  const out = [];
  for (const g of groups) {
    if (uid != null && g.uid !== uid) continue;
    if (!needle) { out.push({ ...g, shown: g.procs, match: 'all' }); continue; }
    if (String(label(g)).toLowerCase().includes(needle)) { out.push({ ...g, shown: g.procs, match: 'group' }); continue; }
    const hit = g.procs.filter((p) => procText(p).includes(needle));
    if (hit.length) out.push({ ...g, shown: hit, match: 'procs' });
  }
  return out;
}

// Sorting: by key ('name', 'cpu', 'rss', 'threads', 'pid', 'user') and dir (1 or -1). For a group, 'rss' is its
// memory and 'pid' its lead's PID; ties go by key, then PID, so the order is always the same for the same values.
const cmp = (va, vb, dir, tie) => (va < vb ? -1 : va > vb ? 1 : tie) * dir;
export function sortProcs(list, { key, dir }, label) {
  return [...list].sort((a, b) => {
    const va = key === 'name' ? String(label(a)).toLowerCase() : a[key], vb = key === 'name' ? String(label(b)).toLowerCase() : b[key];
    return cmp(va ?? 0, vb ?? 0, dir, a.pid - b.pid);
  });
}
export function sortGroups(list, { key, dir }, label) {
  const val = (g) => (key === 'name' ? String(label(g)).toLowerCase() : key === 'rss' ? g.mem : key === 'pid' ? g.lead?.pid ?? 0 : key === 'user' ? g.user || '' : g[key] ?? 0);
  return [...list].sort((a, b) => cmp(val(a), val(b), dir, a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
}

// The rows in a held order. prev: the rows shown last time (gone ones included); fresh: the new rows, sorted; keyOf:
// a row's identity. Released (paused false): fresh as it is. Held: prev's order with each row's new values, the ones
// no longer in fresh kept with gone: true (and their last values), and the new ones at the end in fresh's order.
export function holdOrder(prev, fresh, keyOf, paused) {
  if (!paused || !prev || !prev.length) return fresh;
  const now = new Map(fresh.map((r) => [keyOf(r), r]));
  const out = [];
  const seen = new Set();
  for (const r of prev) {
    const k = keyOf(r);
    if (seen.has(k)) continue;
    seen.add(k);
    const n = now.get(k);
    out.push(n || (r.gone ? r : { ...r, gone: true, cpu: 0 }));
  }
  for (const r of fresh) if (!seen.has(keyOf(r))) out.push(r);
  return out;
}

// The grouped view's groups in a held order, each with its processes (rows) in a held order too. prev: the last
// result (or null); fresh: filtered and sorted groups whose `shown` processes are sorted.
export function holdGroups(prev, fresh, paused) {
  if (!paused || !prev) return fresh.map((g) => ({ ...g, rows: g.shown }));
  const before = new Map(prev.map((g) => [g.key, g]));
  const groups = holdOrder(prev, fresh, (g) => g.key, true);
  return groups.map((g) => {
    const old = before.get(g.key);
    if (g.gone) return { ...g, cpu: 0, rows: (old?.rows || []).map((p) => (p.gone ? p : { ...p, gone: true, cpu: 0 })) };
    return { ...g, rows: holdOrder(old?.rows || [], g.shown, procKey, true) };
  });
}

// What the tooltip of a group row says, taken when it opens: the number of processes, the one using the most CPU and
// the one using the most memory (resident). Only live processes count.
export function groupSummary(g) {
  const live = (g?.rows || g?.procs || []).filter((p) => !p.gone);
  if (!live.length) return { count: 0, cpu: null, mem: null };
  const top = (f) => live.reduce((a, b) => (f(b) > f(a) || (f(b) === f(a) && b.pid < a.pid) ? b : a));
  const c = top((p) => p.cpu || 0), m = top((p) => p.rss || 0);
  return { count: live.length, cpu: { name: c.name, pid: c.pid, cpu: c.cpu || 0 }, mem: { name: m.name, pid: m.pid, rss: m.rss || 0 } };
}

// The processes a group's Quit All acts on, taken when it is chosen: { pid, st, name, uid, user, prot }.
export const snapshotOf = (procs) => (procs || []).filter((p) => !p.gone).map((p) => ({ pid: p.pid, st: p.st, name: p.name, uid: p.uid, user: p.user, prot: p.prot || null }));

// What to do with a snapshot now, against the live list: send (still the same process: PID and start time match),
// reused (the PID is now another process: skipped), gone (already exited) and prot (protected: never signalled).
export function planStop(snapshot, live) {
  const byPid = new Map((live || []).map((p) => [p.pid, p]));
  const out = { send: [], reused: [], gone: [], prot: [] };
  for (const s of snapshot || []) {
    if (s.prot) { out.prot.push(s); continue; }
    const p = byPid.get(s.pid);
    if (!p) out.gone.push(s);
    else if (p.st !== s.st) out.reused.push(s);
    else out.send.push(s);
  }
  return out;
}

// The snapshot's processes still running (same PID and start time).
export function stillRunning(snapshot, live) {
  const ids = new Set((live || []).map(procKey));
  return (snapshot || []).filter((s) => ids.has(procKey(s)));
}

// The table's rows: a group row ({ group }) then, when the group is open (or the search matched only some of its
// processes), its processes ({ proc, parent: group key }).
export function flattenRows(groups, open) {
  const rows = [];
  for (const g of groups) {
    rows.push({ group: g });
    if (open.has(g.key) || g.match === 'procs') for (const p of g.rows) rows.push({ proc: p, parent: g.key });
  }
  return rows;
}
export const rowKeyOf = (r) => (r.group ? groupRowKey(r.group.key) : procKey(r.proc));

// ---- who started it: the chain of ancestors ----
// A process's chain, from the first ancestor below the session's root to the process itself: walking up the parents
// (ppid) of the same process list, it stops at PID 1, a kernel thread, a service manager or lord-sessiond (left out)
// and at the session's lord-server (kept: the root, which the view names after the component the chain runs in, the
// Terminal for one). Each link: { pid, st, name, app, self }. At most CHAIN_MAX links (the nearest).
export const CHAIN_MAX = 16;
const ROOTS = new Set(['systemd', '(sd-pam)', 'init', 'lord-sessiond']);
export function chainOf(p, byPid) {
  const out = [];
  const seen = new Set();
  for (let cur = p; cur && out.length < CHAIN_MAX && !seen.has(cur.pid); cur = byPid.get(cur.ppid)) {
    if (cur !== p && (cur.pid <= 1 || cur.prot === 'kernel' || ROOTS.has(cur.name))) break;
    seen.add(cur.pid);
    out.push({ pid: cur.pid, st: cur.st, name: cur.name, app: cur.app || null, self: !!cur.self });
    if (cur.self) break;
  }
  return out.reverse();
}

// The chains of a session's processes, kept by identity (PID and start time) while every process of the chain is
// still the same one (a parent that exited, or a PID now used by another process, makes it again).
export function chainCache() {
  const cache = new Map();
  return (p, byPid) => {
    const key = procKey(p);
    const hit = cache.get(key);
    if (hit && hit.every((l) => byPid.get(l.pid)?.st === l.st)) return hit;
    const c = chainOf(p, byPid);
    if (cache.size > 4000) cache.clear();
    cache.set(key, c);
    return c;
  };
}

// ---- where a process comes from: the origin badges ----
// Each badge stands on a fact LordOS can check, never on a name:
//   'system'     the executable belongs to the lordos package (or one of LordOS's own lordos-* packages);
//   'lordos'     a LordOS app: an isolated .lpk app (its scope app-lordos-lpk-<id>; facts.signer: who signed it);
//   'flatpak'    a Flatpak app (its scope app-lordos-fp-<id>);
//   'webapp'     a web app (lord-web and WebKit in app-lordos-lordos-web-<app>; facts.webapp(app): its site);
//   'machine'    a machine's QEMU and passt (app-lordos-lordos-vm-<id>; facts.machine(id): its name);
//   'deb'        another installed package owns the executable (facts.origin(pkg): the repository's name);
//   'unpackaged' no package owns it (a program in the home folder, an unpackaged /opt, an AppImage).
// facts.owner(exe): the package that owns exe, '' for none, undefined while unknown (exeown.c answers). With the
// executable unreadable (another user's process) or the answer not in yet there is no badge. updated: the file was
// replaced while it runs (an update): it needs a restart.
export const ORIGINS = ['system', 'lordos', 'flatpak', 'deb', 'webapp', 'machine', 'unpackaged'];
const DELETED = ' (deleted)';
export const exePath = (exe) => (String(exe || '').endsWith(DELETED) ? String(exe).slice(0, -DELETED.length) : String(exe || ''));
export const isLordOSPackage = (pkg) => pkg === 'lordos' || /^lordos-[a-z0-9.+-]+$/.test(pkg || '');
export function originOf(p, facts = {}) {
  if (!p || p.prot === 'kernel') return null;
  const updated = String(p.exe || '').endsWith(DELETED);
  const app = p.app || '';
  let m;
  if ((m = /^lpk:(.+)$/.exec(app))) return { kind: 'lordos', id: m[1], signer: facts.signer?.(m[1]) || null, updated };
  if ((m = /^flatpak:(.+)$/.exec(app))) return { kind: 'flatpak', id: m[1], updated };
  if ((m = /^desktop:lordos-vm-(?:tor-)?(.+)$/.exec(app))) return { kind: 'machine', id: m[1], name: facts.machine?.(m[1]) || null, updated };
  if ((m = /^desktop:lordos-web-(.+)$/.exec(app))) return { kind: 'webapp', id: m[1], site: facts.webapp?.(m[1]) || null, updated };
  const exe = exePath(p.exe);
  if (!exe) return null;
  const pkg = facts.owner?.(exe);
  if (pkg === undefined || pkg === null) return null;
  if (isLordOSPackage(pkg)) return { kind: 'system', pkg, exe, updated };
  if (pkg) return { kind: 'deb', pkg, exe, origin: facts.origin?.(pkg) || null, updated };
  return { kind: 'unpackaged', exe, updated };
}
// A group's badge: its main process's (an app's scope gives every process of it the same one). One of LordOS's
// components (the Terminal's programs: lordos:terminal) gets none: its processes are whatever was run in it.
export const groupOriginOf = (g, facts) => (g.kind === 'kernel' || /^lordos:/.test(g.item?.app || '') ? null : originOf(g.lead, facts));

// ---- filters and the program picker ----
// origins: the badges to show ([]: all); program: a group key (the picker's choice, null: all). A process passes when
// its group is the chosen one and its badge (or, grouped, its group's) is one of origins.
export function passesFilter(o, groupKey, { origins = [], program = null } = {}) {
  if (program && groupKey !== program) return false;
  return !origins.length || (!!o && origins.includes(o.kind));
}
// The picker's programs: [key, label] of every group, by name.
export function programChoices(groups, label) {
  return groups.map((g) => [g.key, String(label(g))]).sort((a, b) => a[1].localeCompare(b[1]) || (a[0] < b[0] ? -1 : 1));
}
