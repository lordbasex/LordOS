// meta.js — what the app keeps about a machine that lord-vm doesn't need: the system it runs (for the card's mark and
// name), notes, whether its guest tools open files (opens: Files' "Open with", openwith.js), what closing its window does (onClose), whether it opens in apps mode (appsMode) and whether it keeps this computer awake (keepAwake, read by
// lord.vm.awake too). <machine>/app.json, written by the app as the person; machine.json stays lord-vm's alone.
import { fs } from '@lordos/sdk';
import { cpuWindows, machineProtectedOf, tagsOf } from './model.js';

const cache = new Map();         // dir → { mtime, meta }: the library reads it every 1.5 s

// Is the machine in dir a protected one (model.js machineProtectedOf: the anonymity profile)? entry: its lord.vm.list()
// entry when the caller has it (read here otherwise); homeProtected: its own window is protected already. The only way
// Machines asks, so its window, its apps-mode windows and its card can't disagree.
export function isProtectedMachine(dir, entry = null, homeProtected = false) {
  let e = entry;
  if (!e) { try { e = typeof lord !== 'undefined' && lord.vm ? lord.vm.list().find((x) => x.dir === dir) || null : null; } catch (err) { e = null; } }
  return machineProtectedOf(e, readMeta(dir), homeProtected);
}

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
      // the snapshot "Fresh install" still to take when its unattended install ends (assistant.jsx, vmwindow.jsx)
      if (typeof j.freshSnap === 'boolean') meta.freshSnap = j.freshSnap;
      // Configure → General: the card's own color (a key of systems.js COLOR_HEX, its mark then the name's first letter) and its tags
      if (typeof j.color === 'string' && /^[a-z]{3,8}$/.test(j.color)) meta.color = j.color;
      if (Array.isArray(j.tags)) meta.tags = tagsOf(j.tags);
      // the LordOS tools brought up to date by themselves (Configure → Agent); absent: Machines settings' default
      if (typeof j.toolsAuto === 'boolean') meta.toolsAuto = j.toolsAuto;
      // its guest tools open files sent from Files' "Open with" (7.3): 'windows' (the system they said), or false
      if (j.opens === 'windows' || j.opens === 'other' || j.opens === false) meta.opens = j.opens;
      // the vCPUs chosen from the recommendation while it ran ("Use N", Configure → CPU & memory): set by startPrep
      // (ui.jsx) with lord-vm --set before its next start
      if (Number.isInteger(j.nextVcpus) && j.nextVcpus >= 1 && j.nextVcpus <= 64) meta.nextVcpus = j.nextVcpus;
    }
  } catch (e) { meta = {}; }
  cache.set(dir, { mtime: st?.mtime, meta });
  return meta;
}

export function writeMeta(dir, patch) {
  const next = { ...readMeta(dir), ...patch };
  try { fs.write(`${dir}/app.json`, JSON.stringify(next, null, 2)); cache.delete(dir); return true; } catch (e) { return false; }
}

// what its vCPUs used (<machine>/cpu-usage.json, written by lord-vm while it runs: model.js cpuWindows), read again only
// when it changed
const cpuCache = new Map();      // dir → { mtime, windows }
export function readCpuUse(dir) {
  let st = null;
  try { st = fs.stat(`${dir}/cpu-usage.json`); } catch (e) { return []; }
  if (!st) return [];
  const hit = cpuCache.get(dir);
  if (hit && hit.mtime === st.mtime) return hit.windows;
  let windows = [];
  try { windows = cpuWindows(fs.read(`${dir}/cpu-usage.json`)); } catch (e) { windows = []; }
  cpuCache.set(dir, { mtime: st.mtime, windows });
  return windows;
}
