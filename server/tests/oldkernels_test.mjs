// Which kernels Cleaner may offer (sdk/oldkernels.js). Run: node server/tests/oldkernels_test.mjs
import assert from 'node:assert/strict';
import { oldKernels, kernelPlanOk } from '../../sdk/oldkernels.js';

const pkgs = (...n) => n.map((name) => ({ name, installedSize: 1000 }));
const names = (l) => l.map((p) => p.name);
const K = (v) => `linux-image-${v}-amd64`;

// the running kernel is the newest: it and the one before stay
const four = pkgs(K('6.12.48+deb13'), K('6.12.73+deb13'), K('6.12.111+deb13'), K('6.12.113+deb13'), 'linux-image-amd64');
assert.deepEqual(names(oldKernels(four, '6.12.113+deb13-amd64')), [K('6.12.48+deb13'), K('6.12.73+deb13')]);
// numbers compare as numbers, not text: 9 is older than 48
assert.deepEqual(names(oldKernels(pkgs(K('6.12.48+deb13'), K('6.12.9+deb13'), K('6.12.73+deb13')), '6.12.73+deb13-amd64')), [K('6.12.9+deb13')]);
// a newer kernel installed and not booted yet: the running one, its way back and the newest stay
assert.deepEqual(names(oldKernels(four, '6.12.111+deb13-amd64')), [K('6.12.48+deb13')]);
// only one or two kernels: nothing
assert.deepEqual(oldKernels(pkgs(K('6.12.113+deb13')), '6.12.113+deb13-amd64'), []);
assert.deepEqual(oldKernels(pkgs(K('6.12.111+deb13'), K('6.12.113+deb13')), '6.12.113+deb13-amd64'), []);
// an older numbering
assert.deepEqual(names(oldKernels(pkgs('linux-image-6.1.0-9-amd64', 'linux-image-6.1.0-18-amd64', 'linux-image-6.1.0-20-amd64'), '6.1.0-20-amd64')), ['linux-image-6.1.0-9-amd64']);
// other flavours and names are never touched
const pi = pkgs('linux-image-6.12.20+rpt-rpi-2712', 'linux-image-6.12.20+rpt-rpi-v8', 'linux-image-6.12.25+rpt-rpi-2712', 'linux-image-6.12.25+rpt-rpi-v8', 'linux-image-6.12.47+rpt-rpi-2712', 'linux-image-6.12.47+rpt-rpi-v8');
assert.deepEqual(names(oldKernels(pi, '6.12.47+rpt-rpi-2712')), ['linux-image-6.12.20+rpt-rpi-2712']);
assert.deepEqual(oldKernels(pkgs('linux-image-amd64', 'linux-image-generic', 'linux-image-lordos'), '6.12.113+deb13-amd64'), []);
// a running kernel that isn't a package here, or unknown: nothing to compare with, nothing offered
assert.deepEqual(oldKernels(four, '6.12.200-custom+'), []);
assert.deepEqual(oldKernels(four, '6.12.200+deb13-amd64'), []);
assert.deepEqual(oldKernels(four, ''), []);
assert.deepEqual(oldKernels(four, undefined), []);

const R = '6.12.113+deb13-amd64';
const rm = (...n) => ({ ok: true, items: n.map((name) => ({ action: 'remove', name })) });
assert.ok(kernelPlanOk(rm(K('6.12.48+deb13'), 'linux-headers-6.12.48+deb13-amd64'), R));
assert.ok(!kernelPlanOk(rm(K('6.12.48+deb13'), 'linux-image-amd64'), R));         // the meta package
assert.ok(!kernelPlanOk(rm(K('6.12.113+deb13')), R));                              // the running kernel
assert.ok(!kernelPlanOk(rm(K('6.12.48+deb13'), 'lordos'), R));
assert.ok(!kernelPlanOk({ ok: true, items: [{ action: 'upgrade', name: K('6.12.48+deb13') }] }, R));
assert.ok(!kernelPlanOk({ ok: false, items: [] }, R));
assert.ok(!kernelPlanOk({ ok: true, items: [] }, R));
assert.ok(!kernelPlanOk(null, R));
console.log('oldkernels-test: ok');
