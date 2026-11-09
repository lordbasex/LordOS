// thumb_cache_test.mjs — useThumb's memory of lord-media's thumbnails (sdk/thumbcache.js): a render doesn't ask
// lord-server, and a folder of thumbnails that all fail (lord-media missing, as 0.8.8.6 shipped) or all arrive makes
// one wake-up per watcher, not one render of everything per answer.
//   node tests/thumb_cache_test.mjs        (from server/; run_suites.sh runs it when node exists)
import { createThumbCache, RETRY_MS, KEEP_MS } from '../../sdk/thumbcache.js';

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`FAIL ${name}: got ${g}, want ${w}`); }
};

let clock = 1000;
const asked = [];
let answer = () => null;
const queue = [];
let anyCount = 0;
const mk = (o = {}) => createThumbCache({
  ask: (p, s) => { asked.push(`${s}|${p}`); return answer(p, s); },
  now: () => clock, schedule: (fn) => queue.push(fn), onAny: () => { anyCount++; }, ...o,
});
const run = () => { while (queue.length) queue.shift()(); };

// ---- a render asks once; again only after RETRY_MS while it's being made ----
{
  const c = mk();
  asked.length = 0;
  eq('first render asks', c.get('/h/v.mp4', 256), null);
  for (let i = 0; i < 100; i++) c.get('/h/v.mp4', 256);
  eq('100 renders, one question', asked.length, 1);
  clock += RETRY_MS;
  c.get('/h/v.mp4', 256);
  eq('asked again after RETRY_MS', asked.length, 2);
  eq('no path, no question', c.get(null, 256), null);
  eq('still two', asked.length, 2);
}

// ---- 1,500 failures (no lord-media): each watcher woken once, in one flush; nothing asked again ----
{
  const c = mk();
  asked.length = 0;
  const woke = new Map();
  for (let i = 0; i < 1500; i++) {
    const p = `/h/c${i}.mp4`;
    c.watch(p, () => woke.set(p, (woke.get(p) || 0) + 1));
    c.get(p, 256);
  }
  eq('1,500 questions', asked.length, 1500);
  for (let i = 0; i < 1500; i++) c.arrived(`/h/c${i}.mp4`, '');
  eq('one flush planned for all the answers', queue.length, 1);
  run();
  eq('each watcher woken once', [woke.size, Math.max(...woke.values())], [1500, 1]);
  eq('nobody else woken', anyCount, 0);
  // the renders that follow: failed ones are not asked again for KEEP_MS
  for (let i = 0; i < 1500; i++) c.get(`/h/c${i}.mp4`, 256);
  clock += RETRY_MS;
  for (let i = 0; i < 1500; i++) c.get(`/h/c${i}.mp4`, 256);
  eq('a failed thumbnail is not asked again before KEEP_MS', asked.length, 1500);
  clock += KEEP_MS;
  c.get('/h/c0.mp4', 256);
  eq('after KEEP_MS it may be tried again', asked.length, 1501);
}

// ---- an answer: the next render gets its path, then keeps it ----
{
  const c = mk();
  asked.length = 0;
  answer = () => null;
  let woke = 0;
  const stop = c.watch('/h/a.webp', () => { woke++; });
  c.get('/h/a.webp', 256);
  answer = (p, s) => `/cache/${s}.png`;
  c.arrived('/h/a.webp', '/cache/256.png');
  run();
  eq('its watcher woken', woke, 1);
  eq('the next render asks and gets it', c.get('/h/a.webp', 256), '/cache/256.png');
  for (let i = 0; i < 50; i++) c.get('/h/a.webp', 256);
  eq('then kept: two questions in all', asked.length, 2);
  stop();
  c.arrived('/h/a.webp', '/cache/256.png');
  run();
  eq('a watcher gone is not woken', woke, 1);
  eq('nor everyone, for a path that is known', anyCount, 0);
  answer = () => null;
}

// ---- an answer under another name (a link): everyone once, coalesced ----
{
  const c = mk();
  anyCount = 0;
  c.watch('/h/link.mp4', () => {});
  c.get('/h/link.mp4', 256);
  c.arrived('/real/target.mp4', '/cache/x.png');
  c.arrived('/real/other.mp4', '/cache/y.png');
  run();
  eq('unknown names wake everyone once', anyCount, 1);
}

// ---- bounded ----
{
  const c = mk({ max: 100 });
  for (let i = 0; i < 1000; i++) c.get(`/h/f${i}.mp4`, 256);
  eq('at most max entries', c.size(), 100);
  const stops = [];
  for (let i = 0; i < 50; i++) stops.push(c.watch(`/h/f${i}.mp4`, () => {}));
  for (const s of stops) s();
  eq('no watchers left behind', c.watching(), 0);
}

console.log(bad ? `thumb-cache: ${bad} of ${n} failed` : `thumb-cache: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
