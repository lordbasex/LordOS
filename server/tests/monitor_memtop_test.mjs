// monitor_memtop_test.mjs — (0.8.8.6) Monitor → Memory, "Using the most memory" (apps/monitor/memtop.js):
// processes grouped by app, the largest first. Run by tests/run_suites.sh (needs node).
import { memTop } from '../../apps/monitor/memtop.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

const MB = 1024 * 1024;
const list = [
  { pid: 1, name: 'chrome', app: 'google-chrome', rss: 300 * MB },
  { pid: 2, name: 'chrome', app: 'google-chrome', rss: 200 * MB },
  { pid: 3, name: 'chrome', app: 'google-chrome', rss: 150 * MB },
  { pid: 4, name: 'lord-server', app: 'lordos', rss: 310 * MB },
  { pid: 5, name: 'lord-vault', app: 'lordos', rss: 10 * MB },
  { pid: 6, name: 'claude', rss: 420 * MB },
  { pid: 7, name: 'bash', rss: 4 * MB },
  { pid: 8, name: 'bash', rss: 4 * MB },
  { pid: 9, name: 'kthreadd', rss: 0 },
  { pid: 10, name: 'mail', app: 'lpk:mail', rss: 30 * MB },
];
const top = memTop(list, 4);
eq('grouped by app, the largest first', top.map((g) => [g.key, g.mem / MB, g.count]),
  [['app:google-chrome', 650, 3], ['name:claude', 420, 1], ['app:lordos', 320, 2], ['app:lpk:mail', 30, 1]]);
eq('a group shows its largest process', top[2].item.name, 'lord-server');
eq('processes without an app go by name', memTop(list, 10).find((g) => g.key === 'name:bash').count, 2);
eq('kernel threads (no memory) are left out', memTop(list, 10).some((g) => g.key === 'name:kthreadd'), false);
eq('nothing to show', memTop([], 5), []);
eq('no list yet', memTop(undefined), []);

// a web app (0.8.8.8): two profiles' lord-web and WebKit processes in its scope, each WebKit process inside
// bubblewrap. Proportional memory where it was read; the wrappers add their memory but aren't processes of it.
const web = [
  { pid: 20, name: 'lord-web', app: 'desktop:lordos-web-whatsapp', rss: 60 * MB, pss: 15 * MB },
  { pid: 21, name: 'bwrap', app: 'desktop:lordos-web-whatsapp', rss: 2 * MB },
  { pid: 22, name: 'bwrap', app: 'desktop:lordos-web-whatsapp', rss: 1 * MB },
  { pid: 23, name: 'WPEWebProcess', app: 'desktop:lordos-web-whatsapp', rss: 900 * MB, pss: 700 * MB },
  { pid: 24, name: 'bwrap', app: 'desktop:lordos-web-whatsapp', rss: 2 * MB },
  { pid: 25, name: 'WPENetworkProcess', app: 'desktop:lordos-web-whatsapp', rss: 200 * MB, pss: 60 * MB },
];
const wt = memTop(web, 3);
eq('proportional memory where read, resident otherwise', wt.map((g) => [g.key, g.mem / MB]), [['app:desktop:lordos-web-whatsapp', 780]]);
eq('the wrappers are not processes of the app', wt[0].count, 3);
eq('the group shows its largest real process', wt[0].item.name, 'WPEWebProcess');
eq('a wrapper first never stays the shown one', memTop([web[1], web[0]], 1)[0].item.name, 'lord-web');

console.log(`monitor_memtop_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
