// monitor_procgroup_test.mjs — Monitor → Processes, the Grouped view and the steady list (apps/monitor/procgroupmodel.js):
// how processes are grouped (a browser's zygotes, renderers and crash reporter; a Flatpak app's scope; a web app; LordOS;
// the kernel; a terminal's trees; orphans of the same program; another user), the search, the sort, the held order
// (no reordering while held, new ones at the end, exited ones greyed in place), the tooltip's summary, and Quit All's
// snapshot (a reused PID is skipped). Run by tests/run_suites.sh (needs node).
import {
  groupKeys, buildGroups, filterGroups, sortGroups, sortProcs, holdOrder, holdGroups, isPaused, groupSummary, snapshotOf, planStop,
  stillRunning, flattenRows, procKey, rowKeyOf, HOLD_MS, chainOf, chainCache, originOf, groupOriginOf, passesFilter, programChoices, exePath,
  isLordOSPackage,
} from '../../apps/monitor/procgroupmodel.js';
import { sealAt, sealPixels } from '../../apps/monitor/seal.js';
import { statsStore, SECS, MINS } from '../../apps/monitor/procstatsmodel.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

const MB = 1024 * 1024;
const ME = 1000, OTHER = 1001;
let st = 5000;
const P = (pid, ppid, name, o = {}) => ({ pid, ppid, name, uid: ME, user: 'alex', cmd: name, exe: `/usr/bin/${name}`, cpu: 0, rss: 10 * MB, threads: 1, st: st += 10, start: st / 100, prot: null, ...o });
const CHROME = { app: 'desktop:google-chrome', appName: 'Google Chrome', exe: '/opt/google/chrome/chrome' };
const list = [
  P(1, 0, 'systemd', { uid: 0, user: 'root', prot: 'init' }),
  P(2, 0, 'kthreadd', { uid: 0, user: 'root', prot: 'kernel', exe: '' }),
  P(3, 2, 'kworker/0:1', { uid: 0, user: 'root', prot: 'kernel', exe: '' }),
  P(400, 1, 'lord-sessiond', { uid: 0, user: 'root', prot: 'system' }),
  P(500, 400, 'lord-server', { app: 'lordos', self: true, prot: 'session', exe: '/usr/lib/lordos/lord-server', rss: 300 * MB, cpu: 0.05 }),
  P(510, 500, 'lord-media', { exe: '/usr/lib/lordos/lord-media', prot: 'system' }),
  P(520, 500, 'lord-fetch', { exe: '/usr/lib/lordos/lord-fetch' }),
  // Chrome from the launcher: its scope; the main process, zygotes, renderers, the GPU process, the crash reporter (double-forked to PID 1)
  P(600, 400, 'chrome', { ...CHROME, rss: 400 * MB, pss: 300 * MB, cpu: 0.10, threads: 40 }),
  P(601, 600, 'chrome', { ...CHROME, cmd: 'chrome --type=zygote', rss: 50 * MB, threads: 2 }),
  P(602, 601, 'chrome', { ...CHROME, cmd: 'chrome --type=zygote', rss: 40 * MB, threads: 2 }),
  P(603, 602, 'chrome', { ...CHROME, cmd: 'chrome --type=renderer', rss: 200 * MB, cpu: 0.30, threads: 20 }),
  P(604, 602, 'chrome', { ...CHROME, cmd: 'chrome --type=renderer', rss: 250 * MB, cpu: 0.05, threads: 18 }),
  P(605, 600, 'chrome', { ...CHROME, cmd: 'chrome --type=gpu-process', rss: 120 * MB, cpu: 0.02, threads: 10 }),
  P(606, 1, 'chrome_crashpad', { ...CHROME, exe: '/opt/google/chrome/chrome_crashpad_handler', rss: 5 * MB, threads: 3 }),
  P(607, 600, 'cat', { ...CHROME, exe: '/usr/bin/cat', rss: 1 * MB }),
  // a Chrome app (its own .desktop id and scope): its own group
  P(650, 400, 'chrome', { app: 'desktop:chrome-hnpfjngllnobngcgfapefoaidbinmjnm-Default', appName: 'WhatsApp Web', rss: 180 * MB }),
  // a Flatpak app: bubblewrap and the app
  P(700, 400, 'bwrap', { app: 'flatpak:org.gimp.GIMP', appName: 'GNU Image Manipulation Program', rss: 2 * MB }),
  P(701, 700, 'gimp-2.10', { app: 'flatpak:org.gimp.GIMP', appName: 'GNU Image Manipulation Program', rss: 160 * MB, cpu: 0.01 }),
  // a web app (lord-web and WebKit's processes, in app-lordos-lordos-web-<app>.scope)
  P(800, 500, 'lord-web', { app: 'desktop:lordos-web-calendar', appName: 'Calendar web', rss: 60 * MB }),
  P(801, 800, 'WebKitWebProces', { app: 'desktop:lordos-web-calendar', appName: 'Calendar web', rss: 140 * MB, cpu: 0.04 }),
  // the Terminal: two shells (children of lord-server) with programs below them
  P(900, 500, 'bash', { rss: 4 * MB }),
  P(901, 900, 'make', { rss: 8 * MB, cpu: 0.2 }),
  P(902, 901, 'cc1', { rss: 90 * MB, cpu: 0.9 }),
  P(910, 500, 'bash', { rss: 4 * MB }),
  P(911, 910, 'vim', { rss: 12 * MB }),
  // orphans of the same program, reparented to PID 1
  P(950, 1, 'sleep', { rss: 1 * MB }),
  P(951, 1, 'sleep', { rss: 1 * MB }),
  // the user manager and its services: each service its own tree, not the manager's
  P(980, 1, 'systemd', { exe: '/usr/lib/systemd/systemd', prot: 'system' }),
  P(981, 980, 'pipewire', { prot: 'system' }),
  P(982, 980, 'dbus-daemon'),
  // another user's: sshd (root) → their shell
  P(1100, 1, 'sshd', { uid: 0, user: 'root' }),
  P(1101, 1100, 'bash', { uid: OTHER, user: 'sam' }),
  P(1102, 1101, 'top', { uid: OTHER, user: 'sam' }),
];

// ---- grouping ----
const keys = groupKeys(list);
const keyOf = (pid) => keys.get(pid).key;
eq('Chrome: main, zygotes, renderers, GPU, cat and the double-forked crash reporter are one group',
  [600, 601, 602, 603, 604, 605, 606, 607].map(keyOf), Array(8).fill(`app:${ME}:desktop:google-chrome`));
eq('a Chrome app (its own scope) is its own group', keyOf(650), `app:${ME}:desktop:chrome-hnpfjngllnobngcgfapefoaidbinmjnm-Default`);
eq('a Flatpak app: bubblewrap and the app together', [keyOf(700), keyOf(701)], [`app:${ME}:flatpak:org.gimp.GIMP`, `app:${ME}:flatpak:org.gimp.GIMP`]);
eq('a web app: lord-web and WebKit together, not in LordOS', [keyOf(800), keyOf(801)], [`app:${ME}:desktop:lordos-web-calendar`, `app:${ME}:desktop:lordos-web-calendar`]);
eq('LordOS: the session and its helpers, one per user', [500, 510, 520, 400].map(keyOf), [`lordos:${ME}`, `lordos:${ME}`, `lordos:${ME}`, 'lordos:0']);
eq('the kernel: one group', [2, 3].map(keyOf), ['kernel', 'kernel']);
eq('the Terminal: each shell\'s tree below lord-server, both shells one program', [900, 901, 902, 910, 911].map(keyOf), Array(5).fill(`tree:${ME}:bash`));
eq('orphans of the same program: one group', [keyOf(950), keyOf(951)], [`tree:${ME}:sleep`, `tree:${ME}:sleep`]);
eq('the user manager\'s services are their own trees', [keyOf(980), keyOf(981), keyOf(982)], [`tree:${ME}:systemd`, `tree:${ME}:pipewire`, `tree:${ME}:dbus-daemon`]);
eq('another user under a root daemon: their own group', [keyOf(1100), keyOf(1101), keyOf(1102)], ['tree:0:sshd', `tree:${OTHER}:bash`, `tree:${OTHER}:bash`]);
eq('a cycle in the parents never hangs', groupKeys([P(1, 2, 'a'), P(2, 1, 'b')]).size, 2);

const groups = buildGroups(list);
const G = (key) => groups.find((g) => g.key === key);
const chrome = G(`app:${ME}:desktop:google-chrome`);
eq('Chrome\'s totals: processes, CPU, threads', [chrome.count, Math.round(chrome.cpu * 100), chrome.threads], [8, 47, 96]);
eq('Chrome\'s memory: proportional where read, else resident', chrome.mem, (300 + 50 + 40 + 200 + 250 + 120 + 5 + 1) * MB);
eq('Chrome\'s lead: the oldest process (the browser), with the app\'s name', [chrome.lead.pid, chrome.item.appName], [600, 'Google Chrome']);
eq('a Flatpak group shows the app, never bubblewrap', G(`app:${ME}:flatpak:org.gimp.GIMP`).lead.pid, 701);
eq('a tree\'s lead is its top process', G(`tree:${ME}:bash`).lead.pid, 900);
eq('LordOS and the kernel are never ended as a group; apps and trees are',
  [G(`lordos:${ME}`).stoppable, G('kernel').stoppable, chrome.stoppable, G(`tree:${ME}:bash`).stoppable], [false, false, true, true]);
eq('a group is one user\'s', groups.every((g) => g.procs.every((p) => p.uid === g.uid)), true);
eq('every process is in exactly one group', groups.reduce((s, g) => s + g.count, 0), list.length);

// ---- search and filter ----
const label = (g) => (g.kind === 'kernel' ? 'Kernel' : g.kind === 'lordos' ? 'LordOS' : g.item.appName || g.item.name);
const mine = filterGroups(groups, { uid: ME, label });
eq('Mine: only my groups', mine.every((g) => g.uid === ME), true);
eq('All: everyone\'s', filterGroups(groups, { label }).length, groups.length);
const byName = filterGroups(groups, { uid: ME, needle: 'google', label });
eq('a search for the group\'s name shows it with all its processes', byName.map((g) => [g.key, g.match, g.shown.length]), [[chrome.key, 'group', 8]]);
const byChild = filterGroups(groups, { uid: ME, needle: 'cc1', label });
eq('a search for a child shows its group with the matching process only', byChild.map((g) => [g.key, g.match, g.shown.map((p) => p.pid)]), [[`tree:${ME}:bash`, 'procs', [902]]]);
eq('a search by PID', filterGroups(groups, { uid: ME, needle: '911', label }).map((g) => g.key), [`tree:${ME}:bash`]);

// ---- sort ----
const byCpu = sortGroups(mine, { key: 'cpu', dir: -1 }, label);
eq('groups by CPU, the busiest first', byCpu.slice(0, 2).map((g) => g.key), [`tree:${ME}:bash`, chrome.key]);
eq('groups by name', sortGroups(mine, { key: 'name', dir: 1 }, label).slice(0, 3).map(label), ['bash', 'Calendar web', 'dbus-daemon']);
eq('processes by memory inside a group', sortProcs(chrome.procs, { key: 'rss', dir: -1 }, (p) => p.name).slice(0, 3).map((p) => p.pid), [600, 604, 603]);

// ---- the held order ----
eq('paused while the pointer is over the list, a menu or sheet is open, and HOLD_MS after it leaves',
  [isPaused({ hover: true }), isPaused({ overlay: true }), isPaused({ leftAt: 1000, now: 1000 + HOLD_MS - 1 }), isPaused({ leftAt: 1000, now: 1000 + HOLD_MS }), isPaused({})],
  [true, true, true, false, false]);
const a = { pid: 10, st: 1, cpu: 0.5 }, b = { pid: 11, st: 1, cpu: 0.3 }, c = { pid: 12, st: 1, cpu: 0.1 };
const shown1 = [a, b, c];
const fresh2 = [{ ...c, cpu: 0.9 }, { ...a, cpu: 0.4 }, { pid: 13, st: 1, cpu: 0.6 }];   // c got busy, b exited, 13 is new
const held = holdOrder(shown1, fresh2, procKey, true);
eq('held: no reordering, values in place', held.map((r) => [r.pid, r.cpu]), [[10, 0.4], [11, 0], [12, 0.9], [13, 0.6]]);
eq('held: the exited one stays greyed, the new one goes at the end', held.map((r) => !!r.gone), [false, true, false, false]);
eq('released: the fresh order, without the exited one', holdOrder(held, fresh2, procKey, false).map((r) => r.pid), [12, 10, 13]);
eq('held twice: a gone row stays gone and in place', holdOrder(held, fresh2, procKey, true).map((r) => [r.pid, !!r.gone]), [[10, false], [11, true], [12, false], [13, false]]);
const reused = holdOrder(shown1, [{ pid: 11, st: 99, cpu: 0.2 }, a, c], procKey, true);
eq('a reused PID is another process: the old one greyed, the new one at the end', reused.map((r) => [r.pid, r.st, !!r.gone]), [[10, 1, false], [11, 1, true], [12, 1, false], [11, 99, false]]);

// groups held: the order of the groups and of each group's processes
const g1 = sortGroups(filterGroups(groups, { uid: ME, label }), { key: 'cpu', dir: -1 }, label).map((g) => ({ ...g, shown: sortProcs(g.shown, { key: 'cpu', dir: -1 }, (p) => p.name) }));
const h1 = holdGroups(null, g1, false);
const list2 = list.filter((p) => p.pid !== 604 && p.pid !== 950 && p.pid !== 951).map((p) => (p.pid === 701 ? { ...p, cpu: 3 } : p));
list2.push(P(608, 602, 'chrome', { ...CHROME, cmd: 'chrome --type=renderer', cpu: 0.8 }));
const g2 = sortGroups(filterGroups(buildGroups(list2), { uid: ME, label }), { key: 'cpu', dir: -1 }, label).map((g) => ({ ...g, shown: sortProcs(g.shown, { key: 'cpu', dir: -1 }, (p) => p.name) }));
const h2 = holdGroups(h1, g2, true);
eq('held groups: the same order though GIMP is now the busiest', h2.map((g) => g.key), h1.map((g) => g.key));
eq('held groups: GIMP\'s total updated in place', Math.round(h2.find((g) => g.key === `app:${ME}:flatpak:org.gimp.GIMP`).cpu * 100), 300);
const hc = h2.find((g) => g.key === chrome.key);
eq('held group: the new renderer at the end of Chrome, the exited one greyed in place', [hc.rows[hc.rows.length - 1].pid, hc.rows.find((p) => p.pid === 604).gone], [608, true]);
eq('held: a group whose processes all exited stays, greyed', h2.find((g) => g.key === `tree:${ME}:sleep`)?.gone, true);
eq('released: the busiest group first', holdGroups(h2, g2, false)[0].key, `app:${ME}:flatpak:org.gimp.GIMP`);

// ---- rows, selection by identity ----
const rows = flattenRows(h2, new Set([chrome.key]));
eq('an open group\'s processes follow it, closed ones don\'t', rows.filter((r) => r.parent === chrome.key).length, hc.rows.length);
eq('a search that matched some processes opens their group', flattenRows(byChild.map((g) => ({ ...g, rows: g.shown })), new Set()).map(rowKeyOf), [`g|tree:${ME}:bash`, procKey(list.find((p) => p.pid === 902))]);
const sel = procKey(list.find((p) => p.pid === 603));
const idx1 = flattenRows(h1, new Set([chrome.key])).findIndex((r) => rowKeyOf(r) === sel);
const idx2 = flattenRows(holdGroups(h1, g2, false), new Set([chrome.key])).findIndex((r) => rowKeyOf(r) === sel);
eq('the selection is the process, wherever its row goes', [idx1 >= 0, idx2 >= 0, idx1 !== idx2], [true, true, true]);

// ---- the tooltip ----
eq('the summary: count, busiest and largest child', groupSummary({ procs: chrome.procs }),
  { count: 8, cpu: { name: 'chrome', pid: 603, cpu: 0.3 }, mem: { name: 'chrome', pid: 600, rss: 400 * MB } });
eq('the summary skips exited processes', groupSummary(hc).count, hc.rows.filter((p) => !p.gone).length);

// ---- Quit All: the snapshot ----
const snap = snapshotOf(chrome.procs);
eq('the snapshot: PID, start time, name, user', Object.keys(snap[0]), ['pid', 'st', 'name', 'uid', 'user', 'prot']);
const later = list.filter((p) => p.pid !== 605).map((p) => (p.pid === 603 ? { ...p, st: p.st + 777 } : p));
const plan = planStop(snap, later);
eq('Quit All later: a reused PID is skipped, an exited one too', [plan.send.length, plan.reused.map((s) => s.pid), plan.gone.map((s) => s.pid)], [6, [603], [605]]);
eq('protected processes are never signalled', planStop(snapshotOf(G(`lordos:${ME}`).procs), list).prot.map((s) => s.pid), [500, 510]);
eq('still running after the signal: same PID and start time only', stillRunning(snap, later).map((s) => s.pid), [600, 601, 602, 604, 606, 607]);

// ---- who started it ----
const T = [
  P(1, 0, 'systemd', { uid: 0, prot: 'init' }),
  P(400, 1, 'lord-sessiond', { uid: 0, prot: 'system' }),
  P(500, 400, 'lord-server', { app: 'lordos', self: true, prot: 'session' }),
  P(900, 500, 'bash', { app: 'lordos:terminal' }),
  P(901, 900, 'claude', { app: 'lordos:terminal' }),
  P(902, 901, 'sh', { app: 'lordos:terminal' }),
  P(903, 902, 'sleep', { app: 'lordos:terminal' }),
  P(600, 400, 'chrome', { ...CHROME }),
  P(603, 600, 'chrome', { ...CHROME }),
  P(950, 1, 'sleep'),
];
const tp = new Map(T.map((p) => [p.pid, p]));
eq('the Terminal\'s chain: from the session (named after the Terminal) down to the process', chainOf(tp.get(903), tp).map((l) => [l.name, l.self]),
  [['lord-server', true], ['bash', false], ['claude', false], ['sh', false], ['sleep', false]]);
eq('the chain carries each link\'s identity and app', chainOf(tp.get(903), tp)[1], { pid: 900, st: tp.get(900).st, name: 'bash', app: 'lordos:terminal', self: false });
eq('a launcher app: it stops below lord-sessiond', chainOf(tp.get(603), tp).map((l) => l.pid), [600, 603]);
eq('an orphan: just itself (PID 1 is left out)', chainOf(tp.get(950), tp).map((l) => l.pid), [950]);
eq('the session itself', chainOf(tp.get(500), tp).map((l) => l.pid), [500]);
const loop = new Map([[7, P(7, 8, 'a')], [8, P(8, 7, 'b')]]);
eq('a loop in the parents never hangs', chainOf(loop.get(7), loop).length, 2);
const chainFor = chainCache();
const c1 = chainFor(tp.get(903), tp);
eq('kept by identity while every link is the same process', chainFor(tp.get(903), tp) === c1, true);
const tp2 = new Map(tp);
tp2.set(901, { ...tp.get(901), st: tp.get(901).st + 1 });       // the PID now belongs to another process
eq('a link whose PID was reused: the chain is made again', chainFor(tp.get(903), tp2) === c1, false);

// ---- origin badges ----
const own = { '/usr/lib/lordos/lord-server': 'lordos', '/usr/lib/lordos/lord-web': 'lordos-web', '/opt/google/chrome/chrome': 'google-chrome-stable',
  '/usr/bin/bash': 'bash', '/home/alex/bin/tool': '' };
const facts = {
  owner: (exe) => own[exe], origin: (pkg) => ({ 'google-chrome-stable': 'Google LLC', bash: 'Debian' })[pkg] || null,
  signer: (id) => (id === 'org.example.notes' ? { trust: 'verified', signer: 'Example' } : null), machine: (id) => (id === 'abc' ? 'Ubuntu' : null),
  webapp: (id) => (id === 'calendar' ? 'calendar.example.org' : null),
};
const O = (o) => (o ? [o.kind, o.pkg || o.id || o.exe || null, o.origin || o.site || o.name || o.signer?.signer || null, o.updated] : null);
eq('LordOS itself: its executable belongs to the lordos package', O(originOf(P(1, 0, 'lord-server', { self: true, app: 'lordos', exe: '/usr/lib/lordos/lord-server' }), facts)),
  ['system', 'lordos', null, false]);
eq('one of LordOS\'s own lordos-* packages', O(originOf(P(1, 0, 'lord-web', { exe: '/usr/lib/lordos/lord-web' }), facts)), ['system', 'lordos-web', null, false]);
eq('never by name: a lord-* program no package owns is unpackaged', O(originOf(P(1, 0, 'lord-server', { exe: '/home/alex/bin/tool' }), facts)),
  ['unpackaged', '/home/alex/bin/tool', null, false]);
eq('a LordOS app (.lpk): its scope, with its signer', O(originOf(P(1, 0, 'lord-server', { app: 'lpk:org.example.notes' }), facts)), ['lordos', 'org.example.notes', 'Example', false]);
eq('a Flatpak app: its scope', O(originOf(P(1, 0, 'gimp', { app: 'flatpak:org.gimp.GIMP', exe: '/app/bin/gimp' }), facts)), ['flatpak', 'org.gimp.GIMP', null, false]);
eq('a web app: its scope and site', O(originOf(P(1, 0, 'WebKitWebProces', { app: 'desktop:lordos-web-calendar' }), facts)), ['webapp', 'calendar', 'calendar.example.org', false]);
eq('a machine: its scope and name', O(originOf(P(1, 0, 'qemu-system-x86', { app: 'desktop:lordos-vm-abc' }), facts)), ['machine', 'abc', 'Ubuntu', false]);
eq('a Debian package with its repository', O(originOf(P(1, 0, 'bash'), facts)), ['deb', 'bash', 'Debian', false]);
eq('a third-party package with its repository', O(originOf(P(1, 0, 'chrome', { ...CHROME }), facts)), ['deb', 'google-chrome-stable', 'Google LLC', false]);
eq('updated on disk: the answer for the file itself, and a restart notice', O(originOf(P(1, 0, 'chrome', { exe: '/opt/google/chrome/chrome (deleted)' }), facts)),
  ['deb', 'google-chrome-stable', 'Google LLC', true]);
eq('not known yet, or an executable that can\'t be read: no badge', [originOf(P(1, 0, 'x', { exe: '/usr/bin/unknown' }), facts), originOf(P(1, 0, 'x', { exe: '' }), facts)], [null, null]);
eq('the kernel: no badge', originOf(P(1, 0, 'kworker', { prot: 'kernel', exe: '' }), facts), null);
eq('lordos packages: lordos and lordos-*, nothing else', ['lordos', 'lordos-web', 'lordosfake', 'notlordos', ''].map(isLordOSPackage), [true, true, false, false, false]);
eq('exe without " (deleted)"', [exePath('/a/b (deleted)'), exePath('/a/b'), exePath(null)], ['/a/b', '/a/b', '']);
eq('a group\'s badge is its main process\'s', groupOriginOf(chrome, facts).kind, 'deb');

// ---- filters ----
const sysO = { kind: 'system' }, debO = { kind: 'deb' };
eq('no filter: everything passes', passesFilter(debO, 'k', {}), true);
eq('by origin', [passesFilter(sysO, 'k', { origins: ['system'] }), passesFilter(debO, 'k', { origins: ['system'] }), passesFilter(null, 'k', { origins: ['system'] })], [true, false, false]);
eq('by program, combined with origins', [passesFilter(debO, 'a', { program: 'a' }), passesFilter(debO, 'b', { program: 'a' }), passesFilter(debO, 'a', { program: 'a', origins: ['flatpak'] })], [true, false, false]);
eq('the program picker: every group by name', programChoices(groups.filter((g) => g.uid === ME), label).slice(0, 2).map(([, l]) => l), ['bash', 'Calendar web']);

// ---- the seal ----
eq('the seal: the check mark in the middle, the seal around it, the corners outside', [sealAt(0.44, 0.67), sealAt(0.5, 0.25), sealAt(0.02, 0.02)], [2, 1, 0]);
const px = sealPixels(13, 2, '#1d9bf0');
eq('the seal\'s pixels: drawn at the screen\'s scale, transparent corners', [px.width, px.height, px.data[3]], [26, 26, 0]);

// ---- rolling statistics ----
{
  const S = statsStore();
  const t0 = Date.UTC(2026, 10, 23, 10, 0, 0);
  const k1 = procKey({ pid: 42, st: 100 }), k2 = procKey({ pid: 42, st: 999 });     // the same PID, restarted
  // 20 minutes at one sample a second: CPU 0.1, a 0.9 spike in the 3rd minute from the end, then 0.5 in the last minute
  for (let i = 0; i < 1200; i++) {
    const t = t0 + i * 1000;
    const cpu = i >= 1140 ? 0.5 : i >= 1020 && i < 1030 ? 0.9 : 0.1;
    S.feed(k1, t, cpu, (i >= 1140 ? 300 : 100) * MB);
  }
  const now = t0 + 1199 * 1000;
  const g = S.get(k1, now);
  const near = (a, b) => Math.abs(a - b) < 1e-6;
  eq('1 minute: the last SECS samples (average and peak)', [g.samples, near(g.cpu[1].avg, 0.5), near(g.cpu[1].peak, 0.5), g.mem[1].peak], [SECS, true, true, 300 * MB]);
  eq('5 minutes: the spike is the peak, the average mixes', [near(g.cpu[5].peak, 0.9), g.cpu[5].avg > 0.1 && g.cpu[5].avg < 0.5], [true, true]);
  eq('15 minutes: the spike too; memory average below its peak', [near(g.cpu[15].peak, 0.9), g.mem[15].avg < g.mem[15].peak], [true, true]);
  // p95: 56 low samples and 4 high ones in the last minute → the 95th percentile is the high value
  const S2 = statsStore();
  for (let i = 0; i < 60; i++) S2.feed('x', t0 + i * 1000, i % 15 === 0 ? 0.8 : 0.2, MB);
  eq('p95 over the last minute', near(S2.get('x', t0 + 59000).cpu[1].p95, 0.8), true);
  const S3 = statsStore();
  for (let i = 0; i < 60; i++) S3.feed('x', t0 + i * 1000, i === 0 ? 0.8 : 0.2, MB);
  eq('one high sample is above the p95, not in it', near(S3.get('x', t0 + 59000).cpu[1].p95, 0.2), true);
  // a restart that got the same PID is another series
  S.feed(k2, now + 1000, 0.05, 10 * MB);
  eq('a restarted process (same PID, another start time) starts over', [S.get(k2, now + 1000).samples, near(S.get(k2, now + 1000).cpu[1].peak, 0.05)], [1, true]);
  eq('and the old one keeps its own', near(S.get(k1, now + 1000).cpu[1].avg, 0.5), true);
  eq('the sparkline: the last minute, oldest first', [g.spark.cpu.length, near(g.spark.cpu[0], 0.5)], [SECS, true]);
  // bounded: a series not fed for MINS minutes is dropped
  S.sweep(now + MINS * 60000 + 5000);
  eq('the sweep drops the series gone for the window', [S.get(k1), S.size()], [null, 0]);
  eq('unknown: null', statsStore().get('nope'), null);
  // a minute bucket that is older than the window is not counted, even when it wraps around the ring
  const S4 = statsStore();
  S4.feed('y', t0, 0.9, MB);
  S4.feed('y', t0 + MINS * 60000, 0.1, MB);              // the same ring slot, MINS minutes later
  eq('an old bucket in a reused slot is replaced, not mixed', near(S4.get('y', t0 + MINS * 60000).cpu[15].peak, 0.1), true);
}

console.log(`monitor_procgroup_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
