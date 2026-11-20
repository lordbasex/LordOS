// vm_windows_test.mjs — apps/machines/windows.js: Windows 11 in the New Machine assistant (docs/VIRTUALIZATION.md 4.3).
// node server/tests/vm_windows_test.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { windowsEntry, driversFor, matchIso, evalFor, evalFiles, newestDownload, windowsZone, inputLocale, editionPlan, normalizeKey,
  checkWindowsSetup, windowsHostname, windowsSize, userLocale, WIN_MIN } from '../../apps/machines/windows.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const CAT = JSON.parse(fs.readFileSync(path.join(here, '../../assets/machines/systems.json'), 'utf8'));
const ZONES = JSON.parse(fs.readFileSync(path.join(here, '../../assets/machines/windows-zones.json'), 'utf8'));
let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}

const W = windowsEntry(CAT, 'amd64');
eq('entry on amd64, none on arm64 yet', [!!W, windowsEntry(CAT, 'arm64')], [true, null]);
eq('drivers: virtio-win 0.1.271, pinned', driversFor(CAT, 'amd64'), { id: 'virtio-win', name: 'virtio-win-0.1.271.iso',
  sha: 'bbe6166ad86a490caefad438fef8aa494926cb0a1b37fa1212925cfd81656429', size: 726501376 });

// Microsoft's published hashes: Spanish (Spain) and Mexico are different ISOs
eq('match: Spanish', matchIso(W, '9828703784880421E6602504DFD99064E4CB739CC9B20ABA22E3934372A14F9A'), { lang: 'es-ES', label: 'Spanish' });
eq('match: Mexico', matchIso(W, '83231bcd4c509cfa51eb9a79056c6f1ca40df09110267832e780a881b9ee8f9e'), { lang: 'es-MX', label: 'Spanish (Mexico)' });
eq('match: Brazil', matchIso(W, 'd9ac87c1e9e47e0eb34eed443e5691c31781074e67608a51aee50d7c7039f684')?.lang, 'pt-BR');
eq('match: anything else refused', [matchIso(W, '0'.repeat(64)), matchIso(W, '')], [null, null]);

// the evaluation
eq('eval: three languages', evalFiles(W).map((f) => f.lang), ['en-US', 'es-ES', 'pt-BR']);
eq('eval: by the person\'s language', [evalFor(W, 'es')?.lang, evalFor(W, 'es_AR')?.lang, evalFor(W, 'pt')?.lang, evalFor(W, 'de')?.lang, evalFor(W, 'en_GB')?.lang],
  ['es-ES', 'es-ES', 'pt-BR', 'en-US', 'en-US']);
eq('eval: Microsoft\'s hash for Spanish', evalFor(W, 'es').sha, '8f344beaa1c84646534e35f416b2086f6bef54e521c09a997dc326a330e4c584');

// the ISO just downloaded from Microsoft's page
const dl = [{ name: 'Win11_26H2_Spanish_x64.iso', size: 7.5e9, mtime: 10 }, { name: 'Win11_26H2_English_x64v2.iso', size: 7.4e9, mtime: 20 },
  { name: 'Win11_26H2_English_Arm64.iso', size: 5e9, mtime: 30 }, { name: 'Win11_26H2_x64.iso.part', size: 3e9, mtime: 40 }, { name: 'notes.iso', size: 2e9, mtime: 50 },
  { name: 'Win11_small_x64.iso', size: 1000, mtime: 60 }];
eq('downloads: the newest x64 one', newestDownload(dl, 'amd64')?.name, 'Win11_26H2_English_x64v2.iso');
eq('downloads: arm64', newestDownload(dl, 'arm64')?.name, 'Win11_26H2_English_Arm64.iso');
eq('downloads: none', newestDownload([], 'amd64'), null);

// time zones (CLDR) and keyboards
eq('zones', [windowsZone(ZONES, 'America/Argentina/Buenos_Aires'), windowsZone(ZONES, 'America/Sao_Paulo'), windowsZone(ZONES, 'Europe/Madrid'),
  windowsZone(ZONES, 'America/Mexico_City'), windowsZone(ZONES, 'UTC'), windowsZone(ZONES, 'Nowhere/City')],
  ['Argentina Standard Time', 'E. South America Standard Time', 'Romance Standard Time', 'Central Standard Time (Mexico)', 'UTC', '']);
eq('keyboards', [inputLocale('latam', '', 'es-ES'), inputLocale('es', '', 'es-ES'), inputLocale('br', '', 'pt-BR'), inputLocale('us', 'intl', 'en-US'),
  inputLocale('jp', '', 'ja-JP')], ['080a:0000080a', '040a:0000040a', '0416:00010416', '0409:00020409', 'ja-JP']);

eq('formats from this computer', [userLocale('es_AR.UTF-8'), userLocale('pt_BR.UTF-8'), userLocale('C.UTF-8'), userLocale('')], ['es-AR', 'pt-BR', '', '']);

// editions and keys
eq('key normalized', [normalizeKey('vk7jg nphtm c97jm 9mpgt 3v66t'), normalizeKey('W269N-WFGWX-YVC9B-4J6C9-T83GX'), normalizeKey('123'), normalizeKey('')],
  ['VK7JG-NPHTM-C97JM-9MPGT-3V66T', 'W269N-WFGWX-YVC9B-4J6C9-T83GX', '', '']);
eq('edition: Pro without a key uses Microsoft\'s KMS client key', editionPlan(W, 'own', 'Windows 11 Pro', ''),
  { name: 'Windows 11 Pro', key: 'W269N-WFGWX-YVC9B-4J6C9-T83GX', ok: true, generic: true });
eq('edition: the person\'s key wins', editionPlan(W, 'own', 'Windows 11 Pro', 'aaaaa-bbbbb-ccccc-ddddd-eeeee'), { name: 'Windows 11 Pro', key: 'AAAAA-BBBBB-CCCCC-DDDDD-EEEEE', ok: true });
eq('edition: Home needs the person\'s key', editionPlan(W, 'own', 'Windows 11 Home', ''), { name: 'Windows 11 Home', key: '', ok: false, err: 'needsKey' });
eq('edition: a key that isn\'t one', editionPlan(W, 'own', 'Windows 11 Pro', 'abc').err, 'key');
eq('edition: evaluation has one image and no key', editionPlan(W, 'eval', 'Windows 11 Pro', 'x'), { name: '', key: '', ok: true });

// the account and computer, as vmunattend.c checks them
eq('setup ok', checkWindowsSetup({ user: 'Federico', hostname: 'WIN-PC', password: '' }).ok, true);
eq('setup errors', checkWindowsSetup({ user: ' bad', hostname: '12345', password: 'x'.repeat(200) }).errors, { user: 'winUser', hostname: 'winHost', password: 'tooLong' });
eq('setup: user too long, host with a dot', checkWindowsSetup({ user: 'a'.repeat(21), hostname: 'a.b' }).errors, { user: 'winUser', hostname: 'winHost' });
eq('hostnames', [windowsHostname('Windows 11 Pro'), windowsHostname('Máquina de prueba larga'), windowsHostname('123'), windowsHostname('')],
  ['WINDOWS-11-PRO', 'MAQUINA-DE-PRUE', 'WINDOWS', 'WINDOWS']);

// sizes
eq('size raised to Windows 11\'s minimum', windowsSize({ vcpus: 1, memoryMb: 2048, diskGb: 20, gfx: '2d' }, { cpus: 8 }), { vcpus: 2, memoryMb: 4096, diskGb: 64, gfx: '2d' });
eq('size kept when bigger', windowsSize({ vcpus: 6, memoryMb: 8192, diskGb: 100 }, { cpus: 8 }), { vcpus: 6, memoryMb: 8192, diskGb: 100 });
eq('minimum', WIN_MIN, { vcpus: 2, memoryMb: 4096, diskGb: 64 });

console.log(`== vm-windows-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
