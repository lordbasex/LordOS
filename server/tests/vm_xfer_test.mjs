// vm_xfer_test.mjs — apps/machines/xfer.js (the machine window's file copy panel) and apps/files/quarantine.js (Files
// never opens a runnable file from a machine by itself) (docs/VIRTUALIZATION.md 6.21). node server/tests/vm_xfer_test.mjs
import { xferStep, xferShown, xferBusy, xferNextExpiry, xferFigures, xferRunnable, runnableName, XFER_KEEP_MS } from '../../apps/machines/xfer.js';
import { quarantineBlocks } from '../../apps/files/quarantine.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}

// a drop and a file from the guest, one list
let rows = xferStep([], { drops: [{ id: 3, name: 'a.iso', size: 1000, pct: 10, state: 'copying', via: 'transfer' }] }, 1000);
rows = xferStep(rows, { recvs: [{ id: 7, name: 'b.bin', size: 4000, got: 0, state: 'receiving', folder: '/l/Received/M' }] }, 1100);
eq('two rows, in order', rows.map((r) => [r.key, r.dir, r.state]), [['in:3', 'in', 'running'], ['out:7', 'out', 'running']]);
eq('busy', xferBusy(rows), true);
// speed and time left from what came over the last seconds
rows = xferStep(rows, { recvs: [{ id: 7, name: 'b.bin', size: 4000, got: 1000, state: 'receiving' }] }, 2100);
const r7 = rows.find((r) => r.key === 'out:7');
eq('speed 1000 B/s, 3 s left', [Math.round(r7.speed), r7.left], [1000, 3]);
eq('figures', xferFigures(r7, (n) => `${n}B`), { done: '1000B', size: '4000B', speed: '1000B', left: { s: 3 } });
eq('minutes past 90 s', xferFigures({ ...r7, left: 200 }, String).left, { min: 4 });
// a drop that a partial update doesn't mention stays as it was
eq('drop kept', rows.find((r) => r.key === 'in:3').done, 100);
// finished: shown XFER_KEEP_MS more, then gone; no Cancel any more
rows = xferStep(rows, { recvs: [{ id: 7, name: 'b.bin', size: 4000, got: 4000, state: 'done', path: '/l/Received/M/b.bin', exec: true }] }, 3000);
const d7 = rows.find((r) => r.key === 'out:7');
eq('done row', [d7.state, d7.path, d7.speed, d7.left, d7.until], ['done', '/l/Received/M/b.bin', 0, -1, 3000 + XFER_KEEP_MS]);
eq('next expiry', xferNextExpiry(rows, 3000), XFER_KEEP_MS);
eq('gone after its time', xferShown(xferStep(rows, {}, 3001 + XFER_KEEP_MS), 3001 + XFER_KEEP_MS).map((r) => r.key), ['in:3']);
// failures and pauses say why
rows = xferStep([], { recvs: [{ id: 1, name: 'x', size: 10, got: 3, state: 'failed', code: 'badHash' }, { id: 2, name: 'y', size: 10, got: 3, state: 'paused' }] }, 0);
eq('why', rows.map((r) => [r.state, r.why]), [['failed', 'badHash'], ['paused', 'later']]);
rows = xferStep([], { drops: [{ id: 9, name: 'z', size: 5, pct: 40, state: 'failed', message: 'cancelled' }] }, 0);
eq('a cancelled drop', rows[0].why, 'cancelled');
// untrusted numbers stay in range
rows = xferStep([], { recvs: [{ id: 4, name: 'n', size: -5, got: 1e18, state: 'weird' }] }, 0);
eq('clamped', [rows[0].size, rows[0].done, rows[0].state], [0, 0, 'failed']);
eq('runnable names', ['a.sh', 'Setup.EXE', 'x.AppImage', 'notes.txt', 'p.desktop', 'photo.jpg'].map(runnableName), [true, true, true, false, true, false]);
eq('runnable by the guest\'s mark', xferRunnable({ name: 'data', exec: true }), true);

// Files: a file from a machine that would run or install something is never opened by a double click
const q = { quarantine: true };
eq('installers and programs blocked', ['a.deb', 'b.AppImage', 'c.desktop', 'd.exe', 'e.msi', 'f.flatpakref', 'g.lpk', 'h.run'].map((n) => quarantineBlocks({ name: n }, q)), [true, true, true, true, true, true, true, true]);
eq('documents open', ['a.pdf', 'b.txt', 'c.png', 'd.sh'].map((n) => quarantineBlocks({ name: n }, q)), [false, false, false, false]);
eq('unmarked files open', quarantineBlocks({ name: 'a.deb' }, {}), false);
eq('folders open', quarantineBlocks({ name: 'x.deb', dir: true }, q), false);
eq('no stat', quarantineBlocks({ name: 'x.deb' }, null), false);

console.log(`vm-xfer-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
