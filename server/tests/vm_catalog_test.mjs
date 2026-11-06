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
const { systemsFor, entryOf, parseSums, listingNames, newest, pickImage, versionIn, progressOf, resumedOf, compareNames, sizeMb, downloadedImages, IMAGES_DIR } = C;
const M = await import(path.join(here, '../../apps/machines/model.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const H = (c) => c.repeat(64);

// ---- the cards ----
eq('amd64 cards, Debian first', systemsFor(CAT, 'amd64').map((s) => s.id), ['debian', 'ubuntu', 'fedora', 'kali']);
eq('arm64 cards', systemsFor(CAT, 'arm64').map((s) => s.id), ['debian', 'ubuntu', 'fedora', 'kali']);
eq('no cards for another arch', systemsFor(CAT, 'riscv64'), []);
eq('Debian 13', [systemsFor(CAT, 'amd64')[0].name, systemsFor(CAT, 'amd64')[0].release], ['Debian', '13']);
eq('every entry has its hash source and date', CAT.systems.every((s) => s.hashSource && /^\d{4}-\d\d-\d\d$/.test(s.checked)), true);
eq('every folder is https', CAT.systems.every((s) => Object.values(s.arch).every((a) => /^https:\/\/[^/]+\/.*\/$/.test(a.dir) && (!a.fileDir || /^https:\/\//.test(a.fileDir)))), true);
eq('Fedora list found in a listing', [systemsFor(CAT, 'amd64')[2].listed, systemsFor(CAT, 'amd64')[0].listed], [true, false]);
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
for (const s of CAT.systems) {
  eq(`${s.id}: signed`, [['detached', 'clearsigned'].includes(s.signature?.type), /^[a-z0-9-]+\.asc$/.test(s.keys), (s.fingerprints || []).every((f) => /^[0-9A-F]{40}$/.test(f)) && s.fingerprints.length > 0,
    typeof s.keySource === 'string' && typeof s.fingerprintChecked === 'string' && s.fingerprintChecked.includes('https://')], [true, true, true, true]);
  eq(`${s.id}: key file pinned in the package`, fs.existsSync(path.join(here, '../../packaging/keys/machines', s.keys)), true);
}
// the Fedora list as the helper keeps it: only the signed text, which still gives the image, its hash and size
const fed = fs.readFileSync(path.join(here, 'vmimage/fedora-amd64.checksum'), 'utf8');
const signed = fed.slice(fed.indexOf('\n\n') + 2, fed.indexOf('-----BEGIN PGP SIGNATURE-----'));
eq('fedora signed text', pickImage(entryOf(CAT, 'fedora', 'amd64'), signed)?.name, 'Fedora-Workstation-Live-45-1.4.x86_64.iso');
eq('debian published list', pickImage(entryOf(CAT, 'debian', 'amd64'), fs.readFileSync(path.join(here, 'vmimage/debian-amd64.sums'), 'utf8'))?.name, 'debian-13.7.0-amd64-netinst.iso');

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
eq('images: a part next to its image', downloadedImages(CAT, [{ name: `${A}.iso.part`, size: 3 }, { name: `${A}.iso`, size: 9 }], () => null, []).map((x) => [x.partial, x.size]), [[false, 9]]);
eq('images: a bad info file', downloadedImages(CAT, [{ name: `${A}.iso`, size: 9 }], () => ({ id: 'debian', arch: 'x', file: '../../etc' }), [])[0].file, '');
const st = { total: 1000, free: 100, machines: [{ total: 300 }], bases: [], isos: 250, trash: 10 };
eq('bar with downloads', M.storageBar(st, [{ size: 200, users: ['Deb'] }, { size: 50, users: [] }]).map((b) => [b.key, b.bytes]), [['machines', 300], ['isos', 50], ['downloads', 250], ['trash', 10], ['other', 290]]);
eq('bar without downloads', M.storageBar(st).map((b) => b.key), ['machines', 'isos', 'trash', 'other']);

console.log(`== vm-catalog-test: ${n - bad} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
