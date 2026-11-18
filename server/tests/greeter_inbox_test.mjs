// themes/greeter/inbox.js: the login screen's notices (GitHub #172).
// A lock's {t:'mode'} notice that comes before the screen's React effect has added its listener still sets the
// idle deadlines (display off, sleep) and is handed to the first listener; without it, a login screen started for
// the lock stayed lit and the computer never slept.
// Run: node server/tests/greeter_inbox_test.mjs
import { makeInbox } from '../../themes/greeter/inbox.js';

let fails = 0;
const eq = (a, b, what) => {
  if (JSON.stringify(a) !== JSON.stringify(b)) { fails++; console.error(`FAIL ${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};

const lockNote = JSON.stringify({ t: 'mode', mode: 'lock', user: 'ana', dark: true, off: 180, sleep: 300, sleepLeft: 0 });

// the race of #172: the notice first, the listener afterwards
{
  const idle = [];
  const box = makeInbox((m) => idle.push([m.mode, m.off, m.sleep, m.dark]));
  box.dispatch(lockNote);
  eq(idle, [['lock', 180, 300, true]], 'idle state applied with nobody listening');
  const got = [];
  box.listen((m) => got.push(m.t + ':' + m.mode));
  eq(got, ['mode:lock'], 'the early mode notice reaches the first listener');
  eq(idle.length, 1, 'idle state not applied twice by the hand-over');
  const again = [];
  box.listen((m) => again.push(m.t));
  eq(again, [], 'handed over only once');
}

// the usual order: listening first; every notice reaches every listener, idle state once per mode notice
{
  const idle = [];
  const box = makeInbox((m) => idle.push(m.mode));
  const a = [], b = [];
  box.listen((m) => a.push(m.t));
  const stop = box.listen((m) => b.push(m.t));
  box.dispatch(JSON.stringify({ t: 'hello', mode: 'login', users: [] }));
  box.dispatch(lockNote);
  stop();
  box.dispatch(JSON.stringify({ t: 'ok' }));
  eq(a, ['hello', 'mode', 'ok'], 'first listener');
  eq(b, ['hello', 'mode'], 'a listener that stopped');
  eq(idle, ['lock'], 'idle state for the mode notice only');
}

// only the last mode notice waits; other notices with nobody listening are not kept; bad JSON is ignored
{
  const box = makeInbox(() => {});
  box.dispatch(JSON.stringify({ t: 'mode', mode: 'login' }));
  box.dispatch(JSON.stringify({ t: 'busy' }));
  box.dispatch('{not json');
  box.dispatch('null');
  box.dispatch(lockNote);
  const got = [];
  box.listen((m) => got.push(`${m.t}:${m.mode || ''}`));
  eq(got, ['mode:lock'], 'the last mode notice only');
}

// an onMode that throws (an older lord-server without lord.display) does not stop the notice
{
  const box = makeInbox(() => { throw new Error('no lord.display'); });
  const got = [];
  box.listen((m) => got.push(m.t));
  box.dispatch(lockNote);
  eq(got, ['mode'], 'notice delivered when the idle call throws');
}

if (fails) { console.error(`greeter-inbox: ${fails} failure(s)`); process.exit(1); }
console.log('greeter-inbox: ok');
