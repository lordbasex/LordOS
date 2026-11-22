// meta.js — what the app keeps about a machine that lord-vm doesn't need: the system it runs (for the card's mark and
// name), notes, whether its guest tools open files (opens: Files' "Open with", openwith.js), what closing its window does (onClose), whether it opens in apps mode (appsMode) and whether it keeps this computer awake (keepAwake, read by
// lord.vm.awake too). <machine>/app.json, written by the app as the person; machine.json stays lord-vm's alone.
import { fs } from '@lordos/sdk';

const cache = new Map();         // dir → { mtime, meta }: the library reads it every 1.5 s

export function readMeta(dir) {
  let st = null;
  try { st = fs.stat(`${dir}/app.json`); } catch (e) { return {}; }
  const hit = cache.get(dir);
  if (hit && hit.mtime === st?.mtime) return hit.meta;
  let meta = {};
  try {
    const j = JSON.parse(fs.read(`${dir}/app.json`));
    if (j && typeof j === 'object') {
      if (typeof j.os === 'string' && /^[a-z]{1,16}$/.test(j.os)) meta.os = j.os;
      if (typeof j.osName === 'string') meta.osName = j.osName.slice(0, 40);
      if (typeof j.notes === 'string') meta.notes = j.notes.slice(0, 4000);
      if (['ask', 'suspend', 'shutdown', 'keep'].includes(j.onClose)) meta.onClose = j.onClose;
      if (typeof j.keepAwake === 'boolean') meta.keepAwake = j.keepAwake;
      if (typeof j.appsMode === 'boolean') meta.appsMode = j.appsMode;      // apps mode (7.2): the machine's window opens in it
      // the profile it was made with: LordOS's ('productivity'…) or the person's own ('u-…', profiles.js)
      if (typeof j.profile === 'string' && /^[a-z][a-z0-9-]{0,31}$/.test(j.profile)) meta.profile = j.profile;
      // keyboard and mouse from its profile (profiles.js): the pointer captured when the window is in front (and on a click
      // in its picture) or automatic, LordOS's shortcuts asked for or never taken. Capturing in full screen is machine.json's
      // capture_fullscreen (Configure, lord.vm.set), never kept here
      if (j.input && typeof j.input === 'object') {
        meta.input = { mouse: j.input.mouse === 'capture' ? 'capture' : 'auto', shortcuts: j.input.shortcuts === 'never' ? 'never' : 'ask' };
      }
      if (typeof j.cloneOf === 'string' && /^[0-9a-f-]{36}$/.test(j.cloneOf)) meta.cloneOf = j.cloneOf;
      if (typeof j.importedFrom === 'string') meta.importedFrom = j.importedFrom.slice(0, 1024);
      if (typeof j.transferSetup === 'boolean') meta.transferSetup = j.transferSetup;   // set up the transfer folder once (Windows)
      // the LordOS tools brought up to date by themselves (Configure → Agent); absent: Machines settings' default
      if (typeof j.toolsAuto === 'boolean') meta.toolsAuto = j.toolsAuto;
      // its guest tools open files sent from Files' "Open with" (7.3): 'windows' (the system they said), or false
      if (j.opens === 'windows' || j.opens === 'other' || j.opens === false) meta.opens = j.opens;
    }
  } catch (e) { meta = {}; }
  cache.set(dir, { mtime: st?.mtime, meta });
  return meta;
}

export function writeMeta(dir, patch) {
  const next = { ...readMeta(dir), ...patch };
  try { fs.write(`${dir}/app.json`, JSON.stringify(next, null, 2)); cache.delete(dir); return true; } catch (e) { return false; }
}
