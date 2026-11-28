// vm_catalog_test.mjs — the New Machine assistant's download cards (apps/machines/catalog.js) over the real
// assets/machines/systems.json: the cards per architecture, the makers' list forms, a folder listing, the newest image,
// the version on the card, the progress lines. The admin helper's own checks are tests/vmcatalog_test.c.
//
//   node tests/vm_catalog_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const C = await import(path.join(here, '../../apps/machines/catalog.js'));
const CAT = JSON.parse(fs.readFileSync(path.join(here, '../../assets/machines/systems.json'), 'utf8'));
const { systemsFor, entryOf, parseSums, listingNames, newest, pickImage, versionIn, progressOf, resumedOf, compareNames, sizeMb, downloadedImages, IMAGES_DIR, unattendedOf, checkSetup, hostnameFor, hostnameNext, userFor, cardSize, lordosLang } = C;
const M = await import(path.join(here, '../../apps/machines/model.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const H = (c) => c.repeat(64);

// ---- the cards ----
eq('amd64 cards, LordOS first', systemsFor(CAT, 'amd64').map((s) => s.id), ['lordos', 'debian', 'ubuntu', 'fedora', 'kali', 'windows']);
eq('arm64 cards (with the virtio-win guest from its maker\'s Arm64 ISO, 4.3.2)', systemsFor(CAT, 'arm64').map((s) => s.id), ['lordos', 'debian', 'ubuntu', 'fedora', 'kali', 'windows']);
// ---- a pinned card (an ISO whose maker signs no list): the catalog pins its one file and hash ----
{
  const PIN = { systems: [{ id: 'pinned-os', name: 'Pinned', release: '1', kind: 'iso', family: 'other', machine: { vcpus: 4, memoryMb: 4096, diskGb: 32, graphics: '3d' },
    arch: { amd64: { pinned: [{ name: 'pinned-1.iso', url: 'https://example.org/pinned-1.iso', sha256: H('c'), bytes: 1000 }], approxMb: 1 } } }] };
  const card = systemsFor(PIN, 'amd64').find((s) => s.id === 'pinned-os');
  eq('pinned: a pinned card, not a listed one', [card.kind, card.pinned, card.listed, card.family], ['iso', true, false, 'other']);
  eq('pinned: no card on another architecture', systemsFor(PIN, 'arm64').length, 0);
  eq('families: the real cards', systemsFor(CAT, 'amd64').map((s) => s.family).slice(1), ['linux', 'linux', 'linux', 'linux', 'windows']);
  const img = C.pinnedImage(entryOf(PIN, 'pinned-os', 'amd64'));
  eq('pinned: its hash and size', [img.name, img.sha, img.size, img.pinned], ['pinned-1.iso', H('c'), 1000, true]);
  eq('pinned: pickImage takes the pin, whatever the text', pickImage(entryOf(PIN, 'pinned-os', 'amd64'), `${H('a')}  other.iso\n`).sha, img.sha);
  eq('pinned: no list to read', entryOf(PIN, 'pinned-os', 'amd64').sums, undefined);
  const host = { cpus: 16, memMaxMb: 30000 };
  eq('machine: its own, whatever the profile', C.cardSize(PIN, 'pinned-os', { vcpus: 8, memoryMb: 16384, diskGb: 128, gfx: '2d', network: 'shared' }, host),
    { vcpus: 4, memoryMb: 4096, diskGb: 32, gfx: '3d', network: 'shared' });
  eq('machine: fitted to a small computer', C.cardSize(PIN, 'pinned-os', { vcpus: 2, memoryMb: 2048, diskGb: 40, gfx: '2d' }, { cpus: 2, memMaxMb: 3000 }),
    { vcpus: 2, memoryMb: 2816, diskGb: 32, gfx: '3d' });
  eq('a card without a machine keeps the profile\'s', C.cardSize(CAT, 'debian', { vcpus: 8 }, host), { vcpus: 8 });
  eq('a pin must be one https ISO', [C.pinnedImage({ pinned: [{ name: 'a.iso', url: 'http://x/a.iso', sha256: H('a'), bytes: 1 }] }),
    C.pinnedImage({ pinned: [{ name: 'a.msi', url: 'https://x/a.msi', sha256: H('a'), bytes: 1 }] }),
    C.pinnedImage({ pinned: [{ name: 'a.iso', url: 'https://x/a.iso', sha256: H('a'), bytes: 1 }, { name: 'b.iso', url: 'https://x/b.iso', sha256: H('b'), bytes: 1 }] }),
    C.pinnedImage({ pinned: [{ name: 'a.iso', url: 'https://x/a.iso', sha256: H('a'), bytes: 0 }] })], [null, null, null, null]);
  eq('the drivers\' and tools\' pins are no cards', systemsFor(CAT, 'amd64').some((s) => ['virtio-win', 'winfsp'].includes(s.id) || /^logo-/.test(s.id)), false);
}
eq('no cards for another arch', systemsFor(CAT, 'riscv64'), []);
const card = (id, arch = 'amd64') => systemsFor(CAT, arch).find((s) => s.id === id);
eq('Debian 13', [card('debian').name, card('debian').release], ['Debian', '13']);
eq('every entry has its hash source and date', CAT.systems.every((s) => s.hashSource && /^\d{4}-\d\d-\d\d$/.test(s.checked)), true);
// a pinned entry (Windows' evaluation, virtio-win; vmcatalog.h) names its files itself; every other entry has a folder
const pinnedOf = (s) => Object.values(s.arch).some((a) => Array.isArray(a.pinned));
// (an entry with "releases" has the folder of its release folders instead: Whonix, docs/VIRTUALIZATION.md 19.6)
eq('every folder is https', CAT.systems.filter((s) => !pinnedOf(s)).every((s) => Object.values(s.arch).every((a) => /^https:\/\/[^/]+\/.*\/$/.test(a.releases ? a.releases.dir : a.dir) && (!a.fileDir || /^https:\/\//.test(a.fileDir)))), true);
eq('the Whonix group is no card without the host\'s KVM (vm_group_ui_test.mjs)', card('whonix'), undefined);
for (const s of CAT.systems.filter(pinnedOf)) {
  const files = Object.values(s.arch).flatMap((a) => a.pinned || []);
  eq(`${s.id}: pinned files`, [files.length > 0, files.every((f) => /^https:\/\/[^/\s"<>\\]+\/\S+$/.test(f.url) && /^[0-9a-f]{64}$/.test(f.sha256) && Number.isInteger(f.bytes) && f.bytes > 0 &&
    /^[A-Za-z0-9_+~][A-Za-z0-9._+~-]{0,94}$/.test(f.name)), Object.values(s.arch).every((a) => new Set((a.pinned || []).map((f) => f.name)).size === (a.pinned || []).length),
    typeof s.hashSource === 'string' && typeof s.license === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(s.checked)], [true, true, true, true]);
}
// an official logo (opt-in, apps/machines/logos.js): a PNG of at most 512 KB, for a listed system, never the other system's
for (const s of CAT.systems.filter((x) => x.kind === 'logo')) {
  const files = Object.values(s.arch).flatMap((a) => a.pinned || []);
  eq(`${s.id}: a logo is a small PNG of a listed system`, [s.hidden === true, /^logo-[a-z0-9-]+$/.test(s.id), s.for !== 'windows' && CAT.systems.some((x) => x.id === s.for && x.kind !== 'logo'),
    files.every((f) => /\.png$/.test(f.name) && f.bytes <= 512 * 1024), typeof s.policy === 'string'], [true, true, true, true, true]);
}
const WIN = CAT.systems.find((s) => s.id === 'windows');
eq('windows: every language of the page, hashes unique', [WIN.arch.amd64.isos.length >= 38, WIN.arch.amd64.isos.every((i) => /^[a-z]{2}(-Latn)?-[A-Z]{2}$/.test(i.lang) && /^[0-9a-f]{64}$/.test(i.sha256)),
  new Set(WIN.arch.amd64.isos.map((i) => i.sha256)).size === WIN.arch.amd64.isos.length], [true, true, true]);
eq('windows: editions with Microsoft\'s KMS client keys, Home without', [WIN.editions.find((e) => e.name === 'Windows 11 Pro').kms, WIN.editions.find((e) => e.name === 'Windows 11 Home').kms,
  WIN.editions.every((e) => e.kms === '' || /^[A-Z0-9]{5}(-[A-Z0-9]{5}){4}$/.test(e.kms))], ['W269N-WFGWX-YVC9B-4J6C9-T83GX', '', true]);
eq('Fedora list found in a listing', [card('fedora').listed, card('debian').listed], [true, false]);
eq('entryOf', [entryOf(CAT, 'debian', 'arm64').sums, entryOf(CAT, 'nothing', 'amd64'), entryOf(CAT, 'debian', 'riscv64')], ['SHA256SUMS', null, null]);
eq('garbage catalog', systemsFor({ systems: [null, 3, { id: 'x' }] }, 'amd64'), []);

// ---- the makers' lists (taken from the real ones on 2026-11-06, hashes replaced) ----
const debian = `${H('a')}  debian-13.7.0-amd64-netinst.iso\n${H('b')}  debian-edu-13.7.0-amd64-netinst.iso\n${H('c')}  debian-mac-13.7.0-amd64-netinst.iso\n`;
eq('gnu list', parseSums(debian).map((f) => f.name), ['debian-13.7.0-amd64-netinst.iso', 'debian-edu-13.7.0-amd64-netinst.iso', 'debian-mac-13.7.0-amd64-netinst.iso']);
eq('debian: the netinst, not edu or mac', pickImage(entryOf(CAT, 'debian', 'amd64'), debian), { name: 'debian-13.7.0-amd64-netinst.iso', sha: H('a'), size: 0 });
const ubuntu = `${H('1')} *ubuntu-26.04-desktop-amd64.iso\n${H('2')} *ubuntu-26.04-live-server-amd64.iso\n${H('3')} *ubuntu-26.04.1-desktop-amd64.iso\r\n${H('4')} *ubuntu-26.04.1-live-server-amd64.iso\n`;
eq('ubuntu: the newest point release', pickImage(entryOf(CAT, 'ubuntu', 'amd64'), ubuntu)?.name, 'ubuntu-26.04.1-desktop-amd64.iso');
eq('ubuntu arm64 list has none for amd64', pickImage(entryOf(CAT, 'ubuntu', 'arm64'), ubuntu), null);
const fedora = `-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\n# Fedora-Workstation-Live-45-1.4.x86_64.iso: 2925092864 bytes\nSHA256 (Fedora-Workstation-Live-45-1.4.x86_64.iso) = ${H('D')}\n-----BEGIN PGP SIGNATURE-----\n\niQIz…\n-----END PGP SIGNATURE-----\n`;
eq('bsd list with size, hash lowered', pickImage(entryOf(CAT, 'fedora', 'amd64'), fedora), { name: 'Fedora-Workstation-Live-45-1.4.x86_64.iso', sha: H('d'), size: 2925092864 });
const kali = `${H('e')}  kali-linux-2026.3-installer-amd64.iso\n${H('f')}  kali-linux-2026.3-installer-amd64.iso.torrent\n${H('0')}  kali-linux-2026.3-installer-everything-amd64.iso\n${H('9')}  kali-linux-2026.3-installer-netinst-amd64.iso\n${H('8')}  kali-linux-2026.3-installer-arm64.iso\n`;
eq('kali: the installer, not .torrent, everything or netinst', pickImage(entryOf(CAT, 'kali', 'amd64'), kali)?.sha, H('e'));
eq('kali arm64', pickImage(entryOf(CAT, 'kali', 'arm64'), kali)?.name, 'kali-linux-2026.3-installer-arm64.iso');
eq('odd lines skipped', parseSums(`${H('a')}  ../x.iso\n${H('a')}\tx.iso\n${H('a').slice(1)}  x.iso\nSHA512 (x.iso) = ${H('a')}\n${H('a')}  x y.iso\n`), []);
eq('empty', [parseSums(''), parseSums(null), pickImage(entryOf(CAT, 'debian', 'amd64'), '')], [[], [], null]);

// ---- a folder's listing (Fedora's, 2026-11-06) ----
const listing = '<a href="?C=N;O=D">Name</a><a href="/pub/fedora/linux/releases/45/Workstation/x86_64/">Parent</a>' +
  '<a href="Fedora-Workstation-45-1.4-x86_64-CHECKSUM">x</a><a href="Fedora-Workstation-Live-45-1.4.x86_64.iso">y</a><a href="Fedora-Workstation-45-1.10-x86_64-CHECKSUM">z</a>';
const fe = entryOf(CAT, 'fedora', 'amd64');
eq('listing names', listingNames(listing, fe.sumsPattern), ['Fedora-Workstation-45-1.4-x86_64-CHECKSUM', 'Fedora-Workstation-45-1.10-x86_64-CHECKSUM']);
eq('newest compose by number, not text', newest(listingNames(listing, fe.sumsPattern), fe.sumsPattern), 'Fedora-Workstation-45-1.10-x86_64-CHECKSUM');
eq('arm64 pattern ignores x86_64', listingNames(listing, entryOf(CAT, 'fedora', 'arm64').sumsPattern), []);

// ---- versions ----
eq('compare', [compareNames('debian-13.10.0', 'debian-13.9.0') > 0, compareNames('ubuntu-26.04', 'ubuntu-26.04.1') < 0, compareNames('a', 'a')], [true, true, 0]);
eq('versions', [versionIn('debian-13.7.0-amd64-netinst.iso', '13'), versionIn('ubuntu-26.04.1-desktop-amd64.iso', '26.04'), versionIn('ubuntu-26.04-desktop-amd64.iso', '26.04'),
  versionIn('Fedora-Workstation-Live-45-1.4.x86_64.iso', '45'), versionIn('kali-linux-2026.3-installer-amd64.iso', '2026'), versionIn('x.iso', '9')], ['13.7', '26.04.1', '26.04', '45', '2026.3', '9']);

// ---- progress and sizes ----
eq('progress', [progressOf('progress download 1048576 2097152'), progressOf('progress lists 1 5'), progressOf('')], [{ done: 1048576, total: 2097152 }, null, null]);
eq('sizes', [sizeMb(760, 0), sizeMb(760, 2925092864)], [760, 2790]);

eq('resumed', [resumedOf('vm.downloadResumed from=412000000'), resumedOf('progress download 1 2'), resumedOf(null)], [412000000, 0, 0]);

// ---- the makers' signed lists: every card names its signature, key file and fingerprints ----
for (const s of CAT.systems.filter((x) => !pinnedOf(x) && x.signature?.type !== 'lordos-release')) {
  eq(`${s.id}: signed`, [['detached', 'clearsigned'].includes(s.signature?.type), /^[a-z0-9-]+\.asc$/.test(s.keys), (s.fingerprints || []).every((f) => /^[0-9A-F]{40}$/.test(f)) && s.fingerprints.length > 0,
    typeof s.keySource === 'string' && typeof s.fingerprintChecked === 'string' && s.fingerprintChecked.includes('https://')], [true, true, true, true]);
  eq(`${s.id}: key file pinned in the package`, fs.existsSync(path.join(here, '../../packaging/keys/machines', s.keys)), true);
}
// the Fedora list as the helper keeps it: only the signed text, which still gives the image, its hash and size
const fed = fs.readFileSync(path.join(here, 'vmimage/fedora-amd64.checksum'), 'utf8');
const signed = fed.slice(fed.indexOf('\n\n') + 2, fed.indexOf('-----BEGIN PGP SIGNATURE-----'));
eq('fedora signed text', pickImage(entryOf(CAT, 'fedora', 'amd64'), signed)?.name, 'Fedora-Workstation-Live-45-1.4.x86_64.iso');
eq('debian published list', pickImage(entryOf(CAT, 'debian', 'amd64'), fs.readFileSync(path.join(here, 'vmimage/debian-amd64.sums'), 'utf8'))?.name, 'debian-13.7.0-amd64-netinst.iso');

// ---- LordOS (its own release on GitHub, signed with the LordOS release key; vmcatalog.h VMCAT_SIG_LORDOS) ----
const LOS = CAT.systems.find((s) => s.id === 'lordos');
eq('lordos: the card', [card('lordos').name, card('lordos').kind, card('lordos', 'arm64').name, card('lordos').approxMb > 900 && card('lordos').approxMb < 2048], ['LordOS', 'iso', 'LordOS', true]);
eq('lordos: the release key, no key file', [LOS.signature, LOS.keys, LOS.fingerprints], [{ type: 'lordos-release', name: 'SHA256SUMS.sig' }, undefined, undefined]);
const tagOf = (dir) => (/\/releases\/download\/(v[0-9]+(?:\.[0-9]+){2,3}(?:-[a-z0-9.]+)?)\/$/.exec(dir) || [])[1] || '';
eq('lordos: one pinned release tag for both', [!!tagOf(LOS.arch.amd64.dir), tagOf(LOS.arch.amd64.dir) === tagOf(LOS.arch.arm64.dir), LOS.arch.amd64.dir.startsWith('https://github.com/lordbasex/LordOS/releases/download/')], [true, true, true]);
// the signed list is read first from its copy on lordos.org (GitHub's release downloads sometimes answer 403), the same
// tag as the ISO folder, with an extension (CloudFront serves extensionless paths as pages); GitHub's list is the fallback
eq('lordos: the list from lordos.org, same tag, GitHub kept as fallback', ['amd64', 'arm64'].map((a) => {
  const x = LOS.arch[a], l = x.list || {};
  return [l.dir === `https://lordos.org/releases/${tagOf(x.dir)}/`, l.sums, l.signature, x.sums, /\.[a-z]+$/.test(l.sums || '')];
}), [[true, 'SHA256SUMS.txt', 'SHA256SUMS.sig', 'SHA256SUMS', true], [true, 'SHA256SUMS.txt', 'SHA256SUMS.sig', 'SHA256SUMS', true]]);
eq('lordos: only LordOS mirrors its list', CAT.systems.filter((s) => Object.values(s.arch || {}).some((a) => a?.list)).map((s) => s.id), ['lordos']);
eq('lordos: the card says the tag\'s version',tagOf(LOS.arch.amd64.dir).replace(/^v/, '').replace(/-.*$/, ''), LOS.release);
// the site's list of current images (web/content/downloads.json) is never newer than the release the card names: the release
// process moves the tag (docs/RELEASING.md, step 1), so a release with new ISOs and an old card fails here
const DL = JSON.parse(fs.readFileSync(path.join(here, '../../web/content/downloads.json'), 'utf8')).downloads;
const vnum = (v) => String(v).replace(/^v/, '').replace(/-.*$/, '').split('.').map(Number);
const cmpv = (a, b) => { const x = vnum(a), y = vnum(b); for (let i = 0; i < 4; i++) { const d = (x[i] || 0) - (y[i] || 0); if (d) return d; } return 0; };
eq('lordos: the card\'s release is the site\'s current ISO or newer', ['iso-amd64', 'iso-arm64'].every((k) => DL[k] && cmpv(tagOf(LOS.arch.amd64.dir), DL[k].release) >= 0), true);
const lsums = fs.readFileSync(path.join(here, 'vmimage/lordos.sums'), 'utf8');
eq('lordos: the live ISO from the signed list', [pickImage(entryOf(CAT, 'lordos', 'amd64'), lsums), pickImage(entryOf(CAT, 'lordos', 'arm64'), lsums)?.name],
  [{ name: 'lordos-live-0.8.9-amd64.iso', sha: '2ef62214e5a5791bc03ccfbdc5fa0f310fa71fc14c810d23527840eaf0344848', size: 0 }, 'lordos-live-0.8.9-arm64.iso']);
eq('lordos: the site\'s hash is the signed list\'s', [DL['iso-amd64'].sha256 === pickImage(entryOf(CAT, 'lordos', 'amd64'), lsums).sha || DL['iso-amd64'].release !== 'v0.8.9-beta'], [true]);
eq('lordos: never the OVA or a source list', pickImage(entryOf(CAT, 'lordos', 'amd64'), `${H('a')}  lordos-0.8.9-amd64.ova\n${H('b')}  lordos-live-0.8.9-amd64.sources.tsv\n`), null);
eq('lordos: the version', [versionIn('lordos-live-0.8.9-amd64.iso', '0.8.9'), versionIn('lordos-live-0.8.9.1-amd64.iso', '0.8.9')], ['0.8.9', '0.8.9.1']);
eq('lordos: unattended from the live ISO itself', unattendedOf(CAT, 'lordos', 'arm64'), { kind: 'lordos', image: 'lordos', kernel: 'live/vmlinuz', initrd: 'live/initrd.img',
  append: 'boot=live components loglevel=3 nosplash lordos.auto=1 console=tty1', release: '0.8.9' });
eq('lordos: language', [lordosLang('es'), lordosLang('zh-Hans'), lordosLang(''), lordosLang('x y'), lordosLang(undefined)], ['es', 'zh-Hans', 'en', 'en', 'en']);
const small = { vcpus: 2, memoryMb: 2048, diskGb: 20, gfx: '2d', network: 'shared' };
eq('lordos: its least size over a small profile', cardSize(CAT, 'lordos', small, { cpus: 8 }), { vcpus: 4, memoryMb: 4096, diskGb: 32, gfx: '3d', network: 'shared' });
eq('lordos: a bigger profile stays', cardSize(CAT, 'lordos', { vcpus: 8, memoryMb: 16384, diskGb: 80, gfx: '2d' }, { cpus: 16 }), { vcpus: 8, memoryMb: 16384, diskGb: 80, gfx: '3d' });
eq('lordos: never more processors than here', cardSize(CAT, 'lordos', small, { cpus: 2 }).vcpus, 2);
eq('other cards: the profile as is', cardSize(CAT, 'debian', small, { cpus: 8 }), small);

// ---- downloaded images (Storage) ----
const A = H('a'), B = H('b'), D = H('d');
const infos = { [A]: { id: 'debian', arch: 'amd64', file: 'debian-13.7.0-amd64-netinst.iso', size: 790626304 }, [B]: { id: 'kali', arch: 'amd64', file: 'kali-linux-2026.3-installer-amd64.iso' } };
const entries = [{ name: `${A}.iso`, size: 790626304 }, { name: `${A}.json`, size: 90 }, { name: `${B}.iso.part`, size: 1000 }, { name: `${B}.json`, size: 90 },
  { name: `${D}.iso`, size: 5 }, { name: 'x.iso', size: 9 }, { name: `${H('e')}.iso`, size: 7, link: true }, { name: `${H('f')}.iso`, dir: true }, { name: `${A}.lock`, size: 0 }];
const ms = [{ name: 'Deb', dir: '/h/Deb.lordvm', iso: `${IMAGES_DIR}/${A}.iso`, state: 'running' }, { name: 'Deb 2', dir: '/h/Deb 2.lordvm', iso: `${IMAGES_DIR}/${A}.iso`, state: 'stopped' },
  { name: 'Other', dir: '/h/O.lordvm', iso: '/home/x/other.iso', state: 'running' }];
const imgs = downloadedImages(CAT, entries, (sha) => { if (!infos[sha]) throw new Error('none'); return infos[sha]; }, ms);
eq('images: which', imgs.map((x) => [x.sha[0], x.partial, x.size]), [['a', false, 790626304], ['b', true, 1000], ['d', false, 5]]);
eq('images: labels', imgs.map((x) => [x.label, x.arch, x.id]), [['Debian 13.7', 'amd64', 'debian'], ['Kali 2026.3', 'amd64', 'kali'], ['', '', 'other']]);
eq('images: users and running', [imgs[0].users, imgs[0].dirs, imgs[0].running, imgs[1].users], [['Deb', 'Deb 2'], ['/h/Deb.lordvm', '/h/Deb 2.lordvm'], ['Deb'], []]);
eq('images: a system the person added shows its file and site, never the query', downloadedImages(CAT, [{ name: `${'c'.repeat(64)}.iso`, size: 5 }],
  () => ({ id: 'user', arch: '', file: 'mint-22.iso', from: 'https://u:pw@mirror.example.org/mint-22.iso?token=SECRET' }), []).map((x) => [x.id, x.label]), [['other', 'mint-22.iso · mirror.example.org']]);
eq('images: a part next to its image', downloadedImages(CAT, [{ name: `${A}.iso.part`, size: 3 }, { name: `${A}.iso`, size: 9 }], () => null, []).map((x) => [x.partial, x.size]), [[false, 9]]);
eq('images: a bad info file', downloadedImages(CAT, [{ name: `${A}.iso`, size: 9 }], () => ({ id: 'debian', arch: 'x', file: '../../etc' }), [])[0].file, '');
const st = { total: 1000, free: 100, machines: [{ total: 300 }], bases: [], isos: 250, trash: 10 };
eq('bar with downloads', M.storageBar(st, [{ size: 200, users: ['Deb'] }, { size: 50, users: [] }]).map((b) => [b.key, b.bytes]), [['machines', 300], ['isos', 50], ['downloads', 250], ['trash', 10], ['other', 290]]);
eq('bar without downloads', M.storageBar(st).map((b) => b.key), ['machines', 'isos', 'trash', 'other']);

// ---- unattended installs ----
eq('hidden entries have no card', systemsFor(CAT, 'amd64').some((s) => s.id.includes('-')), false);
eq('unattended: debian', unattendedOf(CAT, 'debian', 'amd64'), { kind: 'preseed', image: 'debian', kernel: 'install.amd/vmlinuz', initrd: 'install.amd/initrd.gz', append: 'auto=true priority=critical --- quiet', release: '13' });
// the installers on the machine's screen on arm64: the firmware's ACPI SPCR table makes the serial port the kernel's
// console there (seen on the Pi 5: Debian's and Kali's installers only on the serial port), so arm64 has its own line
for (const id of ['debian', 'ubuntu', 'fedora', 'kali']) {
  eq(`unattended: ${id} arm64 on the screen`, / console=tty0 acpi=nospcr /.test(unattendedOf(CAT, id, 'arm64')?.append), true);
  eq(`unattended: ${id} amd64 as its maker boots it`, /console=|acpi=/.test(unattendedOf(CAT, id, 'amd64')?.append), false);
}
eq('unattended: an architecture\'s own line', unattendedOf({ systems: [{ id: 'x', unattended: { kind: 'preseed', append: 'a', arch: { amd64: { kernel: 'k', initrd: 'i' }, arm64: { kernel: 'k', initrd: 'i', append: 'b' } } }, arch: { amd64: { file: '^x$' }, arm64: { file: '^x$' } } }] }, 'x', 'arm64')?.append, 'b');
eq('unattended: debian arm64', unattendedOf(CAT, 'debian', 'arm64')?.kernel, 'install.a64/vmlinuz');
eq('unattended: ubuntu from the server image', [unattendedOf(CAT, 'ubuntu', 'amd64')?.kind, unattendedOf(CAT, 'ubuntu', 'amd64')?.image], ['autoinstall', 'ubuntu-server']);
eq('unattended: fedora from the network installer', [unattendedOf(CAT, 'fedora', 'arm64')?.kind, unattendedOf(CAT, 'fedora', 'arm64')?.image, unattendedOf(CAT, 'fedora', 'amd64')?.append.includes('{label}')], ['kickstart', 'fedora-netinst', true]);
eq('unattended: kali, a preseed from its own disc', unattendedOf(CAT, 'kali', 'amd64'), { kind: 'kali', image: 'kali', kernel: 'install.amd/vmlinuz', initrd: 'install.amd/initrd.gz',
  append: 'net.ifnames=0 preseed/file=/cdrom/simple-cdd/default.preseed simple-cdd/profiles=kali,offline desktop=xfce auto=true priority=critical --- quiet', release: '2026' });
eq('unattended: kali arm64', unattendedOf(CAT, 'kali', 'arm64')?.kernel, 'install.a64/vmlinuz');
eq('unattended: an unknown kind is none', unattendedOf({ systems: [{ id: 'x', unattended: { kind: 'other', arch: { amd64: { kernel: 'k', initrd: 'i' } } }, arch: { amd64: { file: '^x$' } } }] }, 'x', 'amd64'), null);
eq('ubuntu server image in the signed list', pickImage(entryOf(CAT, 'ubuntu-server', 'amd64'), fs.readFileSync(path.join(here, 'vmimage/ubuntu-amd64.sums'), 'utf8'))?.name, 'ubuntu-26.04.1-live-server-amd64.iso');
eq('ubuntu arm64 server image, not largemem', pickImage(entryOf(CAT, 'ubuntu-server', 'arm64'), fs.readFileSync(path.join(here, 'vmimage/ubuntu-arm64.sums'), 'utf8'))?.name, 'ubuntu-26.04.1-live-server-arm64.iso');
eq('fedora netinst pattern', [new RegExp(entryOf(CAT, 'fedora-netinst', 'amd64').file).test('Fedora-Everything-netinst-x86_64-45-1.4.iso'), new RegExp(entryOf(CAT, 'fedora-netinst', 'amd64').sumsPattern).test('Fedora-Everything-45-1.4-x86_64-CHECKSUM')], [true, true]);
eq('setup ok', checkSetup({ user: 'ana', password: 'x', hostname: 'debian-13' }), { ok: true, errors: {} });
eq('setup errors', checkSetup({ user: 'Root', password: '', hostname: '-x' }).errors, { user: 'user', password: 'passwordEmpty', hostname: 'hostname' });
eq('setup reserved', checkSetup({ user: 'root', password: 'x', hostname: 'h' }).errors, { user: 'userReserved' });
eq('hostname from a name', [hostnameFor('Debian 13.7'), hostnameFor('Ubuntu Señal 26.04!'), hostnameFor('***')], ['debian-13-7', 'ubuntu-senal-26-04', 'machine']);
eq('hostname follows the machine', [hostnameNext('', '', 'Debian 13'), hostnameNext('debian-13', 'debian-13', 'LordOS 0.8.9'),
  hostnameNext('my-box', 'debian-13', 'LordOS 0.8.9'), hostnameNext('debian-13', '', 'LordOS 0.8.9')], ['debian-13', 'lordos-0-8-9', 'my-box', 'debian-13']);
eq('user from home', [userFor('/home/federico'), userFor('/home/Root'), userFor('/root')], ['federico', 'user', 'user']);

console.log(`== vm-catalog-test: ${n - bad} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
