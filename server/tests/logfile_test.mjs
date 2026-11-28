// LordOS's own log files in the Logs app (sdk/logfile.js): the stamped lines lord-log writes
// (server/src/lordlog.c), old unstamped ones, and the sidebar's order. node server/tests/logfile_test.mjs
import { parseLordLog, sortFiles, fileGroup, fileLabel, isPrevious, isOlder, levelMatch, LEVEL_MAX, cloudAccounts, inRange, rowText, exportLine, SOURCE_GROUPS, toggleGroup } from '../../sdk/logfile.js';

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
// a web app's log (lord-web's lord-log child, lordlog_stamp_start): lord-web's own lines and WebKit's, both stamped
const wl = parseLordLog([
  'lord-web: an older, unstamped line',
  '2026-11-26T18:22:03.713-03:00 WARN  lord-web: WebKit says the page stopped responding',
  '2026-11-26T18:22:04.001-03:00 WARN  webkit: ** (WPEWebProcess:2): WARNING **: 18:22:04.001: odd',
  '2026-11-26T18:22:07.913-03:00 INFO  lord-web: the page responds again after 4.2 s',
].join('\n'), 'web/whatsapp-personal.log');
eq('web: unstamped older line', [wl[0].t, wl[0].id], [0, 'lord-web']);
eq('web: stamped', wl.slice(1).map((x) => [x.id, x.p]), [['lord-web', 4], ['webkit', 4], ['lord-web', 6]]);
eq('web: time', wl[3].t, Date.UTC(2026, 10, 26, 21, 22, 7, 913) * 1000);
eq('web: WebKit\'s text kept whole', wl[2].m, '** (WPEWebProcess:2): WARNING **: 18:22:04.001: odd');
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
// #183: rclone's own lines in cloud.log: their time (local) and level, a multi-line error as one row
const p2 = (n) => String(n).padStart(2, '0');
// a LordOS stamp in this machine's zone (as lord-log writes it) for a local time
const stamp = (h, m, s, ms) => {
  const o = -new Date(2026, 10, 17, h, m, s).getTimezoneOffset(), a = Math.abs(o);
  return `2026-11-17T${p2(h)}:${p2(m)}:${p2(s)}.${String(ms).padStart(3, '0')}${o < 0 ? '-' : '+'}${p2(Math.floor(a / 60))}:${p2(a % 60)}`;
};
const quota = 'couldn\'t list directory: googleapi: Error 403: Quota exceeded, rateLimitExceeded';
const cl = [
  `${stamp(10, 0, 0, 0)} INFO  cloud: rclone v1.75.2 started`,
  '2026/11/17 10:12:59 NOTICE: Personal Vault: will not be listed',
  `2026/11/17 10:12:59 ERROR : error listing: ${quota}`,
  'Details:',
  '[',
  '  { "reason": "rateLimitExceeded" }',
  ']',
  `2026/11/17 10:12:59 ERROR : rc: "operations/list": error: error in ListJSON: ${quota}`,
  'Details:',
  '[',
  '  { "reason": "rateLimitExceeded" }',
  ']',
  `${stamp(10, 12, 59, 400)} ERROR c1a2b3c4d: operations/list: error in ListJSON: ${quota} Details: [   { "reason": "rateLimitExceeded" } ]`,
  '2026/11/17 10:13:30 ERROR : Mes photos/amis.jpg: vfs cache: detected external removal of cache file',
  '2026/11/17 10:13:31.250000 DEBUG : a debug line with microseconds',
  '2026/11/17 10:13:32 CRITICAL: Failed to create file system',
].join('\n');
const cr = parseLordLog(cl, 'cloud.log');
eq('rclone rows', cr.length, 8);
eq('rclone time, local', cr[1].t, new Date(2026, 10, 17, 10, 12, 59).getTime() * 1000);
eq('rclone levels', cr.map((x) => x.p), [6, 5, 3, 3, 3, 3, 7, 2]);
eq('rclone id', [cr[1].id, cr[2].id, cr[0].id], ['rclone', 'rclone', 'cloud']);
eq('rclone message', cr[1].m, 'Personal Vault: will not be listed');
eq('a multi-line error is one row', cr[2].m.split('\n').length, 5);
eq('microseconds', cr[6].t, new Date(2026, 10, 17, 10, 13, 31, 250).getTime() * 1000);
eq('a LordOS line ends rclone\'s', parseLordLog(`2026/11/17 10:12:59 ERROR : x\n${stamp(10, 0, 0, 0)} INFO  cloud: y\nplain`, 'cloud.log').map((x) => x.m), ['x', 'y', 'plain']);
eq('a Linux app\'s lines stay rows of their own', parseLordLog('a\nb', 'apps/x.log').length, 2);

// the level filter: that level only
eq('levels', [0, 2, 3, 4, 5, 6, 7, -1].map((p) => ['all', 'info', 'warning', 'error'].map((l) => +levelMatch(p, l)).join('')),
  ['1001', '1001', '1001', '1010', '1100', '1100', '1000', '1100']);
eq('journal bound', LEVEL_MAX, { info: 6, warning: 4, error: 3 });

// the account each line is about; rclone's copy of lord-server's line folded into it
const ca = cloudAccounts(cr);
eq('folded', ca.length, 7);
eq('attributed', ca.map((x) => x.acct || ''), ['', '', 'c1a2b3c4d', 'c1a2b3c4d', '', '', '']);
eq('the folded row keeps rclone\'s lines', ca[3].m.startsWith('operations/list: error in ListJSON') && ca[3].m.includes('\n]'), true);
eq('the notice of that second is not taken', ca[1].acct, undefined);
eq('a line of another moment is not taken', ca[4].acct, undefined);
eq('nothing to do', cloudAccounts(parseLordLog('2026/11/17 10:12:59 ERROR : x', 'cloud.log')).map((x) => x.acct), [undefined]);
eq('an id with no rclone line', cloudAccounts(parseLordLog(`${stamp(10, 0, 0, 0)} ERROR c0000000a: mount/mount: failed`, 'cloud.log'))[0].acct, 'c0000000a');
eq('not an account id', cloudAccounts(parseLordLog(`${stamp(10, 0, 0, 0)} ERROR compositor: x: y`, 'cloud.log'))[0].acct, undefined);
eq('the rows given are not changed', cr[2].acct, undefined);

// #194: rclone's own lines that name the remote belong to that account
eq('rclone names the remote', cloudAccounts(parseLordLog("2026/11/18 19:27:01 ERROR : cc4ed54b5: mount helper error\n2026/11/18 19:27:02 ERROR : Google drive root 'cc4ed54b5:': x\n2026/11/18 19:27:03 ERROR : notc0000000: y", 'cloud.log')).map((x) => x.acct),
  ['cc4ed54b5', 'cc4ed54b5', undefined]);

// #194: the time filter for a file's rows
const tr = parseLordLog([`${stamp(9, 0, 0, 0)} INFO  cloud: old`, 'its next line', `${stamp(10, 0, 0, 0)} INFO  cloud: new`, 'after new'].join('\n'), 'apps/x.log');
const at10 = new Date(2026, 10, 17, 9, 30, 0).getTime() * 1000;
eq('in range: newer only, an unstamped line with the one before', inRange(tr, at10).map((x) => x.m), ['new', 'after new']);
eq('in range: all', inRange(tr, 0).length, 4);
eq('in range: no time at all is kept', inRange(parseLordLog('a\nb', 'apps/x.log'), at10).length, 2);
eq('in range: rclone rows', inRange(cr, new Date(2026, 10, 17, 10, 13, 30).getTime() * 1000).map((x) => x.m.slice(0, 12)), ['Mes photos/a', 'a debug line', 'Failed to cr']);

// #194: the list and the export say the same thing, on one line, with the account
const one = cloudAccounts(cr)[3];
eq('row text: label first, one line', rowText(one, 'Google Drive · Personal').startsWith('Google Drive · Personal: operations/list: error in ListJSON'), true);
eq('row text: no line break', rowText(one, 'x').includes('\n'), false);
eq('row text: breaks shown', rowText({ m: 'a\nb' }, ''), 'a ⏎ b');
eq('export line: the list\'s text after time, level and process', exportLine(one, 'Wed 19:27:01.643', 'Error', 'Google Drive · Personal'),
  `Wed 19:27:01.643 Error c1a2b3c4d: ${rowText(one, 'Google Drive · Personal')}`);
eq('export line: pid', exportLine({ id: 'lord-sessiond', pid: 812, m: 'x' }, 'T', 'Info', ''), 'T Info lord-sessiond[812]: x');

// #199: the sidebar's groups fold and unfold; anything else in the saved list is dropped
eq('fold', toggleGroup([], 'apps'), ['apps']);
eq('unfold', toggleGroup(['journal', 'apps'], 'apps'), ['journal']);
eq('in the sidebar\'s order', toggleGroup(['web'], 'journal'), ['journal', 'web']);
eq('not a group', toggleGroup(['x', 'web', 'web'], 'nope'), ['web']);
eq('nothing saved', toggleGroup(null, 'old'), ['old']);
eq('groups', SOURCE_GROUPS, ['journal', 'session', 'apps', 'web', 'old']);
console.log(`logfile-test: ${ok} ok`);
