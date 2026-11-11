// jobqueue.js — copies and moves wait their turn, no screen: lord-server runs a few jobs at a time (FSJOB_MAX in
// server/src/fsops.h) and refuses one more with fs.tooManyCopies, so a paste of many items started the first ones and
// the rest failed at once (GitHub #104). Here the rest wait and start as the running ones finish.

// start(src, dst, move) → job id, or throws (e.code is the error's code); jobs() → lord.fs.jobs(); forget(id) frees a
// finished job's slot; busy(e): the error says no job is free now.
export function createJobQueue({ start, jobs, forget, busy = (e) => !!e && e.code === 'fs.tooManyCopies' }) {
  const waiting = [];                       // [{ src, dst, move }], in the order asked
  // starts what it can, in order; returns [{ src, error }] for the ones that could not start for another reason
  function pump() {
    const failed = [];
    while (waiting.length) {
      const it = waiting[0];
      try { start(it.src, it.dst, it.move); } catch (e) {
        if (busy(e)) break;                 // every job is taken: this one waits for the next pump
        failed.push({ src: it.src, error: e });
      }
      waiting.shift();
    }
    return failed;
  }
  // one poll: the finished jobs are forgotten and the waiting ones take their slots; a move on the same disk is done as
  // soon as it starts, so this goes on while that frees slots. → { finished: [jobs], failed: [{ src, error }], running }
  function drain() {
    let list = jobs();
    const finished = [], failed = [];
    for (;;) {
      const done = list.filter((j) => j.done);
      for (const j of done) { forget(j.id); finished.push(j); }
      if (!waiting.length || (!done.length && list.length)) break;
      failed.push(...pump());
      list = jobs();
      if (!done.length) break;
    }
    return { finished, failed, running: list.filter((j) => !j.done) };
  }
  return {
    drain,
    // queued after what is already waiting, so the order asked is kept
    add(items) { for (const it of items) waiting.push({ src: it.src, dst: it.dst, move: !!it.move }); return pump(); },
    pump,
    waiting: () => waiting.map((it) => ({ ...it })),
    size: () => waiting.length,
    // a cancel of the job in front also drops what waits behind it (the person stopped the copy)
    clear() { waiting.length = 0; },
  };
}
