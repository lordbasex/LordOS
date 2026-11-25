// nv_report_test.mjs — Monitor → GPU's "Couldn't read NVIDIA data" and its report (apps/monitor/nvreport.js): which reason
// each lord-nvgpu state shows, when nothing is shown, the report's lines, and that it never carries the person's data
// (home paths, host or user name, pids, hostile strings in the fields). Run: node tests/nv_report_test.mjs
import { nvFailure, nvReportText, NV_STATES } from '../../apps/monitor/nvreport.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

let ok = 0, bad = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) ok++;
  else { bad++; console.log(`FAIL ${name}\n  got  ${g}\n  want ${w}`); }
}
const yes = (name, c) => eq(name, !!c, true);

const pci = [{ slot: '0000:01:00.0', id: '10de:2684', class: '030000', driver: 'nvidia', name: 'AD102 [GeForce RTX 4090]' }];
const diag = (state, more = {}) => ({ state, detail: null, denied: 0, syscalls: [], ioctls: [], driverVersion: '550.54.14', pci, ...more });

// ---- the reason ----
eq('no diag', nvFailure(null), null);
eq('ok', nvFailure(diag('ok'), true), null);
eq('ok, no sample yet', nvFailure(diag('ok'), false), null);
eq('starting', nvFailure(diag('starting')), null);
eq('ok with refusals', nvFailure(diag('ok', { denied: 3 }), true), { state: 'ok', key: 'monitor.gpu.nv.blocked', params: {}, partial: true });
eq('data wins over a stale failure', nvFailure(diag('timeout'), true), null);
eq('nouveau', nvFailure(diag('noDriver', { pci: [{ ...pci[0], driver: 'nouveau' }] })).key, 'monitor.gpu.nv.r.otherDriver');
eq('nouveau param', nvFailure(diag('noDriver', { pci: [{ ...pci[0], driver: 'nouveau' }] })).params, { driver: 'nouveau' });
eq('no driver', nvFailure(diag('noDriver', { pci: [{ ...pci[0], driver: null }] })).key, 'monitor.gpu.nv.r.noDriver');
eq('hostile driver name', nvFailure(diag('noDriver', { pci: [{ ...pci[0], driver: '<b>x</b>' }] })).key, 'monitor.gpu.nv.r.noDriver');
eq('lib', nvFailure(diag('libMissing')).key, 'monitor.gpu.nv.r.libMissing');
eq('dlopen', nvFailure(diag('dlopen')).key, 'monitor.gpu.nv.r.libBroken');
eq('symbol', nvFailure(diag('symbol', { detail: 1 })).key, 'monitor.gpu.nv.r.libBroken');
eq('init 9', nvFailure(diag('init', { detail: 9 })).key, 'monitor.gpu.nv.r.driverNotLoaded');
eq('init 18', nvFailure(diag('init', { detail: 18 })).key, 'monitor.gpu.nv.r.mismatch');
eq('count 4', nvFailure(diag('count', { detail: 4 })).key, 'monitor.gpu.nv.r.noPermission');
eq('init 999', nvFailure(diag('init', { detail: 999 })), { state: 'init', key: 'monitor.gpu.nv.r.init', params: { code: '999' }, partial: false });
eq('no device', nvFailure(diag('noDevice')).key, 'monitor.gpu.nv.r.noDevice');
eq('sandbox', nvFailure(diag('sandbox', { detail: 70 })).key, 'monitor.gpu.nv.r.sandbox');
eq('killed', nvFailure(diag('killed', { detail: 31 })).key, 'monitor.gpu.nv.r.killed');
for (const s of ['crash', 'exit', 'badMessage']) eq(s, nvFailure(diag(s)).key, 'monitor.gpu.nv.r.crash');
eq('timeout', nvFailure(diag('timeout')).key, 'monitor.gpu.nv.r.timeout');
for (const s of ['noHelper', 'spawn']) eq(s, nvFailure(diag(s)).key, 'monitor.gpu.nv.r.noStart');
eq('unknown state', nvFailure(diag('rm -rf')).key, 'monitor.gpu.nv.r.unknown');

// every key the model can return is in every language (the i18n check covers t() calls, not keys built here)
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '../..');
const keys = new Set(['monitor.gpu.nv.title', 'monitor.gpu.nv.hint', 'monitor.gpu.nv.copy', 'monitor.gpu.nv.copied', 'monitor.gpu.nv.blocked']);
for (const s of [...NV_STATES, 'x']) for (const det of [null, 4, 9, 18, 1]) {
  const f = nvFailure(diag(s, { detail: det }));
  if (f) keys.add(f.key);
}
keys.add(nvFailure(diag('noDriver', { pci: [{ ...pci[0], driver: 'nouveau' }] })).key);
const langs = fs.readdirSync(path.join(root, 'assets/locales')).filter((d) => fs.existsSync(path.join(root, 'assets/locales', d, 'monitor.json')));
for (const l of langs) {
  const cat = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales', l, 'monitor.json'), 'utf8'));
  for (const k of keys) yes(`${l}: ${k}`, typeof cat[k.replace(/^monitor\./, '')] === 'string');
}
yes('35 languages', langs.length >= 35);

// ---- the report ----
const who = { home: '/home/secretuser', user: 'secretuser', host: 'secrethost' };
const text = nvReportText({
  diag: diag('init', { detail: 18, denied: 5, syscalls: [41, 16, 99999, -1, 'x'], ioctls: [0xc0104629, 0x5401, -3, 2 ** 40] }),
  version: '0.8.9.6.1-beta', kernel: '6.12.118+deb13-amd64', arch: 'x86_64',
}, who);
for (const line of [
  'LordOS: 0.8.9.6.1-beta',
  'Kernel: 6.12.118+deb13-amd64 (x86_64)',
  'GPU: 0000:01:00.0 [10de:2684] AD102 [GeForce RTX 4090] (class 030000), driver: nvidia',
  'NVIDIA driver (/sys/module/nvidia/version): 550.54.14',
  'Helper state: init (nvmlReturn 18 NVML_ERROR_LIB_RM_VERSION_MISMATCH) - nvmlInit failed',
  'Sandbox refusals: 5',
  'Refused syscalls (numbers for the arch above): 41, 16',
  'Refused ioctl requests: 0xc0104629 (type 0x46, nr 0x29), 0x00005401 (type 0x54, nr 0x01)',
  'Please open an issue and paste this report: https://github.com/lordbasex/LordOS/issues/new?template=gpu-report.yml',
]) yes(`report has: ${line}`, text.split('\n').includes(line));
yes('killed', nvReportText({ diag: diag('killed', { detail: 31 }) }).includes('Helper state: killed (signal 31 SIGSYS) - killed by the sandbox (SIGSYS)'));
yes('symbol', nvReportText({ diag: diag('symbol', { detail: 1 }) }).includes('Helper state: symbol (nvmlInit_v2)'));
yes('no driver version', nvReportText({ diag: diag('noDriver', { driverVersion: null }) }).includes('NVIDIA driver (/sys/module/nvidia/version): not readable'));
yes('no card', nvReportText({ diag: diag('noDriver', { pci: [] }) }).includes('GPU: none found in sysfs'));
yes('nothing at all', nvReportText({}).includes('Helper state: unknown'));

// no private data: hostile values in every field are dropped, and the scrub catches the person's names
const evil = nvReportText({
  diag: {
    state: 'crash', detail: 11, denied: 1, syscalls: [1], ioctls: [],
    driverVersion: '550 /home/secretuser/x',
    pci: [{ slot: '/home/secretuser', id: 'secrethost', class: 'pid 4242', driver: 'nvidia\nHost: secrethost', name: 'card of secretuser on secrethost at /home/secretuser/.cache' }],
    pid: 4242, home: '/home/secretuser', hostname: 'secrethost', user: 'secretuser',
  },
  version: '0.8.9.6.1 /home/secretuser', kernel: 'secrethost 6.12', arch: 'x86_64; pid=4242',
}, who);
for (const leak of ['secretuser', 'secrethost', '/home/', '4242', '.cache']) eq(`no ${leak}`, evil.includes(leak), false);
yes('hostile card listed without its fields', evil.includes('GPU: ? [10de:?], driver: none'));
yes('version dropped', evil.includes('LordOS: unknown'));
yes('kernel dropped', evil.includes('Kernel: unknown\n'));
eq('only expected lines', evil.split('\n').every((l) => l === '' || /^(LordOS|Kernel|GPU|NVIDIA driver|Helper state|Sandbox refusals|Refused syscalls|Refused ioctl|Please open)/.test(l)), true);
// ASCII-only (no control characters but the newlines)
eq('printable', /^[\x20-\x7e\n]*$/.test(text) && /^[\x20-\x7e\n]*$/.test(evil), true);

console.log(`nv-report-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
