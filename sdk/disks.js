// disks.js — disks and partitions (Disks app; see docs/SYSTEM.md, "Disks").
//
//   const [list, reload] = useDisks()       // updates itself on mount, unmount or plug-in
//   disks.list()  disks.tools()             // lord.disks, read-only and without root
//
// Anything that changes something goes through the helper with the closed disk-* actions
// (admin.run): disk-smart, disk-mount, disk-unmount, disk-eject, disk-format, disk-label,
// disk-erase, disk-write-image and disk-cancel. Change notifications (mountinfo and udev, no
// polling) are only on while someone uses useDisks().
import { useEffect, useState } from 'react';
import { wm } from './windows.js';
import { stat } from './fs.js';

const listeners = new Set();
let timer = null;

globalThis.__lordDisks = () => {
  // udev events come in bursts: coalesce them into one
  if (timer) clearTimeout(timer);
  timer = setTimeout(() => {
    timer = null;
    for (const fn of listeners) { try { fn(); } catch (e) { console.log(`disks: ${e}`); } }
  }, 150);
};

const api = () => (typeof lord !== 'undefined' && lord.disks ? lord.disks : null);

export const disks = {
  available: () => !!api(),
  list: () => (api() ? api().list() || [] : []),
  tools: () => (api() ? api().tools() || {} : {}),
  watch(fn) {
    listeners.add(fn);
    if (listeners.size === 1) api()?.watch(true);
    return () => {
      listeners.delete(fn);
      if (!listeners.size) api()?.watch(false);
    };
  },
};

// The formats that come with the Store's Disk Formats item (docs/FILES.md); lord-server asks the same in disk-mount.
export const COMMERCIAL_FORMATS = ['vfat', 'exfat', 'ntfs', 'hfsplus', 'apfs'];
const FORMATS_MARK = '/usr/lib/lordos/disk-formats.enabled';
export const diskFormatsOn = () => { try { stat(FORMATS_MARK); return true; } catch (e) { return false; } };
export const needsDiskFormats = (fstype) => COMMERCIAL_FORMATS.includes(fstype) && !diskFormatsOn();
export const openDiskFormats = () => wm.open('store', { args: { page: 'commercial', app: 'lordos-disk-formats' } });

export function useDisks() {
  const [list, setList] = useState(disks.list);
  useEffect(() => disks.watch(() => setList(disks.list())), []);
  return [list, () => setList(disks.list())];
}
