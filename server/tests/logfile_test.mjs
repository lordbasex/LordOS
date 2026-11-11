// LordOS's own log files in the Logs app (sdk/logfile.js): the stamped lines lord-log writes
// (server/src/lordlog.c), old unstamped ones, and the sidebar's order. node server/tests/logfile_test.mjs
import { parseLordLog, sortFiles, fileGroup, fileLabel, isPrevious, isOlder } from '../../sdk/logfile.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

const text = [
  '2026-11-11T12:43:56.123-03:00 INFO  xwayland: starting Xwayland on :0',
  '2026-11-11T12:43:57.000+00:00 ERROR compositor: start failed: no card',
  '2026-11-11T12:43:58.500-03:00 WARN  lord-sessiond[812]: no session bus: it starts without it',
  '2026-11-11T12:43:59.000-03:00 ERROR js:     at f (theme.js:1)',
  '',
  'Gtk-WARNING: a Linux app writing on its own',
  'plain text from an older version',
].join('\n');
const r = parseLordLog(text, 'apps/org.gnome.Calculator.log');
eq('rows', r.length, 6);
eq('time with its offset', r[0].t, Date.UTC(2026, 10, 11, 15, 43, 56, 123) * 1000);
eq('utc', r[1].t, Date.UTC(2026, 10, 11, 12, 43, 57, 0) * 1000);
eq('level info', r[0].p, 6);
eq('level error', r[1].p, 3);
eq('level warn', r[2].p, 4);
eq('subsystem', r[0].id, 'xwayland');
eq('message', r[1].m, 'start failed: no card');
eq('pid split off', [r[2].id, r[2].pid], ['lord-sessiond', 812]);
eq('a stack line keeps its indent', r[3].m, '    at f (theme.js:1)');
eq('line numbers', r.map((x) => x.line), [1, 2, 3, 4, 6, 7]);
eq('unstamped: no time, its prefix', [r[4].t, r[4].id, r[4].p], [0, 'Gtk-WARNING', 4]);
eq('unstamped: the file name', [r[5].id, r[5].p], ['org.gnome.Calculator', 6]);
eq('empty', parseLordLog('', 'x.log'), []);
eq('not a stamp', parseLordLog('2026-11-11 12:00 INFO x: y', 'a.log')[0].t, 0);

eq('groups', ['compositor.log', 'apps/a.log', 'web/b-c.log', 'cache/session.log'].map(fileGroup), ['session', 'apps', 'web', 'old']);
eq('labels', ['compositor.log', 'compositor.log.prev', 'apps/org.x.log', 'web/gmail-default.log'].map(fileLabel), ['compositor', 'compositor', 'org.x', 'gmail-default']);
eq('previous', ['compositor.log.prev', 'compositor.log', 'compositor.log.old'].map(isPrevious), [true, false, false]);
eq('older part', ['compositor.log.old', 'compositor.log', 'compositor.log.prev', 'apps/a.log.old'].map(isOlder), [true, false, false, true]);
eq('older label', fileLabel('apps/org.x.log.old'), 'org.x');
eq('order with the older part', sortFiles(['compositor.log.prev', 'compositor.log.old', 'compositor.log', 'apps/a.log.old', 'apps/a.log'].map((name) => ({ name }))).map((f) => f.name),
  ['compositor.log', 'compositor.log.old', 'compositor.log.prev', 'apps/a.log', 'apps/a.log.old']);
const names = ['web/b.log', 'xwayland.log', 'apps/z.log', 'shell.log.prev', 'cache/session.log', 'compositor.log.prev', 'apps/a.log', 'cloud.log', 'shell.log', 'compositor.log'];
eq('order', sortFiles(names.map((name) => ({ name }))).map((f) => f.name),
  ['compositor.log', 'compositor.log.prev', 'shell.log', 'shell.log.prev', 'cloud.log', 'xwayland.log', 'apps/a.log', 'apps/z.log', 'web/b.log', 'cache/session.log']);
console.log(`logfile-test: ${ok} ok`);
