// memtop.js — Monitor → Memory, "Using the most memory": the processes grouped by app, the largest
// first. An app's processes are added up (a browser's tabs, an isolated app's helpers): by its app id
// when the process has one (LordOS, an isolated "lpk:" app, a Linux app's desktop id), else by the
// process name. Each process counts its proportional memory (pss: a page shared by n processes counts
// 1/n in each) when lord-server could read it (the person's own processes of 8 MB or more), else its
// resident memory, which counts a shared library in every process using it (a web app's WebKit
// processes: 2.3 GB resident, 1.5 GB proportional). A sandbox's wrapper (bwrap) adds its memory to the
// app but is not one of its processes. Pure logic (server/tests/monitor_memtop_test.mjs).

const WRAPPERS = new Set(['bwrap']);
export const memOf = (p) => (p && p.pss > 0 ? p.pss : p && p.rss > 0 ? p.rss : 0);

export function memTop(list, n = 8) {
  const groups = new Map();
  for (const p of list || []) {
    const mem = memOf(p);
    if (!(mem > 0)) continue;
    const key = p.app ? `app:${p.app}` : `name:${p.name || p.pid}`;
    let g = groups.get(key);
    if (!g) groups.set(key, (g = { key, item: p, mem: 0, count: 0 }));
    g.mem += mem;
    if (!WRAPPERS.has(p.name)) g.count++;
    // the group shows its largest process's name and icon, never a wrapper's when it has another
    if (!WRAPPERS.has(p.name) && (mem > memOf(g.item) || WRAPPERS.has(g.item.name))) g.item = p;
  }
  return [...groups.values()].sort((a, b) => b.mem - a.mem || a.key.localeCompare(b.key)).slice(0, n);
}
