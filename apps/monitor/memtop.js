// memtop.js — Monitor → Memory, "Using the most memory": the processes grouped by app, the largest
// first. An app's processes are added up (a browser's tabs, an isolated app's helpers): by its app id
// when the process has one (LordOS, an isolated "lpk:" app, a Linux app's desktop id), else by the
// process name. Resident memory: a page shared between processes counts in each of them, so the
// groups can add up to more than "Used by apps". Pure logic (server/tests/monitor_memtop_test.mjs).

export function memTop(list, n = 8) {
  const groups = new Map();
  for (const p of list || []) {
    if (!p || !(p.rss > 0)) continue;
    const key = p.app ? `app:${p.app}` : `name:${p.name || p.pid}`;
    let g = groups.get(key);
    if (!g) groups.set(key, (g = { key, item: p, rss: 0, count: 0 }));
    g.rss += p.rss;
    g.count++;
    if (p.rss > g.item.rss) g.item = p;              // the group shows its largest process's name and icon
  }
  return [...groups.values()].sort((a, b) => b.rss - a.rss || a.key.localeCompare(b.key)).slice(0, n);
}
