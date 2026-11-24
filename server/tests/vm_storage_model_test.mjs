// Machines' storage choices, the app's pure side (apps/machines/storagemodel.js; docs/VIRTUALIZATION.md 6.2, 6.4, 8).
// node server/tests/vm_storage_model_test.mjs
import assert from 'node:assert/strict';
import { keyStep, normalizeRecoveryKey, formatRecoveryKey, disposableOf, disposableAllowed, disposableCapMb, exportRefusal, exportName,
  exportTarget, jobProgress, jobError, encryptionNote, storageBadges } from '../../apps/machines/storagemodel.js';

let n = 0;
const t = (name, f) => { f(); n++; };

t('keyStep', () => {
  assert.equal(keyStep('none'), 'go');
  assert.equal(keyStep('ready'), 'go');
  assert.equal(keyStep('pending'), 'wait');
  assert.equal(keyStep('locked'), 'unlock');
  assert.equal(keyStep('missing'), 'recovery');
  for (const x of ['error', '', undefined, null, 'READY', 'ready ']) assert.equal(keyStep(x), 'fail');
});

t('recovery key', () => {
  const k = '0123456789abcdef'.repeat(4);
  assert.equal(normalizeRecoveryKey(k), k);
  assert.equal(normalizeRecoveryKey(k.toUpperCase()), k);
  assert.equal(normalizeRecoveryKey(formatRecoveryKey(k)), k);
  assert.equal(formatRecoveryKey(k).split('-').length, 8);
  assert.equal(normalizeRecoveryKey(` ${k.slice(0, 32)}\n${k.slice(32)} `), k);
  for (const bad of ['', k.slice(1), `${k}0`, `${k.slice(1)}g`, null, undefined, 42, `${k.slice(0, 63)}\u0000`]) assert.equal(normalizeRecoveryKey(bad), '');
  assert.equal(formatRecoveryKey('nope'), '');
});

t('disposable', () => {
  assert.equal(disposableOf({ disposable: 'memory' }), 'memory');
  assert.equal(disposableOf({ disposable: 'disk' }), 'disk');
  assert.equal(disposableOf({ disposable: 'weird' }), 'off');
  assert.equal(disposableOf(null), 'off');
  assert.equal(disposableAllowed({ dir: '/x' }), true);
  assert.equal(disposableAllowed({ dir: '/x', group: { id: 'g' } }), false);
  assert.equal(disposableCapMb({ memoryMb: 512 }), 1024);
  assert.equal(disposableCapMb({ memoryMb: 8192 }), 8192);
});

t('export', () => {
  const m = { name: 'Debian 13', encrypted: false };
  assert.equal(exportRefusal(m, 'lordvm'), '');
  assert.equal(exportRefusal(m, 'ova'), '');
  assert.equal(exportRefusal(m, 'lordvm', { running: true }), 'running');
  assert.equal(exportRefusal({ ...m, group: {} }, 'lordvm'), 'member');
  assert.equal(exportRefusal({ ...m, suspended: true }, 'lordvm'), 'suspended');
  assert.equal(exportRefusal({ ...m, encrypted: true }, 'ova'), 'encryptedOva');
  assert.equal(exportRefusal({ ...m, encrypted: true }, 'lordvm'), '');
  assert.equal(exportRefusal(null, 'lordvm'), 'gone');
  assert.equal(exportName(m, 'lordvm'), 'Debian 13.lordvm');
  assert.equal(exportName(m, 'ova'), 'Debian 13.ova');
  assert.equal(exportName({ name: '../../etc/x:y' }, 'ova'), 'etc-x-y.ova');
  assert.equal(exportName({ name: '...' }, 'ova'), 'Machine.ova');
  assert.equal(exportTarget('/home/a/Debian', 'lordvm'), '/home/a/Debian.lordvm');
  assert.equal(exportTarget('/home/a/Debian.LORDVM', 'lordvm'), '/home/a/Debian.LORDVM');
  assert.equal(exportTarget('/home/a/x.ova', 'ova'), '/home/a/x.ova');
  assert.equal(exportTarget('relative', 'ova'), '');
  assert.equal(exportTarget(null, 'ova'), '');
});

t('job lines', () => {
  const out = '{"progress":12.5}\nnoise\n{"progress":95}\n{"exported":true,"to":"/h/x.lordvm","bytes":10}\n';
  const g = jobProgress(out);
  assert.equal(g.pct, 95);
  assert.equal(g.last.to, '/h/x.lordvm');
  assert.deepEqual(jobProgress('{bad json\n'), { pct: null, last: null });
  assert.equal(jobError('x\nlord-vm: first\nlord-vm: the key doesn\'t open this machine\'s disks\n'), 'the key doesn\'t open this machine\'s disks');
  assert.equal(jobError(''), '');
});

t('notes and badges', () => {
  assert.equal(encryptionNote({ encrypted: true }), 'encrypted');
  assert.equal(encryptionNote({ diskFormat: 'raw' }), 'rawPlain');
  assert.equal(encryptionNote({}), 'plain');
  assert.deepEqual(storageBadges({ encrypted: true, disposable: 'memory', diskFormat: 'qcow2' }), ['encrypted', 'disposable']);
  assert.deepEqual(storageBadges({ diskFormat: 'raw', disposable: 'off' }), ['raw']);
});

console.log(`vm_storage_model: ${n} groups passed`);
