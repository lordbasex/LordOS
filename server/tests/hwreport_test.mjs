// hwreport_test.mjs — community hardware reports (0.8.7, docs/BENCHMARK.md "0.8.7 — Community hardware reports"):
// the schema with good and bad reports, the report System Information builds (apps/sysinfo/share.js: only the
// schema's fields, the summary and the issue link, the page), the workflow's checks (tools/hwreport/lib.mjs:
// attachment links, the download's limits with a fake fetch, parsing limits, privacy, plausibility, labels, the
// comment), the catalogue and its snapshot (tools/hwreport/catalog.mjs) and the Compatibility panel's community
// data (apps/sysinfo/community.js). Run by tests/run_suites.sh (needs node).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { validate } from '../../tools/hwreport/jsonschema.mjs';
import { pick, buildShareReport, summaryRows, summaryText, issueTitle, issueUrl, shareFileNames, sharePage, reportKind, gpuVendor, cpuVendor, TITLE_MAX } from '../../apps/sysinfo/share.js';
import { deviceKeys, communityFor, communitySummary } from '../../apps/sysinfo/community.js';
import { compatibility } from '../../apps/sysinfo/model.js';
import { findAttachments, allowedUrl, fetchAttachment, parseLimited, privacyScan, plausibility, checkReport, labelsFor, commentBody, code, MARKER } from '../../tools/hwreport/lib.mjs';
import { readReports, buildSnapshot, snapshotText, buildModel, renderSections, SNAPSHOT_FILE, REPORTS_DIR } from '../../tools/hwreport/catalog.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const schema = JSON.parse(fs.readFileSync(path.join(ROOT, 'apps/sysinfo/hwreport.schema.json'), 'utf8'));
const readJson = (p) => JSON.parse(fs.readFileSync(path.join(ROOT, p), 'utf8'));
const clone = (x) => JSON.parse(JSON.stringify(x));

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};
const ok = (what, cond) => eq(what, !!cond, true);
const throws = async (what, f, re) => {
  n++;
  try { await f(); bad++; console.log(`✗ ${what}: did not throw`); } catch (e) { if (re && !re.test(e.message)) { bad++; console.log(`✗ ${what}: ${e.message}`); } }
};
const valid = (r) => validate(schema, r).length === 0;

const laptop = readJson('tools/hwreport/test/good-laptop-live.json');
const vm = readJson('web/content/hardware/reports/reference-vm116.json');
const pi = readJson('web/content/hardware/reports/reference-pi5-16gb.json');

// ---- the schema ----
ok('the laptop report is valid', valid(laptop));
ok('the VM 116 reference is valid', valid(vm));
ok('the Pi 5 reference is valid', valid(pi));
const variant = (f) => { const r = clone(laptop); f(r); return r; };
const badCases = {
  'a serial number field': (r) => { r.system.machine.serial = 'PF2ABCDE'; },
  'a host name field': (r) => { r.hostname = 'alice-laptop'; },
  'a MAC field on a network': (r) => { r.system.network[0].mac = '3c:22:fb:12:34:56'; },
  'a disk name (sda) is not a field': (r) => { r.system.storage[0].name = 'nvme0n1'; },
  'the render node path': (r) => { r.system.session.renderNode = '/dev/dri/renderD128'; },
  'the bench timestamp': (r) => { r.bench.at = 1795000000; },
  'wrong format': (r) => { r.format = 'something'; },
  'version 2': (r) => { r.version = 2; },
  'no system': (r) => { delete r.system; },
  'a control character': (r) => { r.system.cpu.model = 'Core\u0007'; },
  'a too long text': (r) => { r.system.cpu.model = 'x'.repeat(129); },
  'an unknown compatibility row': (r) => { r.compatibility.push({ id: 'camera', state: 'works' }); },
  'a bad state': (r) => { r.compatibility[0].state = 'great'; },
  'a string count': (r) => { r.system.cpu.threads = '12'; },
  'a fractional count': (r) => { r.system.cpu.threads = 1.5; },
  'a bad result name': (r) => { r.bench.results['../../x'] = { value: 1 }; },
  'a result without value': (r) => { r.bench.results['cpu.js.1'] = { unit: 'runs/s' }; },
  'a bad version': (r) => { r.lordos = '0.8.7; rm -rf /'; },
  'a bad date': (r) => { r.createdAt = 'yesterday'; },
  'too many PCI devices': (r) => { r.system.pci = Array.from({ length: 129 }, () => r.system.pci[0]); },
  'a kernel option that is not CONFIG_': (r) => { r.system.software.kernelConfig.HOME = 'y'; },
  'live as a string': (r) => { r.live = 'yes'; },
  'a device name as the boot medium': (r) => { r.system.software.bootMedium = 'sdb'; },
  'a mount point as the boot medium': (r) => { r.system.software.bootMedium = '/run/live/medium'; },
  'a boot link speed as text': (r) => { r.system.software.bootLinkMbps = '5000'; },
  'a zero boot link speed': (r) => { r.system.software.bootLinkMbps = 0; },
  'toram as a string': (r) => { r.system.software.bootToRam = 'yes'; },
};
for (const [name, f] of Object.entries(badCases)) ok(`invalid: ${name}`, !valid(variant(f)));
await throws('the validator refuses keywords it does not implement', () => validate({ type: 'string', format: 'email' }, 'x'), /unsupported/);
await throws('only local $refs', () => validate({ $ref: 'https://example.com/s.json' }, 1), /\$ref/);

// ---- the report System Information builds (share.js) ----
const raw = {
  schema: 1, date: '2026-10-28', hostname: 'alice-laptop',
  machine: { vendor: 'LENOVO', product: '21CB', chassis: 'laptop', firmware: 'uefi', secureBoot: false, virtualization: '', serial: 'PF2ABCDE' },
  cpu: { model: 'Intel Core i5', vendor: 'GenuineIntel', arch: 'x86_64', threads: 12, flags: 'sse4_2 avx', microcode: '0x430' },
  memory: { totalKB: 16073328, swapKB: 0, zram: [] },
  gpus: [{ card: 'card1', bus: 'pci', vendorId: '8086', deviceId: '46a8', vendor: 'Intel', device: 'Iris Xe', driver: 'i915', bootVga: true, renderNode: true, connectors: 4, connected: 1 }],
  network: [{ kind: 'wifi', driver: 'iwlwifi', up: true, speedMbps: null, mac: '3c:22:fb:12:34:56', ssid: 'HomeNet' }],
  storage: [{ name: 'nvme0n1', model: 'SSD', bytes: 512e9, rotational: false, removable: false, transport: 'nvme', link: '', serial: 'S64' }],
  software: { kernel: '6.12.57', lordos: '0.8.7-beta', landlockAbi: 6, bpfLsm: true, cgroup2: true, live: false, home: '/home/alice', bootMedium: 'internal', bootLinkMbps: null, bootToRam: false, bootDevice: 'nvme0n1p2' },
  session: { composition: 'gpu', gpuInit: 'on', renderer: 'Mesa Intel', renderNode: '/dev/dri/renderD128', dmabuf: true, scanouts: 1, fixture: false },
};
const bench = { lordbench: 1, mode: 'quick', at: 1, status: 'done', lordos: '0.8.7-beta', live: true, results: { 'cpu.js.1': { value: 50, unit: 'runs/s', extra: 1 } }, secret: 'x' };
const built = buildShareReport({ report: raw, bench, compat: compatibility(raw), schema, date: '2026-10-28' });
ok('the built report is valid', valid(built));
const text = JSON.stringify(built);
for (const s of ['alice', 'PF2ABCDE', '3c:22', 'HomeNet', 'renderD128', 'nvme0n1', 'S64', '0x430', 'sse4_2', '"at"', 'secret', 'fixture', '"extra"'])
  ok(`the built report leaves out ${s}`, !text.includes(s));
eq('format, version, source', [built.format, built.version, built.source, built.createdAt, built.lordos], ['lordos-hwreport', 1, 'community', '2026-10-28', '0.8.7-beta']);
eq('the bench keeps only listed fields', built.bench.results['cpu.js.1'], { value: 50, unit: 'runs/s' });
ok('live from LordBench\'s flag', built.live === true);
eq('a bench of another version is left out', buildShareReport({ report: raw, bench: { ...bench, lordos: '0.8.6-beta' }, compat: [], schema, date: '2026-10-28' }).bench, null);
ok('live from the report', buildShareReport({ report: { ...raw, software: { ...raw.software, live: true } }, compat: [], schema, date: '2026-10-28' }).live);
ok('not live', !buildShareReport({ report: raw, compat: [], schema, date: '2026-10-28' }).live);
eq('the boot medium is shared as a class', [built.system.software.bootMedium, built.system.software.bootLinkMbps, built.system.software.bootToRam, 'bootDevice' in built.system.software], ['internal', null, false, false]);
eq('pick drops a boot medium outside the list', pick({ bootMedium: 'mmcblk0', bootLinkMbps: 480 }, schema.$defs.system.properties.software, schema), { bootLinkMbps: 480 });
eq('pick cuts long text and drops a bad enum', pick({ chassis: 'spaceship', model: 'y'.repeat(200) }, schema.$defs.system.properties.machine, schema), { model: 'y'.repeat(128) });
eq('pick drops a value with control characters', pick({ model: 'a\u0001b' }, schema.$defs.system.properties.cpu, schema), {});
eq('pick caps arrays', pick(Array(40).fill({ kind: 'ethernet' }), schema.$defs.system.properties.network, schema).length, 32);
eq('kind, vendors', [reportKind(laptop), gpuVendor(laptop), cpuVendor(laptop)], ['laptop', 'intel', 'intel']);
eq('kind and vendors of the references', [reportKind(vm), gpuVendor(vm), cpuVendor(vm), reportKind(pi), gpuVendor(pi), cpuVendor(pi)], ['vm', 'virtual', 'intel', 'board', 'broadcom', 'arm']);
const rows = Object.fromEntries(summaryRows(laptop));
eq('summary rows', [rows.machine, rows.kind, rows.lordos, rows.live, rows.memory, rows.boot], ['LENOVO 21CB', 'laptop', '0.8.7-beta', 'yes', '15 GB', 'usb, 5000 Mbps']);
eq('boot row with the copy to memory', Object.fromEntries(summaryRows(variant((r) => { r.system.software.bootToRam = true; delete r.system.software.bootLinkMbps; }))).boot, 'usb, toram');
ok('no boot row when unknown', !summaryRows(variant((r) => { r.system.software.bootMedium = ''; r.system.software.bootLinkMbps = null; })).some(([k]) => k === 'boot'));
ok('the summary carries the boot class', summaryText(laptop).includes('boot: usb, 5000 Mbps'));
ok('the title says live', issueTitle(laptop).endsWith(', live)') && issueTitle(laptop).length <= TITLE_MAX);
ok('a long machine name keeps the title short', issueTitle({ ...laptop, system: { ...laptop.system, machine: { vendor: 'V'.repeat(80), product: 'P'.repeat(80) } } }).length <= TITLE_MAX);
const url = new URL(issueUrl(laptop));
eq('the link goes to the template', [url.origin + url.pathname, url.searchParams.get('template'), url.searchParams.get('lordos')], ['https://github.com/lordbasex/LordOS/issues/new', 'hardware-report.yml', '0.8.7-beta']);
eq('only the title, version and summary travel in the link', [...url.searchParams.keys()], ['template', 'title', 'lordos', 'summary']);
ok('the summary is short', url.searchParams.get('summary').length <= 600 && issueUrl(laptop).length < 2000);
ok('the summary has no report-only detail', !/i915.*46a8|totalKB|kernelConfig|results/.test(summaryText(laptop)));
const taken = new Set(['lordos-hwreport-2026-10-28.json', 'lordos-hwreport-2026-10-28-2.html']);
eq('file names skip taken ones', shareFileNames('2026-10-28', (f) => taken.has(f)), { json: 'lordos-hwreport-2026-10-28-3.json', html: 'lordos-hwreport-2026-10-28-3.html' });
eq('file names with a bad date', shareFileNames('../x', () => false).json, 'lordos-hwreport-today.json');
const page = sharePage({ text: { title: 'Share <b>', step2: 'Attach {file}', open: 'Open the form' }, rows: [['Machine', '<script>alert(1)</script>']], url: 'https://github.com/x?a=1&b="2"', jsonName: 'r.json', lang: 'ar', rtl: true });
ok('the page escapes everything', !page.includes('<script>alert') && page.includes('&lt;script&gt;') && page.includes('Share &lt;b&gt;') && page.includes('a=1&amp;b=&quot;2&quot;'));
ok('the page is right to left for Arabic', page.includes('<html lang="ar" dir="rtl">'));
ok('the page runs nothing and loads nothing', !/<script|src=|@import|url\(/i.test(page) && page.includes("default-src 'none'"));
ok('the page names the file', page.includes('Attach r.json'));

// ---- the workflow's checks (lib.mjs) ----
const body = `Summary\n\n[lordos-hwreport-2026-10-28.json](https://github.com/user-attachments/files/123456/lordos-hwreport-2026-10-28.json)\n
https://github.com/lordbasex/LordOS/files/77/old.json and https://github.com/someone/else/files/88/evil.json
https://evil.example/user-attachments/files/1/x.json https://github.com/user-attachments/files/9/x.json.exe javascript:alert(1)`;
eq('attachment links: GitHub\'s, this repository only, .json only', findAttachments(body, 'lordbasex/LordOS'),
  ['https://github.com/user-attachments/files/123456/lordos-hwreport-2026-10-28.json', 'https://github.com/lordbasex/LordOS/files/77/old.json']);
eq('at most three', findAttachments(Array.from({ length: 6 }, (_, i) => `https://github.com/user-attachments/files/${i}/a.json`).join(' '), 'a/b').length, 3);
eq('a bad repository name matches only user-attachments', findAttachments('https://github.com/x/y/files/1/a.json', '../x'), []);
eq('allowed hosts', ['https://github.com/user-attachments/files/1/a.json', 'https://objects.githubusercontent.com/x', 'https://github-production-user-asset-6210df.s3.amazonaws.com/x',
  'http://github.com/x', 'https://evil.com/x', 'https://github.com.evil.com/x', 'https://user:pw@github.com/x', 'https://github.com:8443/x', 'https://s3.amazonaws.com/x'].map(allowedUrl),
[true, true, true, false, false, false, false, false, false]);

// a fake fetch: routes → { status, headers, body }
const fake = (routes, seen = []) => async (u, opts) => {
  seen.push({ u, opts });
  const r = routes[u];
  if (!r) throw new Error(`unexpected ${u}`);
  const bytes = typeof r.body === 'string' ? new TextEncoder().encode(r.body) : r.body || new Uint8Array();
  return {
    status: r.status || 200, headers: { get: (k) => (r.headers || {})[k.toLowerCase()] ?? null },
    body: { getReader() { let done = false; return { read: async () => (done ? { done: true } : ((done = true), { done: false, value: bytes })), cancel: async () => {} }; } },
  };
};
const A = 'https://github.com/user-attachments/files/1/a.json', S3 = 'https://github-production-user-asset-6210df.s3.amazonaws.com/a?sig=1';
const seen = [];
eq('downloads through GitHub\'s redirect', await fetchAttachment(A, { fetchImpl: fake({ [A]: { status: 302, headers: { location: S3 } }, [S3]: { body: '{"a":1}' } }, seen) }), '{"a":1}');
ok('without credentials and without following redirects blindly', seen.every((s) => s.opts.redirect === 'manual' && s.opts.credentials === 'omit' && !s.opts.headers.authorization));
await throws('a redirect elsewhere is refused', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { status: 302, headers: { location: 'https://evil.example/x' } } }) }), /hosts/);
await throws('a redirect to http is refused', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { status: 302, headers: { location: 'http://objects.githubusercontent.com/x' } } }) }), /hosts/);
await throws('too many redirects', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { status: 302, headers: { location: A } } }) }), /redirects/);
await throws('a large Content-Length', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { headers: { 'content-length': String(10 * 1024 * 1024) }, body: '{}' } }) }), /larger/);
await throws('a large body without Content-Length', () => fetchAttachment(A, { maxBytes: 100, fetchImpl: fake({ [A]: { body: 'x'.repeat(101) } }) }), /larger/);
await throws('not UTF-8', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { body: new Uint8Array([0x7b, 0xff, 0x7d]) } }) }));
await throws('a 404', () => fetchAttachment(A, { fetchImpl: fake({ [A]: { status: 404 } }) }), /404/);
await throws('a URL that is not an attachment', () => fetchAttachment('https://evil.example/a.json', { fetchImpl: fake({}) }), /hosts/);

eq('parse: a report', parseLimited('﻿{"a":[1,2]}'), { a: [1, 2] });
await throws('parse: too deep', () => parseLimited('['.repeat(20) + ']'.repeat(20)), /deep/);
await throws('parse: too many values', () => parseLimited(JSON.stringify(Array(50000).fill(1))), /values/);
await throws('parse: __proto__', () => parseLimited('{"__proto__":{"x":1}}'), /not allowed/);
await throws('parse: not JSON (never evaluated)', () => parseLimited('({a:1})'));

eq('privacy: nothing in the good reports', [privacyScan(laptop), privacyScan(vm), privacyScan(pi)], [[], [], []]);
eq('privacy: what it finds', privacyScan({ a: 'eth 3c:22:fb:12:34:56', b: 'at 192.168.1.20', c: '4c4c4544-0042-3510-8051-b4c04f4e3732', d: 'me@example.org', e: 'saved in /home/alice/x', lordos: '0.8.4.1-beta', kernel: '10.0.0.1' }).map((x) => x.kind),
  ['mac', 'ipv4', 'uuid', 'email', 'path']);

eq('plausible: the laptop', plausibility(laptop, { today: '2026-10-29' }), []);
const silly = variant((r) => { r.bench.results['cpu.sha256.n'].value = 9e9; r.bench.results['mem.copy'].value = 5000; r.bench.results['gpu.scene'].value = 1e5; r.bench.results['cpu.matmul.n'].value = -1; r.system.memory.totalKB = 1000; });
eq('implausible numbers', plausibility(silly, { today: '2026-10-29' }).map((f) => f.id), ['memory-size', 'beyond-ceiling', 'not-positive', 'beyond-ceiling', 'far-above-references']);
eq('mismatches', plausibility(variant((r) => { r.bench.lordos = '0.8.6-beta'; r.bench.arch = 'aarch64'; r.bench.cpus = 4; r.live = false; r.createdAt = '2027-01-01'; }), { today: '2026-10-29' }).map((f) => f.id),
  ['date-future', 'bench-version', 'live-mismatch', 'arch-mismatch', 'cpu-mismatch']);

const today = { today: '2026-10-29' };
eq('check: valid', checkReport(laptop, schema, today).status, 'valid');
eq('check: labels from fixed lists', checkReport(laptop, schema, today).labels, ['hardware-report', 'report:valid', 'kind:laptop', 'gpu:intel', 'cpu:intel', 'live', 'boot:usb']);
eq('no boot label when unknown', labelsFor('valid', variant((r) => { r.system.software.bootMedium = ''; })).includes('boot:'), false);
eq('boot plausibility', plausibility(variant((r) => { r.live = false; r.bench.live = false; r.desktop = null; r.system.software.live = false; r.system.software.bootToRam = true; r.system.software.bootMedium = 'sd'; }), { today: '2026-10-29' }).map((f) => f.id),
  ['boot-mismatch', 'boot-mismatch']);
ok('the comment names the boot medium', commentBody(checkReport(laptop, schema, today)).includes('started from `usb`'));
eq('check: implausible → a person looks', checkReport(silly, schema, today).status, 'check');
eq('check: an extra field → invalid', checkReport(variant(badCases['a serial number field']), schema, today).status, 'invalid');
eq('check: a MAC in an allowed text → invalid', checkReport(variant((r) => { r.system.cpu.model = 'cpu 3c:22:fb:12:34:56'; }), schema, today).status, 'invalid');
eq('check: a reference can\'t come from an issue', checkReport(vm, schema, today).errors.map((e) => e.path), ['/source']);
eq('labels for an invalid report', labelsFor('invalid', null), ['hardware-report', 'report:invalid']);
const evil = variant((r) => { r.system.machine.vendor = '@everyone `x` [link](https://evil)'; r.system.machine.product = 'A B'; });
const cm = commentBody(checkReport(evil, schema, today), { fileName: 'a`b.json' });
ok('the comment starts with its marker', cm.startsWith(MARKER));
ok('report text only inside code spans', cm.includes('`@everyone  x  [link](https://evil) A B`') && !/(^|[^`])@everyone/.test(cm.replace(/`[^`]*`/g, '')));
ok('the file name loses its backtick', cm.includes('`a b.json`'));
eq('code(): empty and long', [code(''), code('x'.repeat(200)).length], ['—', 122]);
const errComment = commentBody({ status: 'invalid', errors: [{ path: '/system/x`y', message: 'is not a field of the report' }], labels: [] });
ok('error paths are code too', errComment.includes('`/system/x y`'));

// ---- the catalogue (catalog.mjs) ----
const entries = readReports(REPORTS_DIR, schema);
eq('the accepted reports', entries.map((e) => [e.file, e.report.source, e.issue]), [['reference-pi5-16gb.json', 'reference', null], ['reference-vm116.json', 'reference', null]]);
const snap = buildSnapshot(entries);
eq('the snapshot shipped is the one the reports give', fs.readFileSync(SNAPSHOT_FILE, 'utf8'), snapshotText(snap));
eq('snapshot header', [snap.format, snap.version, snap.generated, snap.reports], ['lordos-hwsnapshot', 1, '2026-10-27', 2]);
ok('snapshot entries add up', snap.devices.every((d) => d.works + d.limits + d.no === d.reports && d.references <= d.reports && /^\d+\.\d+\.\d+$/.test(d.lordos)));
ok('the snapshot is deterministic', snapshotText(buildSnapshot(readReports(REPORTS_DIR, schema))) === snapshotText(snap));
eq('the virtio GPU is "works with limits" on VM 116', snap.devices.find((d) => d.key === 'pci:1af4:1050'), { key: 'pci:1af4:1050', component: 'gpu', name: 'Red Hat, Inc. Virtio 1.0 GPU', lordos: '0.8.5', works: 0, limits: 1, no: 0, reports: 1, references: 1 });
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hwreport-test-'));
try {
  fs.writeFileSync(path.join(tmp, 'a.json'), JSON.stringify(laptop));
  await throws('a community report needs its issue', () => readReports(tmp, schema), /issue/);
  fs.writeFileSync(path.join(tmp, 'index.json'), JSON.stringify({ reports: { 'a.json': { issue: 42, fixedIn: { suspend: '0.8.8', bogus: '1', gpu: 'x' } } } }));
  const e2 = readReports(tmp, schema);
  eq('index: issue and fixedIn (validated)', [e2[0].issue, e2[0].fixedIn], [42, { suspend: '0.8.8' }]);
  const s2 = buildSnapshot(e2);
  eq('a community laptop in the snapshot', s2.devices.filter((d) => d.key === 'pci:8086:46a8').map((d) => [d.component, d.lordos, d.works, d.references]), [['gpu', '0.8.7', 1, 0]]);
  ok('the machine key answers for its own rows only', s2.devices.filter((d) => d.key.startsWith('machine:')).map((d) => d.component).sort().join() === 'display,power,storage');
  fs.writeFileSync(path.join(tmp, 'b.json'), JSON.stringify(variant(badCases['a host name field'])));
  await throws('an invalid report stops the build', () => readReports(tmp, schema), /b\.json/);
} finally { fs.rmSync(tmp, { recursive: true, force: true }); }
const model = buildModel(entries);
eq('the page model', model.machines.map((m) => [m.name, m.kind, m.source, m.perf.map((p) => p.cat)]),
  [['QEMU Standard PC (Q35 + ICH9, 2009)', 'vm', 'reference', ['cpu', 'memory', 'gpu', 'storage']], ['Raspberry Pi 5 Model B Rev 1.1', 'board', 'reference', ['cpu', 'memory', 'gpu', 'storage']]]);
eq('problems listed', model.problems.map((p) => [p.id, p.state, p.note]), [['gpu', 'limits', 'cpu.software']]);
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const html = renderSections({ ...model, machines: [{ ...model.machines[0], name: '<img src=x onerror=alert(1)>' }] }, { reference: 'Reference', state: { works: 'Works' } }, esc, (k) => `https://github.com/x/issues/${k}`);
ok('the page sections escape report text', !html.machines.includes('<img') && html.machines.includes('&lt;img'));
ok('references are marked as such', html.machines.includes('class="hw-ref">Reference'));

// ---- community data in the Compatibility panel (community.js) ----
const keys = deviceKeys(laptop.system);
eq('device keys of the laptop', keys.map((k) => `${k.component} ${k.key}`), [
  'display machine:LENOVO|21CB', 'storage machine:LENOVO|21CB', 'power machine:LENOVO|21CB', 'suspend machine:LENOVO|21CB',
  'gpu pci:8086:46a8', 'wifi wifi:iwlwifi', 'network net:e1000e', 'bluetooth bt:btusb', 'audio audio:HDA Intel PCH']);
ok('no machine key for a virtual machine', deviceKeys(vm.system).every((k) => !k.key.startsWith('machine:')));
const sn = { format: 'lordos-hwsnapshot', version: 1, generated: '2026-10-29', reports: 3, devices: [
  { key: 'pci:8086:46a8', component: 'gpu', name: 'Iris Xe', lordos: '0.8.7', works: 2, limits: 1, no: 0, reports: 3, references: 0 },
  { key: 'pci:8086:46a8', component: 'gpu', name: 'Iris Xe', lordos: '0.8.6', works: 0, limits: 0, no: 5, reports: 5, references: 0 },
  { key: 'wifi:iwlwifi', component: 'wifi', name: 'iwlwifi', lordos: '0.8.7', works: 0, limits: 0, no: 1, reports: 1, references: 1 },
  { key: 'machine:LENOVO|21CB', component: 'suspend', name: 'LENOVO 21CB', lordos: '0.8.7', works: 1, limits: 0, no: 0, reports: 1, references: 0 },
] };
const cf = communityFor(sn, laptop.system, '0.8.7-beta');
eq('only the same LordOS version', cf.gpu, [{ name: 'Intel Corporation Alder Lake-UP3 GT2 [Iris Xe Graphics]', works: 2, limits: 1, no: 0, reports: 3, references: 0 }]);
eq('rows with data', Object.keys(cf).sort(), ['gpu', 'suspend', 'wifi']);
eq('another version sees none of it', communityFor(sn, laptop.system, '0.8.8'), {});
eq('summary: works', communitySummary(cf.gpu), { works: 2, limits: 1, no: 0, reports: 3, references: 0, state: 'works', onlyReferences: false });
eq('summary: only references', communitySummary(cf.wifi).onlyReferences, true);
eq('summary: nothing', communitySummary([]), null);
eq('not a snapshot', [communityFor({ ...sn, format: 'x' }, laptop.system, '0.8.7'), communityFor(null, laptop.system, '0.8.7'), communityFor({ ...sn, version: 2 }, laptop.system, '0.8.7')], [{}, {}, {}]);

console.log(`hwreport-test: ${n - bad} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
