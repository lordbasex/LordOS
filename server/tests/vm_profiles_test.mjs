// vm_profiles_test.mjs — Machines' profiles, the person's own systems and the library's filters (apps/machines/
// profiles.js, systems.js, model.js, catalog.js logoEntry): a .lordprofile is read with known fields only, every value
// checked and fitted to this computer, what was ignored listed; a system needs an https address or a file under the
// home and a 64-hex SHA-256; logos are PNG only. Includes a fuzz pass: random profiles never throw and what they give
// reads back the same.
//
//   node tests/vm_profiles_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const P = await import(path.join(here, '../../apps/machines/profiles.js'));
const S = await import(path.join(here, '../../apps/machines/systems.js'));
const M = await import(path.join(here, '../../apps/machines/model.js'));
const C = await import(path.join(here, '../../apps/machines/catalog.js'));
const CAT = JSON.parse(fs.readFileSync(path.join(here, '../../assets/machines/systems.json'), 'utf8'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, v) => eq(name, !!v, true);

const host = { cpus: 20, vcpusRecommended: 19, ramMb: 131072, memMaxMb: 126976, arch: 'amd64' };
const small = { cpus: 4, vcpusRecommended: 3, ramMb: 8192, memMaxMb: 6144, arch: 'amd64' };
const SHA = 'a'.repeat(64);
const home = '/home/ana';

// ---- built-in profiles ----
eq('five built-in profiles', P.BUILTIN, ['productivity', 'development', 'games', 'design', 'light']);
eq('half the processors', P.cpuCap(host), 10);
eq('half of a small computer', P.cpuCap(small), 2);
eq('one processor at least', P.cpuCap({ cpus: 1 }), 1);
const prod = P.onHost(P.builtinProfile('productivity', host), host, { gpuReady: false, no3d: '' });
eq('productivity here (the mock: 4 of 20, 8 GB, 64 GB)', [prod.vcpus, prod.memoryMb, prod.diskGb, prod.graphics, prod.clipboard, prod.transfer], [4, 8192, 64, '3d', 'both', true]);
eq('development: half the processors', P.onHost(P.builtinProfile('development', host), host).vcpus, 10);
eq('games: a graphics card only when one is set up', [P.onHost(P.builtinProfile('games', host), host, { gpuReady: false }).gpu, P.onHost(P.builtinProfile('games', host), host, { gpuReady: true }).gpu], ['', 'intel-shared']);
eq('games captures the mouse', P.builtinProfile('games', host).input.mouse, 'capture');
eq('3D only where it can be had', P.onHost(P.builtinProfile('design', host), host, { no3d: 'gpu' }).graphics, '2d');
const sm = P.onHost(P.builtinProfile('design', small), small);
ok('small computer: within what machines may use', sm.memoryMb <= small.memMaxMb && sm.vcpus <= 2);
eq('light', [P.onHost(P.builtinProfile('light', host), host).vcpus, P.onHost(P.builtinProfile('light', host), host).clipboard], [2, 'off']);
eq('default profile id', [P.defaultProfileId(null, []), P.defaultProfileId('games', []), P.defaultProfileId('u-00000001', [{ id: 'u-00000001' }]), P.defaultProfileId('u-00000002', [])],
  ['productivity', 'games', 'u-00000001', 'productivity']);

// ---- reading a profile (the mock "ProfileImport") ----
const mint = {
  format: 'lordos-machine-profile/1', name: 'Linux Mint para oficina', author: 'Ana Pérez', color: 'green', script: 'rm -rf ~',
  system: { kind: 'linux', suggested: ['mint-22', 'debian', 'nowhere'], image: { id: 'mint-22', name: 'Linux Mint 22.1 Cinnamon', kind: 'linux', install: 'manual',
    url: 'https://mirror.example.org/linuxmint-22.1-cinnamon-64bit.iso', sha256: SHA.toUpperCase(), size: 2900000000, extra: 1,
    signature: { url: 'https://mirror.example.org/sha256sum.txt.gpg', sums: 'https://mirror.example.org/sha256sum.txt', fingerprint: '27DE B156 44C6 B3CF 3BD7  D291 300F 846B A25B AE09' } } },
  resources: { cpus: 32, memoryMb: 4096, diskGb: 40, graphics: '2d', turbo: true },
  network: { mode: 'hostonly', ports: [{ proto: 'tcp', host: 2222, guest: 22 }, { proto: 'tcp', host: 8443, guest: 443 }, { proto: 'tcp', host: 80, guest: 80 }] },
  integration: { clipboard: 'off', share: { path: '~/Servidor', writable: true } },
};
const r = P.readProfile(JSON.stringify(mint), { host, gpuReady: false, systems: ['debian', 'ubuntu'], home, id: 'u-0000000a', imageId: 's-0000000b' });
eq('import ok', [r.ok, r.profile.name, r.profile.author, r.profile.id], [true, 'Linux Mint para oficina', 'Ana Pérez', 'u-0000000a']);
eq('cpus 32 becomes 10 (half)', r.adjusted.find((a) => a.field === 'resources.cpus'), { field: 'resources.cpus', from: 32, to: 10, why: 'half' });
eq('unknown fields listed', r.ignored.sort(), ['resources.turbo', 'script', 'system.image.extra']);
eq('the system it brings', [r.image.id, r.image.url, r.image.sha256, r.image.source, r.image.profile, r.image.signature.fingerprint],
  ['s-0000000b', 'https://mirror.example.org/linuxmint-22.1-cinnamon-64bit.iso', SHA, 'url', 'u-0000000a', '27DEB15644C6B3CF3BD7D291300F846BA25BAE09']);
eq('suggested: the new system, the known one; an unknown one left out', r.profile.system.suggested, ['s-0000000b', 'debian']);
ok('unknown suggested system noted', r.adjusted.some((a) => a.why === 'system' && a.from === 'nowhere'));
eq('ports: a host port under 1024 dropped', r.profile.network.ports.map((x) => x.host), [2222, 8443]);
eq('host-only ports open to this computer only', r.profile.network.ports.every((x) => x.open === 'this-computer'), true);
eq('share kept as a path under the home', r.profile.integration.share, { path: '~/Servidor', writable: true });
eq('clipboard', r.profile.integration.clipboard, 'off');
const http = P.readProfile({ ...mint, system: { kind: 'linux', image: { ...mint.system.image, url: 'http://mirror.example.org/x.iso' } } }, { host });
eq('an http:// image is refused (the profile still reads)', [http.ok, http.image, http.imageErrors], [true, null, ['http']]);
const nosha = P.readProfile({ ...mint, system: { kind: 'linux', image: { ...mint.system.image, sha256: 'abc' } } }, { host });
eq('an image without a SHA-256 is refused', [nosha.image, nosha.imageErrors], [null, ['sha']]);
const fileImg = P.readProfile({ ...mint, system: { kind: 'linux', image: { ...mint.system.image, source: 'file', path: '/home/ana/x.iso', url: undefined } } }, { host });
eq('a profile never brings a file of another computer', fileImg.image, null);
eq('format', P.readProfile({ ...mint, format: 'lordos-machine-profile/2' }).error, 'format');
eq('not JSON', P.readProfile('{').error, 'json');
eq('too big', P.readProfile(`{"x":"${'a'.repeat(70000)}"}`).error, 'tooBig');
eq('no name', P.readProfile({ format: P.PROFILE_FORMAT }).error, 'name');
eq('name with a control character', P.readProfile({ format: P.PROFILE_FORMAT, name: 'a\u0007b' }).error, 'name');
eq('bidi override refused', P.readProfile({ format: P.PROFILE_FORMAT, name: 'abc‮def' }).error, 'name');
const g = P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', resources: { gpu: 'intel-whole', memoryMb: 999999, diskGb: 99999, cpus: 'many' } }, { host: small, gpuReady: false });
eq('gpu dropped where none is set up', [g.profile.resources.gpu, g.adjusted.some((a) => a.why === 'gpu')], ['', true]);
eq('memory fitted to what machines may use', [g.profile.resources.memoryMb, g.adjusted.some((a) => a.why === 'memory')], [6144, true]);
eq('disk clamped', g.profile.resources.diskGb, 16384);
ok('cpus: not a number, the default kept', g.adjusted.some((a) => a.field === 'resources.cpus' && a.why === 'invalid'));
for (const bad0 of ['/etc', '~/../etc', '~/.config/lordos', '~/a/../../b', '~', '~/', 'Projects', '~/a/.ssh', '~/a\nb']) {
  const x = P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', integration: { share: { path: bad0, writable: true } } });
  eq(`share ${JSON.stringify(bad0)} refused`, x.profile.integration.share, null);
}
eq('share paths', [P.sharePathOk('~/Proyectos'), P.sharePathOk('~/Proyectos/web site'), P.shareAbs('~/Proyectos/', home), P.shareRel('/home/ana/Proyectos', home), P.shareRel('/etc', home), P.shareRel('/home/ana/.ssh', home)],
  [true, true, '/home/ana/Proyectos', '~/Proyectos', '', '']);
const enums = P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', color: 'pink', system: { kind: 'beos' }, network: { mode: 'internet' }, integration: { onClose: 'explode', clipboard: 'all', appsMode: 'yes' }, input: { mouse: 'grab', shortcuts: 'always' } });
eq('enums fall back', [enums.profile.color, enums.profile.system.kind, enums.profile.network.mode, enums.profile.integration.onClose, enums.profile.integration.clipboard, enums.profile.integration.appsMode, enums.profile.input.mouse, enums.profile.input.shortcuts],
  ['violet', 'linux', 'shared', 'ask', 'both', false, 'auto', 'ask']);
eq('every wrong enum noted', enums.adjusted.filter((a) => a.why === 'invalid').length, 8);
eq('a stored profile keeps its id; a built-in id is never taken', [P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', id: 'u-12345678' }, { stored: true }).profile.id,
  P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', id: 'games' }, { stored: true }).profile.id], ['u-12345678', '']);
eq('a logo: PNG over https only', [P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', logo: { url: 'https://a.org/l.png', sha256: SHA } }).profile.logo?.url,
  P.readProfile({ format: P.PROFILE_FORMAT, name: 'x', logo: { url: 'https://a.org/l.svg', sha256: SHA } }).profile.logo], ['https://a.org/l.png', undefined]);

// ---- writing it back ----
const written = P.writeProfile(r.profile, [r.image]);
const again = P.readProfile(written, { host, systems: ['debian'], home, id: 'u-0000000a', imageId: 's-0000000b' });
eq('export reads back the same', [again.profile, again.image], [r.profile, r.image]);
eq('nothing ignored on its own export', again.ignored, []);
ok('the export holds the address and hash, never an image', /"url": "https:\/\/mirror/.test(written) && !/"source"/.test(written) && !/verified/.test(written));
const stored = P.writeProfile(r.profile, [], { withId: true });
eq('a stored profile keeps its id and no system', [JSON.parse(stored).id, JSON.parse(stored).system.image], ['u-0000000a', undefined]);
eq('export name', [P.exportName({ name: 'Desarrollo web' }), P.exportName({ name: '¡¿' })], ['desarrollo-web.lordprofile', 'profile.lordprofile']);

// ---- what lord.vm.set gets after create ----
const v = P.onHost(r.profile, host, { gpuReady: true });
eq('set after create', P.setPatch(v, { bridge: 'enp3s0', shareAbsPath: '/home/ana/Servidor' }),
  { network: 'hostonly', ports: v.ports, transfer: true, shares: [{ tag: 'share', path: '/home/ana/Servidor', writable: true }] });
eq('productivity needs only the transfer folder', P.setPatch(prod), { transfer: true });
eq('bridged with its card', P.setPatch({ ...prod, network: 'bridged', transfer: false }, { bridge: 'eno1' }), { network: 'bridged', bridge: 'eno1' });

// ---- fuzz: random profiles never throw; what they give reads back the same ----
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const junk = () => pick([undefined, null, 0, -1, 1.5, 1e9, NaN, '', 'x', 'half', true, [], {}, [1, 2], { a: 1 }, '~/ok', '../..', 'https://a.org/x.iso', SHA, 'tcp', 2222, 'lan']);
const rndObj = (keys) => Object.fromEntries(keys.filter(() => rnd() < 0.7).map((k) => [k, rnd() < 0.5 ? junk() : pick([junk(), junk()])]));
let threw = 0, unstable = 0;
for (let i = 0; i < 3000; i++) {
  const o = { format: rnd() < 0.95 ? P.PROFILE_FORMAT : junk(), name: rnd() < 0.9 ? `P${i}` : junk(), color: junk(), author: junk(), description: junk(),
    system: rnd() < 0.8 ? { kind: pick(['linux', 'windows', 'other', junk()]), suggested: rnd() < 0.5 ? [junk(), 'debian', junk()] : junk(),
      image: rnd() < 0.4 ? { name: junk(), kind: pick(['linux', junk()]), url: pick(['https://a.org/x.iso', 'http://a.org/x.iso', junk()]), sha256: pick([SHA, junk()]), install: junk(), size: junk() } : undefined } : junk(),
    resources: rnd() < 0.8 ? rndObj(['cpus', 'memoryMb', 'diskGb', 'graphics', 'gpu', 'zz']) : junk(),
    network: rnd() < 0.8 ? { mode: pick(M.NET_MODES.concat([junk()])), ports: rnd() < 0.6 ? [{ proto: pick(['tcp', 'udp', junk()]), host: pick([2222, 80, 70000, junk()]), guest: pick([22, 0, junk()]), open: junk() }, junk()] : junk() } : junk(),
    integration: rnd() < 0.8 ? { ...rndObj(['clipboard', 'transfer', 'appsMode', 'onClose']), share: rnd() < 0.5 ? { path: pick(['~/a', '~/../b', '/etc', junk()]), writable: junk() } : junk() } : junk(),
    input: rnd() < 0.8 ? rndObj(['mouse', 'shortcuts', 'captureFullscreen']) : junk(), logo: rnd() < 0.2 ? { url: junk(), sha256: junk() } : undefined };
  let x;
  try { x = P.readProfile(rnd() < 0.5 ? JSON.stringify(o) : o, { host: pick([host, small, null]), gpuReady: rnd() < 0.5, systems: ['debian'], home, id: 'u-00000001', imageId: 's-00000002' }); } catch (e) { threw++; continue; }
  if (!x.ok) continue;
  const y = P.readProfile(P.writeProfile(x.profile, x.image ? [x.image] : []), { host: null, systems: ['debian'], home, id: 'u-00000001', imageId: 's-00000002' });
  if (!y.ok || JSON.stringify(y.profile) !== JSON.stringify(x.profile) || y.adjusted.length) unstable++;
  const p0 = x.profile;
  if (!(M.NET_MODES.includes(p0.network.mode) && P.CLIPBOARDS.includes(p0.integration.clipboard) && (p0.resources.cpus === 'half' || (p0.resources.cpus >= 1 && p0.resources.cpus <= 64))
    && (!p0.integration.share || P.sharePathOk(p0.integration.share.path)) && M.portProblems(p0.network.ports, p0.network.mode).length === 0)) unstable++;
}
eq('fuzz: never throws', threw, 0);
eq('fuzz: always valid, reads back the same', unstable, 0);

// ---- the person's systems ----
const sys0 = { name: 'Linux Mint 22', kind: 'linux', color: 'green', profile: 'development', install: 'manual', source: 'url', url: 'https://mirror.example.org/linuxmint-22-cinnamon-64bit.iso', sha256: SHA };
const vs = S.validateSystem(sys0, { home, newId: 's-00000001' });
eq('a system by address', [vs.ok, vs.system.id, vs.system.url, vs.system.source], [true, 's-00000001', sys0.url, 'url']);
for (const [why, url] of [['http', 'http://a.org/x.iso'], ['url', 'ftp://a.org/x.iso'], ['url', 'https://user:pw@a.org/x.iso'], ['url', 'https://a.org/x y.iso'], ['url', 'https:///x.iso'],
  ['url', 'https://a.org/'], ['url', 'javascript:alert(1)'], ['url', 'https://a.org:99999/x.iso'], ['url', `https://a.org/${'a'.repeat(3000)}.iso`]]) {
  eq(`refused: ${url.slice(0, 40)}`, S.validateSystem({ ...sys0, url }, { home, newId: 's-1' }).errors.includes(why), true);
}
eq('SHA-256: 64 hex digits only', [S.shaOf(SHA), S.shaOf(SHA.toUpperCase()), S.shaOf('a'.repeat(63)), S.shaOf(`${'a'.repeat(63)}g`), S.shaOf(' ' + SHA + ' ')], [SHA, SHA, '', '', SHA]);
eq('a system needs its SHA-256', S.validateSystem({ ...sys0, sha256: '' }, { home, newId: 's-1' }).errors, ['sha']);
eq('a local ISO under the home', [S.validateSystem({ ...sys0, source: 'file', path: '/home/ana/Descargas/mint.iso' }, { home, newId: 's-1' }).ok,
  S.validateSystem({ ...sys0, source: 'file', path: '/etc/mint.iso' }, { home, newId: 's-1' }).errors, S.validateSystem({ ...sys0, source: 'file', path: '/home/ana/../bob/x.iso' }, { home, newId: 's-1' }).errors,
  S.validateSystem({ ...sys0, source: 'file', path: '/home/ana/x.img' }, { home, newId: 's-1' }).errors], [true, ['path'], ['path'], ['path']]);
eq('windows installs manually here', S.validateSystem({ ...sys0, kind: 'windows', install: 'preseed' }, { home, newId: 's-1' }).errors, ['install']);
eq('unknown fields of a system ignored', S.validateSystem({ ...sys0, run: 'x' }, { home, newId: 's-1' }).ignored, ['run']);
eq('signature: needs all three', [S.signatureOf({ url: 'https://a.org/s.gpg', sums: 'https://a.org/s.txt', fingerprint: 'ab'.repeat(20) })?.fingerprint, S.signatureOf({ url: 'https://a.org/s.gpg' })],
  ['AB'.repeat(20), null]);
eq('logo: PNG over https with its SHA-256', [S.logoOf({ url: 'https://a.org/l.png', sha256: SHA })?.url, S.logoOf({ url: 'https://a.org/l.svg', sha256: SHA }), S.logoOf({ url: 'http://a.org/l.png', sha256: SHA }), S.logoOf({ url: 'https://a.org/l.png', sha256: 'x' })],
  ['https://a.org/l.png', null, null, null]);
const list = S.readSystems(S.writeSystems([vs.system, vs.system, { ...vs.system, id: 's-00000002', url: 'http://x' }]), { home });
eq('systems.json: broken and repeated entries left out', list.map((x) => x.id), ['s-00000001']);
eq('systems.json: wrong format', S.readSystems('{"format":"x","systems":[]}'), []);
eq('file of an address', [S.fileOfUrl('https://a.org/dir/linuxmint-22.iso'), S.fileOfUrl('https://a.org/a%20b.iso'), S.fileOfUrl('https://a.org/x.iso?mirror=1'), S.fileOfUrl('https://a.org/')], ['linuxmint-22.iso', '', 'x.iso', '']);
eq('state', [S.systemState(vs.system), S.systemState({ ...vs.system, verified: { path: '/home/ana/x.iso', at: 't' } }), S.systemState({ source: 'file' })], ['download', 'ready', 'pick']);
eq('downloads that may be it', S.downloadCandidates({ url: 'https://a.org/mint.iso' }, [{ name: 'mint (1).iso' }, { name: 'mint.iso' }, { name: 'mint.iso.part' }, { name: 'other.iso' }, { name: 'mint.iso', dir: true }]).map((x) => x.name),
  ['mint.iso', 'mint (1).iso']);
eq('unattended of an added system: the catalog way', [S.customUnattended({ kind: 'linux', install: 'preseed' }, CAT, 'amd64')?.kernel, S.customUnattended({ kind: 'linux', install: 'manual' }, CAT, 'amd64'),
  S.customUnattended({ kind: 'linux', install: 'kickstart' }, CAT, 'arm64')?.initrd], ['install.amd/vmlinuz', null, 'images/pxeboot/initrd.img']);
eq('new ids', [S.newId('s', [], () => 0), S.newId('s', ['s-00000000'], (() => { let k = 0; return () => (k++ ? 0.5 : 0); })())], ['s-00000000', 's-7fffffff']);

// ---- PNG only, and the catalog's logos ----
const png = (w, h, len = 64) => { const b = new Uint8Array(len); b.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52]);
  b.set([w >>> 24, (w >>> 16) & 255, (w >>> 8) & 255, w & 255, h >>> 24, (h >>> 16) & 255, (h >>> 8) & 255, h & 255, 8, 6, 0, 0, 0], 16); return b; };
eq('a PNG', S.pngHeader(png(101, 124)), { w: 101, h: 124 });
eq('larger than 1024', S.pngHeader(png(2048, 10)), null);
eq('zero side', S.pngHeader(png(0, 10)), null);
eq('too many bytes', S.pngHeader(png(10, 10, 600 * 1024)), null);
eq('an SVG', S.pngHeader(new TextEncoder().encode('<svg xmlns="http://www.w3.org/2000/svg"><script>x</script></svg>'.padEnd(64))), null);
eq('a JPEG', S.pngHeader(new Uint8Array([0xff, 0xd8, 0xff, 0xe0, ...new Array(60).fill(0)])), null);
const notIhdr = png(10, 10); notIhdr[12] = 0x41;
eq('IHDR first', S.pngHeader(notIhdr), null);
const badDepth = png(10, 10); badDepth[24] = 7;
eq('bit depth', S.pngHeader(badDepth), null);
eq('logo file', [S.logoFile('/home/ana', SHA), S.logoFile('/home/ana', '../x')], [`/home/ana/.cache/lordos/machines/logos/${SHA}.png`, '']);
eq('Debian has a logo; Ubuntu, Fedora and the other system don\'t', ['debian', 'ubuntu', 'fedora', 'kali', 'windows'].map((id) => !!C.logoEntry(CAT, id, 'amd64')), [true, false, false, false, false]);
eq('the Debian logo is pinned for both architectures', [C.logoEntry(CAT, 'debian', 'arm64')?.sha, C.logoEntry(CAT, 'debian', 'amd64')?.bytes],
  ['92aad0be9b2be94420cfb6c1daf5985338e402110353b4fa051fb429f4cde2b8', 1444]);

// ---- the library's chips, search and numbers ----
const lib = [
  { dir: 'a', name: 'Ubuntu 26.04.1', state: 'running', meta: { os: 'ubuntu' }, vcpus: 15, memoryMb: 16384, diskGb: 64, network: 'shared', graphics: '3d' },
  { dir: 'b', name: 'Win Pro', state: 'stopped', windowsDevices: true, vcpus: 10, memoryMb: 8192, diskGb: 64, network: 'bridged', gpu: 'intel-shared' },
  { dir: 'c', name: 'Fedora 43', state: 'stopped', suspended: true, meta: { os: 'fedora' }, vcpus: 4, memoryMb: 8192, diskGb: 40, network: 'shared' },
  { dir: 'd', name: 'FreeBSD', state: 'stopped', meta: { os: 'freebsd' }, vcpus: 2, memoryMb: 2048, diskGb: 20, network: 'isolated' },
];
eq('chip counts', M.chipCounts(lib, ''), { all: 4, running: 1, linux: 2, windows: 1 });
eq('chip counts with a search', M.chipCounts(lib, 'fed'), { all: 1, running: 0, linux: 1, windows: 0 });
eq('linux chip', M.filterMachines(lib, { show: 'linux' }).map((m) => m.dir), ['a', 'c']);
eq('the other system\'s chip', M.filterMachines(lib, { show: 'windows' }).map((m) => m.dir), ['b']);
eq('family', lib.map(M.familyOf), ['linux', 'windows', 'linux', 'other']);
eq('a system the person added says Linux', M.familyOf({ name: 'Mint', meta: { os: 'other', family: 'linux' } }), 'linux');
eq('spec line parts', M.specParts(lib[1]).map((x) => x.k), ['cpu', 'mem', 'disk', 'net.bridged', 'gpu']);
eq('3D in the spec line', M.specParts(lib[0]).map((x) => x.k).pop(), '3d');
const now = 1_800_000_000_000;
eq('rest line', [M.restLine(lib[2], now), M.restLine({ thumbTime: (now - 2 * 3600e3) / 1000 }, now), M.restLine({ thumbTime: now - 5 * 60e3 }, now), M.restLine({ thumbTime: (now - 3 * 86400e3) / 1000 }, now), M.restLine({ meta: { profile: 'games' } }, now), M.restLine({}, now)],
  [{ k: 'resumes' }, { k: 'lastHours', n: 2 }, { k: 'lastMin', n: 5 }, { k: 'lastDays', n: 3 }, { k: 'profile', id: 'games' }, null]);

console.log(`== vm-profiles-test: ${n - bad} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
