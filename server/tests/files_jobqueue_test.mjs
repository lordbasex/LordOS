// files_jobqueue_test.mjs — a paste of many items in Files: the copies past lord-server's job slots wait their turn
// instead of failing (apps/files/jobqueue.js, GitHub #104).
import { createJobQueue } from '../../apps/files/jobqueue.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

// lord-server's job table as fsops.c keeps it: FSJOB_MAX slots, a slot freed only by forget, a move on the same
// disk done as soon as it starts
const SLOTS = 8;
function server({ instantMoves = false, refuse = () => null } = {}) {
  let next = 1;
  const table = [];
  const started = [];
  return {
    table, started,
    start(src, dst, move) {
      const why = refuse(src);
      if (why) { const e = new Error(why); e.code = why; throw e; }
      if (table.length >= SLOTS) { const e = new Error('Too many copies in progress'); e.code = 'fs.tooManyCopies'; throw e; }
      const j = { id: next++, src, dst, move, done: !!(move && instantMoves), failed: false };
      table.push(j);
      started.push(src);
      return j.id;
    },
    jobs: () => table.map((j) => ({ ...j })),
    forget(id) { const i = table.findIndex((j) => j.id === id && j.done); if (i >= 0) table.splice(i, 1); },
    finish(n) { for (const j of table.filter((x) => !x.done).slice(0, n)) j.done = true; },
  };
}
const items = (n, move = false) => Array.from({ length: n }, (_, i) => ({ src: `/home/u/f${i}`, dst: `/home/u/dst/f${i}`, move }));
const make = (s) => createJobQueue({ start: s.start, jobs: s.jobs, forget: s.forget });

// 20 copies: 8 start, 12 wait, none fails (it was: 12 × "Too many copies in progress")
{
  const s = server(), q = make(s);
  eq('20 copies: no errors', q.add(items(20)), []);
  eq('20 copies: 8 started', s.started.length, 8);
  eq('20 copies: 12 wait', q.size(), 12);
  eq('nothing finished: nothing more starts', q.drain().running.length, 8);
  eq('nothing finished: still 12', q.size(), 12);
  s.finish(3);
  const d = q.drain();
  eq('3 finish: forgotten', d.finished.length, 3);
  eq('3 finish: 3 more start', s.started.length, 11);
  eq('3 finish: 8 running', d.running.length, 8);
  while (q.size() || s.table.length) { s.finish(8); q.drain(); }
  eq('all 20 copied, in order', s.started, items(20).map((i) => i.src));
}

// 100 moves on the same disk: each is done at once, one poll moves them all
{
  const s = server({ instantMoves: true }), q = make(s);
  eq('100 moves: no errors', q.add(items(100, true)), []);
  const d = q.drain();
  eq('100 moves: one poll', s.started.length, 100);
  eq('100 moves: all forgotten', d.finished.length, 100);
  eq('100 moves: nothing waits', q.size(), 0);
  eq('100 moves: slots free', s.table.length, 0);
}

// another error is that item's own: it is reported and the rest go on
{
  const s = server({ refuse: (src) => (src === '/home/u/f2' ? 'fs.exists' : null) }), q = make(s);
  const f = q.add(items(5));
  eq('a refused one: reported', f.map((x) => [x.src, x.error.code]), [['/home/u/f2', 'fs.exists']]);
  eq('a refused one: the others start', s.started, ['/home/u/f0', '/home/u/f1', '/home/u/f3', '/home/u/f4']);
}
{
  const s = server({ refuse: (src) => (src === '/home/u/f10' ? 'fs.protected' : null) }), q = make(s);
  q.add(items(12));
  s.finish(8);
  const d = q.drain();
  eq('a refused waiting one: reported on the poll', d.failed.map((x) => x.src), ['/home/u/f10']);
  eq('a refused waiting one: the rest start', s.started.length, 11);
}

// a second paste waits behind the first one's items
{
  const s = server(), q = make(s);
  q.add(items(10));
  q.add([{ src: '/home/u/late', dst: '/home/u/dst/late', move: false }]);
  eq('second paste: behind', q.waiting().map((x) => x.src), ['/home/u/f8', '/home/u/f9', '/home/u/late']);
  s.finish(8); q.drain();
  eq('second paste: started after', s.started.slice(-3), ['/home/u/f8', '/home/u/f9', '/home/u/late']);
}

// cancel: what waits doesn't start
{
  const s = server(), q = make(s);
  q.add(items(12));
  q.clear();
  s.finish(8);
  q.drain();
  eq('cancel: nothing more starts', s.started.length, 8);
}

// all slots taken by another app's copies: Files' wait, and start once one finishes
{
  const s = server(), q = make(s);
  for (let i = 0; i < SLOTS; i++) s.start(`/other/${i}`, `/other/d${i}`, false);
  eq('slots taken: no error', q.add(items(2)), []);
  eq('slots taken: waiting', q.size(), 2);
  s.finish(1); q.drain();
  eq('slots taken: one starts', q.size(), 1);
}

console.log(`files-jobqueue-test: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
