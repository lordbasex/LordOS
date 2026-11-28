// guest_apps_test.mjs — sdk/guestappsmodel.js: a machine's guest apps in the launcher before it runs
// (docs/VIRTUALIZATION.md 6.12). node server/tests/guest_apps_test.mjs
import {
  cleanGuestApps, cleanGuestText, cleanGuestToken, sameGuestApps, guestAppId, parseGuestAppId, isGuestAppId, tokenHash,
  showsGuestApps, guestLauncherItems, launchAskStep, LAUNCH_ASK_MS, GUEST_APPS_MAX, GUEST_TOKEN_MAX, GUEST_NAME_MAX,
  keptGuestItems, shownGuestItems,
} from '../../sdk/guestappsmodel.js';
import { parseVmState, vmState, vmLibraryChanged, vmAnyActive } from '../../sdk/vmlibrary.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const bytes = (s) => Buffer.byteLength(s, 'utf8');

// untrusted names: controls and direction tricks become spaces, spaces collapse, cut to the protocol's bytes
eq('controls and bidi out', cleanGuestText('Paint\u0000\u202e exe.\u2066tnioP\n', 100), 'Paint exe. tnioP');
eq('C1 out', cleanGuestText('a\u0085b\u009bc', 100), 'a b c');
eq('not a string', [cleanGuestText(5, 10), cleanGuestText(null, 10)], ['', '']);
const long = 'é'.repeat(300);
eq('cut on a character, within the bytes', [bytes(cleanGuestText(long, GUEST_NAME_MAX)) <= GUEST_NAME_MAX, cleanGuestText(long, GUEST_NAME_MAX).length], [true, 128]);
// tokens stay byte for byte, or are refused
eq('a token as sent', cleanGuestToken('C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Paint.lnk'), 'C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Paint.lnk');
eq('a token with a control character is refused', [cleanGuestToken('a\nb'), cleanGuestToken('a\u0000'), cleanGuestToken(''), cleanGuestToken(7)], ['', '', '', '']);
eq('a token too long is refused', [cleanGuestToken('x'.repeat(GUEST_TOKEN_MAX)), cleanGuestToken('x'.repeat(GUEST_TOKEN_MAX + 1))], ['x'.repeat(GUEST_TOKEN_MAX), '']);

// the saved list: each token once, cleaned, sorted, capped
const raw = [{ name: 'Zed', token: 'z' }, { name: 'Alpha', token: 'a' }, { name: 'dup', token: 'a' }, { name: '', token: 'e' }, { name: 'NoTok' },
  null, 'x', { name: 'Bad', token: 'b\u0007' }, { name: 'Calc\u202e', token: 'c' }];
eq('cleaned list', cleanGuestApps(raw), [{ name: 'Alpha', token: 'a' }, { name: 'Calc', token: 'c' }, { name: 'Zed', token: 'z' }]);
eq('not an array', [cleanGuestApps(null), cleanGuestApps({})], [[], []]);
const many = Array.from({ length: 400 }, (_, i) => ({ name: `App ${String(i).padStart(3, '0')}`, token: `t${i}` }));
eq('at most LAG_APPS_MAX', cleanGuestApps(many).length, GUEST_APPS_MAX);
eq('same list', [sameGuestApps(cleanGuestApps(raw), cleanGuestApps([...raw].reverse())), sameGuestApps([], [{ name: 'a', token: 'a' }]), sameGuestApps(undefined, [])], [false, false, false]);
eq('same list, same order', sameGuestApps(cleanGuestApps(raw), cleanGuestApps(raw)), true);

// ids
eq('hash: 8 hex, stable', [tokenHash('a').length, tokenHash('a') === tokenHash('a'), tokenHash('a') !== tokenHash('b')], [8, true, true]);
const id = guestAppId('0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b', 'a');
eq('id parses back', [isGuestAppId(id), parseGuestAppId(id)], [true, { mid: '0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b', hash: tokenHash('a') }]);
eq('not ids', [parseGuestAppId('files'), parseGuestAppId('vmapp:'), parseGuestAppId('vmapp:a/b:12345678'), parseGuestAppId('vmapp:a:xyz'), isGuestAppId(5)], [null, null, null, null, false]);

// which machines show: a list, the switch, never a locked one or a group's member
eq('shows', [showsGuestApps({}, { guestApps: [{ name: 'a', token: 'a' }], launcherApps: true }), showsGuestApps({}, { guestApps: [], launcherApps: true }), showsGuestApps({}, {}),
  showsGuestApps({}, { guestApps: [{ name: 'a', token: 'a' }], launcherApps: false }),
  showsGuestApps({ profileLock: 'anonymity' }, { guestApps: [{ name: 'a', token: 'a' }], launcherApps: true }),
  showsGuestApps({ groupDir: '/m/W.lordgroup' }, { guestApps: [{ name: 'a', token: 'a' }], launcherApps: true })], [true, false, false, false, false, false]);
// since 0.8.9.6.2 the switch is off unless turned on: a machine's saved list alone no longer puts its apps in the launcher
eq('off by default', showsGuestApps({}, { guestApps: [{ name: 'a', token: 'a' }] }), false);

const machines = [
  { dir: '/m/w.lordvm', id: 'w1', name: 'Win\u202e 11', state: 'stopped' },
  { dir: '/m/u.lordvm', id: 'u1', name: 'Ubuntu', state: 'running' },
  { dir: '/m/x.lordvm', id: 'x1', name: 'Whonix', state: 'stopped', groupDir: '/m/W.lordgroup', profileLock: 'anonymity' },
  { dir: '/m/b.lordvm', id: 'bad/id', name: 'Bad', state: 'stopped' },
  { dir: '/m/o.lordvm', id: 'o1', name: 'Off', state: 'stopped' },
  { dir: '/m/l.lordvm', id: 'l1', name: 'Locked by caller', state: 'stopped' },
];
const apps = [{ name: 'Paint', token: 'p' }, { name: 'Notepad', token: 'n' }];
const on = { guestApps: apps, launcherApps: true };
const meta = { '/m/w.lordvm': on, '/m/u.lordvm': on, '/m/x.lordvm': on, '/m/b.lordvm': on,
  '/m/o.lordvm': { guestApps: apps, launcherApps: false }, '/m/l.lordvm': on };
const items = guestLauncherItems(machines, (d) => meta[d], { active: (m) => m.state === 'running', locked: (m) => m.id === 'l1' });
eq('entries: only the off, shown, unlocked machine', items.map((x) => [x.machine, x.name, x.token, x.kind, x.mid]), [['Win 11', 'Notepad', 'n', 'machine', 'w1'], ['Win 11', 'Paint', 'p', 'machine', 'w1']]);
eq('entry ids', items.map((x) => x.id), [guestAppId('w1', 'n'), guestAppId('w1', 'p')]);
eq('a broken app.json lists nothing', guestLauncherItems(machines, () => { throw new Error('x'); }), []);
const longName = guestLauncherItems([{ dir: '/d', id: 'q', name: 'Q' }], () => ({ guestApps: [{ name: 'N'.repeat(200), token: 't' }], launcherApps: true }));
eq('a long name shortened', [[...longName[0].name].length, longName[0].name.endsWith('…')], [80, true]);

// kept in settings.json (guestLauncherKept) and read back checked; shown without the machines that run
const allW = guestLauncherItems([{ dir: '/m/w.lordvm', id: 'w1', name: 'Win 11', state: 'running' }], (d) => meta[d]);
eq('kept: what guestLauncherItems gave comes back the same', keptGuestItems(JSON.parse(JSON.stringify(allW))), allW);
eq('kept: never read is null, [] is nothing', [keptGuestItems(null), keptGuestItems(undefined), keptGuestItems({}), keptGuestItems([])], [null, null, null, []]);
eq('kept: a forged id, a control character, a dir that is no machine folder, a wrong kind: left out', keptGuestItems([
  { ...allW[0], id: 'vmapp:w1:00000000' }, { ...allW[0], token: 'a\u0007b', id: guestAppId('w1', 'a\u0007b') },
  { ...allW[0], dir: '/etc' }, { ...allW[0], dir: 'rel.lordvm' }, { ...allW[0], kind: 'linux' }, { ...allW[0], mid: '../x', id: guestAppId('../x', allW[0].token) },
  null, 7, allW[1],
]).map((x) => x.token), [allW[1].token]);
eq('kept: a name with a direction mark cleaned', keptGuestItems([{ ...allW[0], name: 'Pa\u202eint' }])[0].name, 'Pa int');
eq('shown: none of a running machine', shownGuestItems(allW, ['/m/w.lordvm']), []);
eq('shown: all when it is off', shownGuestItems(allW, []).length, 2);

// sdk/vmlibrary.js: lord-server's __lordVm events, never a call
eq('state: parsed', parseVmState('{"active":["/h/Machines/a.lordvm"],"apps":[]}'), { known: true, active: ['/h/Machines/a.lordvm'], apps: [] });
eq('state: junk is ignored', [parseVmState('x'), parseVmState(null), parseVmState('[1]') && parseVmState('[1]').active], [null, null, []]);
eq('state: only absolute folders', parseVmState('{"active":["rel", 3, "/ok"],"apps":"no"}'), { known: true, active: ['/ok'], apps: [] });
let fired = 0;
vmState.subscribe(() => fired++);
eq('state: nothing known, nothing active', [vmState.get().known, vmAnyActive()], [false, false]);
globalThis.__lordVm('state', '{"active":[],"apps":[]}');
globalThis.__lordVm('state', '{"active":[],"apps":[]}');
eq('state: the same state twice is one change', [fired, vmState.get().known], [1, true]);
globalThis.__lordVm('state', '{"active":["/h/M/a.lordvm"],"apps":["/h/M/a.lordvm"]}');
eq('state: a start', [fired, vmAnyActive(), vmState.get().apps], [2, true, ['/h/M/a.lordvm']]);
globalThis.__lordVm('library');
globalThis.__lordVm('nonsense', '{}');
eq('library: counted; anything else ignored', [vmLibraryChanged.get(), fired], [1, 2]);

// the launch: only the exact token of the guest's own fresh list
const ask = { token: 'p', name: 'Paint', at: 1000 };
eq('in the list: launch', launchAskStep(ask, [{ name: 'Paint', token: 'p' }], 2000), 'launch');
eq('the list lacks it: nothing launched', launchAskStep(ask, [{ name: 'Paint', token: 'P' }], 2000), 'missing');
eq('no list yet: wait', launchAskStep(ask, [], 2000), 'wait');
eq('no list for too long: timeout', launchAskStep(ask, [], 1000 + LAUNCH_ASK_MS + 1), 'timeout');
eq('a bad ask never launches', [launchAskStep(null, [{ token: 'p' }], 0), launchAskStep({ token: 'a\nb', at: 0 }, [{ token: 'a\nb' }], 0)], ['missing', 'missing']);

console.log(`guest-apps-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
