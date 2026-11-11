// memfigure.js — the one figure for memory in use, kept free of imports so node tests load it.
// Memory in use, one figure everywhere (GitHub #118: the Control Center showed total − MemAvailable, Monitor and
// About the server's memUsedKB, so the same moment read 1.9 GB in one place and 1.7 GB in the other). memUsedKB
// leaves out the page cache and the kernel's reserve (server/src/meminfo.h); the older reading is only
// the fallback when it is missing.
export function memUsedKB(info) {
  if (!info) return 0;
  if (Number.isFinite(info.memUsedKB)) return info.memUsedKB;
  return Math.max(0, (info.memTotalKB || 0) - (info.memAvailableKB || 0));
}
