// monitor_denied_test.mjs — Monitor → Network's "Recently denied" (apps/monitor/deniedmodel.js): every direction is
// listed, as lord-netperm's Denied counter counts them (QA ws174: "Denied 5" beside "Nothing denied in the last ten
// minutes", the list kept only outgoing ones), the latest of each first, at most four, ten minutes back.
//
//   node tests/monitor_denied_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { recentDenials, deniedTarget, DENIED_WINDOW_MS } = await import(path.join(here, '../../apps/monitor/deniedmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const now = 1_000_000_000;
const ev = (o) => ({ app: 'desktop:firefox', v: 'deny', dir: 'out', addr: '1.2.3.4', port: 443, at: now - 1000, ...o });
eq('nothing', recentDenials([], now), { rows: [], total: 0 });
eq('no events at all', recentDenials(undefined, now).total, 0);
const inOnly = recentDenials([ev({ dir: 'in', addr: '0.0.0.0', port: 5353 })], now);
eq('an incoming denial is listed', inOnly.total, 1);
eq('it names the local port', deniedTarget(inOnly.rows[0].e), '← :5353');
eq('allowed events are not denials', recentDenials([ev({ v: 'allow' })], now).total, 0);
eq('older than ten minutes: gone', recentDenials([ev({ at: now - DENIED_WINDOW_MS - 1 })], now).total, 0);
eq('no time (just arrived): kept', recentDenials([ev({ at: 0 })], now).total, 1);
const many = recentDenials([ev({}), ev({ hosts: ['a.example'], addr: '5.6.7.8' }), ev({}), ev({ addr: '9.9.9.9' }),
  ev({ addr: '8.8.8.8' }), ev({ addr: '7.7.7.7' }), ev({ dir: 'in', port: 22 })], now);
eq('the total counts every one', many.total, 7);
eq('at most four, newest first, repeats merged', many.rows.map(({ e, n: k }) => `${deniedTarget(e)}×${k}`),
  ['← :22×1', '7.7.7.7×1', '8.8.8.8×1', '9.9.9.9×1']);
const twice = recentDenials([ev({}), ev({ addr: '9.9.9.9' }), ev({})], now);
eq('a repeat moves to the newest', twice.rows.map(({ e, n: k }) => `${deniedTarget(e)}×${k}`), ['1.2.3.4×2', '9.9.9.9×1']);
eq('the resolved name first', deniedTarget(ev({ hosts: ['x.example'] })), 'x.example');

console.log(bad ? `monitor-denied: ${bad}/${n} failed` : `monitor-denied: ${n} ok`);
process.exit(bad ? 1 : 0);
