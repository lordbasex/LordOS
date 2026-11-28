// diskmodel.js — reading install-disks' answer (server/src/install/install.c): one LOG line of JSON per disk.
// Pure logic (node tests/installer_disk_model_test.mjs).
//
// Before #280 a line that didn't parse was skipped as "plain text", so a healthy disk whose description the helper
// had cut (a GPT disk with seven partitions didn't fit its 1200-byte lines) vanished and the installer said there
// was no disk at all. Now a line that looks like a disk (it starts with "{") and doesn't parse, or parses into
// something that isn't a disk, is counted, and the Disk step says some disk couldn't be read.

const isNum = (v) => typeof v === 'number' && Number.isFinite(v) && v >= 0;
const isStr = (v) => typeof v === 'string';

// The fields the Disk step reads, with the shape it expects; anything else makes the line unreadable.
export function validDisk(d) {
  if (!d || typeof d !== 'object' || Array.isArray(d)) return false;
  if (!isStr(d.name) || !d.name || !isStr(d.path) || !isNum(d.size)) return false;
  if (!isStr(d.model) || !isStr(d.vendor) || !isStr(d.transport)) return false;
  if (typeof d.usable !== 'boolean' || typeof d.removable !== 'boolean' || !Array.isArray(d.parts)) return false;
  return d.parts.every((p) => p && typeof p === 'object' && isStr(p.name) && isStr(p.fstype) && isStr(p.label) && isNum(p.size));
}

// A collector for admin.run's onLog: add(line) for every line, then result() → { disks, unreadable }.
export function diskCollector() {
  const disks = [];
  let unreadable = 0;
  return {
    add(line) {
      const s = String(line);
      if (!s.trimStart().startsWith('{')) return;           // not a disk's line
      let d;
      try { d = JSON.parse(s); } catch (e) { unreadable++; return; }
      if (validDisk(d)) disks.push(d); else unreadable++;
    },
    result: () => ({ disks, unreadable }),
  };
}
