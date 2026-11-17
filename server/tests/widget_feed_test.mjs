// The desk widgets' data feed (sdk/widgetfeedmodel.js): one read per source, aligned ticks, nothing read for a
// widget nobody can see, a fresh read when it shows again, and the visibility's grace period.
import assert from 'node:assert/strict';
import { alignedDue, coveredBy, createFeed, createVisibility } from '../../sdk/widgetfeedmodel.js';
import { sameValue } from '../../sdk/samevalue.js';

// a fake clock with timers
function clock(start = 1_000_000) {
  let t = start, seq = 0;
  const timers = new Map();
  return {
    now: () => t,
    setTimer: (fn, ms) => { const id = ++seq; timers.set(id, { at: t + ms, fn }); return id; },
    clearTimer: (id) => { timers.delete(id); },
    pending: () => timers.size,
    advance(ms) {
      const end = t + ms;
      for (;;) {
        let next = null;
        for (const [id, x] of timers) if (x.at <= end && (!next || x.at < next[1].at)) next = [id, x];
        if (!next) break;
        timers.delete(next[0]);
        t = Math.max(t, next[1].at);
        next[1].fn();
      }
      t = end;
    },
  };
}

// aligned ticks
assert.equal(alignedDue(1000, 1000), 2000);
assert.equal(alignedDue(1999, 1000), 2000);
assert.equal(alignedDue(4100, 2000), 6000);

// covered: wholly inside one frame (with a little slack), not by two halves
const r = { x: 100, y: 100, w: 200, h: 100 };
assert.ok(coveredBy(r, [{ x: 0, y: 0, w: 1000, h: 800 }]));
assert.ok(coveredBy(r, [{ x: 101, y: 101, w: 198, h: 98 }]), 'a frame a pixel short still hides it');
assert.ok(!coveredBy(r, [{ x: 150, y: 0, w: 1000, h: 800 }]), 'partly covered is seen');
assert.ok(!coveredBy(r, [{ x: 0, y: 0, w: 200, h: 800 }, { x: 200, y: 0, w: 800, h: 800 }]), 'two halves: still updating');
assert.ok(!coveredBy(r, []));
assert.ok(!coveredBy(r, [{ x: 0, y: 0, w: 0, h: 0 }]));

{
  // one source, two widgets: read once per tick, at the shortest visible interval
  const c = clock(1_000_500);
  let screen = true;
  const feed = createFeed({ ...c, seen: () => screen, same: sameValue });
  let n = 0;
  const read = () => ++n;
  const got = { a: [], b: [] };
  assert.equal(feed.peek('cpu', read), 1, 'the first render reads once');
  const a = feed.subscribe('cpu', read, 2000, true, (v) => got.a.push(v));
  const b = feed.subscribe('cpu', read, 1000, true, (v) => got.b.push(v));
  assert.equal(n, 1, 'subscribing with a fresh value reads nothing');
  c.advance(1000);
  assert.equal(n, 2, 'one read for both, at the shortest interval');
  assert.deepEqual(got.a, [2]);
  assert.deepEqual(got.b, [2]);
  assert.equal(feed.stats('cpu').ms, 1000);

  // b hidden: the source slows to a's 2 s, ticks on even seconds
  b.setVisible(false);
  const before = n;
  c.advance(4000);
  assert.equal(n - before, 2, 'every 2 s while only a is seen');
  assert.equal(got.b.length, 1, 'a hidden widget is not told');

  // both hidden: the source stops, no timer left
  a.setVisible(false);
  const stopped = n;
  c.advance(10_000);
  assert.equal(n, stopped, 'nothing read with no widget seen');
  assert.equal(c.pending(), 0, 'no timer left at all');

  // shown again: a fresh read at once (its value was 10 s old), then the ticks again
  b.setVisible(true);
  assert.equal(n, stopped + 1, 'a fresh read when it shows again');
  assert.equal(got.b[got.b.length - 1], n);
  c.advance(1000);
  assert.equal(n, stopped + 2);

  // nobody sees the screen: the tick comes but reads nothing
  screen = false;
  const dark = n;
  c.advance(5000);
  assert.equal(n, dark, 'no reads while the screen is off');
  screen = true;
  c.advance(1000);
  assert.equal(n, dark + 1);

  // closed: the source and its timer go
  a.close();
  b.close();
  assert.equal(feed.stats('cpu'), null);
  assert.equal(c.pending(), 0);
}

{
  // the same data notifies nobody; sources due together read in the same tick
  const c = clock(0);
  const feed = createFeed({ ...c, same: sameValue });
  let same = 0, x = 0;
  const calls = [];
  const s1 = feed.subscribe('same', () => ({ v: 1 }), 1000, true, () => same++);
  const s2 = feed.subscribe('net', () => ++x, 2000, true, () => calls.push(['net', c.now()]));
  const s3 = feed.subscribe('cpu', () => ++x, 1000, true, () => calls.push(['cpu', c.now()]));
  same = 0;
  calls.length = 0;
  c.advance(4000);
  assert.equal(same, 0, 'an unchanged value is not passed on');
  const at = (k) => calls.filter((e) => e[0] === k).map((e) => e[1]);
  assert.deepEqual(at('net'), [2000, 4000]);
  assert.deepEqual(at('cpu'), [1000, 2000, 3000, 4000]);
  assert.ok(at('net').every((t) => at('cpu').includes(t)), 'the 2 s source ticks with the 1 s one');
  s1.close(); s2.close(); s3.close();
}

{
  // a source's own state (counters between reads); a read that throws gives null
  const c = clock(0);
  const feed = createFeed({ ...c, same: sameValue });
  let total = 0, boom = false;
  const rate = (st) => { if (boom) throw new Error('gone'); total += 10; const p = st.p; st.p = total; return p == null ? null : total - p; };
  const seen = [];
  assert.equal(feed.peek('rate', rate), null, 'the first read of a rate has nothing to compare');
  const s = feed.subscribe('rate', rate, 1000, true, (v) => seen.push(v));
  c.advance(1000);
  assert.deepEqual(seen, [10]);
  boom = true;
  c.advance(1000);
  assert.equal(seen[seen.length - 1], null);
  s.close();
}

{
  // visibility: shown at once, hidden after the grace period, a quick pass over it does nothing
  const c = clock(0);
  const vis = createVisibility({ setTimer: c.setTimer, clearTimer: c.clearTimer, hideMs: 1000 });
  const log = [];
  vis.listen('w1', (v) => log.push(v));
  assert.equal(vis.visible('w1'), true);
  vis.set('w1', false);
  c.advance(500);
  vis.set('w1', true);
  c.advance(2000);
  assert.deepEqual(log, [], 'a window passing over it: nothing changes');
  vis.set('w1', false);
  c.advance(999);
  assert.deepEqual(log, []);
  c.advance(1);
  assert.deepEqual(log, [false], 'hidden after a second');
  vis.set('w1', true);
  assert.deepEqual(log, [false, true], 'shown at once');
  vis.forget('w1');
  assert.equal(vis.visible('w1'), true, 'an unknown widget counts as seen');
}

console.log('widget feed: ok');
