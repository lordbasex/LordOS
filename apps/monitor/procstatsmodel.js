// procstatsmodel.js — Monitor → Processes' rolling statistics (docs/MONITOR.md, "Rolling statistics"): for each process
// (by identity: PID and start time, so a restart that gets the same PID starts over) and each group, the average and
// the peak of its CPU and memory over the last 1, 5 and 15 minutes, the 95th percentile of its CPU over the last minute,
// and the last minute as a sparkline. Fed from the process list lord-server builds once a second. Pure logic, tested by
// server/tests/monitor_procgroup_test.mjs.
//
// Cheap and bounded: a feed is O(1) per series (a ring of the last SECS samples with running sums, and a ring of MINS
// one-minute buckets with their sums and maximums); a series not fed for MINS minutes is dropped by sweep. Reading
// one walks at most SECS samples (only for the rows on screen).
export const SECS = 60;
export const MINS = 15;
export const WINDOWS = [1, 5, 15];

function series() {
  return {
    seen: 0, cpu: new Float32Array(SECS), mem: new Float64Array(SECS), head: 0, count: 0,
    b0: new Float64Array(MINS).fill(-1), bc: new Float64Array(MINS), bm: new Float64Array(MINS), bxc: new Float64Array(MINS), bxm: new Float64Array(MINS),
    bn: new Uint32Array(MINS),
  };
}

export function statsStore() {
  const map = new Map();
  // one sample of a series: t in ms, cpu as a fraction of one core (0.5 = 50 %), mem in bytes
  function feed(key, t, cpu, mem) {
    let s = map.get(key);
    if (!s) map.set(key, (s = series()));
    s.seen = t;
    s.cpu[s.head] = cpu;
    s.mem[s.head] = mem;
    s.head = (s.head + 1) % SECS;
    if (s.count < SECS) s.count++;
    const mi = Math.floor(t / 60000), b = mi % MINS;
    if (s.b0[b] !== mi) { s.b0[b] = mi; s.bc[b] = 0; s.bm[b] = 0; s.bxc[b] = 0; s.bxm[b] = 0; s.bn[b] = 0; }
    s.bc[b] += cpu; s.bm[b] += mem; s.bn[b]++;
    if (cpu > s.bxc[b]) s.bxc[b] = cpu;
    if (mem > s.bxm[b]) s.bxm[b] = mem;
  }
  // drops the series not fed for MINS minutes (an exited process, a group that is gone)
  function sweep(t) {
    for (const [k, s] of map) if (t - s.seen > MINS * 60000) map.delete(k);
  }
  // the last SECS samples, oldest first
  function last(s, arr) {
    const out = new Array(s.count);
    for (let i = 0; i < s.count; i++) out[i] = arr[(s.head - s.count + i + SECS) % SECS];
    return out;
  }
  // { cpu: { 1: { avg, peak, p95 }, 5: { avg, peak }, 15: { avg, peak } }, mem: { …same without p95 }, spark: { cpu: [], mem: [] }, samples }
  // at time t (null when the series is unknown). The 1-minute window is the last SECS samples; 5 and 15 minutes are the
  // one-minute buckets of that many minutes, the current one included.
  function get(key, t = Date.now()) {
    const s = map.get(key);
    if (!s || !s.count) return null;
    const cpu = last(s, s.cpu), mem = last(s, s.mem);
    const sum = (a) => a.reduce((x, y) => x + y, 0);
    const sorted = [...cpu].sort((a, b) => a - b);
    const p95 = sorted[Math.min(sorted.length - 1, Math.ceil(0.95 * sorted.length) - 1)];
    const out = {
      samples: s.count,
      cpu: { 1: { avg: sum(cpu) / cpu.length, peak: Math.max(...cpu), p95 } },
      mem: { 1: { avg: sum(mem) / mem.length, peak: Math.max(...mem) } },
      spark: { cpu, mem },
    };
    const mi = Math.floor(t / 60000);
    for (const w of [5, 15]) {
      let c = 0, m = 0, n = 0, xc = 0, xm = 0;
      for (let b = 0; b < MINS; b++) {
        if (s.b0[b] < 0 || s.b0[b] <= mi - w || s.b0[b] > mi) continue;
        c += s.bc[b]; m += s.bm[b]; n += s.bn[b];
        if (s.bxc[b] > xc) xc = s.bxc[b];
        if (s.bxm[b] > xm) xm = s.bxm[b];
      }
      out.cpu[w] = n ? { avg: c / n, peak: xc } : out.cpu[1];
      out.mem[w] = n ? { avg: m / n, peak: xm } : out.mem[1];
    }
    return out;
  }
  return { feed, sweep, get, size: () => map.size };
}
