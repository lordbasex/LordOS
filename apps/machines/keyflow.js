// keyflow.js — asking lord-server for an encrypted machine's key before a start, a copy or an export (docs/VIRTUALIZATION.md
// 8; the steps are storagemodel.js keyStep). A key never comes to JS: only its state. When the session keyring is locked
// the system's own unlock prompt opens; when the keyring has no key for the machine (one brought from another computer)
// the recovery key's window opens (vmkey.jsx RecoveryKeyWindow, a protected window).
import { wm, askKeyringUnlock, t as tr } from '@lordos/sdk';
import { keyStep } from './storagemodel.js';

const has = () => typeof lord !== 'undefined' && lord.vm && typeof lord.vm.keyState === 'function';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// the recovery-key windows waiting for an answer, by keyring id
const asking = new Map();
export function askRecoveryKey(id, name) {
  return new Promise((resolve) => {
    const prev = asking.get(id);
    if (prev) prev(false);
    asking.set(id, resolve);
    wm.open('machines', { instance: `key-${id}`, protected: true, args: { recoveryKey: id, name, nonce: Date.now() } });
  });
}
export function answerRecoveryKey(id, ok) {
  const r = asking.get(id);
  asking.delete(id);
  if (r) r(ok);
}

// Resolves once the key of target (a machine's or group's folder, or a keyring id) is held by lord-server, or at once for
// one that isn't encrypted; rejects with the translated reason. id and name: for the recovery key's window
export async function ensureKey(target, { id = '', name = '' } = {}) {
  if (!has() || !target) return;
  for (let tries = 0; tries < 600; tries++) {
    const step = keyStep(lord.vm.keyState(target, true));
    if (step === 'go') return;
    if (step === 'wait') { await sleep(100); continue; }
    if (step === 'unlock') {
      if (!(await askKeyringUnlock())) throw new Error(tr('machines.key.locked'));
      continue;
    }
    if (step === 'recovery' && id) {
      if (!(await askRecoveryKey(id, name))) throw new Error(tr('machines.key.missing', { name }));
      continue;
    }
    throw new Error(step === 'recovery' ? tr('machines.key.missing', { name }) : tr('machines.key.failed'));
  }
  throw new Error(tr('machines.key.failed'));
}

// a new key made and kept for something about to be made (a machine, a group): its id, once the keyring has it
export async function newKey(name) {
  if (!has()) throw new Error(tr('machines.key.failed'));
  for (let round = 0; round < 3; round++) {
    const id = lord.vm.keyNew(name);
    if (!id) throw new Error(tr('machines.key.failed'));
    let step = 'wait';
    for (let tries = 0; tries < 300 && step === 'wait'; tries++) {
      step = keyStep(lord.vm.keyState(id));
      if (step === 'wait') await sleep(100);
    }
    if (step === 'go') return id;
    if (step !== 'unlock') break;
    // a locked keyring: unlocked by the person, then a new id is tried (the one asked for didn't get its key)
    if (!(await askKeyringUnlock())) throw new Error(tr('machines.key.locked'));
  }
  throw new Error(tr('machines.key.failed'));
}

// an encrypted machine's Clone gets the same key under its own new id: that id, once kept (null when not encrypted)
export async function cloneKey(m) {
  if (!has() || !m?.encrypted) return null;
  await ensureKey(m.dir, { id: m.keyId, name: m.name });
  const id = lord.vm.keyCopy(m.dir);
  if (!id) throw new Error(tr('machines.key.failed'));
  await ensureKey(id);
  return id;
}
