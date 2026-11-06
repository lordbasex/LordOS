// meta.js — what the app keeps about a machine that lord-vm doesn't need: the system it runs (for the card's mark and
// name), notes, what closing its window does (onClose) and whether it keeps this computer awake (keepAwake, read by
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
      if (typeof j.profile === 'string' && /^[a-z]{1,16}$/.test(j.profile)) meta.profile = j.profile;
      if (typeof j.cloneOf === 'string' && /^[0-9a-f-]{36}$/.test(j.cloneOf)) meta.cloneOf = j.cloneOf;
    }
  } catch (e) { meta = {}; }
  cache.set(dir, { mtime: st?.mtime, meta });
  return meta;
}

export function writeMeta(dir, patch) {
  const next = { ...readMeta(dir), ...patch };
  try { fs.write(`${dir}/app.json`, JSON.stringify(next, null, 2)); cache.delete(dir); return true; } catch (e) { return false; }
}
