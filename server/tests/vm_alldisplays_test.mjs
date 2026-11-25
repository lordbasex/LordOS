// vm_alldisplays_test.mjs — apps/machines/alldisplays.js: All Displays, a machine across this computer's displays
// (docs/VIRTUALIZATION.md 5.4). node server/tests/vm_alldisplays_test.mjs
import { allDisplaysPlan, displayOrder, headSize, fitSizes, headArgs, headInstance, headWindowId, HEADS_MAX } from '../../apps/machines/alldisplays.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const D = (id, x, y, w, h, main = false) => ({ id, x, y, w, h, main });
const two = [D(1, 0, 0, 1920, 1080, true), D(2, 1920, 0, 2560, 1440)];
const m = (o = {}) => ({ id: 'a', display: 'window', gpu: '', heads: 2, ...o });

// the order: the window's display first, then left to right, top to bottom; at most four
eq('order: window display first', displayOrder(two, 2).map((d) => d.id), [2, 1]);
eq('order: main when the window is unknown', displayOrder(two, 99).map((d) => d.id), [1, 2]);
eq('order: left to right then top', displayOrder([D(1, 0, 0, 800, 600), D(3, 800, 600, 800, 600), D(2, 800, 0, 800, 600), D(4, -800, 0, 800, 600)], 1).map((d) => d.id), [1, 4, 2, 3]);
eq('order: at most four', displayOrder([1, 2, 3, 4, 5, 6].map((i) => D(i, i * 100, 0, 100, 100)), 1).length, HEADS_MAX);
eq('order: bad entries left out', displayOrder([null, { id: 9 }, D(1, 0, 0, 0, 10), D(2, 0, 0, 640, 480)], 1).map((d) => d.id), [2]);
eq('order: none', displayOrder([], 1), []);

// sizes: the display's own, within 320..8192, even
eq('size', headSize(D(1, 0, 0, 1921, 1081)), [1920, 1080]);
eq('size: clamped', [headSize(D(1, 0, 0, 100, 50)), headSize(D(1, 0, 0, 10000, 9000))], [[320, 320], [8192, 8192]]);
eq('fit: under 32767 together', fitSizes([[8192, 100], [8192, 100], [8192, 100], [8192, 100]]).length, 3);
eq('fit: all', fitSizes([[1920, 1080], [2560, 1440]]).length, 2);

// the plan
eq('plan: two displays, two outputs, running', allDisplaysPlan(m(), two, { first: 1 }), { ok: true, why: '', n: 2, order: two, sizes: [[1920, 1080], [2560, 1440]], restart: false });
eq('plan: needs more outputs while running → restart', (({ ok, why, n, restart }) => ({ ok, why, n, restart }))(allDisplaysPlan(m({ heads: 1 }), two, { first: 1 })), { ok: true, why: 'restart', n: 2, restart: true });
eq('plan: off, fewer outputs: set them and start', (({ ok, why, restart }) => ({ ok, why, restart }))(allDisplaysPlan(m({ heads: 1 }), two, { first: 1, running: false })), { ok: true, why: '', restart: true });
eq('plan: off, outputs there', (({ ok, why }) => ({ ok, why }))(allDisplaysPlan(m(), two, { first: 1, running: false })), { ok: false, why: 'notRunning' });
eq('plan: one display', allDisplaysPlan(m(), [two[0]], {}).why, 'oneDisplay');
eq('plan: a vGPU', allDisplaysPlan(m({ gpu: 'intel-shared' }), two, {}).why, 'vgpu');
eq('plan: no window', allDisplaysPlan(m({ display: 'none' }), two, {}).why, 'noWindow');
eq('plan: protected', allDisplaysPlan(m(), two, { prot: true }).why, 'anonymity');
eq('plan: anonymity profile', allDisplaysPlan(m({ profileLock: 'anonymity' }), two, {}).why, 'anonymity');
eq('plan: a Windows guest', allDisplaysPlan(m(), two, { family: 'windows' }).why, 'windows');
eq('plan: apps mode', allDisplaysPlan(m(), two, { appsOn: true }).why, 'apps');
eq('plan: no machine', allDisplaysPlan(null, two, {}).why, 'noMachine');
eq('plan: four displays, two outputs: restart for four', (({ n, restart }) => ({ n, restart }))(allDisplaysPlan(m(), [1, 2, 3, 4].map((i) => D(i, i * 1000, 0, 1000, 800)), { first: 1 })), { n: 4, restart: true });
// every refusal says why with a key that exists in machines.alld.why.*
const whys = ['noMachine', 'noWindow', 'vgpu', 'anonymity', 'windows', 'apps', 'oneDisplay', 'notRunning', 'restart'];
const { readFileSync } = await import('node:fs');
const en = JSON.parse(readFileSync(new URL('../../assets/locales/en/machines.json', import.meta.url)));
eq('every why has its text', whys.filter((w) => !en[`machines.alld.why.${w}`] && !en[`alld.why.${w}`]), []);

// a further output's window
eq('head args', headArgs({ dir: '/home/a/Machines/x.lordvm', k: 1 }), { dir: '/home/a/Machines/x.lordvm', k: 1 });
eq('head args: refused', [headArgs(null), headArgs({ dir: 'rel', k: 1 }), headArgs({ dir: '/x', k: 0 }), headArgs({ dir: '/x', k: 4 }), headArgs({ dir: '/x', k: '1.5' }), headArgs({ dir: '/x' })], [null, null, null, null, null, null]);
eq('head ids', [headInstance('abc', 2), headWindowId('abc', 2)], ['vmhead-abc-2', 'machines#vmhead-abc-2']);

console.log(`vm-alldisplays-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
