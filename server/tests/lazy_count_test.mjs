// lazy_count_test.mjs — sdk/lazycount.js (QA 0.8.9.6.2, the Pi's first __lordTick): a slow count (the crown menu's
// package updates, lord.pkg.upgrades()) is never read inside the render that asks for it; the first read waits `first`
// ms, then at most one read per `ttl`; subscribers hear only real changes; invalidate() asks for a new read.
//   node server/tests/lazy_count_test.mjs
import { lazyCount } from '../../sdk/lazycount.js';
let bad = 0, n = 0;
const eq = (name, a, b) => { n++; if (JSON.stringify(a) !== JSON.stringify(b)) { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
let t = 1000, reads = 0, value = 3;
const timers = [];
const later = (fn, ms) => timers.push({ fn, at: t + ms });
const tick = (ms) => { t += ms; for (const x of timers.splice(0)) { if (x.at <= t) x.fn(); else timers.push(x); } };
const c = lazyCount(() => { reads++; return value; }, { first: 15000, ttl: 60000, now: () => t, later });
let told = [];
c.subscribe((v) => told.push(v));
eq('the first ask returns at once, without reading', [c.get(), reads], [0, 0]);
eq('many renders: one read scheduled', [c.get(), c.get(), timers.length], [0, 0, 1]);
tick(14000);
eq('not before `first`', reads, 0);
tick(1000);
eq('read once after `first`, subscribers told', [reads, c.get(), told], [1, 3, [3]]);
value = 5;
tick(30000);
eq('fresh: no new read', [c.get(), reads, timers.length], [3, 1, 0]);
tick(31000);
eq('stale: the ask returns the last value and schedules one read', [c.get(), c.get(), timers.length], [3, 3, 1]);
tick(0);
eq('the new value', [reads, c.peek(), told], [2, 5, [3, 5]]);
tick(61000);
c.get(); tick(0);
eq('same value: subscribers not told again', [reads, told], [3, [3, 5]]);
value = 1;
c.invalidate(); tick(0);
eq('invalidate reads again at once', [reads, c.peek()], [4, 1]);
const e = lazyCount(() => { throw new Error('x'); }, { first: 0, now: () => t, later });
e.get(); tick(0);
eq('a read that throws counts as 0', e.peek(), 0);
// a read that answers later (lord.pkg.upgradeCountAsync through sdk/fs.js workerReply)
let settle = null, areads = 0;
const a = lazyCount(() => { areads++; return new Promise((res, rej) => { settle = { res, rej }; }); }, { first: 0, ttl: 60000, now: () => t, later });
a.get(); tick(0);
eq('async: asked once, still the old value', [areads, a.get(), a.get(), timers.length], [1, 0, 0, 0]);
settle.res(7); await null; await null;
eq('async: the answer is the value', a.peek(), 7);
tick(61000); a.get(); tick(0);
settle.rej(new Error('busy')); await null; await null;
eq('async: a failed read keeps the last value', [areads, a.peek()], [2, 7]);
console.log(bad ? `lazy-count-test: ${bad}/${n} failed` : `lazy-count-test: ${n} ok`);
process.exit(bad ? 1 : 0);
