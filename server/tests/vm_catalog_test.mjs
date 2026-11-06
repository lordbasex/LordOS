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
const { systemsFor, entryOf, parseSums, listingNames, newest, pickImage, versionIn, progressOf, compareNames, sizeMb } = C;

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

console.log(`== vm-catalog-test: ${n - bad} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
